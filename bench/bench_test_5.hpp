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

// Every arm's scanner renders leaf values as JSON, so this is a direct
// dependency of this header rather than one inherited from whichever arm
// happens to include it first.
#include <nlohmann/json.hpp>
#include <set>

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

  // The same leaves, UNHASHED. --check only ever needs the hashes, but a
  // DECODER needs the strings back: rebuilding a POCO means addressing a field
  // by name. Carrying both here means all four arms get it from the single
  // funnel they already pass through, instead of four parallel extractions
  // that can drift apart.
  std::vector<std::pair<std::string, std::string>> raw;

  void add_leaf(const std::string& path, const std::string& value);

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

// ── CANONICAL FHIR LEAF PATHS ───────────────────────────────────────────────
//
// Every arm emits the SAME key for the same datum:
//
//     <ResourceType>/<id>.<element path>     e.g. Patient/0fb515d6.address[0].city
//
// Keyed by RESOURCE ID, never by entry index. A positional key would require all
// four encodings to preserve Bundle ordering, and nothing guarantees that --
// protobuf is a record stream, v2 is a segment stream. Every encoding carries
// resourceType and id, so this key survives re-encoding.
//
// `resourceType` is NOT emitted as a leaf: it is already in the key, and the
// encodings disagree about whether it is a stored field at all (FastFHIR
// synthesizes it from the block tag). A field that only some arms can produce
// is a difference in the ENCODERS, not in the data, and counting it would
// charge that difference to the format.
//
// Identity is the path hash, content is the value hash -- which fits the
// existing UnitRef and fingerprint layout unchanged: `offset` carries the path
// hash, `parent`/`tag` go unused, and --check's linear merge already compares
// identity first and content second. That is precisely the four-outcome
// comparison, now over a population all four arms can agree on.
inline UnitRef canonical_leaf(const std::string& path, const std::string& value_raw);

// NUMBERS COMPARE AS NUMBERS.
//
// FastFHIR preserves a decimal's source SCALE (that is a feature -- 1.50 and 1.5
// are different in FHIR), while nlohmann emits shortest-round-trip. So the same
// datum rendered by two arms can differ in text while being the same value, and
// 576 of 1,496 float leaves did. Hashing the text would charge a RENDERING
// difference to the format.
//
// Only bare numbers are normalized: a JSON string arrives quoted, so a string
// field holding "6.07" is untouched and still compares as text. %.17g is
// round-trip exact for a double, so genuinely different values stay different.
// Serialising a leaf value from a CORRUPTED wire.
//
// nlohmann's dump() throws type_error.316 when a string holds invalid UTF-8,
// and a corruption sweep produces exactly that: at k=512 a flipped byte inside
// an HL7v2 ZFX payload aborted the whole run with
// "invalid UTF-8 byte at index 36: 0xFC". A recovery benchmark cannot abort on
// damaged input -- damaged input is the experiment.
//
// error_handler_t::replace substitutes U+FFFD for the bad bytes. The unit stays
// in the census and its content hash no longer matches the baseline, so it is
// scored `wrong` -- the datum changed. That is the accurate verdict; throwing
// reports nothing and dropping the leaf would score it `missing`, which
// understates the damage by calling altered data absent.
inline std::string safe_dump(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline std::string canonical_number(const std::string& value) {
    if (value.empty() || value.front() == '"') return value;
    const char* begin = value.c_str();
    char* end = nullptr;
    const double d = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return value;  // not a bare number
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}

inline void StreamFingerprint::add_leaf(const std::string& path, const std::string& value) {
    units.push_back(canonical_leaf(path, value));
    raw.emplace_back(path, value);
}

inline UnitRef canonical_leaf(const std::string& path, const std::string& value_raw) {
    const std::string value = canonical_number(value_raw);
    return UnitRef{
        0,
        static_cast<std::size_t>(
            content_hash(reinterpret_cast<const std::uint8_t*>(path.data()), path.size())),
        0,
        content_hash(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()),
    };
}

// The key a resource's leaves hang from. Empty when either half is missing --
// an unidentifiable resource cannot be compared across encodings, and guessing
// a key would manufacture agreement.
inline std::string resource_key(const std::string& type, const std::string& id) {
    if (type.empty() || id.empty()) return {};
    return type + "/" + id;
}

// ---------------------------------------------------------------------------
// 1. calc_stream_hash -- structural fingerprint of a CLEAN stream
// ---------------------------------------------------------------------------
#if defined(ARM_FASTFHIR)
// THE FORWARD MAPPING, USED BACKWARDS.
//
// A choice ([x]) element is written as base + variant type name --
// `value` + `Quantity` -> `valueQuantity` -- and the EXPORTER already knows how
// to build that (FF_Parser.cpp's choice_suffix, which defers complex variants to
// reflected_choice_suffix). Mirroring it here is what lets the lens name a leaf
// the same way the JSON document does; deriving the rule independently is how
// the two arms end up 10,753 paths apart while holding identical data.
//
// reflected_resource_type is deliberately NOT the lookup: it enumerates
// resources only and returns "" for Quantity, CodeableConcept, Period and every
// other datatype -- the exporter records that it printed a bare `value` for
// 1,416 fields that way.
inline std::string bench_choice_suffix(RECOVERY_TAG tag) {
    switch (tag) {
        case RECOVER_FF_BOOL:     return "Boolean";
        case RECOVER_FF_INT32:    return "Integer";
        case RECOVER_FF_FLOAT64:  return "Decimal";
        case RECOVER_FF_STRING:   return "String";
        case RECOVER_FF_CODE:     return "Code";
        case RECOVER_FF_DATE:     return "Date";
        case RECOVER_FF_DATETIME: return "DateTime";
        case RECOVER_FF_TIME:     return "Time";
        case RECOVER_FF_INSTANT:  return "Instant";
        default:                  return std::string(FastFHIR::reflected_choice_suffix(tag));
    }
}

// The lens walk, emitting the SAME canonical paths the JSON arm does.
//
// This replaces a block-REFERENCE fingerprint (Recovery::reachable_blocks): the
// two counted different populations -- 54,504 edges against JSON's 1,473
// resources -- so a percentage over either meant nothing next to the other.
// Both now count FHIR leaves, which is the thing the formats are actually being
// asked to preserve.
//
// PER LEAF, not per document: print_json() over the whole tree would make one
// broken subtree unparseable and score every surviving value as lost, which
// flatters the failure. Walking leaf by leaf keeps the damage local, which is
// the honest accounting.
inline void ffhr_walk_leaves(const Reflective::Node& node, const std::string& path,
                             StreamFingerprint& fp, uint32_t version, int depth) {
    if (!node || depth > 64) return;

    if (node.is_array()) {
        std::size_t n = 0;
        try { n = node.size(); } catch (const std::exception&) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            try {
                ffhr_walk_leaves(node[i], path + "[" + std::to_string(i) + "]", fp, version,
                                 depth + 1);
            } catch (const std::exception&) {
                // this element is unreadable; the rest of the array is not
            }
        }
        return;
    }

    if (!node.is_object()) {
        std::ostringstream v;
        node.print_json(v);
        fp.add_leaf(path, v.str());
        return;
    }

    std::span<const FF_FieldInfo> fields;
    try { fields = node.fields(); } catch (const std::exception&) { return; }
    for (const auto& f : fields) {
        const FF_FieldKey key = FF_FieldKey::from_cstr(node.recovery(), f.kind, f.field_offset,
                                                       f.child_recovery,
                                                       f.array_entries_are_offsets, f.name);
        const Reflective::Entry e = node[key];
        if (!e) continue;
        std::string child = path + "." + std::string(f.name);
        if (f.kind == FF_FIELD_CHOICE) {
            try { child += bench_choice_suffix(e.as_node().recovery()); }
            catch (const std::exception&) {}
        }
        if (ff_kind_is_inline_scalar(f.kind)) {
            std::ostringstream v;
            try { e.print_scalar_json(v, version); } catch (const std::exception&) { continue; }
            fp.add_leaf(child, v.str());
            continue;
        }
        try { ffhr_walk_leaves(e.as_node(), child, fp, version, depth + 1); }
        catch (const std::exception&) {}
    }
}

