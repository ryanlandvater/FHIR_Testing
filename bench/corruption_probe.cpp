// ===========================================================================
// Corruption probe -- Instrument G test 5 (cross-format recovery comparison)
// ===========================================================================
// Three INDEPENDENT processes, per the spec:
//
//   --corrupt  <format> --bits K --seed S --in CLEAN --out CORRUPTED
//              flips K random STRUCTURAL bits (per-format syntax regions:
//              FFHR FF_HEADER+block headers; JSON brace/bracket/quote/colon/
//              comma chars; protobuf TLV record headers; HL7v2 segment
//              terminators + names) and writes the corrupted artifact.
//
//   --count    <format> --in ARTIFACT
//              prints the clean recoverable-unit count. THE UNIT MUST MATCH
//              WHAT --recover COUNTS, per format, or the driver's ratio is
//              meaningless: block references for FFHR (Recovery::
//              reachable_blocks), resource markers for JSON/protobuf,
//              segments for HL7v2.
//
//   --recover  <format> --in CORRUPTED --clean-count N
//              attempts recovery FROM THE CORRUPTED BYTES ONLY (a scanner's
//              view: no offsets from the corruptor, no clean artifact) and
//              prints "recovered=M resyncs=R" for the driver to turn into a
//              percentage.
//
// The driver (scripts/recovery_sweep.py) invokes corrupt and recover as
// separate subprocesses so neither can leak state to the other.
//
// Recovery semantics per format (the resync idea, applied to each syntax):
//   fastfhir  -- verify each entry's block VALIDATION word; on damage, scan
//               forward for the next self-consistent block (VALIDATION==offset
//               + known resource tag). Header unsafe -> whole-stream scan.
//   json      -- attempt a full parse; on failure, resync at each '"resource"'
//               entry marker: try parsing the span to the next marker.
//   protobuf  -- walk the TLV records ('P'/'O' + u32 length); on an
//               out-of-bounds length, scan forward for the next record header
//               whose length fits; ParseFromArray to confirm.
//   hl7v2     -- segments are '\r'-terminated; resync at each line whose first
//               3 chars are a known segment name (MSH/PID/OBX/...).
//
// Structural bit flips never touch payload bytes (strings, numbers, message
// bodies) -- the JSON analogue is a syntax-region flip, not a value edit.
// Qualifiers preserved: malformed-not-hostile; integrity-not-authenticity.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <span>
#include <random>
#include <string>
#include <vector>

#if defined(PROBE_HAS_FASTFHIR)
#include "harness.hpp"
#endif
#if defined(PROBE_HAS_JSON)
#include <nlohmann/json.hpp>
#endif
#if defined(PROBE_HAS_PROTOBUF)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#endif
#if defined(PROBE_HAS_HL7V2)
#include "hl7v2_message.hpp"
#endif

namespace {

std::vector<uint8_t> read_file(const std::string &path)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("cannot open " + path);
  const auto n = in.tellg();
  std::vector<uint8_t> bytes(static_cast<std::size_t>(n));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(bytes.data()), n);
  return bytes;
}

void write_file(const std::string &path, const std::vector<uint8_t> &bytes)
{
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------
// Structural-position enumeration + bit flips
// ---------------------------------------------------------------------------

#if defined(PROBE_HAS_FASTFHIR)
std::vector<std::size_t> fastfhir_structural_positions(const std::vector<uint8_t> &b)
{
  std::vector<std::size_t> positions;
  // FF_HEADER region (54 bytes) -- parse-critical syntax.
  for (std::size_t i = 0; i < 54 && i < b.size(); ++i)
    positions.push_back(i);
  // Every reachable resource block's 10-byte header (VALIDATION + RECOVERY_TAG).
  FastFHIR::Parser p(b.data(), b.size());
  auto root = p.root();
  auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY];
  if (!entries)
    return positions;
  const auto n = entries.as_node().size();
  for (std::size_t i = 0; i < n; ++i)
  {
    auto resource = entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    if (!resource)
      continue;
    const auto slot = static_cast<std::size_t>(resource.absolute_offset());
    if (slot > b.size() || b.size() - slot < 8)
      continue;
    // F2 (IN-G2): the parent's own {offset|tag} tuple must be a flip target
    // too, or failure mode 1 (parent pointer corrupt) is unreachable and the
    // orphan sweep is never exercised by the probe that produced the numbers.
    for (std::size_t j = 0; j < 10 && slot + j < b.size(); ++j)
      positions.push_back(slot + j);
    const auto target = static_cast<std::size_t>(LOAD_U64(b.data() + slot));
    for (std::size_t j = 0; j < 10 && target + j < b.size(); ++j)
      positions.push_back(target + j);
  }
  return positions;
}
#endif

