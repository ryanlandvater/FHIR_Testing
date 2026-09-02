// ===========================================================================
// Test 5 -- corruption & recovery, macro-parity architecture (IN-G2)
// ===========================================================================
// Shared header compiled once per arm (D1): each arm TU defines ARM_* and
// includes this file, so bench::test_5::<arm_ns> gets that format's
// implementations. The driver (bench_test_5.cpp) links the arm TUs -- each
// of which exports its operations through a non-inline arm_ops_*() accessor
// at the bottom of this header -- and dispatches INDEPENDENT process modes:
//
//   --hash    <format> --in WIRE            calc_stream_hash -- structural
//                                           fingerprint (anchored units +
//                                           sha256); the BASELINE producer
//   --corrupt <format> --bits K --seed S    corrupt_stream -- flip k random
//               --in WIRE --out DAMAGED     STRUCTURAL bits (per-format)
//   --recover <format> --in DAMAGED         recover_stream -- resync from the
//                                           corrupted bytes ONLY, report the
//                                           recovered units + digest
//   --check   --baseline FILE --recovered FILE   verify recovered ⊆ baseline
//                                           (offset+tag AND parent anchor),
//                                           report integrity, print the
//                                           content-verified %
//   --positions <format> --in WIRE          structural-position count of a
//                                           clean wire (the damage-density
//                                           denominator -- flaw B)
//
// The check is a THIRD process holding the baseline: the recoverer never sees
// the clean artifact. "Recovered" means a unit whose two halves corroborate
// the clean structure -- content verification, not boundary survival, and the
// parent anchor (F3) makes misattachment fail the subset check instead of
// passing it (fixes recovery-test flaws C/F; handoff.md § Test 5).
// ===========================================================================

#pragma once

#include "harness.hpp"
#include "provenance.hpp"  // sha256

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifndef BENCH_ARM_NS
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
#elif defined(ARM_HL7V2)
#define BENCH_ARM_NS arm_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#define BENCH_ARM_NS arm_google_fhir
#else
#define BENCH_ARM_NS arm_none
#endif
#endif

