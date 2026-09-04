#pragma once
// Enumerate a POCO's leaves -- the NEUTRAL instrument.
//
// Why this file exists
// --------------------
// The four arms were compared against `json.bin`, which is the JSON ARM'S OWN
// output (bundle.dump() of POCO 1). That made one branch the ground truth: the
// JSON arm scored 100% by construction, and any defect in it BECAME the
// reference -- `urlIndex` was, and `{"comparator": 255}` would have been.
//
// A neutral reference has to belong to no arm. The obstacle was that the only
// POCO walkers in existence WERE the arms' encoders (to_json_*, hl7_*, the
// proto assign), so "the same document" could only ever be expressed as one
// encoder's opinion. FastFHIR's generated visit_fields() closes that: it walks
// the struct itself, through neither a wire nor an encoder.
//
// The comparison this enables is POCO 1 vs POCO 2 -- the model every arm was
// HANDED, against the model recovered from what that arm wrote. Both sides are
// the same C++ types, so representation is identical by construction: an enum
// is an ordinal on both sides, a string_view is bytes on both sides, and no
// FHIR-name or value-format mapping is needed anywhere. That is what makes this
// instrument small enough to trust.
//
// Rendering is deliberately crude and deliberately SYMMETRIC. It is not FHIR
// JSON and is not meant to be read as such; it only has to be a faithful,
// stable projection applied identically to both sides.

#include "harness.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bench::poco {

using Leaf = std::pair<std::string, std::string>;

template <typename T>
void walk(const T& value, const std::string& path, std::vector<Leaf>& out);