#if defined(PROBE_HAS_JSON)
// Syntax characters are the JSON "header region": braces, brackets, quotes,
// colons, commas. A bit flip in one is a syntax-region flip.
bool json_syntax_char(char c)
{
  return c == '{' || c == '}' || c == '[' || c == ']' || c == '"' || c == ':' || c == ',';
}

std::vector<std::size_t> json_structural_positions(const std::vector<uint8_t> &b)
{
  std::vector<std::size_t> positions;
  for (std::size_t i = 0; i < b.size(); ++i)
    if (json_syntax_char(static_cast<char>(b[i])))
      positions.push_back(i);
  return positions;
}
#endif

#if defined(PROBE_HAS_PROTOBUF)
std::vector<std::size_t> protobuf_structural_positions(const std::vector<uint8_t> &b)
{
  // TLV record headers: the 1-byte type ('P'/'O') + the 4-byte LE length.
  std::vector<std::size_t> positions;
  for (std::size_t pos = 0; pos + 5 <= b.size();)
  {
    if (b[pos] == 'P' || b[pos] == 'O')
    {
      const uint32_t len = static_cast<uint32_t>(b[pos + 1]) |
                           (static_cast<uint32_t>(b[pos + 2]) << 8) |
                           (static_cast<uint32_t>(b[pos + 3]) << 16) |
                           (static_cast<uint32_t>(b[pos + 4]) << 24);
      for (std::size_t j = 0; j < 5; ++j)
        positions.push_back(pos + j);
      pos += 5 + len;
    }
    else
    {
      ++pos;
    }
  }
  return positions;
}
#endif

#if defined(PROBE_HAS_HL7V2)
bool hl7v2_segment_name(char a, char b, char c)
{
  static const char *kNames[] = {"MSH", "PID", "OBX", "OBR", "PV1", "ORC", "ZFX", "NTE"};
  char name[4] = {a, b, c, 0};
  for (const char *n : kNames)
    if (std::strcmp(name, n) == 0)
      return true;
  return false;
}

std::vector<std::size_t> hl7v2_structural_positions(const std::vector<uint8_t> &b)
{
  // Segment terminators ('\r') and the first 3 chars of every segment name.
  std::vector<std::size_t> positions;
  for (std::size_t i = 0; i < b.size(); ++i)
  {
    if (b[i] == '\r')
      positions.push_back(i);
  }
  // Segment starts: line beginnings (offset 0 or after a '\r').
  std::size_t start = 0;
  while (start < b.size())
  {
    if (start + 3 <= b.size())
      for (std::size_t j = 0; j < 3; ++j)
        positions.push_back(start + j);
    const auto cr = std::find(b.begin() + static_cast<std::ptrdiff_t>(start), b.end(),
                              static_cast<uint8_t>('\r'));
    if (cr == b.end())
      break;
    start = static_cast<std::size_t>(cr - b.begin()) + 1;
  }
  return positions;
}
#endif

// ---------------------------------------------------------------------------
// Recovery -- works from the corrupted bytes ONLY
// ---------------------------------------------------------------------------

#if defined(PROBE_HAS_FASTFHIR)
// Recovery requires a Memory arena -- it is not a read-only file view like
// Parser -- but the probe pipeline operates on raw byte vectors. Wrap them in
// a scratch arena whose written extent equals the byte length (claim_space
// advances the head Memory::size() reports).
FastFHIR::Memory fastfhir_wrap(const std::vector<uint8_t> &b)
{
  FastFHIR::Memory mem = FastFHIR::Memory::create(std::max<std::size_t>(b.size(), 1));
  if (!b.empty()) {
    mem.claim_space(b.size());
    std::memcpy(mem.base(), b.data(), b.size());
  }
  return mem;
}