namespace bench::test_5 {
inline namespace BENCH_ARM_NS {

// A recoverable unit's location + identity -- the atom the recovery is
// verified against. "Recovered" requires BOTH halves to match the baseline.
// `parent` is the referencing slot's absolute offset (parent block + field),
// the F3 anchor: without it a resync onto another block's child passes the
// subset check, and misattachment is invisible (the flaw the edge-level
// fingerprint exists to close). Non-FFHR arms leave it 0.
struct UnitRef {
  std::size_t parent = 0;
  std::size_t offset = 0;
  // 32-bit: the FFHR arms zero-extend 16-bit RECOVERY_TAGs, but HL7v2's tag is
  // the full 3-char segment name (OBX vs OBR must not collide, and a flip on
  // the name's third byte must not be invisible to the anchored check).
  std::uint32_t tag = 0;

  // THE DATA THE UNIT CARRIES, not merely the fact that it is findable.
  //
  // Every arm used to report a unit as recovered on the strength of its
  // HEADER alone -- v2 scanned for `XXX|`, the protobuf arm asked only whether
  // ParseFromArray succeeded, JSON looked for a `"resource"` marker. None of
  // them read the payload, so a record whose every data byte was destroyed
  // still counted. Measured on the v2 artifact: obliterating 95.4% of the
  // file -- every byte of clinical content replaced with 'Z' -- reported
  // pct=100.0 with digest_ok=1. Destroying all 14,701 segment terminators, or
  // 20,673 interior field separators, likewise reported 100.0.
  //
  // `content` is a hash of the unit's own data bytes, so an entry that is
  // still FINDABLE but no longer CORRECT is reported as `wrong` instead of
  // recovered: 01223 != 01223Nsomething. Identity (parent, offset, tag) says
  // the unit is there; content says it is intact. They are different
  // questions and the check now asks both.
  std::uint64_t content = 0;

  // Identity only -- deliberately NOT content. The check compares the two
  // separately so it can distinguish a LOST unit from a WRONG one.
  bool operator==(const UnitRef& o) const {
    return parent == o.parent && offset == o.offset && tag == o.tag;
  }
};

// FNV-1a: the content digest is an equality check over bytes, never a
// security boundary, and it has to stay identical across the four arms.
inline std::uint64_t content_hash(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// The bytes of one unit, clamped to the wire. An empty or out-of-range extent
// hashes as 0, which is distinguishable from any real payload.
inline std::uint64_t content_of(const std::vector<uint8_t>& wire, std::size_t from,
                                std::size_t to) {
  if (from >= wire.size() || to <= from)
    return 0;
  return content_hash(wire.data() + from, std::min(to, wire.size()) - from);
}

#if defined(ARM_FASTFHIR)
// Recovery requires a Memory arena — it is not a read-only file view like
// Parser — but the probe pipeline operates on raw byte vectors. Wrap them in
// a scratch arena whose written extent equals the byte length (claim_space
// advances the head that Memory::size() reports).
inline FastFHIR::Memory wrap_wire_bytes(const std::vector<uint8_t>& wire) {
  FastFHIR::Memory mem =
      FastFHIR::Memory::create(std::max<std::size_t>(wire.size(), 1));
  if (!wire.empty()) {
    mem.claim_space(wire.size());
    std::memcpy(mem.base(), wire.data(), wire.size());
  }
  return mem;
}
#endif

struct StreamFingerprint {
  std::vector<UnitRef> units;  // sorted by offset
  std::string digest;          // sha256 of the unit list -- report integrity stamp

  void finalize() {
    // Canonical, offset-sorted serialization of the unit list.
    std::string canon;
    for (const auto& u : units) {
      canon.append(reinterpret_cast<const char*>(&u.parent), sizeof(u.parent));
      canon.append(reinterpret_cast<const char*>(&u.offset), sizeof(u.offset));
      canon.append(reinterpret_cast<const char*>(&u.tag), sizeof(u.tag));
      canon.append(reinterpret_cast<const char*>(&u.content), sizeof(u.content));
    }
    bench::provenance::sha256::Ctx c;
    bench::provenance::sha256::update(c, canon.data(), canon.size());
    digest.assign(32, '\0');
    bench::provenance::sha256::finish(c, reinterpret_cast<unsigned char*>(digest.data()));
  }
};

// ---------------------------------------------------------------------------
// 1. calc_stream_hash -- structural fingerprint of a CLEAN stream
// ---------------------------------------------------------------------------
#if defined(ARM_FASTFHIR)
// Block-level baseline (IN-G2/F3): built from the block-reference enumerator
// over the offset-chain walk (Recovery::reachable_blocks), NOT the recovery
// pipeline —
// recover() exists for damaged streams and a baseline never touches it.
// The per-block enumerator is shared with recovery, so recovered ⊆ baseline
// is like-for-like. The unit is the referenced child block (offset, tag) —
// the two halves corroborated.
// A block's own bytes, sized from the COMPILED reflection table so the
// baseline and the recovery measure the same span for the same unit. This is
// what makes a repaired-but-mis-attached reference visible: the reference is
// present and its identity matches, but the block it now names carries
// another block's data, so the content halves disagree.
inline std::uint64_t ffhr_block_content(const std::vector<uint8_t>& wire,
                                        std::size_t child, RECOVERY_TAG tag) {
  // PAYLOAD ONLY -- past the VALIDATION word and RECOVERY tag.
  //
  // Those two are WITNESSES, not data. They are also the bytes corruption
  // targets, so hashing them made a block that recovery had correctly
  // diagnosed and repaired still compare unequal: the repair fixed the
  // witness, and the witness was in the hash. Identity already carries the
  // tag, so content asks only "is the DATA the same".
  const std::size_t span =
      static_cast<std::size_t>(FastFHIR::Recovery::derived_block_size(tag));
  const std::size_t head = static_cast<std::size_t>(DATA_BLOCK::HEADER_SIZE);
  if (span <= head)
    return 0;  // header-only block: no payload to compare
  return content_of(wire, child + head, child + span);
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  FastFHIR::Memory mem = wrap_wire_bytes(wire);
  FastFHIR::Recovery rec(mem);
  const auto refs = rec.reachable_blocks();
  for (const auto& ref : refs)
    fp.units.push_back(UnitRef{static_cast<std::size_t>(ref.parent + ref.field),
                               static_cast<std::size_t>(ref.child),
                               static_cast<std::uint16_t>(ref.declared),
                               ffhr_block_content(wire, static_cast<std::size_t>(ref.child),
                                                  ref.declared)});
  std::sort(fp.units.begin(), fp.units.end(),
            [](const UnitRef& a, const UnitRef& b) { return a.offset < b.offset; });
  fp.finalize();
  return fp;
}
#elif defined(ARM_JSON)
// The byte span of the resource object a `"resource"` marker introduces: back
// up to its opening brace, forward to the closing one before the next marker.
// Shared by the baseline and both recovery paths -- a content comparison is
// only meaningful if every path hashes the SAME bytes.
inline std::pair<std::size_t, std::size_t> json_resource_extent(const std::string& text,
                                                                std::size_t marker) {
  const auto next = text.find("\"resource\"", marker + 10);
  const auto end = (next == std::string::npos) ? text.size() : next;
  std::size_t open = marker;
  while (open > 0 && text[open] != '{')
    --open;
  std::size_t close = end;
  while (close > open) {
    const auto brace = text.rfind('}', close - 1);
    if (brace == std::string::npos || brace < open)
      break;
    if (brace + 1 >= end || text[brace + 1] == ',' || text[brace + 1] == ']' ||
        text[brace + 1] == '}') {
      close = brace + 1;
      break;
    }
    close = brace;
  }
  return {open, close};
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  const std::string text(wire.begin(), wire.end());
  for (std::size_t i = 0; i + 10 <= text.size(); ++i)
    if (text.compare(i, 10, "\"resource\"") == 0) {
      const auto [open, close] = json_resource_extent(text, i);
      fp.units.push_back(UnitRef{0, i, 0, content_of(wire, open, close)});
    }
  fp.finalize();
  return fp;
}
#elif defined(ARM_GOOGLE_FHIR)
// FIELD-LEVEL PROTOBUF, via reflection.
//
// The arm used to emit ONE unit per record and call it recovered whenever
// ParseFromArray() returned true. That answers "is this still valid protobuf",
// never "is this still the same data": protobuf's wire format is permissive,
// so a record with scrambled field bytes reparses happily and scored as fully
// recovered. It also made the arm's units 1,473 whole resources against
// FastFHIR's 44,130 references -- damage per unit differed by orders of
// magnitude, so the percentages were never comparable.
//
// Reflection walks whatever the descriptors say is populated, so this needs no
// per-resource code and cannot drift from the schema. One unit per LEAF value:
//   parent  = the record's byte offset (which record it belongs to)
//   offset  = a hash of the field PATH (stable identity inside the record)
//   tag     = the field number
//   content = a hash of the value itself
inline std::uint64_t pb_path_hash(const std::string& path) {
  return content_hash(reinterpret_cast<const std::uint8_t*>(path.data()), path.size());
}

inline std::string pb_scalar_string(const google::protobuf::Message& m,
                                    const google::protobuf::Reflection* refl,
                                    const google::protobuf::FieldDescriptor* f,
                                    int index) {
  using FD = google::protobuf::FieldDescriptor;
  const bool rep = f->is_repeated();
  switch (f->cpp_type()) {
    case FD::CPPTYPE_INT32:  return std::to_string(rep ? refl->GetRepeatedInt32(m, f, index)  : refl->GetInt32(m, f));
    case FD::CPPTYPE_INT64:  return std::to_string(rep ? refl->GetRepeatedInt64(m, f, index)  : refl->GetInt64(m, f));
    case FD::CPPTYPE_UINT32: return std::to_string(rep ? refl->GetRepeatedUInt32(m, f, index) : refl->GetUInt32(m, f));
    case FD::CPPTYPE_UINT64: return std::to_string(rep ? refl->GetRepeatedUInt64(m, f, index) : refl->GetUInt64(m, f));
    case FD::CPPTYPE_DOUBLE: return std::to_string(rep ? refl->GetRepeatedDouble(m, f, index) : refl->GetDouble(m, f));
    case FD::CPPTYPE_FLOAT:  return std::to_string(rep ? refl->GetRepeatedFloat(m, f, index)  : refl->GetFloat(m, f));
    case FD::CPPTYPE_BOOL:   return (rep ? refl->GetRepeatedBool(m, f, index) : refl->GetBool(m, f)) ? "1" : "0";
    case FD::CPPTYPE_ENUM: {
      const auto* e = rep ? refl->GetRepeatedEnum(m, f, index) : refl->GetEnum(m, f);
      return e ? e->name() : std::string();
    }
    case FD::CPPTYPE_STRING: {
      std::string scratch;
      return rep ? refl->GetRepeatedStringReference(m, f, index, &scratch)
                 : refl->GetStringReference(m, f, &scratch);
    }
    default: return std::string();
  }
}

inline void pb_walk(const google::protobuf::Message& msg, std::size_t record_off,
                    const std::string& prefix, StreamFingerprint& fp) {
  const auto* refl = msg.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  refl->ListFields(msg, &fields);
  for (const auto* f : fields) {
    const std::string base = prefix + "." + std::to_string(f->number());
    const bool is_msg =
        f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE;
    const int count = f->is_repeated() ? refl->FieldSize(msg, f) : 1;
    for (int i = 0; i < count; ++i) {
      const std::string path =
          f->is_repeated() ? base + "[" + std::to_string(i) + "]" : base;
      if (is_msg) {
        pb_walk(f->is_repeated() ? refl->GetRepeatedMessage(msg, f, i)
                                 : refl->GetMessage(msg, f),
                record_off, path, fp);
        continue;
      }
      const std::string value = pb_scalar_string(msg, refl, f, i);
      fp.units.push_back(UnitRef{
          record_off, static_cast<std::size_t>(pb_path_hash(path)),
          static_cast<std::uint32_t>(f->number()),
          content_hash(reinterpret_cast<const std::uint8_t*>(value.data()), value.size())});
    }
  }
}

// Parse one length-prefixed record and emit its leaf fields. Returns false if
// the record does not parse at all -- then it contributes nothing and every
// field it held is reported missing, which is the honest accounting.
inline bool pb_emit_record(const std::vector<uint8_t>& wire, std::size_t pos,
                           uint32_t len, char type, StreamFingerprint& fp) {
  if (type == 'P') {
    google::fhir::r4::core::Patient m;
    if (!m.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len))) return false;
    pb_walk(m, pos, "P", fp);
    return true;
  }
  google::fhir::r4::core::Observation m;
  if (!m.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len))) return false;
  pb_walk(m, pos, "O", fp);
  return true;
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    if (wire[pos] == 'P' || wire[pos] == 'O') {
      const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                           (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                           (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                           (static_cast<uint32_t>(wire[pos + 4]) << 24);
      pb_emit_record(wire, pos, len, static_cast<char>(wire[pos]), fp);
      pos += 5 + len;
    } else {
      ++pos;
    }
  }
  fp.finalize();
  return fp;
}
#elif defined(ARM_HL7V2)
// v2's segment types as little-endian 3-byte tags. The dictionary is the set
// the arm actually emits (hl7v2_message.hpp): MSH/PID once per message, OBX
// per observation, ZFX for the JSON passthrough -- measured on the fresh
// artifact: MSH=5, PID=5, OBX=1,468, ZFX=13,223. It is the v2 analogue of the
// FFHR reflection table or the protobuf arm's P/O pair: v2 declares no type
// set in-band, so a scanner can only recognize what the writer emits.
inline constexpr std::uint32_t v2_tag(char a, char b, char c) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16);
}
inline constexpr std::uint32_t kV2NameTags[] = {
    v2_tag('M', 'S', 'H'), v2_tag('P', 'I', 'D'),
    v2_tag('O', 'B', 'X'), v2_tag('Z', 'F', 'X')};

