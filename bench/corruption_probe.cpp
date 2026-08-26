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
//              prints the clean recoverable-unit count (entries for
//              FFHR/JSON/protobuf, segments for HL7v2).
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
#include <fstream>
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
std::size_t fastfhir_recover(const std::vector<uint8_t> &b)
{
  auto valid_validation = [&](std::size_t off)
  {
    if (off > b.size() || b.size() - off < 8)
      return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v == static_cast<uint64_t>(off);
  };
  auto known_resource_tag = [&](std::size_t off)
  {
    if (off > b.size() || b.size() - off < 10)
      return false;
    const uint16_t tag = static_cast<uint16_t>(b[off + 8]) |
                         static_cast<uint16_t>(b[off + 9] << 8);
    return tag == static_cast<uint16_t>(RECOVER_FF_PATIENT) ||
           tag == static_cast<uint16_t>(RECOVER_FF_OBSERVATION) ||
           tag == static_cast<uint16_t>(RECOVER_FF_CONDITION) ||
           tag == static_cast<uint16_t>(RECOVER_FF_ENCOUNTER) ||
           tag == static_cast<uint16_t>(RECOVER_FF_PROCEDURE);
  };
  auto read_u64 = [&](std::size_t off)
  {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    return v;
  };
  auto block_ok = [&](uint64_t off)
  {
    return off == FF_NULL_OFFSET ||
           (static_cast<std::size_t>(off) <= b.size() &&
            b.size() - static_cast<std::size_t>(off) >= 10 &&
            valid_validation(static_cast<std::size_t>(off)));
  };
  std::size_t recovered = 0;

  // Header pointer fields (CAPI-13: the Parser ctor SEGVs on in-bounds-but-
  // garbage offsets; pre-validate before constructing).
  const bool header_safe = b.size() >= 54 && block_ok(read_u64(16)) &&
                           block_ok(read_u64(26)) && block_ok(read_u64(34)) &&
                           block_ok(read_u64(42));
  if (header_safe)
  {
    const auto root = static_cast<std::size_t>(read_u64(16));
    if (root <= b.size() && b.size() - root >= 10 && valid_validation(root))
    {
      try
      {
        FastFHIR::Parser p(b.data(), b.size());
        auto root_node = p.root();
        if (root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>())
        {
          auto entries = root_node[FastFHIR::Fields::BUNDLE::ENTRY];
          if (entries)
          {
            const auto n = entries.as_node().size();
            for (std::size_t i = 0; i < n; ++i)
            {
              auto resource = entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
              if (!resource)
                continue;
              const auto slot = static_cast<std::size_t>(resource.absolute_offset());
              if (slot > b.size() || b.size() - slot < 8)
                continue;
              const auto target = static_cast<std::size_t>(LOAD_U64(b.data() + slot));
              if (target > b.size() || b.size() - target < 10)
                continue;
              if (valid_validation(target) && known_resource_tag(target))
              {
                ++recovered;
                continue;
              }
              // Resync: scan forward for the next self-consistent block.
              for (std::size_t p2 = target + 10; p2 <= b.size() && b.size() - p2 >= 10; ++p2)
              {
                if (valid_validation(p2) && known_resource_tag(p2))
                {
                  ++recovered;
                  break;
                }
              }
            }
            return recovered;
          }
        }
      }
      catch (const std::exception &)
      {
      }
    }
  }
  // Header/root unsafe: whole-stream scan for self-consistent resource blocks.
  for (std::size_t off = 0; off <= b.size() && b.size() - off >= 10; ++off)
    if (valid_validation(off) && known_resource_tag(off))
      ++recovered;
  return recovered;
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

int mode_count(const std::string &format, const std::string &in)
{
  const auto b = read_file(in);
  if (format == "fastfhir")
  {
#if defined(PROBE_HAS_FASTFHIR)
    FastFHIR::Parser p(b.data(), b.size());
    auto entries = p.root()[FastFHIR::Fields::BUNDLE::ENTRY];
    std::printf("%zu\n", entries ? entries.as_node().size() : 0);
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
  // --mode {corrupt|count|recover} --format F --bits K --seed S --in F --out F
  std::string mode, format, in, out;
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
  }

  try
  {
    if (mode == "count")
      return mode_count(format, in);
    if (mode == "recover")
      return mode_recover(format, in);
    if (mode == "corrupt")
      return mode_corrupt(format, bits, seed, in, out);
  }
  catch (const std::exception &ex)
  {
    std::fprintf(stderr, "error: %s\n", ex.what());
    return 2;
  }
  std::fprintf(stderr, "usage: corruption_probe --mode {corrupt|count|recover} --format F ...\n");
  return 2;
}