// The CLEAN baseline: the denominator of the recovery rate.
//
// This used to be `Bundle.entry`'s array length, which predates the recovery
// engine and is a DIFFERENT ATOM from what recovery counts. On the shipped
// 1.05 MB artifact it read 1,473 while the recovery half read 16,071 block
// references, so recovery_sweep.py divided references by bundle entries and
// reported 1091% recovered AT ZERO BITS CORRUPTED. Any FFHR figure on the
// pre-2026-08-31 fig8 curve is invalid for that reason alone.
//
// reachable_blocks() is the library's own baseline entry point and enumerates
// exactly the atom recover() reconciles -- the parent->child block reference --
// so numerator and denominator are now the same unit and a clean stream scores
// 100%. It is also the RIGHT call: the offset-chain walk with no byte census
// and no classification, because a baseline is bytes the caller vouches for.
// recover() on a clean stream would pay for a scan it has no use for, and
// FF_Recovery.hpp says so at the entry point.
std::size_t fastfhir_count(const std::vector<uint8_t> &b)
{
  FastFHIR::Memory mem = fastfhir_wrap(b);
  FastFHIR::Recovery rec(mem);
  return rec.reachable_blocks().size();
}

// Diagnostic: the full reconciliation breakdown for one artifact. Exists to
// answer "is a shortfall recovery FAILING, or references never being
// ENUMERATED" -- those are different findings and the single recovered= number
// cannot tell them apart.
void fastfhir_report(const std::vector<uint8_t> &b)
{
  FastFHIR::Memory mem = fastfhir_wrap(b);
  FastFHIR::Recovery rec(mem);
  const auto rep = rec.recover();
  std::printf("blocks_total=%zu intact=%zu corroborated=%zu tag_repaired=%zu "
              "position_repaired=%zu extent_derived=%zu ambiguous=%zu unrecovered=%zu "
              "holes=%zu version_skew=%zu\n",
              rep.blocks_total, rep.intact, rep.corroborated, rep.tag_repaired,
              rep.position_repaired, rep.extent_derived, rep.ambiguous,
              rep.unrecovered, rep.holes, rep.version_skew);
}

std::size_t fastfhir_recover(const std::vector<uint8_t> &b)
{
  // The recovery routine lives in the LIBRARY (FastFHIR::Recovery): a scanner
  // that reconciles BOTH witnesses of every parent→child block reference
  // (TASKS.md P0-3, REC-10…17).
  //
  // Ambiguous and Unrecovered are excluded because neither is a recovery: the
  // first has more than one candidate at equal bit cost, the second none. The
  // remaining classes each restored a reference whose two halves corroborate.
  //
  // Holes (REC-18) are deliberately NOT counted or reported here. A hole is
  // what is left when damage happens to take out both witnesses of the same
  // block; it is an outcome of the corruption draw, not a property of the
  // format worth its own measurement, and reporting it as a separate rate
  // would invite reading it as a fifth arm.
  FastFHIR::Memory mem = fastfhir_wrap(b);
  FastFHIR::Recovery rec(mem);
  const auto rep = rec.recover();
  return rep.blocks_total - rep.ambiguous - rep.unrecovered;
}
#endif

#if defined(PROBE_HAS_JSON)
std::size_t json_recover(const std::vector<uint8_t> &b)
{
  const std::string text(b.begin(), b.end());
  // Fast path: the whole document still parses.
  try
  {
    (void)nlohmann::json::parse(text);
    // Count entries by a light scan (parse succeeded; the clean count from
    // --count is what the driver divides by).
    std::size_t n = 0;
    for (std::size_t i = 0; i + 10 <= text.size(); ++i)
      if (text.compare(i, 10, "\"resource\"") == 0)
        ++n;
    return n;
  }
  catch (const std::exception &)
  {
  }
  // Full parse failed: resync at each '"resource"' marker and try to parse the
  // span up to the next marker (the JSON analogue of VALIDATION resync).
  std::size_t recovered = 0;
  std::size_t pos = 0;
  while (true)
  {
    const auto marker = text.find("\"resource\"", pos);
    if (marker == std::string::npos)
      break;
    const auto next = text.find("\"resource\"", marker + 10);
    const auto end = (next == std::string::npos) ? text.size() : next;
    // Back up to the entry's opening brace, then find its CLOSING brace: the
    // last '}' before the next marker whose successor is a terminator (',',
    // ']' or '}'). The span between them is one complete entry -- including
    // the trailing ',' or the next entry's '{' would fail every parse.
    std::size_t open = marker;
    while (open > 0 && text[open] != '{')
      --open;
    std::size_t close = end;
    while (close > open)
    {
      const auto brace = text.rfind('}', close - 1);
      if (brace == std::string::npos || brace < open)
        break;
      if (brace + 1 >= end || text[brace + 1] == ',' || text[brace + 1] == ']' ||
          text[brace + 1] == '}')
      {
        close = brace + 1;
        break;
      }
      close = brace;
    }
    if (close <= open)
    {
      pos = (next == std::string::npos) ? text.size() : next;
      continue;
    }
    const std::string span = text.substr(open, close - open);
    try
    {
      (void)nlohmann::json::parse(span);
      ++recovered;
    }
    catch (const std::exception &)
    {
    }
    pos = (next == std::string::npos) ? text.size() : next;
  }
  return recovered;
}
#endif