inline bool is_v2_known_tag(std::uint32_t tag) {
  for (std::uint32_t known : kV2NameTags)
    if (tag == known)
      return true;
  return false;
}

inline std::uint32_t v2_tag_at(const std::vector<uint8_t>& wire, std::size_t i) {
  return v2_tag(static_cast<char>(wire[i]), static_cast<char>(wire[i + 1]),
                static_cast<char>(wire[i + 2]));
}

// A segment header: the 3-char type name immediately followed by the field
// separator (|). This is v2's ONLY self-identifying structural anchor -- the
// analogue of a VALIDATION word / TLV header / {"resource" marker: it is what
// lets a receiver resync when a \r terminator is destroyed (the next segment
// is still findable inside the merged line, at its true offset).
inline bool v2_segment_start(const std::vector<uint8_t>& wire, std::size_t i) {
  return i + 4 <= wire.size() && wire[i + 3] == '|' &&
         is_v2_known_tag(v2_tag_at(wire, i));
}

// Whole-stream header scan. Baseline (clean bytes) and recovery (damaged
// bytes) are the SAME enumeration, so recovered ⊆ baseline is like-for-like:
// on the clean artifact this scan finds exactly the 14,701 \r-delimited
// segments, at identical offsets, with zero content false positives.
// A segment's DATA: everything after the `XXX|` header up to the terminator.
// Unterminated (the \r was destroyed) runs to end-of-wire, which is the point
// -- a merged record must not hash like an intact one.
inline std::size_t v2_segment_end(const std::vector<uint8_t>& wire, std::size_t i) {
  const auto cr = std::find(wire.begin() + static_cast<std::ptrdiff_t>(i), wire.end(),
                            static_cast<uint8_t>('\r'));
  return static_cast<std::size_t>(cr - wire.begin());
}

