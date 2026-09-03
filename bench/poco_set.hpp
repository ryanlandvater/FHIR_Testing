#pragma once
// Assign a leaf into a POCO by its FHIR element path -- the generic inverse.
//
// Why this exists
// ---------------
// Each arm's encoder is ~2,000 lines of per-field functions (assign_patient_*,
// one overload per target). Hand-inverting three of those is a lot of surface
// to get subtly wrong, so the first attempt went the other way: reconstruct
// FHIR JSON from each wire and hand it to FastFHIR's ingestor.
//
// That was wrong, and not merely longer. It puts ONE reader in the path of every
// arm's score. Anything FastFHIR's ingest drops would then vanish from all four
// arms alike -- masking precisely the differences a format comparison exists to
// find, and quietly making FastFHIR's coverage the ceiling for its competitors.
//
// Inverting generically avoids that. The POCO became reflectable (visit_fields)
// and now WRITABLE through the same reflection, so a decoder that can name a
// field -- `name[0].family` -- can populate the struct directly. Each arm
// inverts its own encoder; no arm borrows another's parser.
//
// The path grammar is the one every arm already produces:
//     segment := <fhirName> | <fhirName>[<index>]
//     path    := segment ("." segment)*

#include "harness.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bench::poco {

template <typename T>
bool set_path(T &target, std::string_view path, const nlohmann::json &value);

namespace detail {

template <typename T>
concept Writable = requires(T &v) {
    visit_fields(v, [](const char *, auto &) {});
};

// Split "value{516}" into ("value", 516); tag is -1 when absent. The walker
// puts the variant tag in the path on purpose -- `value` holding a Quantity and
// `value` holding a string are different data at the same element -- so the
// inverse has to read it back to know WHICH variant to build.
inline std::pair<std::string_view, int> split_tag(std::string_view seg) {
    const auto br = seg.find('{');
    if (br == std::string_view::npos) return {seg, -1};
    int tag = -1;
    const auto tail = seg.substr(br + 1);
    std::from_chars(tail.data(), tail.data() + tail.size(), tag);
    return {seg.substr(0, br), tag};
}

// Split "name[3]" into ("name", 3); index is -1 when absent.
inline std::pair<std::string_view, int> split_index(std::string_view seg) {
    const auto br = seg.find('[');
    if (br == std::string_view::npos) return {seg, -1};
    int idx = -1;
    const auto tail = seg.substr(br + 1);
    std::from_chars(tail.data(), tail.data() + tail.size(), idx);
    return {seg.substr(0, br), idx};
}

// Assign a JSON scalar into whatever the member actually is. Anything that does
// not convert is REFUSED rather than coerced: a silent 0 for a string is the
// kind of "success" that makes a comparison meaningless.
template <typename M>
bool assign_scalar(M &member, const nlohmann::json &v) {
    using U = std::decay_t<M>;
    if constexpr (std::is_same_v<U, std::string_view> || std::is_same_v<U, std::string>) {
        if (!v.is_string()) return false;
        // string_view must outlive this call, so the bytes are interned.
        static std::vector<std::unique_ptr<std::string>> pool;
        pool.push_back(std::make_unique<std::string>(v.get<std::string>()));
        member = *pool.back();
        return true;
    } else if constexpr (std::is_same_v<U, bool>) {
        if (!v.is_boolean()) return false;
        member = v.get<bool>();
        return true;
    } else if constexpr (std::is_enum_v<U>) {
        if (!v.is_number_integer()) return false;
        member = static_cast<U>(v.get<long long>());
        return true;
    } else if constexpr (std::is_arithmetic_v<U>) {
        if (!v.is_number()) return false;
        member = static_cast<U>(v.get<double>());
        return true;
    } else {
        return false;
    }
}

// A choice's INLINE variant: the value area, not a block. The variant arm is
// chosen by what the JSON actually is, since the tag names the FHIR type and
// several FHIR types share one C++ representation.
inline bool assign_choice_scalar(ChoiceEntry &c, const nlohmann::json &v) {
    if (v.is_boolean()) { c.value = v.get<bool>(); return true; }
    if (v.is_number_integer()) { c.value = static_cast<int64_t>(v.get<long long>()); return true; }
    if (v.is_number()) { c.value = v.get<double>(); return true; }
    if (v.is_string()) {
        static std::vector<std::unique_ptr<std::string>> pool;
        pool.push_back(std::make_unique<std::string>(v.get<std::string>()));
        c.value = std::string_view(*pool.back());
        return true;
    }
    return false;
}

}  // namespace detail