inline void ffhr_collect(const Reflective::Node& root, StreamFingerprint& fp,
                         uint32_t version) {
    if (!root) return;
    std::vector<Reflective::Node> entries;
    try { entries = root[FastFHIR::Fields::BUNDLE::ENTRY].as_node().entries(); }
    catch (const std::exception&) { return; }

    for (const auto& entry : entries) {
        Reflective::Node resource;
        try { resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE].as_node(); }
        catch (const std::exception&) { continue; }
        if (!resource) continue;

        // The type comes from the block's own tag (FastFHIR does not store
        // resourceType as a field), the id from the wire.
        const std::string type(reflected_resource_type(resource.recovery()));
        std::span<const FF_FieldInfo> fields;
        try { fields = resource.fields(); } catch (const std::exception&) { continue; }

        // `id` is found through the reflection table, not a type-specific key:
        // Fields::PATIENT::ID names Patient's slot, so using it for every
        // resource left every Observation keyless and silently skipped -- 437
        // leaves against the JSON arm's 34,831.
        std::string id;
        for (const auto& f : fields) {
            if (std::string_view(f.name) != "id") continue;
            const FF_FieldKey idk = FF_FieldKey::from_cstr(resource.recovery(), f.kind,
                                                           f.field_offset, f.child_recovery,
                                                           f.array_entries_are_offsets, f.name);
            try {
                const Reflective::Entry e = resource[idk];
                if (e) id = std::string(e.as<std::string_view>());
            } catch (const std::exception&) {}
            break;
        }
        const std::string key = resource_key(type, id);
        if (key.empty()) continue;
        for (const auto& f : fields) {
            const FF_FieldKey fk = FF_FieldKey::from_cstr(resource.recovery(), f.kind,
                                                          f.field_offset, f.child_recovery,
                                                          f.array_entries_are_offsets, f.name);
            const Reflective::Entry e = resource[fk];
            if (!e) continue;
            std::string child = key + "." + std::string(f.name);
            if (f.kind == FF_FIELD_CHOICE) {
                try { child += bench_choice_suffix(e.as_node().recovery()); }
                catch (const std::exception&) {}
            }
            if (ff_kind_is_inline_scalar(f.kind)) {
                std::ostringstream v;
                try { e.print_scalar_json(v, version); } catch (const std::exception&) { continue; }
                fp.add_leaf(child, v.str());
                continue;
            }
            try { ffhr_walk_leaves(e.as_node(), child, fp, version, 1); }
            catch (const std::exception&) {}
        }
    }
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    FastFHIR::Memory mem = wrap_wire_bytes(wire);
    FastFHIR::Parser parser(mem);
    ffhr_collect(parser.root(), fp, FHIR_VERSION_R5);
    fp.finalize();
    return fp;
}
#elif defined(ARM_JSON)
// The byte span of the RESOURCE OBJECT a `"resource"` marker introduces: the
// '{' after the marker's colon, brace-matched forward. Used by the resync path
// when the document as a whole will not parse.
//
// The span must be the resource object itself, not the enclosing entry object.
// Scanning BACKWARD from the marker to a brace lands on the entry's '{', and a
// parsed entry root has no resourceType -- so the salvage parsed fine and then
// skipped every resource as unidentifiable, scoring 0 for any damage at all
// (measured at every k >= 1 across seeds). Matching forward from the value
// brace keeps resourceType at the parsed root, exactly where json_collect
// reads it. Strings and escapes are honoured: braces inside string values must
// not count, and nested objects (contained resources, extensions) must.
inline std::pair<std::size_t, std::size_t> json_resource_extent(const std::string& text,
                                                                std::size_t marker) {
    const auto colon = text.find(':', marker);
    std::size_t open = (colon == std::string::npos) ? marker : colon + 1;
    while (open < text.size() && text[open] != '{')
        ++open;
    if (open >= text.size())
        return {marker, marker};  // no resource object to salvage
    int depth = 0;
    bool in_str = false, esc = false;
    std::size_t close = open;
    for (; close < text.size(); ++close) {
        const char c = text[close];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) { ++close; break; }
    }
    if (depth != 0)
        return {marker, marker};  // unbalanced: nothing salvageable here
    return {open, close};
}