#if defined(PROBE_HAS_PROTOBUF)
std::size_t protobuf_recover(const std::vector<uint8_t> &b)
{
  std::size_t recovered = 0;
  for (std::size_t pos = 0; pos + 5 <= b.size();)
  {
    const char type = static_cast<char>(b[pos]);
    if (type != 'P' && type != 'O')
    {
      // Damaged record header: resync at the next plausible header.
      ++pos;
      continue;
    }
    const uint32_t len = static_cast<uint32_t>(b[pos + 1]) |
                         (static_cast<uint32_t>(b[pos + 2]) << 8) |
                         (static_cast<uint32_t>(b[pos + 3]) << 16) |
                         (static_cast<uint32_t>(b[pos + 4]) << 24);
    if (len > 0 && pos + 5 + len <= b.size())
    {
      if (type == 'P')
      {
        google::fhir::r4::core::Patient patient;
        if (patient.ParseFromArray(b.data() + pos + 5, static_cast<int>(len)))
          ++recovered;
      }
      else
      {
        google::fhir::r4::core::Observation obs;
        if (obs.ParseFromArray(b.data() + pos + 5, static_cast<int>(len)))
          ++recovered;
      }
      pos += 5 + len;
    }
    else
    {
      // Length points past EOF: damaged header. Resync: scan for the next
      // 'P'/'O' whose length fits.
      ++pos;
      for (; pos + 5 <= b.size(); ++pos)
      {
        if (b[pos] != 'P' && b[pos] != 'O')
          continue;
        const uint32_t nlen = static_cast<uint32_t>(b[pos + 1]) |
                              (static_cast<uint32_t>(b[pos + 2]) << 8) |
                              (static_cast<uint32_t>(b[pos + 3]) << 16) |
                              (static_cast<uint32_t>(b[pos + 4]) << 24);
        if (nlen > 0 && pos + 5 + nlen <= b.size())
          break;
      }
    }
  }
  return recovered;
}
#endif

#if defined(PROBE_HAS_HL7V2)
std::size_t hl7v2_recover(const std::vector<uint8_t> &b)
{
  // Segments are '\r'-terminated. Recovery = count lines that still begin
  // with a known segment name after resync at the next '\r'.
  std::size_t recovered = 0;
  std::size_t start = 0;
  while (start < b.size())
  {
    const auto cr = std::find(b.begin() + static_cast<std::ptrdiff_t>(start), b.end(),
                              static_cast<uint8_t>('\r'));
    const std::size_t line_end = (cr == b.end()) ? b.size()
                                                 : static_cast<std::size_t>(cr - b.begin());
    const std::size_t len = line_end - start;
    if (len >= 3 && hl7v2_segment_name(static_cast<char>(b[start]),
                                       static_cast<char>(b[start + 1]),
                                       static_cast<char>(b[start + 2])))
      ++recovered;
    if (cr == b.end())
      break;
    start = line_end + 1;
  }
  return recovered;
}
#endif

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

// ===========================================================================
// CONTENT VERIFICATION -- what the count-based modes structurally cannot see
// ===========================================================================
// --count/--recover answer "how many units still look plausible". They cannot
// answer the question that matters: how much of the DATA came back, and how
// much came back WRONG. A value silently restored to the wrong number is worse
// than one reported missing, and no count can tell those apart.
//
// So each arm also repairs a document and extracts its leaf values as sorted
// `path<TAB>value` lines, in its OWN native addressing -- FHIR element paths
// for the FHIR-shaped arms, SEG[n]-field.component for HL7v2. Native, not a
// shared schema: HL7v2 is a lossy transform of FHIR and genuinely does not hold
// the same leaf set, so each arm is scored against its own ground truth and the
// comparison across formats is on rates of self-preservation.
//
// A third process (--mode verify) walks baseline against repaired and reports
// four outcomes, because two are not enough:
//
//   correct   present, and equal to the original
//   lost      absent after repair            -- honest loss
//   wrong     present, but a DIFFERENT value -- silent corruption
//   spurious  present, with no original      -- invented data
//
// `wrong` and `spurious` are the integrity figures. A format can score well on
// recovery and badly on those, and that is the finding worth publishing.
// ---------------------------------------------------------------------------