// Descend one segment into `member` and continue, or assign if this is the last.
template <typename M>
bool set_member(M &member, std::string_view rest, const nlohmann::json &value) {
    using U = std::decay_t<M>;

    if constexpr (requires { member.reset(); *member; }) {  // unique_ptr
        // The POINTEE decides, not the pointer: a unique_ptr<PeriodData> keeps
        // descending, a unique_ptr<std::string_view> is a leaf behind an
        // indirection. Recursing on the pointer alone tried to reflect a
        // string_view and failed to compile -- the right check is always "is
        // the thing I am about to descend into writable?".
        using E = std::decay_t<decltype(*member)>;
        if (!member) member = std::make_unique<E>();
        if constexpr (detail::Writable<E>)
            return rest.empty() ? false : set_path(*member, rest, value);
        else
            return rest.empty() ? detail::assign_scalar(*member, value) : false;

    } else if constexpr (requires { member.size(); member.emplace_back(); }) {
        return false;  // vectors are handled by the caller, which knows the index

    } else if constexpr (detail::Writable<U>) {
        return rest.empty() ? false : set_path(member, rest, value);

    } else {
        return rest.empty() ? detail::assign_scalar(member, value) : false;
    }
}

template <typename T>
bool set_path(T &target, std::string_view path, const nlohmann::json &value) {
    const auto dot = path.find('.');
    const std::string_view seg = path.substr(0, dot);
    const std::string_view rest =
        (dot == std::string_view::npos) ? std::string_view{} : path.substr(dot + 1);
    const auto [tagged_name, choice_tag] = detail::split_tag(seg);
    const auto [name, index] = detail::split_index(tagged_name);

    bool done = false;
    visit_fields(target, [&](const char *field, auto &member) {
        if (done || name != std::string_view(field)) return;
        using M = std::decay_t<decltype(member)>;

        if constexpr (std::is_same_v<M, ChoiceEntry>) {
            if (choice_tag < 0) return;  // a choice addressed without its variant
            member.tag = static_cast<RECOVERY_TAG>(choice_tag);
            if (rest.empty()) {
                done = detail::assign_choice_scalar(member, value);
                return;
            }
            // A block-typed variant: build the alternative the tag names, then
            // descend into it. FF_MakeChoiceBlock is the generator's own inverse
            // of the decode switch, so a tag this profile cannot build is
            // refused rather than approximated.
            if (!member.block) member.block = FF_MakeChoiceBlock(member.tag);
            if (!member.block) return;
            std::visit(
                [&](auto &alt) {
                    using A = std::decay_t<decltype(alt)>;
                    if constexpr (!std::is_same_v<A, std::monostate>)
                        done = set_path(alt, rest, value);
                },
                member.block->value);
            return;
        }

        if constexpr (requires { member.size(); member.emplace_back(); }) {
            if (index < 0) return;  // an array member addressed without an index
            while (static_cast<int>(member.size()) <= index) member.emplace_back();
            done = set_member(member[static_cast<std::size_t>(index)], rest, value);
        } else {
            if (index >= 0) return;  // a scalar member addressed with an index
            done = set_member(member, rest, value);
        }
        (void)sizeof(M);
    });
    return done;
}

}  // namespace bench::poco