// The JSON arm's document IS FHIR, so it defines the canonical scheme the other
// arms are measured against: walk to every scalar and name it by its element
// path under <ResourceType>/<id>.
inline void json_walk_leaves(const nlohmann::json& node, const std::string& path,
                             StreamFingerprint& fp) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            json_walk_leaves(it.value(), path + "." + it.key(), fp);
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i)
            json_walk_leaves(node[i], path + "[" + std::to_string(i) + "]", fp);
    } else {
        // dump() rather than a type-specific formatter: every arm has to render
        // the value the SAME way or equal data compares unequal. JSON's own
        // canonical form is the one all four can reach.
        fp.add_leaf(path, safe_dump(node));
    }
}

inline void json_collect(const nlohmann::json& doc, StreamFingerprint& fp) {
    const auto entries = doc.find("entry");
    if (entries == doc.end() || !entries->is_array()) return;
    for (const auto& entry : *entries) {
        const auto res = entry.find("resource");
        if (res == entry.end() || !res->is_object()) continue;
        const auto type = res->value("resourceType", std::string{});
        const auto id = res->value("id", std::string{});
        const auto key = resource_key(type, id);
        if (key.empty()) continue;  // unidentifiable -- not comparable
        // Walk the resource's members with the key already in the path: the
        // prefix is part of what gets hashed, not something bolted on after.
        for (auto it = res->begin(); it != res->end(); ++it) {
            if (it.key() == "resourceType") continue;  // it IS the key
            json_walk_leaves(it.value(), key + "." + it.key(), fp);
        }
    }
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    try {
        const auto doc = nlohmann::json::parse(std::string(wire.begin(), wire.end()));
        json_collect(doc, fp);
    } catch (const std::exception&) {
        // Unparseable: no units. The check reports the loss rather than this
        // throwing and taking the whole measurement with it.
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

// JSON-RENDERED, like every other arm. The canonical value is the JSON form
// (a string arrives quoted and escaped), and returning a bare string here made
// 16,213 matched paths compare unequal on every single one -- the arms agreed
// on WHERE the datum was and disagreed only on how to spell it.
// proto generation lowercases FHIR's camelCase into snake_case
// (multipleBirth -> multiple_birth), so the inverse is the mapping that lets
// this arm name an element the way the document does. Without it every
// multi-word field lands as a path JSON does not have -- counted spurious on
// one side and missing on the other, from data that is present and correct.
inline std::string pb_camel(const std::string& snake) {
    std::string out;
    out.reserve(snake.size());
    bool up = false;
    for (const char c : snake) {
        if (c == '_') { up = true; continue; }
        out.push_back(up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        up = false;
    }
    return out;
}

inline std::string pb_json_value(const std::string& raw, bool is_string) {
    return is_string ? safe_dump(nlohmann::json(raw)) : raw;
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
    // "true"/"false", not "1"/"0" -- the same spelling problem as the enum
    // case below. JSON has a boolean literal and every other arm emits it, so
    // "0" arrived as the NUMBER 0 and the POCO setter refused it: five
    // multipleBirthBoolean leaves, present and correct on the wire, counted as
    // missing.
    case FD::CPPTYPE_BOOL:   return (rep ? refl->GetRepeatedBool(m, f, index) : refl->GetBool(m, f)) ? "true" : "false";
    case FD::CPPTYPE_ENUM: {
      // proto generation upper-snakes the FHIR code (final -> FINAL,
      // not-done -> NOT_DONE), so the inverse is lowercase with underscores
      // back to hyphens. Emitting the proto spelling made this arm report
      // "FINAL" where every other arm reports "final" -- 4,414 leaves that are
      // the SAME datum, counted as a difference between the formats.
      const auto* e = rep ? refl->GetRepeatedEnum(m, f, index) : refl->GetEnum(m, f);
      if (e == nullptr) return std::string();
      std::string code = e->name();
      for (char& c : code) {
        c = (c == '_') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return code;
    }
    case FD::CPPTYPE_STRING: {
      std::string scratch;
      return rep ? refl->GetRepeatedStringReference(m, f, index, &scratch)
                 : refl->GetStringReference(m, f, &scratch);
    }
    default: return std::string();
  }
}

// google-fhir WRAPS PRIMITIVES: `Patient.id` is a message `Id { value }`, and
// the encoder writes it as mutable_id()->set_value(...). So a proto path is one
// level deeper than the FHIR element path everywhere. Collapsing a wrapper --
// a message whose populated content is a single `value` -- is what makes this
// arm name a leaf the way the JSON document does.
// Recognised from the DESCRIPTOR, not from which fields happen to be set.
//
// ListFields omits any proto3 scalar sitting at its default, so a
// Boolean{value:false} lists NOTHING and the old shape test (`exactly one field
// set, named value`) rejected it -- the message was then walked as an ordinary
// submessage, found empty, and emitted no leaf at all. Every `false`, every 0,
// every "" inside a FhirProto primitive wrapper disappeared; in this corpus
// that was multipleBirthBoolean for all five patients.
//
// It is the same mistake as reading FastFHIR's enum ordinal 0 as absence.
// Presence is carried by the WRAPPER MESSAGE existing at all -- the parent's
// ListFields is what decides that -- and the scalar inside is then just a
// value, default or not.
inline bool pb_is_primitive_wrapper(const google::protobuf::Message& m,
                                    const google::protobuf::FieldDescriptor** out) {
    const auto* f = m.GetDescriptor()->FindFieldByName("value");
    if (f == nullptr || f->is_repeated()) return false;
    // A scalar `value` is what makes it a wrapper: Quantity and ContactPoint
    // also have a `value`, but theirs is a message and they are real datatypes.
    if (f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) return false;
    *out = f;
    return true;
}

// FhirProto's JSON conventions, as its own printer implements them. There is
// no one-to-one mapping between FhirProto fields and FHIR JSON fields, so
// google/fhir ships custom parsers/printers rather than using protobuf's
// generic JSON support; a reflection walk that ignores those conventions names
// elements the FHIR document does not have. Two rules matter here, and both
// are readable off the descriptors, so neither can drift from the schema.

// Which member of a single-oneof message is set, if any.
inline const google::protobuf::FieldDescriptor* pb_oneof_set(
    const google::protobuf::Message& m) {
    const auto* d = m.GetDescriptor();
    if (d->real_oneof_decl_count() != 1) return nullptr;
    return m.GetReflection()->GetOneofFieldDescriptor(m, d->real_oneof_decl(0));
}

inline std::string pb_ucfirst(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

inline std::string pb_upper_camel(const std::string& snake) {
    return pb_ucfirst(pb_camel(snake));
}

// RULE 3 -- a timelike primitive. FhirProto stores dateTime/instant/date/time
// as absolute microseconds plus the timezone the event was recorded in and a
// precision, because FHIR JSON writes some timelike primitives without any
// zone at all. Reflection therefore sees `issued.valueUs`, `issued.timezone`
// and `issued.precision` where the document says
// `"issued": "2018-06-14T06:06:32.842+00:00"`. Precision is what says how much
// of the value was actually written -- rendering every one to seconds would
// invent a different string, and a different instant, for anything sub-second.
inline bool pb_timelike_text(const google::protobuf::Message& m, std::string* out) {
    const auto* d = m.GetDescriptor();
    const auto* f_us = d->FindFieldByName("value_us");
    const auto* f_tz = d->FindFieldByName("timezone");
    const auto* f_pr = d->FindFieldByName("precision");
    if (f_us == nullptr || f_tz == nullptr || f_pr == nullptr) return false;

    const auto* refl = m.GetReflection();
    const std::int64_t us = refl->GetInt64(m, f_us);
    std::string tz = refl->GetString(m, f_tz);
    const std::string precision = refl->GetEnum(m, f_pr)->name();

    // The zone is an OFFSET in FHIR JSON; "UTC" is FhirProto's spelling of +00:00.
    std::int64_t offset_s = 0;
    if (tz == "UTC" || tz.empty()) tz = "+00:00";
    if (tz != "Z" && tz.size() >= 6 && (tz[0] == '+' || tz[0] == '-')) {
        const int sign = tz[0] == '-' ? -1 : 1;
        offset_s = sign * (std::atoi(tz.substr(1, 2).c_str()) * 3600 +
                           std::atoi(tz.substr(4, 2).c_str()) * 60);
    }

    // Floor-divide: a negative microsecond count must not round toward zero.
    std::int64_t local = us + offset_s * 1000000LL;
    std::int64_t secs = local / 1000000LL;
    std::int64_t frac = local % 1000000LL;
    if (frac < 0) { frac += 1000000LL; --secs; }

    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    if (precision == "DAY" || precision == "YEAR" || precision == "MONTH") {
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        *out = buf;
        return true;
    }
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    *out = buf;
    if (precision == "MILLISECOND") {
        std::snprintf(buf, sizeof buf, ".%03d", static_cast<int>(frac / 1000));
        *out += buf;
    } else if (precision == "MICROSECOND") {
        std::snprintf(buf, sizeof buf, ".%06d", static_cast<int>(frac));
        *out += buf;
    }
    *out += tz;
    return true;
}

// RULE 4 -- Decimal is a JSON NUMBER. FhirProto stores it as a `string` field
// precisely so the original lexical form survives (FHIR treats trailing zeros
// as significant), but FHIR JSON writes it unquoted. Emitting it quoted, the
// way every other string-typed wrapper is emitted, made 1,263 Quantity values
// arrive as "93" where the document has 93 -- refused by the POCO setter, and
// counted as a missing value rather than a mis-spelled one.
inline bool pb_is_unquoted_primitive(const google::protobuf::Message& m) {
    const std::string& n = m.GetDescriptor()->name();
    return n == "Decimal";
}

// RULE 1 -- a choice element. `Observation.value[x]` is a nested message ValueX
// wrapping one oneof, so reflection sees `value.quantity.value`. FHIR JSON has
// no wrapper: the element is spelled `valueQuantity`. json_name carries the
// FHIR datatype where the proto field name differs -- `string_value` is
// declared [json_name = "string"], giving `valueString`, not `valueStringValue`.
inline bool pb_is_choice_wrapper(const google::protobuf::Message& m) {
    const auto* d = m.GetDescriptor();
    const std::string& n = d->name();
    return d->real_oneof_decl_count() == 1 && n.size() > 1 && n.back() == 'X' &&
           n != "Reference";
}

// RULE 2 -- a typed reference. FhirProto splits FHIR's single `reference`
// string into a oneof of per-resource ReferenceId fields, so `subject` arrives
// as `subject.patientId.value = "abc"` where the document says
// `subject.reference = "Patient/abc"`. The resource type is the field name
// minus its `_id` suffix, which is exactly what the field's
// (referenced_fhir_type) annotation states -- read here off the name so this
// needs no annotation-extension linkage.
inline bool pb_reference_text(const google::protobuf::Message& ref,
                              std::string* out) {
    if (ref.GetDescriptor()->name() != "Reference") return false;
    const auto* f = pb_oneof_set(ref);
    if (f == nullptr) return false;
    const auto* inner = f->message_type() != nullptr ? ref.GetReflection()
                            ->GetMessage(ref, f).GetDescriptor()->FindFieldByName("value")
                                                    : nullptr;
    if (inner == nullptr) return false;
    const auto& sub = ref.GetReflection()->GetMessage(ref, f);
    const std::string id = sub.GetReflection()->GetString(sub, inner);
    const std::string& name = f->name();
    if (name == "uri" || name == "fragment") { *out = id; return true; }
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "_id") == 0) {
        *out = pb_upper_camel(name.substr(0, name.size() - 3)) + "/" + id;
        return true;
    }
    return false;
}

inline void pb_walk(const google::protobuf::Message& msg, const std::string& path,
                    StreamFingerprint& fp) {
    const auto* refl = msg.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    refl->ListFields(msg, &fields);
    const bool in_reference = msg.GetDescriptor()->name() == "Reference";
    for (const auto* f : fields) {
        // Already emitted as `.reference` by RULE 2; walking it again would
        // add `subject.patientId.value`, a path no FHIR document has.
        if (in_reference && f->containing_oneof() != nullptr) continue;
        const std::string base = path + "." + pb_camel(f->name());
        const bool is_msg =
            f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE;
        const int count = f->is_repeated() ? refl->FieldSize(msg, f) : 1;
        for (int i = 0; i < count; ++i) {
            const std::string here =
                f->is_repeated() ? base + "[" + std::to_string(i) + "]" : base;
            if (is_msg) {
                const auto& sub = f->is_repeated() ? refl->GetRepeatedMessage(msg, f, i)
                                                   : refl->GetMessage(msg, f);
                // RULE 3: a timelike primitive renders as its FHIR string.
                std::string when;
                if (pb_timelike_text(sub, &when)) {
                    fp.add_leaf(here, safe_dump(nlohmann::json(when)));
                    continue;
                }

                const google::protobuf::FieldDescriptor* inner = nullptr;
                if (pb_is_primitive_wrapper(sub, &inner)) {
                    // RULE 4: Decimal is stored as a string but written as a number.
                    const bool istr =
                        !pb_is_unquoted_primitive(sub) &&
                        (inner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                         inner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM);
                    fp.add_leaf(
                        here, pb_json_value(pb_scalar_string(sub, sub.GetReflection(), inner, 0),
                                            istr));
                    continue;
                }

                // RULE 2: a Reference collapses to one `reference` string.
                std::string ref_text;
                if (pb_reference_text(sub, &ref_text)) {
                    fp.add_leaf(here + ".reference", safe_dump(nlohmann::json(ref_text)));
                    // `display`, `type` and `identifier` are ordinary siblings
                    // and still belong in the output; only the oneof folded.
                    pb_walk(sub, here, fp);
                    continue;
                }

                // RULE 1: a choice wrapper folds into the element's FHIR name.
                if (pb_is_choice_wrapper(sub)) {
                    if (const auto* ch = pb_oneof_set(sub)) {
                        const std::string folded = here + pb_ucfirst(ch->json_name());
                        const auto& picked = sub.GetReflection()->GetMessage(sub, ch);
                        const google::protobuf::FieldDescriptor* pinner = nullptr;
                        std::string picked_when;
                        if (pb_timelike_text(picked, &picked_when)) {
                            fp.add_leaf(folded, safe_dump(nlohmann::json(picked_when)));
                        } else if (pb_is_primitive_wrapper(picked, &pinner)) {
                            const bool istr =
                                !pb_is_unquoted_primitive(picked) &&
                                (pinner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                                 pinner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM);
                            fp.add_leaf(folded,
                                        pb_json_value(pb_scalar_string(picked, picked.GetReflection(),
                                                                       pinner, 0), istr));
                        } else {
                            pb_walk(picked, folded, fp);
                        }
                    }
                    continue;
                }

                pb_walk(sub, here, fp);
                continue;
            }
            const bool str = f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                             f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM;
            fp.units.push_back(
                canonical_leaf(here, pb_json_value(pb_scalar_string(msg, refl, f, i), str)));
        }
    }
}

// The resource's own key: type from the record marker, id from the wrapped
// `id.value`. A record with neither is not comparable and contributes nothing.
inline std::string pb_resource_key(const google::protobuf::Message& msg, char type) {
    const auto* refl = msg.GetReflection();
    const auto* desc = msg.GetDescriptor();
    std::string id;
    if (const auto* f = desc->FindFieldByName("id")) {
        if (f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
            refl->HasField(msg, f)) {
            const auto& sub = refl->GetMessage(msg, f);
            const google::protobuf::FieldDescriptor* inner = nullptr;
            if (pb_is_primitive_wrapper(sub, &inner))
                id = pb_scalar_string(sub, sub.GetReflection(), inner, 0);
        }
    }
    return resource_key(type == 'P' ? "Patient" : "Observation", id);
}

// Parse one length-prefixed record and emit its leaf fields. Returns false if
// the record does not parse at all -- then it contributes nothing and every
// field it held is reported missing, which is the honest accounting.
inline bool pb_emit_record(const std::vector<uint8_t>& wire, std::size_t pos,
                           uint32_t len, char type, StreamFingerprint& fp) {
    const auto emit = [&](const google::protobuf::Message& m) {
        const std::string key = pb_resource_key(m, type);
        if (key.empty()) return;
        pb_walk(m, key, fp);
    };
    if (type == 'P') {
        google::fhir::r4::core::Patient m;
        if (!m.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len))) return false;
        emit(m);
        return true;
    }
    google::fhir::r4::core::Observation m;
    if (!m.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len))) return false;
    emit(m);
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
// ── THE INVERSE OF THE v2 ENCODER ───────────────────────────────────────────
//
// Derived from the forward mapping (hl7v2_message.hpp + the ARM_HL7V2 assign
// path), not from reading the bytes and guessing. Each rule below names the
// encoder line it inverts:
//
//   MSH ... |<message_control_id>|P|2.5      MshSegment::serialize -- field 9
//                                            carries the PATIENT id.
//   PID|1||<id>||<name>||<dob>|<sex>|||...   PidSegment::serialize -- fixed
//                                            positions; birthDate and gender
//                                            reach the wire ONLY here.
//   OBX|<n>|<type>|<code>||<value>|<units>   ObxSegment::serialize -- the
//                                            observation VALUE is only here.
//   ZFX|<fhir.path>|<json>                   CustomFieldSegment::serialize --
//                                            the path is literal, the payload
//                                            is JSON. `.details` means "the
//                                            whole object at this path"
//                                            (hl7_append_json_field(...,
//                                            "patient.name[0].details", ...)),
//                                            so the inverse drops the marker
//                                            and walks the payload beneath it.
//
// Grouping is by ORDER, and the encoder makes that safe: assign_observation
// writes `observation.id` FIRST, so every observation.* segment after one
// belongs to it until the next.
inline std::string hl7_unescape(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 2 < src.size() && src[i + 2] == '\\') {
            switch (src[i + 1]) {
                case 'F': out += '|'; i += 2; continue;
                case 'S': out += '^'; i += 2; continue;
                case 'T': out += '&'; i += 2; continue;
                case 'R': out += '~'; i += 2; continue;
                case 'E': out += '\\'; i += 2; continue;
                default: break;
            }
        }
        out += src[i];
    }
    return out;
}