inline StreamFingerprint scan_v2_segments(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  for (std::size_t i = 0; i + 4 <= wire.size(); ++i)
    if (v2_segment_start(wire, i))
      // Content spans the payload only (past `XXX|`): the header is already
      // the identity, and hashing it twice would just double-count a name
      // flip that the tag half reports on its own.
      fp.units.push_back(UnitRef{0, i, v2_tag_at(wire, i),
                                 content_of(wire, i + 4, v2_segment_end(wire, i))});
  fp.finalize();
  return fp;
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  return scan_v2_segments(wire);
}
#endif

// ---------------------------------------------------------------------------
// 2. corrupt_stream -- polymorphic structural corruption
// ---------------------------------------------------------------------------
// Shared flip engine (used by every arm): XOR one random bit in each of the
// first min(k, N) shuffled structural positions. Positions come from the
// per-arm structural_positions() enumeration below; keeping the shuffle here
// means corruption semantics (deterministic per seed, never payload bytes)
// cannot drift between arms. `positions` is taken by value so callers can
// move a freshly enumerated vector in.
inline std::vector<uint8_t> flip_positions(const std::vector<uint8_t>& wire,
                                           std::vector<std::size_t> positions,
                                           std::size_t k, unsigned seed) {
  auto damaged = wire;
  std::vector<std::size_t> idx(positions.size());
  for (std::size_t i = 0; i < positions.size(); ++i)
    idx[i] = i;
  std::mt19937 rng(seed);
  std::shuffle(idx.begin(), idx.end(), rng);
  for (std::size_t i = 0; i < k && i < idx.size(); ++i)
    damaged[positions[idx[i]]] ^= static_cast<uint8_t>(1u << (rng() % 8));
  return damaged;
}
#if defined(ARM_FASTFHIR)
// Corruption targets = the structural witnesses of the LIVE-EDGE census, the
// same model FastFHIR's own recovery tests use (test_recovery.cpp picks a ref
// from a clean recover() and flips base[child] / base[slot]): the stream
// FF_HEADER region, then for every parent→child block reference from
// Recovery::reachable_blocks() -- the child's 10-byte header (VALIDATION +
// RECOVERY_TAG) and the parent's pointer slot (8 bytes, or the 10-byte
// {offset, tag} tuple a choice/resource slot stores). Scalar VALUES (string
// payloads, numbers, codes) are NEVER corrupted. Leaf-data slots (string/code
// references) stay excluded: recovery does not cross-validate them and a
// broken leaf reference has no witness to heal it (70feda9 design note).
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  std::vector<std::size_t> positions;
  // 1. FF_HEADER region (stream-level syntax).
  for (std::size_t i = 0; i < 54 && i < wire.size(); ++i)
    positions.push_back(i);
  FastFHIR::Memory mem = wrap_wire_bytes(wire);
  FastFHIR::Recovery rec(mem);
  const auto refs = rec.reachable_blocks();
  for (const auto& r : refs) {
    const bool tuple = r.kind == FF_FIELD_CHOICE || r.kind == FF_FIELD_RESOURCE;
    if (!(r.kind == FF_FIELD_BLOCK || r.kind == FF_FIELD_ARRAY || tuple))
      continue;
    // 2. The parent's slot naming the child (slot + tuple tag half).
    const std::size_t slot = static_cast<std::size_t>(r.parent + r.field);
    const std::size_t slot_len = tuple ? 10 : 8;
    for (std::size_t j = 0; j < slot_len && slot + j < wire.size(); ++j)
      positions.push_back(slot + j);
    // 3. The child's 10-byte block header (VALIDATION + RECOVERY_TAG).
    const std::size_t child = static_cast<std::size_t>(r.child);
    for (std::size_t j = 0; j < 10 && child + j < wire.size(); ++j)
      positions.push_back(child + j);
  }
  // Overlapping witnesses (an array element that is itself a header target)
  // must flip once: a double-XOR would silently cancel.
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  return positions;
}

inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  return flip_positions(wire, structural_positions(wire), k, seed);
}
#elif defined(ARM_JSON)
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  // Comparable blast rule (Ryan 2026-09-02): EVERY syntax character is a
  // corruption target unless it is EXPLICITLY ESCAPED -- in a string or out.
  // An escape pair (\X) skips both bytes; nothing else is exempt. A brace or
  // comma inside a string value is unescaped content that still LOOKS like
  // syntax to a blind scanner, so it gets blasted exactly like a structural
  // brace -- the recovery (reparse) is what decides what survives. This is
  // the same rule v2 applies to its delimiters (v2 has no escapes for them),
  // which is what makes the four arms comparable.
  auto is_syntax = [](uint8_t c) {
    return c == '{' || c == '}' || c == '[' || c == ']' || c == '"' || c == ':' || c == ',';
  };
  std::vector<std::size_t> positions;
  bool escaped = false;
  for (std::size_t i = 0; i < wire.size(); ++i) {
    if (escaped) {
      escaped = false;
      continue;  // \X: the escaped byte is explicitly exempt
    }
    if (wire[i] == '\\') {
      escaped = true;
      continue;  // the escape introducer itself is not a syntax char
    }
    if (is_syntax(wire[i]))
      positions.push_back(i);
  }
  return positions;
}

inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  return flip_positions(wire, structural_positions(wire), k, seed);
}
#elif defined(ARM_GOOGLE_FHIR)
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  std::vector<std::size_t> positions;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    if (wire[pos] == 'P' || wire[pos] == 'O') {
      const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                           (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                           (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                           (static_cast<uint32_t>(wire[pos + 4]) << 24);
      for (std::size_t j = 0; j < 5; ++j)
        positions.push_back(pos + j);
      pos += 5 + len;
    } else {
      ++pos;
    }
  }
  return positions;
}

inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  return flip_positions(wire, structural_positions(wire), k, seed);
}
#elif defined(ARM_HL7V2)
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  // v2's syntactic elements, matching what the recovery can resync on:
  // 1. segment terminators (\r) -- the boundary bytes;
  // 2. the 3-char type name of every segment -- the identity a resync
  //    recognises (a dictionary repair is impossible: nothing in-band
  //    cross-validates a name, so damage here is genuinely lost);
  // 3. the MSH-2 encoding-character set | ^ & ~ \ wherever they occur -- the
  //    field/component/subcomponent/repetition/escape separators. v2 has no
  //    in-string quoting, so a delimiter byte is syntax wherever it sits.
  // Value bytes between delimiters are NEVER corrupted.
  std::vector<std::size_t> positions;
  std::size_t start = 0;
  while (start < wire.size()) {
    if (start + 3 <= wire.size())
      for (std::size_t j = 0; j < 3; ++j)
        positions.push_back(start + j);
    const auto cr = std::find(wire.begin() + static_cast<std::ptrdiff_t>(start),
                              wire.end(), static_cast<uint8_t>('\r'));
    if (cr == wire.end())
      break;
    positions.push_back(static_cast<std::size_t>(cr - wire.begin()));
    start = static_cast<std::size_t>(cr - wire.begin()) + 1;
  }
  for (std::size_t i = 0; i < wire.size(); ++i)
    if (wire[i] == '|' || wire[i] == '^' || wire[i] == '&' || wire[i] == '~' ||
        wire[i] == '\\')
      positions.push_back(i);
  return positions;
}

inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  return flip_positions(wire, structural_positions(wire), k, seed);
}
#endif

// ---------------------------------------------------------------------------
// 3. recover_stream -- polymorphic recovery (corrupted bytes ONLY)
// ---------------------------------------------------------------------------
#if defined(ARM_FASTFHIR)
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  FastFHIR::Memory mem = wrap_wire_bytes(wire);
  FastFHIR::Recovery rec(mem);
  const auto rep = rec.recover();

  // APPLY THE REPAIRS BEFORE READING THE DATA.
  //
  // recover() only DIAGNOSES; apply() is the only mutating entry point and it
  // writes into a copy. Hashing `wire` here read the damaged bytes and scored
  // every correctly-repaired block as `wrong` -- the arm was reporting the
  // damage it had just fixed. The honest pipeline is
  // uncorrupted -> corrupted -> RECOVERED -> compare, and `repaired` is the
  // third step. A failed apply leaves the copy as-is, so this never reads
  // better than the engine actually achieved.
  std::vector<uint8_t> repaired;
  rec.apply(rep, repaired);
  const std::vector<uint8_t>& src = repaired.empty() ? wire : repaired;

  for (const auto& verdict : rep.blocks) {
    // Only RESTORED block references count (IN-G2): the two halves
    // corroborate. An ambiguous or unrecovered reference produces no unit, so
    // the subset check reports the under-recovery instead of papering over it.
    if (verdict.class_ == FastFHIR::RepairClass::Unrecovered ||
        verdict.class_ == FastFHIR::RepairClass::Ambiguous)
      continue;
    fp.units.push_back(UnitRef{static_cast<std::size_t>(verdict.block.parent + verdict.block.field),
                               static_cast<std::size_t>(verdict.block.child),
                               static_cast<std::uint16_t>(verdict.block.declared),
                               ffhr_block_content(src, static_cast<std::size_t>(verdict.block.child),
                                                  verdict.block.declared)});
  }
  std::sort(fp.units.begin(), fp.units.end(),
            [](const UnitRef& a, const UnitRef& b) { return a.offset < b.offset; });
  fp.finalize();
  return fp;
}
#elif defined(ARM_JSON)
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  const std::string text(wire.begin(), wire.end());
  try {
    (void)nlohmann::json::parse(text);
    for (std::size_t i = 0; i + 10 <= text.size(); ++i)
      if (text.compare(i, 10, "\"resource\"") == 0) {
        // A parseable document is not an INTACT one: a flipped byte inside a
        // string value keeps the JSON well-formed and changes the data.
        const auto [open, close] = json_resource_extent(text, i);
        fp.units.push_back(UnitRef{0, i, 0, content_of(wire, open, close)});
      }
    fp.finalize();
    return fp;
  } catch (const std::exception&) {
  }
  // Resync at each '"resource"' marker; a unit is recovered only when its
  // span parses (content verification).
  std::size_t pos = 0;
  while (true) {
    const auto marker = text.find("\"resource\"", pos);
    if (marker == std::string::npos)
      break;
    const auto next = text.find("\"resource\"", marker + 10);
    const auto end = (next == std::string::npos) ? text.size() : next;
    std::size_t open = marker;
    while (open > 0 && text[open] != '{')
      --open;
    std::size_t close = end;
    while (close > open) {
      const auto brace = text.rfind('}', close - 1);
      if (brace == std::string::npos || brace < open)
        break;
      if (brace + 1 >= end || text[brace + 1] == ',' || text[brace + 1] == ']' ||
          text[brace + 1] == '}') {
        close = brace + 1;
        break;
      }
      close = brace;
    }
    if (close > open) {
      try {
        (void)nlohmann::json::parse(text.substr(open, close - open));
        fp.units.push_back(UnitRef{0, marker, 0, content_of(wire, open, close)});
      } catch (const std::exception&) {
      }
    }
    pos = (next == std::string::npos) ? text.size() : next;
  }
  fp.finalize();
  return fp;
}
#elif defined(ARM_GOOGLE_FHIR)
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    const char type = static_cast<char>(wire[pos]);
    if (type != 'P' && type != 'O') {
      ++pos;
      continue;
    }
    const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                         (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                         (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                         (static_cast<uint32_t>(wire[pos + 4]) << 24);
    if (len > 0 && pos + 5 + len <= wire.size()) {
      bool ok = false;
      if (type == 'P') {
        google::fhir::r4::core::Patient patient;
        ok = patient.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len));
      } else {
        google::fhir::r4::core::Observation obs;
        ok = obs.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len));
      }
      (void)ok;  // pb_emit_record re-parses and walks; see its contract.
      pb_emit_record(wire, pos, len, type, fp);
      pos += 5 + len;
    } else {
      ++pos;
      for (; pos + 5 <= wire.size(); ++pos) {
        if (wire[pos] != 'P' && wire[pos] != 'O')
          continue;
        const uint32_t nlen = static_cast<uint32_t>(wire[pos + 1]) |
                              (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                              (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                              (static_cast<uint32_t>(wire[pos + 4]) << 24);
        if (nlen > 0 && pos + 5 + nlen <= wire.size())
          break;
      }
    }
  }
  fp.finalize();
  return fp;
}
#elif defined(ARM_HL7V2)
// Recovery = the SAME header scan as the baseline, run over the damaged bytes
// (like FastFHIR's scan()-style resync): a segment is recovered when its
// header pattern -- known 3-char type + '|' separator -- is still present at
// its true offset. That makes a destroyed \r recoverable (the merged line
// still contains the next segment's header) and a type-name or separator
// flip unrecoverable (v2 has no second witness to repair either).
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
  return scan_v2_segments(wire);
}
#endif

}  // inline namespace BENCH_ARM_NS