using ValueLines = std::vector<std::string>;

static void emit(ValueLines &out, const std::string &path, const std::string &value)
{
  out.push_back(path + "\t" + value);
}

#if defined(PROBE_HAS_FASTFHIR)
// Repair = recover() then apply(): the report is turned into a repaired COPY,
// the damaged original is left alone (REC-15). Everything downstream reads the
// copy, so the extraction measures what a consumer would actually get back.
static std::vector<uint8_t> fastfhir_repair(const std::vector<uint8_t> &b)
{
  FastFHIR::Memory mem = fastfhir_wrap(b);
  FastFHIR::Recovery rec(mem);
  const auto rep = rec.recover();
  std::vector<BYTE> fixed;
  rec.apply(rep, fixed);
  return std::vector<uint8_t>(fixed.begin(), fixed.end());
}

// Walk the node tree rather than print_json()+reparse: a single broken subtree
// makes whole-document JSON unparseable, which would score every surviving
// value as lost and flatter the failure. Walking per leaf keeps the damage
// local, which is the honest accounting.
static void ffhr_walk(const FastFHIR::Reflective::Node &node, const std::string &path,
                      ValueLines &out, int depth, uint32_t version)
{
  if (!node || depth > 64)
    return;
  if (node.is_array()) {
    // PER ELEMENT, NOT PER ARRAY. entries() throws on a damaged element -- a
    // polymorphic tuple whose declared tag disagrees with the block it names
    // raises rather than returning a partial list -- and the document's whole
    // payload hangs off ONE array (Bundle.entry). Taking that array in one
    // call meant a single bad element aborted the entire extraction: measured,
    // this fell off a cliff from 10,483 values at 32 flipped bits to ZERO at
    // 48, which reads as total data loss and is nothing of the kind.
    std::size_t n = 0;
    try { n = node.size(); } catch (const std::exception &) { return; }
    for (std::size_t i = 0; i < n; ++i) {
      try {
        ffhr_walk(node[i], path + "[" + std::to_string(i) + "]", out, depth + 1, version);
      } catch (const std::exception &) {
        // this element is unreadable; the rest of the array is not
      }
    }
    return;
  }
  if (!node.is_object()) {
    std::ostringstream v;
    node.print_json(v);
    emit(out, path, v.str());
    return;
  }
  std::span<const FF_FieldInfo> fields;
  try { fields = node.fields(); } catch (const std::exception &) { return; }
  for (const auto &f : fields) {
    const FF_FieldKey key = FF_FieldKey::from_cstr(
        node.recovery(), f.kind, f.field_offset, f.child_recovery,
        f.array_entries_are_offsets, f.name);
    const FastFHIR::Reflective::Entry e = node[key];
    if (!e)
      continue;
    const std::string child = path.empty() ? std::string(f.name) : path + "." + f.name;
    if (ff_kind_is_inline_scalar(f.kind)) {
      std::ostringstream v;
      try { e.print_scalar_json(v, version); } catch (const std::exception &) { continue; }
      emit(out, child, v.str());
      continue;
    }
    try { ffhr_walk(e.as_node(), child, out, depth + 1, version); } catch (const std::exception &) {}
  }
}

static ValueLines fastfhir_extract(const std::vector<uint8_t> &b)
{
  ValueLines out;
  try {
    FastFHIR::Parser p(b.data(), b.size());
    const auto root = p.root();
    ffhr_walk(root, "", out, 0, p.version());
  } catch (const std::exception &) {
  }
  return out;
}
#endif

#if defined(PROBE_HAS_JSON)
static void json_flatten(const nlohmann::json &j, const std::string &path, ValueLines &out)
{
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      json_flatten(it.value(), path.empty() ? it.key() : path + "." + it.key(), out);
  } else if (j.is_array()) {
    for (std::size_t i = 0; i < j.size(); ++i)
      json_flatten(j[i], path + "[" + std::to_string(i) + "]", out);
  } else {
    emit(out, path, j.dump());
  }
}

// Repair is a no-op transform: JSON's recovery IS the resync-and-reparse the
// extractor already performs, so repairing then extracting would run it twice.
static std::vector<uint8_t> json_repair(const std::vector<uint8_t> &b) { return b; }

