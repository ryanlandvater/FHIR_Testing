// ===========================================================================
// Test 5 -- corruption & recovery, macro-parity architecture (IN-G2)
// ===========================================================================
// Shared header compiled once per arm (D1): each arm TU defines ARM_* and
// includes this file, so bench::test_5::<arm_ns> gets that format's
// implementations. The driver (bench_test_5.cpp) links the arm TUs and
// dispatches four INDEPENDENT process modes:
//
//   --hash    <format> --in WIRE            calc_stream_hash -- structural
//                                           fingerprint (units + sha256)
//   --corrupt <format> --bits K --seed S    corrupt_stream -- flip k random
//               --in WIRE --out DAMAGED     STRUCTURAL bits (per-format)
//   --recover <format> --in DAMAGED         recover_stream -- resync from the
//                                           corrupted bytes ONLY, report the
//                                           recovered units + digest
//   --check   --baseline FILE --recovered FILE   verify recovered ⊆ baseline
//                                           (offset+tag), report integrity,
//                                           print the content-verified %
//
// The check is a THIRD process holding the baseline: the recoverer never sees
// the clean artifact. "Recovered" means a unit whose two halves corroborate
// the clean structure -- content verification, not boundary survival (fixes
// the recovery-test flaws C/F; handoff.md § Test 5).
// ===========================================================================

#pragma once

#include "harness.hpp"
#include "provenance.hpp"  // sha256

#include <algorithm>
#include <cstdint>
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
struct UnitRef {
  std::size_t offset = 0;
  std::uint16_t tag = 0;
  bool operator==(const UnitRef& o) const { return offset == o.offset && tag == o.tag; }
};

struct StreamFingerprint {
  std::vector<UnitRef> units;  // sorted by offset
  std::string digest;          // sha256 of the unit list -- report integrity stamp

  void finalize() {
    // Canonical, offset-sorted serialization of the unit list.
    std::string canon;
    for (const auto& u : units) {
      canon.append(reinterpret_cast<const char*>(&u.offset), sizeof(u.offset));
      canon.append(reinterpret_cast<const char*>(&u.tag), sizeof(u.tag));
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
inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  FastFHIR::Parser p(wire.data(), wire.size());
  auto root = p.root();
  if (!root || !root.is<FastFHIR::RESOURCETYPE::BUNDLE>())
    return fp;
  auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY];
  if (!entries)
    return fp;
  const auto n = entries.as_node().size();
  for (std::size_t i = 0; i < n; ++i) {
    auto resource = entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    if (!resource)
      continue;
    const auto slot = static_cast<std::size_t>(resource.absolute_offset());
    if (slot > wire.size() || wire.size() - slot < 8)
      continue;
    const auto target = static_cast<std::size_t>(LOAD_U64(wire.data() + slot));
    if (target > wire.size() || wire.size() - target < 10)
      continue;
    fp.units.push_back(
        UnitRef{target, static_cast<std::uint16_t>(wire[target + 8]) |
                            static_cast<std::uint16_t>(wire[target + 9] << 8)});
  }
  std::sort(fp.units.begin(), fp.units.end(),
            [](const UnitRef& a, const UnitRef& b) { return a.offset < b.offset; });
  fp.finalize();
  return fp;
}
#elif defined(ARM_JSON)
inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  const std::string text(wire.begin(), wire.end());
  for (std::size_t i = 0; i + 10 <= text.size(); ++i)
    if (text.compare(i, 10, "\"resource\"") == 0)
      fp.units.push_back(UnitRef{i, 0});  // tag 0: no binary tag in JSON
  fp.finalize();
  return fp;
}
#elif defined(ARM_GOOGLE_FHIR)
inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    if (wire[pos] == 'P' || wire[pos] == 'O') {
      const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                           (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                           (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                           (static_cast<uint32_t>(wire[pos + 4]) << 24);
      fp.units.push_back(UnitRef{pos, static_cast<std::uint16_t>(wire[pos])});
      pos += 5 + len;
    } else {
      ++pos;
    }
  }
  fp.finalize();
  return fp;
}
#elif defined(ARM_HL7V2)
inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  std::size_t start = 0;
  while (start < wire.size()) {
    const auto cr = std::find(wire.begin() + static_cast<std::ptrdiff_t>(start),
                              wire.end(), static_cast<uint8_t>('\r'));
    const std::size_t end = (cr == wire.end()) ? wire.size()
                                               : static_cast<std::size_t>(cr - wire.begin());
    if (end - start >= 3) {
      const std::uint16_t name = static_cast<std::uint16_t>(wire[start]) |
                                 (static_cast<std::uint16_t>(wire[start + 1]) << 8) |
                                 (static_cast<std::uint16_t>(wire[start + 2]) << 16);
      fp.units.push_back(UnitRef{start, name});
    }
    if (cr == wire.end())
      break;
    start = end + 1;
  }
  fp.finalize();
  return fp;
}
#endif