inline std::vector<std::string> hl7_split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}

// Walk a ZFX payload, which is a JSON fragment rooted at `path`.
inline void hl7_walk_json(const nlohmann::json& node, const std::string& path,
                          StreamFingerprint& fp) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            hl7_walk_json(it.value(), path + "." + it.key(), fp);
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i)
            hl7_walk_json(node[i], path + "[" + std::to_string(i) + "]", fp);
    } else {
        fp.add_leaf(path, safe_dump(node));
    }
}

inline StreamFingerprint scan_v2_canonical(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    const std::string text(wire.begin(), wire.end());
    std::string patient_key, obs_key;

    // OBX IS DECODED, which requires deferring it.
    //
    // A message is MSH, PID, every OBX, then the Z-segments -- Z last is the
    // convention -- so an OBX is read before the ZFX that names the observation
    // it belongs to. The OBX rows are therefore buffered and resolved at the end
    // of the message, matching OBX-1 (set id, 1-based) to the Nth observation.
    //
    // Only observations with no ZFX value[x] are decoded from OBX: the encoder
    // emits one carrier per datum, OBX for a Quantity and ZFX for the datatypes
    // OBX cannot hold. Decoding both would double-count, and would let a blasted
    // OBX resurrect from the passthrough.
    struct PendingObx { long set_id; std::string v5, v6; };
    std::vector<PendingObx> pending_obx;
    std::vector<std::string> msg_obs;              // observation keys, in order
    std::set<std::string> obs_with_zfx_value;      // had a ZFX value[x]

    const auto flush_obx = [&]() {
        for (const auto& o : pending_obx) {
            if (o.set_id < 1 || static_cast<std::size_t>(o.set_id) > msg_obs.size()) continue;
            const std::string& key = msg_obs[static_cast<std::size_t>(o.set_id) - 1];
            if (key.empty() || obs_with_zfx_value.count(key)) continue;
            if (o.v5.empty()) continue;
            // OBX-5 is the number; OBX-6 is a CWE of units, code^unit^system.
            try {
                fp.add_leaf(key + ".valueQuantity.value",
                            nlohmann::json(std::stod(o.v5)).dump());
            } catch (const std::exception&) { continue; }
            const auto comp = hl7_split(o.v6, '^');
            if (comp.size() > 0 && !comp[0].empty())
                fp.add_leaf(key + ".valueQuantity.code", safe_dump(nlohmann::json(comp[0])));
            if (comp.size() > 1 && !comp[1].empty())
                fp.add_leaf(key + ".valueQuantity.unit", safe_dump(nlohmann::json(comp[1])));
            if (comp.size() > 2 && !comp[2].empty())
                fp.add_leaf(key + ".valueQuantity.system", safe_dump(nlohmann::json(comp[2])));
        }
        pending_obx.clear();
        msg_obs.clear();
        obs_with_zfx_value.clear();
    };

    for (const auto& seg : hl7_split(text, '\r')) {
        if (seg.size() < 4) continue;
        const auto f = hl7_split(seg, '|');
        const std::string& name = f[0];

        if (name == "MSH" && f.size() > 9) {
            flush_obx();                      // resolve the previous message
            patient_key = resource_key("Patient", f[9]);
            // MSH-10 carries the patient id, and it is the ONLY place this arm
            // writes it. Using it to key the resource but never emitting it as
            // a leaf lost `Patient.id` for every patient in the corpus.
            if (!f[9].empty())
                fp.add_leaf(patient_key + ".id", safe_dump(nlohmann::json(f[9])));
        } else if (name == "PID" && f.size() > 8) {
            // birthDate and gender exist ONLY here; name/address/telecom are
            // carried structurally by ZFX `.details` and would double-count.
            if (!patient_key.empty()) {
                // PID-7 is a v2 DT: YYYYMMDD, no separators. Emitting it raw
                // compared "19510216" against the document's "1951-02-16" --
                // the same date, spelled the way v2 spells it. Converting on
                // the way back is exactly what a v2 reader does.
                if (!f[7].empty()) {
                    std::string d = f[7];
                    if (d.size() >= 8 && d.find('-') == std::string::npos)
                        d = d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
                    fp.add_leaf(patient_key + ".birthDate", safe_dump(nlohmann::json(d)));
                }
                // PID-8 is a v2 sex code (M/F/O/U); FHIR's ValueSet spells them
                // out. sex_code() wrote the v2 form during Test 1, so the
                // inverse belongs here -- the raw letter matches no FHIR code
                // and was refused by the POCO setter for every patient.
                if (!f[8].empty()) {
                    const std::string &sx = f[8];
                    const char *g = sx == "M" ? "male"
                                  : sx == "F" ? "female"
                                  : sx == "O" ? "other"
                                              : "unknown";
                    fp.add_leaf(patient_key + ".gender", safe_dump(nlohmann::json(g)));
                }
            }
        } else if (name == "OBX") {
            if (f.size() > 6) {
                long sid = 0;
                try { sid = std::stol(f[1]); } catch (const std::exception&) { sid = 0; }
                pending_obx.push_back({sid, hl7_unescape(f[5]), hl7_unescape(f[6])});
            }
            (void)0;
            // Buffered above, resolved by flush_obx() at the end of the
            // message. An earlier version emitted `<Observation>.value`, which
            // is not a FHIR path and so matched nothing in POCO 1; the fix then
            // was to emit no leaf at all, which left OBX unmeasured and made
            // its 14,516 delimiter bytes free armor -- a flipped '|' inside an
            // OBX left the digest bit-identical. The canonical path is
            // `.valueQuantity.value`, and that is what flush_obx emits.
        } else if (name == "ZFX" && f.size() > 2) {
            const std::string field = hl7_unescape(f[1]);
            const std::string payload = hl7_unescape(f[2]);

            std::string key;
            std::string sub;
            if (field.rfind("patient.", 0) == 0) {
                key = patient_key;
                sub = field.substr(8);
            } else if (field.rfind("observation.", 0) == 0) {
                key = obs_key;
                sub = field.substr(12);
            } else {
                continue;
            }

            nlohmann::json payload_json;
            try { payload_json = nlohmann::json::parse(payload); }
            catch (const std::exception&) { continue; }

            if (sub == "id") {
                // Starts a new scope AND is a leaf in its own right.
                if (field.rfind("observation.", 0) == 0 && payload_json.is_string()) {
                    obs_key = resource_key("Observation", payload_json.get<std::string>());
                    msg_obs.push_back(obs_key);   // OBX-1 indexes into this
                }
                key = (field.rfind("observation.", 0) == 0) ? obs_key : patient_key;
                if (!key.empty())
                    fp.add_leaf(key + ".id", safe_dump(payload_json));
                continue;
            }
            if (key.empty()) continue;

            // An observation whose value came through ZFX must not ALSO be
            // decoded from its OBX -- one carrier per datum.
            if (sub == "value[x]") obs_with_zfx_value.insert(key);

            // `.details` / `[*]` are container markers, not path elements.
            if (sub.size() > 8 && sub.compare(sub.size() - 8, 8, ".details") == 0)
                sub.erase(sub.size() - 8);
            if (sub.size() > 3 && sub.compare(sub.size() - 3, 3, "[*]") == 0)
                sub.erase(sub.size() - 3);

            // A CHOICE ELEMENT. The ZFX name is the `[x]` BASE --
            // `observation.effective[x]` -- and the concrete element name only
            // exists in the payload, which the encoder writes as a single-key
            // object: {"effectiveDateTime": ...}. Walking the base would emit
            // `Observation/<id>.effective[x].effectiveDateTime`, which matches
            // no canonical leaf, so every choice field on this arm was refused.
            if (sub.size() > 3 && sub.compare(sub.size() - 3, 3, "[x]") == 0) {
                if (payload_json.is_object() && payload_json.size() == 1) {
                    hl7_walk_json(payload_json.begin().value(),
                                  key + "." + payload_json.begin().key(), fp);
                    continue;
                }
                sub.erase(sub.size() - 3);
            }
            hl7_walk_json(payload_json, key + "." + sub, fp);
        }
    }
    flush_obx();          // the last message has no following MSH
    fp.finalize();
    return fp;
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    return scan_v2_canonical(wire);
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
// Protobuf's OWN structure, not just the container this benchmark wrapped
// around it.
//
// The previous model enumerated the 5-byte record framing and nothing else:
// 1,473 records x 5 = 7,365 positions, against 423,846 for FastFHIR. Protobuf's
// actual wire structure -- the field keys, the length prefixes of every
// embedded message -- was never corrupted, so the arm's recovery number
// described the durability of a header this benchmark invented rather than
// anything about protobuf.
//
// A protobuf field is a varint KEY (field_number << 3 | wire_type) followed by
// a payload whose shape the wire type decides. Structural bytes are the key
// varints and, for wire type 2, the length varints: damage there changes what
// the following bytes ARE. Payload bytes of a string or a varint value are
// content and stay untouched, matching every other arm.
//
// Embedded messages are found by descent, not by schema: a length-delimited
// payload that scans cleanly as a message is treated as one. That is the same
// heuristic protoc --decode_raw uses, and it is the only one available without
// linking the descriptors into the position enumerator.
inline bool pb_read_varint(const std::vector<uint8_t>& w, std::size_t& pos,
                           std::size_t end, std::uint64_t& out) {
  out = 0;
  int shift = 0;
  while (pos < end && shift <= 63) {
    const uint8_t b = w[pos++];
    out |= static_cast<std::uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

// Returns false if the range does not scan as a well-formed message. When
// `positions` is null the scan only validates, which is how a length-delimited
// payload is tested before recursing into it.
inline bool pb_scan_message(const std::vector<uint8_t>& w, std::size_t pos, std::size_t end,
                            std::vector<std::size_t>* positions, int depth) {
  if (depth > 12) return false;
  while (pos < end) {
    const std::size_t key_start = pos;
    std::uint64_t key = 0;
    if (!pb_read_varint(w, pos, end, key)) return false;
    const std::size_t key_end = pos;
    const unsigned wire_type = static_cast<unsigned>(key & 7);
    if ((key >> 3) == 0) return false;  // field number 0 is not legal

    std::size_t len_start = 0, len_end = 0, payload_end = 0;
    switch (wire_type) {
      case 0: {  // varint value -- content
        std::uint64_t v = 0;
        if (!pb_read_varint(w, pos, end, v)) return false;
        break;
      }
      case 1:                                   // 64-bit
        if (pos + 8 > end) return false;
        pos += 8;
        break;
      case 5:                                   // 32-bit
        if (pos + 4 > end) return false;
        pos += 4;
        break;
      case 2: {                                 // length-delimited
        len_start = pos;
        std::uint64_t len = 0;
        if (!pb_read_varint(w, pos, end, len)) return false;
        len_end = pos;
        if (len > static_cast<std::uint64_t>(end - pos)) return false;
        payload_end = pos + static_cast<std::size_t>(len);
        // Descend only if it reads as a message; a string would otherwise have
        // its bytes counted as structure.
        if (len > 0 && pb_scan_message(w, pos, payload_end, nullptr, depth + 1) &&
            positions != nullptr)
          pb_scan_message(w, pos, payload_end, positions, depth + 1);
        pos = payload_end;
        break;
      }
      default:
        return false;                           // 3/4 are deprecated groups
    }

    if (positions != nullptr) {
      for (std::size_t i = key_start; i < key_end; ++i) positions->push_back(i);
      for (std::size_t i = len_start; i < len_end; ++i) positions->push_back(i);
    }
  }
  return true;
}

inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  std::vector<std::size_t> positions;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    if (wire[pos] == 'P' || wire[pos] == 'O') {
      const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                           (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                           (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                           (static_cast<uint32_t>(wire[pos + 4]) << 24);
      // The container framing is real structure too -- it is what finds the
      // record boundaries -- so it stays eligible alongside the message's own.
      for (std::size_t j = 0; j < 5; ++j) positions.push_back(pos + j);
      const std::size_t body = pos + 5;
      if (body + len <= wire.size())
        pb_scan_message(wire, body, body + len, &positions, 0);
      pos = body + len;
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
  // v2's syntactic elements -- every byte whose corruption changes the SHAPE or
  // IDENTITY of data this arm reads, and no byte that merely holds a value.
  //
  //  1. segment terminators (\r) and the 3-char segment name. A name cannot be
  //     repaired: nothing in-band cross-validates it.
  //  2. the encoding characters | ^ & ~ \ -- field, component, subcomponent,
  //     repetition and escape separators. v2 has no in-string quoting, so a
  //     delimiter is syntax wherever it sits.
  //  3. inside a ZFX segment: the FHIR path in field 1, and the JSON
  //     punctuation in field 2.
  //
  // (3) is what this model was missing, and it is where the data is. 91.5% of
  // this wire is ZFX payload, and a ZFX payload is JSON -- its braces, brackets,
  // quotes, colons and commas are exactly as structural as a pipe. Treating
  // them as content left the arm's own carrier untouched by every run. The
  // field-1 path is identity for the same reason a segment name is: a flipped
  // path does not fail, it addresses something else.
  //
  // Bytes inside a JSON string literal stay content, which is why the scan
  // below tracks quoting rather than matching punctuation blindly.
  //
  // OBX interiors ARE eligible. They were excluded while this arm emitted no
  // leaf from an OBX -- a position that cannot change the fingerprint is free
  // armor, and 14,516 of the 108,270 positions were exactly that. The fix was
  // to decode OBX rather than to stop damaging it: OBX-5/-6 now carry the whole
  // Quantity and flush_obx() reads them back, so a flipped '|' there costs real
  // units. That is the `OBX|some|data|` -> `OBX|someLdata|` case: the fields
  // merge, OBX-5 becomes something else and OBX-6 disappears.
  std::vector<std::size_t> positions;
  std::size_t seg_start = 0;
  while (seg_start < wire.size()) {
    std::size_t seg_end = seg_start;
    while (seg_end < wire.size() && wire[seg_end] != '\r') ++seg_end;

    for (std::size_t j = 0; j < 3 && seg_start + j < seg_end; ++j)
      positions.push_back(seg_start + j);
    if (seg_end < wire.size()) positions.push_back(seg_end);

    const bool is_zfx = seg_end - seg_start >= 3 && wire[seg_start] == 'Z' &&
                        wire[seg_start + 1] == 'F' && wire[seg_start + 2] == 'X';

    {
      // Field boundaries within this segment.
      std::size_t bar1 = seg_end, bar2 = seg_end;
      for (std::size_t i = seg_start; i < seg_end; ++i) {
        if (wire[i] == '|' || wire[i] == '^' || wire[i] == '&' || wire[i] == '~' ||
            wire[i] == '\\')
          positions.push_back(i);
        if (wire[i] == '|') {
          if (bar1 == seg_end) bar1 = i;
          else if (bar2 == seg_end) bar2 = i;
        }
      }
      if (is_zfx && bar1 < seg_end) {
        // Field 1: the FHIR path. Identity -- every byte counts.
        const std::size_t path_end = (bar2 < seg_end) ? bar2 : seg_end;
        for (std::size_t i = bar1 + 1; i < path_end; ++i) positions.push_back(i);

        // Field 2: JSON. Punctuation outside string literals is structure.
        if (bar2 < seg_end) {
          bool in_str = false, esc = false;
          for (std::size_t i = bar2 + 1; i < seg_end; ++i) {
            const char c = static_cast<char>(wire[i]);
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { in_str = !in_str; positions.push_back(i); continue; }
            if (in_str) continue;
            if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')
              positions.push_back(i);
          }
        }
      }
    }
    seg_start = seg_end + 1;
  }

  // One position per byte. The two passes this replaced pushed segment names
  // and delimiters independently, so a byte could appear twice and be selected
  // twice -- and two flips of one bit is no flip at all, which silently made
  // some runs less damaged than the k they reported.
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
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

    // APPLY BEFORE READING. recover() only DIAGNOSES; apply() is the only
    // mutating entry point and it writes into a copy. Reading the damaged bytes
    // here would score every correctly-repaired value as wrong -- reporting the
    // damage the engine had just fixed. A failed apply leaves the copy as-is,
    // so this never reads better than the engine actually achieved.
    std::vector<BYTE> repaired;
    rec.apply(rep, repaired);
    const std::vector<uint8_t>& src =
        repaired.empty() ? wire : reinterpret_cast<const std::vector<uint8_t>&>(repaired);

    FastFHIR::Memory fixed = wrap_wire_bytes(src);
    FastFHIR::Parser parser(fixed);
    ffhr_collect(parser.root(), fp, FHIR_VERSION_R5);
    fp.finalize();
    return fp;
}
#elif defined(ARM_JSON)
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    const std::string text(wire.begin(), wire.end());

    // Whole document still parses: every leaf is reachable.
    try {
        json_collect(nlohmann::json::parse(text), fp);
        fp.finalize();
        return fp;
    } catch (const std::exception&) {
    }

    // RESYNC. One broken brace makes the whole document unparseable, and
    // scoring that as total loss would flatter the failure -- JSON's real
    // behaviour is that the damage is local to the object it lands in. So
    // recover each resource independently: find each `"resource"` marker, take
    // its brace-matched span, and keep the ones that still parse.
    std::size_t pos = 0;
    while (true) {
        const auto marker = text.find("\"resource\"", pos);
        if (marker == std::string::npos) break;
        const auto [open, close] = json_resource_extent(text, marker);
        if (close > open) {
            try {
                const auto res = nlohmann::json::parse(text.substr(open, close - open));
                const auto type = res.value("resourceType", std::string{});
                const auto id = res.value("id", std::string{});
                const auto key = resource_key(type, id);
                if (!key.empty()) {
                    for (auto it = res.begin(); it != res.end(); ++it) {
                        if (it.key() == "resourceType") continue;
                        json_walk_leaves(it.value(), key + "." + it.key(), fp);
                    }
                }
            } catch (const std::exception&) {
                // this resource is unreadable; the rest of the document is not
            }
        }
        const auto next = text.find("\"resource\"", marker + 10);
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
    // Same walk as the baseline: v2 has no repair step to apply, which IS the
    // finding -- its "recovery" reads whatever survived, and a segment whose
    // header or field positions were damaged simply yields nothing.
    return scan_v2_canonical(wire);
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