static ValueLines json_extract(const std::vector<uint8_t> &b)
{
  ValueLines out;
  const std::string text(b.begin(), b.end());
  try {
    json_flatten(nlohmann::json::parse(text), "", out);
    return out;
  } catch (const std::exception &) {
  }
  // Whole-document parse failed: salvage each resource span, exactly as
  // json_recover resyncs, and index it by its position so a lost span shifts
  // nothing after it.
  std::size_t pos = 0, idx = 0;
  while (true) {
    const auto marker = text.find("\"resource\"", pos);
    if (marker == std::string::npos)
      break;
    const auto next = text.find("\"resource\"", marker + 10);
    const auto open = text.find('{', marker + 10);
    if (open != std::string::npos) {
      const auto end = (next == std::string::npos) ? text.size() : next;
      int depth = 0;
      std::size_t close = std::string::npos;
      for (std::size_t i = open; i < end; ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0) { close = i; break; }
      }
      if (close != std::string::npos) {
        try {
          json_flatten(nlohmann::json::parse(text.substr(open, close - open + 1)),
                       "entry[" + std::to_string(idx) + "].resource", out);
        } catch (const std::exception &) {
        }
      }
    }
    ++idx;
    if (next == std::string::npos)
      break;
    pos = next;
  }
  return out;
}
#endif

#if defined(PROBE_HAS_HL7V2)
static std::vector<uint8_t> hl7v2_repair(const std::vector<uint8_t> &b) { return b; }

// SEG[n]-field.component, the format's own addressing. This is where a
// misplaced pipe finally shows up: it shifts every later field in the segment,
// so values land under the WRONG path -- scored `wrong`, not `lost`, which the
// segment-name survival check cannot see at all.
static ValueLines hl7v2_extract(const std::vector<uint8_t> &b)
{
  ValueLines out;
  const std::string text(b.begin(), b.end());
  std::size_t start = 0, seg_idx = 0;
  while (start < text.size()) {
    const auto cr = text.find('\r', start);
    const std::string line = text.substr(start, (cr == std::string::npos ? text.size() : cr) - start);
    if (line.size() >= 3) {
      const std::string name = line.substr(0, 3);
      std::size_t fpos = 0, fidx = 0;
      while (fpos <= line.size()) {
        const auto bar = line.find('|', fpos);
        const std::string field = line.substr(fpos, (bar == std::string::npos ? line.size() : bar) - fpos);
        if (!field.empty() && fidx > 0) {
          std::size_t cpos = 0, cidx = 1;
          while (cpos <= field.size()) {
            const auto hat = field.find('^', cpos);
            const std::string comp = field.substr(cpos, (hat == std::string::npos ? field.size() : hat) - cpos);
            if (!comp.empty())
              emit(out, name + "[" + std::to_string(seg_idx) + "]-" + std::to_string(fidx) +
                            "." + std::to_string(cidx), comp);
            if (hat == std::string::npos) break;
            cpos = hat + 1; ++cidx;
          }
        }
        if (bar == std::string::npos) break;
        fpos = bar + 1; ++fidx;
      }
      ++seg_idx;
    }
    if (cr == std::string::npos) break;
    start = cr + 1;
  }
  return out;
}
#endif

// --mode extract: repair, then emit the repaired document's leaf values as
// sorted `path<TAB>value` lines. Sorted so verify is a linear merge and the
// output is diffable by hand when a result looks wrong.
int mode_extract(const std::string &format, const std::string &in)
{
  const auto b = read_file(in);
  ValueLines lines;
  // Repair runs outside the extractor's own try/catch, and FastFHIR::Recovery
  // is documented to RETHROW worker exceptions -- on extreme damage
  // recover()/apply() can throw. A recovery routine that throws on its input
  // recovered nothing: score the trial as zero values instead of letting one
  // bad draw abort the whole sweep (observed: k=512 FFHR trial crashed the
  // data-axis run).
  auto guarded = [&](std::vector<uint8_t> (*repair)(const std::vector<uint8_t> &),
                     ValueLines (*extract)(const std::vector<uint8_t> &)) {
    try {
      return extract(repair(b));
    } catch (const std::exception &ex) {
      std::fprintf(stderr, "extract: %s repair threw -- 0 values recovered: %s\n",
                   format.c_str(), ex.what());
      return ValueLines{};
    }
  };
  if (format == "fastfhir") {
#if defined(PROBE_HAS_FASTFHIR)
    lines = guarded(fastfhir_repair, fastfhir_extract);
#else
    return 2;
#endif
  } else if (format == "json") {
#if defined(PROBE_HAS_JSON)
    lines = guarded(json_repair, json_extract);
#else
    return 2;
#endif
  } else if (format == "hl7v2") {
#if defined(PROBE_HAS_HL7V2)
    lines = guarded(hl7v2_repair, hl7v2_extract);
#else
    return 2;
#endif
  } else {
    std::fprintf(stderr, "extract: unsupported format '%s'\n", format.c_str());
    return 2;
  }
  std::sort(lines.begin(), lines.end());
  for (const std::string &l : lines)
    std::printf("%s\n", l.c_str());
  return 0;
}