// ---------------------------------------------------------------------------
// Per-arm dispatch (driver side)
// ---------------------------------------------------------------------------
// The implementations above are `inline`: each arm TU compiles exactly its own
// macro-guarded copy, and a driver TU that CALLS them cross-TU would link
// against definitions it never saw (the ODR failure mode notes.md § 1 was
// written about). Each arm TU therefore exports one non-inline accessor below;
// the driver builds its format table from the four accessors and never touches
// the inline functions itself.
struct ArmOps {
  const char* name;  // format string accepted on the command line
  StreamFingerprint (*calc_hash)(const std::vector<uint8_t>& wire);
  std::vector<uint8_t> (*corrupt)(const std::vector<uint8_t>& wire,
                                  std::size_t bits, unsigned seed);
  StreamFingerprint (*recover)(const std::vector<uint8_t>& wire);
  // Structural-position count of a CLEAN wire -- the damage-density
  // denominator (handoff.md test-5 flaw B): k flips means something different
  // per format until the axis is "fraction of positions corrupted".
  std::size_t (*count_positions)(const std::vector<uint8_t>& wire);
};

const ArmOps& arm_ops_fastfhir();
const ArmOps& arm_ops_json();
const ArmOps& arm_ops_google_fhir();
const ArmOps& arm_ops_hl7v2();

}  // namespace bench::test_5
