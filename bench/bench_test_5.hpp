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
        fp.units.push_back(canonical_leaf(path, v.str()));
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
            fp.units.push_back(canonical_leaf(child, v.str()));
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
                fp.units.push_back(canonical_leaf(child, v.str()));
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
// The byte span of the resource object a `"resource"` marker introduces: back
// up to its opening brace, forward to the closing one before the next marker.
// Used by the resync path when the document as a whole will not parse.
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
        fp.units.push_back(canonical_leaf(path, node.dump()));
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
    return is_string ? nlohmann::json(raw).dump() : raw;
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

// google-fhir WRAPS PRIMITIVES: `Patient.id` is a message `Id { value }`, and
// the encoder writes it as mutable_id()->set_value(...). So a proto path is one
// level deeper than the FHIR element path everywhere. Collapsing a wrapper --
// a message whose populated content is a single `value` -- is what makes this
// arm name a leaf the way the JSON document does.
inline bool pb_is_primitive_wrapper(const google::protobuf::Message& m,
                                    const google::protobuf::FieldDescriptor** out) {
    const auto* refl = m.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    refl->ListFields(m, &fields);
    if (fields.size() != 1) return false;
    const auto* f = fields.front();
    if (f->name() != "value" || f->is_repeated()) return false;
    if (f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) return false;
    *out = f;
    return true;
}

inline void pb_walk(const google::protobuf::Message& msg, const std::string& path,
                    StreamFingerprint& fp) {
    const auto* refl = msg.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    refl->ListFields(msg, &fields);
    for (const auto* f : fields) {
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
                const google::protobuf::FieldDescriptor* inner = nullptr;
                if (pb_is_primitive_wrapper(sub, &inner)) {
                    const bool istr =
                        inner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                        inner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM;
                    fp.units.push_back(canonical_leaf(
                        here, pb_json_value(pb_scalar_string(sub, sub.GetReflection(), inner, 0),
                                            istr)));
                } else {
                    pb_walk(sub, here, fp);
                }
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