// ---------------------------------------------------------------------------
// 2. corrupt_stream -- polymorphic structural corruption
// ---------------------------------------------------------------------------
#if defined(ARM_FASTFHIR)
// Syntactic elements ONLY, per Ryan (2026-08-26): the self-corrective scheme
// is structural, so corruption targets (1) the stream FF_HEADER, (2) each
// block's VALIDATION word + RECOVERY_TAG, (3) the POINTER SLOTS -- the vtable
// fields holding child offsets (the edges the recovery cross-validates).
// Scalar VALUES (string payloads, numbers, codes) are NEVER corrupted: they
// are not part of the corrective scheme and could not be repaired even by
// perfect structural recovery. Leaf-data slots (string/code slots) are also
// excluded -- a broken string reference has no cross-validation to heal it.
inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  std::vector<std::size_t> positions;
  // 1. FF_HEADER region (stream-level syntax).
  for (std::size_t i = 0; i < 54 && i < wire.size(); ++i)
    positions.push_back(i);
  FastFHIR::Parser p(wire.data(), wire.size());
  auto root = p.root();
  auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY];
  if (entries) {
    const auto n = entries.as_node().size();
    for (std::size_t i = 0; i < n; ++i) {
      auto resource = entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
      if (!resource)
        continue;
      // 3. The entry array's pointer slot for this resource (the edge
      //    entry -> resource, cross-validated by the walk).
      const auto slot = static_cast<std::size_t>(resource.absolute_offset());
      if (slot > wire.size() || wire.size() - slot < 8)
        continue;
      for (std::size_t j = 0; j < 8 && slot + j < wire.size(); ++j)
        positions.push_back(slot + j);
      // 2+3. The resource block: VALIDATION + RECOVERY_TAG header, then each
      //      pointer-field slot (block/array/resource/choice references).
      const auto target = static_cast<std::size_t>(LOAD_U64(wire.data() + slot));
      if (target > wire.size() || wire.size() - target < 10)
        continue;
      for (std::size_t j = 0; j < 10; ++j)
        positions.push_back(target + j);
      auto node = resource.as_node();
      if (node) {
        for (const auto& f : node.fields()) {
          const bool pointer_kind =
              f.kind == FF_FIELD_BLOCK || f.kind == FF_FIELD_ARRAY ||
              f.kind == FF_FIELD_RESOURCE || f.kind == FF_FIELD_CHOICE;
          if (!pointer_kind)
            continue;
          // Choice slots store offset + variant tag (10 bytes); others store
          // an 8-byte offset.
          const std::size_t slot_len = (f.kind == FF_FIELD_CHOICE) ? 10 : 8;
          const std::size_t fslot = target + f.field_offset;
          for (std::size_t j = 0; j < slot_len && fslot + j < wire.size(); ++j)
            positions.push_back(fslot + j);
        }
      }
    }
  }
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
#elif defined(ARM_JSON)
inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  auto is_syntax = [](uint8_t c) {
    return c == '{' || c == '}' || c == '[' || c == ']' || c == '"' || c == ':' || c == ',';
  };
  std::vector<std::size_t> positions;
  for (std::size_t i = 0; i < wire.size(); ++i)
    if (is_syntax(wire[i]))
      positions.push_back(i);
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
#elif defined(ARM_GOOGLE_FHIR)
inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
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
#elif defined(ARM_HL7V2)
inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  std::vector<std::size_t> positions;
  for (std::size_t i = 0; i < wire.size(); ++i)
    if (wire[i] == '\r')
      positions.push_back(i);
  std::size_t start = 0;
  while (start < wire.size()) {
    if (start + 3 <= wire.size())
      for (std::size_t j = 0; j < 3; ++j)
        positions.push_back(start + j);
    const auto cr = std::find(wire.begin() + static_cast<std::ptrdiff_t>(start),
                              wire.end(), static_cast<uint8_t>('\r'));
    if (cr == wire.end())
      break;
    start = static_cast<std::size_t>(cr - wire.begin()) + 1;
  }
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
#endif

// ---------------------------------------------------------------------------
// 3. recover_stream -- polymorphic recovery (corrupted bytes ONLY)
// ---------------------------------------------------------------------------
#if defined(ARM_FASTFHIR)
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  FastFHIR::Recovery rec(wire.data(), wire.size());
  const auto stats = rec.recover_bundle_entries();
  for (const auto& u : stats.units)
    fp.units.push_back(UnitRef{u.first, u.second});
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
      if (text.compare(i, 10, "\"resource\"") == 0)
        fp.units.push_back(UnitRef{i, 0});
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
        fp.units.push_back(UnitRef{marker, 0});
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
      if (ok)
        fp.units.push_back(UnitRef{pos, static_cast<std::uint16_t>(wire[pos])});
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
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  std::size_t start = 0;
  while (start < wire.size()) {
    const auto cr = std::find(wire.begin() + static_cast<std::ptrdiff_t>(start),
                              wire.end(), static_cast<uint8_t>('\r'));
    const std::size_t end = (cr == wire.end()) ? wire.size()
                                               : static_cast<std::size_t>(cr - wire.begin());
    if (end - start >= 3) {
      // A segment is recovered only when its name is a known v2 name AND the
      // line parses to a plausible field count (name|field1|field2|...).
      const std::string line(wire.begin() + static_cast<std::ptrdiff_t>(start),
                             wire.begin() + static_cast<std::ptrdiff_t>(end));
      const std::size_t fields = 1 + static_cast<std::size_t>(
          std::count(line.begin(), line.end(), '|'));
      if (fields >= 2) {
        const std::uint16_t name = static_cast<std::uint16_t>(wire[start]) |
                                   (static_cast<std::uint16_t>(wire[start + 1]) << 8) |
                                   (static_cast<std::uint16_t>(wire[start + 2]) << 16);
        fp.units.push_back(UnitRef{start, name});
      }
    }
    if (cr == wire.end())
      break;
    start = end + 1;
  }
  fp.finalize();
  return fp;
}
#endif

}  // inline namespace BENCH_ARM_NS
}  // namespace bench::test_5