namespace detail {

// A struct is anything the generator emitted a visitor for. Detecting it this
// way rather than listing types means a new FHIR datatype is covered the day it
// is generated, with no edit here.
template <typename T>
concept HasVisitor = requires(const T& v) {
    visit_fields(v, [](const char*, const auto&) {});
};

// Newlines are ESCAPED, not passed through: the leaves file is one record per
// line, and FHIR narrative (`text.div`) routinely contains them. Emitting them
// raw split single values across several records, which read as dozens of
// missing leaves on one side and dozens of unmatched fragments on the other --
// a diff artifact entirely of the instrument's making.
inline std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace detail

template <typename T>
void walk(const T& value, const std::string& path, std::vector<Leaf>& out) {
    using U = std::decay_t<T>;

    if constexpr (std::is_same_v<U, std::string_view> || std::is_same_v<U, std::string>) {
        if (!value.empty()) out.emplace_back(path, detail::quote(value));

    } else if constexpr (std::is_same_v<U, bool>) {
        out.emplace_back(path, value ? "true" : "false");

    } else if constexpr (std::is_enum_v<U>) {
        // The ORDINAL, not the FHIR code name: both sides are this same enum,
        // so the ordinal compares exactly and needs no code table. FF_UNSET is
        // absence, not a value.
        const auto n = static_cast<long long>(value);
        if (n != static_cast<long long>(U::FF_UNSET)) out.emplace_back(path, std::to_string(n));

    } else if constexpr (std::is_arithmetic_v<U>) {
        // FF_NULL_F64 is a NaN, and `x != sentinel` is ALWAYS true for NaN --
        // so the absence test let every unset double through and a
        // default-constructed Quantity reported `value = nan`.
        if constexpr (std::is_floating_point_v<U>) {
            if (std::isnan(value)) return;
        }
        if (value != static_cast<U>(FF_NULL_UINT32) && value != static_cast<U>(0))
            out.emplace_back(path, std::to_string(value));

    } else if constexpr (requires { *value; value != nullptr; }) {  // unique_ptr
        if (value != nullptr) walk(*value, path, out);

    } else if constexpr (requires { value.size(); value.begin(); }) {  // vector
        for (std::size_t i = 0; i < value.size(); ++i)
            walk(value[i], path + "[" + std::to_string(i) + "]", out);

    } else if constexpr (detail::HasVisitor<U>) {
        visit_fields(value, [&](const char* name, const auto& member) {
            // Extension.url is EXCLUDED, on both sides, and this is a gap in the
            // comparison rather than a tidy-up.
            //
            // The POCO stores it as a uint32 intern index into the arena's URL
            // trie -- the same class of defect as ChoiceEntry's old raw offset:
            // a structural handle where a value belongs. The arms all emit the
            // resolved STRING, and there is no honest way to turn that back
            // into an index here; interning locally would mint numbers that
            // cannot match POCO 1's, which is worse than not comparing, because
            // it would look like a difference between the formats.
            //
            // Consequence worth stating plainly: this comparison CANNOT detect
            // a format losing extension URLs. Closing it means giving the POCO
            // the string, which is a change to FastFHIR, not to this walker.
            using M = std::decay_t<decltype(member)>;
            if constexpr (std::is_same_v<M, std::uint32_t>) {
                if (std::string_view(name) == "url") return;
            }
            walk(member, path.empty() ? std::string(name) : path + "." + name, out);
        });
    }
    // Anything else (ChoiceEntry is handled by its own overload below) is
    // deliberately not guessed at.
}

// A choice carries its variant tag plus either an inline value or a decoded
// block; the tag is part of the identity, since `value` holding a Quantity and
// `value` holding a string are different data at the same path.
inline void walk_choice(const ChoiceEntry& c, const std::string& path, std::vector<Leaf>& out) {
    if (c.is_empty()) return;
    const std::string tagged = path + "{" + std::to_string(static_cast<int>(c.tag)) + "}";
    if (c.block) {
        std::visit(
            [&](const auto& v) {
                using V = std::decay_t<decltype(v)>;
                if constexpr (!std::is_same_v<V, std::monostate>) walk(v, tagged, out);
            },
            c.block->value);
        return;
    }
    // A PACKED date/time is 8 bytes of civil parts, not a number. Dumping the
    // raw integer made 1,470 leaves compare unequal against arms that render
    // the text -- the same value in two spellings, counted as a difference.
    // Verified: 1620368297335717891 re-packs from "2018-06-14T06:06:32+00:00"
    // to the identical bits.
    //
    // to_string() is FastFHIR's own renderer, so the projection cannot drift
    // from what the format says the value is.
    if (FF_IsDateTimeTag(c.tag)) {
        const std::string text = c.to_string();
        if (!text.empty()) out.emplace_back(tagged, detail::quote(text));
        return;
    }

    std::visit(
        [&](const auto& v) {
            using V = std::decay_t<decltype(v)>;
            if constexpr (!std::is_same_v<V, std::monostate>) walk(v, tagged, out);
        },
        c.value);
}

template <>
inline void walk<ChoiceEntry>(const ChoiceEntry& c, const std::string& path,
                              std::vector<Leaf>& out) {
    walk_choice(c, path, out);
}

// Every leaf of one bundle item, keyed by <ResourceType>/<id> so the set is
// order-independent -- protobuf is a record stream and v2 a segment stream, and
// neither is obliged to preserve Bundle order.
inline std::vector<Leaf> leaves(const BundlePatient& item) {
    std::vector<Leaf> out;
    if (!item.patient.id.empty())
        walk(item.patient, "Patient/" + std::string(item.patient.id), out);
    for (const auto& obs : item.observations)
        if (!obs.id.empty()) walk(obs, "Observation/" + std::string(obs.id), out);
    return out;
}

// POCO 2: every resource in a PARSED FastFHIR stream, hydrated back into the
// same structs POCO 1 is made of, and walked by the same instrument.
//
// This is the second half of the comparison the study needs. POCO 1 is the
// model an arm was HANDED; this is the model recovered from what that arm
// actually wrote. Both sides are the identical C++ types walked by identical
// code, so a difference is the ENCODING's, not the reader's.
//
// Grouping is irrelevant here -- leaves are keyed <ResourceType>/<id>, so the
// set does not care whether a decoder recovered resources in bundle order, in
// record order, or in segment order.
inline std::vector<Leaf> leaves_from_stream(const FastFHIR::Reflective::Node& root) {
    std::vector<Leaf> out;
    if (!root) return out;

    std::vector<FastFHIR::Reflective::Node> entries;
    try { entries = root[FastFHIR::Fields::BUNDLE::ENTRY].as_node().entries(); }
    catch (const std::exception&) { return out; }

    for (const auto& entry : entries) {
        FastFHIR::Reflective::Node resource;
        try { resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE].as_node(); }
        catch (const std::exception&) { continue; }
        if (!resource) continue;

        // Per resource, not per document: one unreadable resource must not take
        // the rest of the bundle with it, or damage anywhere reads as damage
        // everywhere.
        try {
            if (resource.recovery() == RECOVER_FF_PATIENT) {
                const PatientData d = resource.as<PatientData>();
                if (!d.id.empty()) walk(d, "Patient/" + std::string(d.id), out);
            } else if (resource.recovery() == RECOVER_FF_OBSERVATION) {
                const ObservationData d = resource.as<ObservationData>();
                if (!d.id.empty()) walk(d, "Observation/" + std::string(d.id), out);
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}

// Reassemble path/value leaves into a FHIR Bundle. The arms that are not
// already FHIR JSON (protobuf, HL7v2) reconstruct their document this way and
// then take the SAME ingest every other arm takes -- so no arm gets its own
// private route into the POCO.
inline nlohmann::json leaves_to_bundle(const std::vector<Leaf>& leaves) {
    std::map<std::string, nlohmann::json> resources;
    for (const auto& [path, value] : leaves) {
        const auto dot = path.find('.');
        if (dot == std::string::npos) continue;
        const std::string key = path.substr(0, dot);
        const auto slash = key.find('/');
        if (slash == std::string::npos) continue;

        nlohmann::json parsed;
        try { parsed = nlohmann::json::parse(value); }
        catch (const std::exception&) { parsed = value; }

        // Walk the element path, creating objects and array slots as needed.
        nlohmann::json* cursor = &resources[key];
        if (cursor->is_null()) {
            *cursor = nlohmann::json::object();
            (*cursor)["resourceType"] = key.substr(0, slash);
            (*cursor)["id"] = key.substr(slash + 1);
        }
        std::string rest = path.substr(dot + 1);
        std::size_t pos = 0;
        while (pos <= rest.size()) {
            const auto next = rest.find('.', pos);
            std::string seg = rest.substr(pos, next == std::string::npos ? std::string::npos
                                                                          : next - pos);
            const bool last = (next == std::string::npos);
            const auto br = seg.find('[');
            int index = -1;
            if (br != std::string::npos) {
                index = std::stoi(seg.substr(br + 1));
                seg = seg.substr(0, br);
            }
            if (index < 0) {
                if (last) { (*cursor)[seg] = parsed; break; }
                if (!(*cursor)[seg].is_object()) (*cursor)[seg] = nlohmann::json::object();
                cursor = &(*cursor)[seg];
            } else {
                if (!(*cursor)[seg].is_array()) (*cursor)[seg] = nlohmann::json::array();
                auto& arr = (*cursor)[seg];
                while (static_cast<int>(arr.size()) <= index) arr.push_back(nullptr);
                if (last) { arr[index] = parsed; break; }
                if (!arr[index].is_object()) arr[index] = nlohmann::json::object();
                cursor = &arr[index];
            }
            if (last) break;
            pos = next + 1;
        }
    }

    nlohmann::json bundle = {{"resourceType", "Bundle"}, {"type", "collection"}};
    bundle["entry"] = nlohmann::json::array();
    for (auto& [k, r] : resources) bundle["entry"].push_back({{"resource", std::move(r)}});
    return bundle;
}

}  // namespace bench::poco