// --mode verify: a THIRD process, holding the baseline the recoverer never saw.
// Reports the four outcomes. `wrong` is the one no count-based mode can produce
// and the one that matters most: data that came back, and came back false.
int mode_verify(const std::string &baseline, const std::string &recovered)
{
  const auto load = [](const std::string &path) {
    std::multimap<std::string, std::string> m;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      const auto tab = line.find('\t');
      if (tab == std::string::npos)
        continue;
      m.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
    return m;
  };
  auto base = load(baseline);
  auto rep  = load(recovered);

  std::size_t correct = 0, wrong = 0, lost = 0;
  const std::size_t total = base.size();
  for (auto it = base.begin(); it != base.end(); ++it) {
    auto range = rep.equal_range(it->first);
    if (range.first == range.second) { ++lost; continue; }
    // A path may repeat (repeating fields); match a value if any instance of
    // that path carries it, and consume it so counts cannot double-credit.
    bool matched = false;
    for (auto r = range.first; r != range.second; ++r)
      if (r->second == it->second) { rep.erase(r); matched = true; break; }
    if (matched) ++correct;
    else { ++wrong; rep.erase(range.first); }
  }
  const std::size_t spurious = rep.size();
  std::printf("total=%zu correct=%zu lost=%zu wrong=%zu spurious=%zu\n",
              total, correct, lost, wrong, spurious);
  return 0;
}

int mode_count(const std::string &format, const std::string &in)
{
  const auto b = read_file(in);
  if (format == "fastfhir")
  {
#if defined(PROBE_HAS_FASTFHIR)
    std::printf("%zu\n", fastfhir_count(b));
    return 0;
#endif
  }
  else if (format == "json")
  {
#if defined(PROBE_HAS_JSON)
    const std::string text(b.begin(), b.end());
    std::size_t n = 0;
    for (std::size_t i = 0; i + 10 <= text.size(); ++i)
      if (text.compare(i, 10, "\"resource\"") == 0)
        ++n;
    std::printf("%zu\n", n);
    return 0;
#endif
  }
  else if (format == "protobuf")
  {
#if defined(PROBE_HAS_PROTOBUF)
    std::size_t n = 0;
    for (std::size_t pos = 0; pos + 5 <= b.size();)
    {
      if (b[pos] == 'P' || b[pos] == 'O')
      {
        ++n;
        const uint32_t len = static_cast<uint32_t>(b[pos + 1]) |
                             (static_cast<uint32_t>(b[pos + 2]) << 8) |
                             (static_cast<uint32_t>(b[pos + 3]) << 16) |
                             (static_cast<uint32_t>(b[pos + 4]) << 24);
        pos += 5 + len;
      }
      else
      {
        ++pos;
      }
    }
    std::printf("%zu\n", n);
    return 0;
#endif
  }
  else if (format == "hl7v2")
  {
#if defined(PROBE_HAS_HL7V2)
    std::size_t n = 0;
    std::size_t start = 0;
    while (start < b.size())
    {
      const auto cr = std::find(b.begin() + static_cast<std::ptrdiff_t>(start), b.end(),
                                static_cast<uint8_t>('\r'));
      const std::size_t line_end = (cr == b.end()) ? b.size()
                                                   : static_cast<std::size_t>(cr - b.begin());
      if (line_end - start >= 3 && hl7v2_segment_name(static_cast<char>(b[start]),
                                                      static_cast<char>(b[start + 1]),
                                                      static_cast<char>(b[start + 2])))
        ++n;
      if (cr == b.end())
        break;
      start = line_end + 1;
    }
    std::printf("%zu\n", n);
    return 0;
#endif
  }
  std::fprintf(stderr, "unknown format '%s' (or support not compiled in)\n", format.c_str());
  return 2;
}

int mode_recover(const std::string &format, const std::string &in)
{
  const auto b = read_file(in);
  std::size_t recovered = 0;
  if (format == "fastfhir")
  {
#if defined(PROBE_HAS_FASTFHIR)
    recovered = fastfhir_recover(b);
#else
    return 2;
#endif
  }
  else if (format == "json")
  {
#if defined(PROBE_HAS_JSON)
    recovered = json_recover(b);
#else
    return 2;
#endif
  }
  else if (format == "protobuf")
  {
#if defined(PROBE_HAS_PROTOBUF)
    recovered = protobuf_recover(b);
#else
    return 2;
#endif
  }
  else if (format == "hl7v2")
  {
#if defined(PROBE_HAS_HL7V2)
    recovered = hl7v2_recover(b);
#else
    return 2;
#endif
  }
  else
  {
    std::fprintf(stderr, "unknown format '%s'\n", format.c_str());
    return 2;
  }
  std::printf("recovered=%zu\n", recovered);
  return 0;
}

int mode_corrupt(const std::string &format, std::size_t bits, unsigned seed,
                 const std::string &in, const std::string &out)
{
  const auto clean = read_file(in);
  std::vector<std::size_t> positions;
  if (format == "fastfhir")
  {
#if defined(PROBE_HAS_FASTFHIR)
    positions = fastfhir_structural_positions(clean);
#else
    return 2;
#endif
  }
  else if (format == "json")
  {
#if defined(PROBE_HAS_JSON)
    positions = json_structural_positions(clean);
#else
    return 2;
#endif
  }
  else if (format == "protobuf")
  {
#if defined(PROBE_HAS_PROTOBUF)
    positions = protobuf_structural_positions(clean);
#else
    return 2;
#endif
  }
  else if (format == "hl7v2")
  {
#if defined(PROBE_HAS_HL7V2)
    positions = hl7v2_structural_positions(clean);
#else
    return 2;
#endif
  }
  else
  {
    std::fprintf(stderr, "unknown format '%s'\n", format.c_str());
    return 2;
  }

  if (positions.empty())
  {
    std::fprintf(stderr, "no structural positions found in %s\n", in.c_str());
    return 2;
  }

  auto corrupt = clean;
  std::vector<std::size_t> idx(positions.size());
  for (std::size_t i = 0; i < positions.size(); ++i)
    idx[i] = i;
  std::mt19937 rng(seed);
  std::shuffle(idx.begin(), idx.end(), rng);
  const std::size_t nflip = std::min(bits, idx.size());
  for (std::size_t i = 0; i < nflip; ++i)
    corrupt[positions[idx[i]]] ^= static_cast<uint8_t>(1u << (rng() % 8));

  write_file(out, corrupt);
  std::fprintf(stderr, "[corrupt] %s: flipped %zu structural bits at %zu positions\n",
               format.c_str(), nflip, positions.size());
  return 0;
}

}  // namespace

int main(int argc, char **argv)
{
  // --mode {corrupt|count|recover|report|extract|verify} --format F --bits K
  //        --seed S --in F --out F --baseline F --recovered F
  std::string mode, format, in, out, baseline, recovered;
  std::size_t bits = 0;
  unsigned seed = 0;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&](const char *name) -> std::string
    {
      if (i + 1 >= argc)
        throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (a == "--mode")
      mode = next("--mode");
    else if (a == "--format")
      format = next("--format");
    else if (a == "--bits")
      bits = static_cast<std::size_t>(std::strtoull(next("--bits").c_str(), nullptr, 10));
    else if (a == "--seed")
      seed = static_cast<unsigned>(std::strtoul(next("--seed").c_str(), nullptr, 10));
    else if (a == "--in")
      in = next("--in");
    else if (a == "--out")
      out = next("--out");
    else if (a == "--baseline")
      baseline = next("--baseline");
    else if (a == "--recovered")
      recovered = next("--recovered");
  }

  try
  {
    if (mode == "count")
      return mode_count(format, in);
    if (mode == "recover")
      return mode_recover(format, in);
    if (mode == "corrupt")
      return mode_corrupt(format, bits, seed, in, out);
    if (mode == "extract")
      return mode_extract(format, in);
    if (mode == "verify")
      return mode_verify(baseline, recovered);
#if defined(PROBE_HAS_FASTFHIR)
    if (mode == "report") {
      fastfhir_report(read_file(in));
      return 0;
    }
#endif
  }
  catch (const std::exception &ex)
  {
    std::fprintf(stderr, "error: %s\n", ex.what());
    return 2;
  }
  std::fprintf(stderr, "usage: corruption_probe --mode {corrupt|count|recover|report|extract|verify} --format F ...\n");
  return 2;
}
