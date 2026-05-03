#include "harness.hpp"

#include <FF_CodeSystems.hpp>
#include <FF_Patient.hpp>
#include <FF_Primitives.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifdef HAVE_HL7PARSER
#include <hl7parser/alloc.h>
#include <hl7parser/buffer.h>
#include <hl7parser/element.h>
#include <hl7parser/message.h>
#include <hl7parser/parser.h>
#include <hl7parser/segment.h>
#include <hl7parser/settings.h>
#endif

namespace bench {

namespace {

struct PathValue {
    std::string path;
    std::string value;
};

struct SerializedPatientMessage {
    std::string control_id;
    std::vector<PathValue> snapshot;
    std::string message;
};

struct ParsedMessageOutcome {
    bool parsed = false;
    bool parity_ok = false;
    int identifiers_found = 0;
    int contacts_found = 0;
    int z_segments_found = 0;
    std::string birthdate;
    std::string gender;
    std::string family_name;
    std::string given_name;
    std::string city;
    std::string home_phone;
    std::string marital;
    std::string deceased;
    std::vector<PathValue> snapshot;
};

bool is_present_uint8(const uint8_t value) {
    return value != FF_NULL_UINT8;
}

bool is_present_uint32(const uint32_t value) {
    return value != FF_NULL_UINT32;
}

bool is_present_f64(const double value) {
    return std::isfinite(value);
}

std::string value_to_string(std::string_view value) {
    return std::string(value);
}

std::string value_to_string(const bool value) {
    return value ? "true" : "false";
}

std::string value_to_string(const int32_t value) {
    return std::to_string(value);
}

std::string value_to_string(const uint32_t value) {
    return std::to_string(value);
}

std::string value_to_string(const int64_t value) {
    return std::to_string(value);
}

std::string value_to_string(const uint64_t value) {
    return std::to_string(value);
}

std::string value_to_string(const double value) {
    return std::to_string(value);
}

std::string enum_to_string(const AdministrativeGender value) {
    return FF_AdministrativeGenderToString(value);
}

std::string enum_to_string(const AddressType value) {
    return FF_AddressTypeToString(value);
}

std::string enum_to_string(const AddressUse value) {
    return FF_AddressUseToString(value);
}

std::string enum_to_string(const ContactPointSystem value) {
    return FF_ContactPointSystemToString(value);
}

std::string enum_to_string(const ContactPointUse value) {
    return FF_ContactPointUseToString(value);
}

std::string enum_to_string(const LinkType value) {
    return FF_LinkTypeToString(value);
}

std::string enum_to_string(const NameUse value) {
    return FF_NameUseToString(value);
}

std::string hl7_escape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (const char ch : raw) {
        switch (ch) {
            case '|':
                out += "\\F\\";
                break;
            case '^':
                out += "\\S\\";
                break;
            case '~':
                out += "\\R\\";
                break;
            case '&':
                out += "\\T\\";
                break;
            case '\\':
                out += "\\E\\";
                break;
            case '\r':
                out += "\\X0D\\";
                break;
            case '\n':
                out += "\\X0A\\";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

int hex_value(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return -1;
}

std::string hl7_unescape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\') {
            out.push_back(raw[i]);
            continue;
        }

        const std::size_t end = raw.find('\\', i + 1);
        if (end == std::string_view::npos) {
            out.push_back(raw[i]);
            continue;
        }

        const std::string_view token = raw.substr(i + 1, end - i - 1);
        if (token == "F") {
            out.push_back('|');
        } else if (token == "S") {
            out.push_back('^');
        } else if (token == "R") {
            out.push_back('~');
        } else if (token == "T") {
            out.push_back('&');
        } else if (token == "E") {
            out.push_back('\\');
        } else if (token.size() >= 3 && token[0] == 'X' && token.size() % 2 == 1) {
            for (std::size_t j = 1; j + 1 < token.size(); j += 2) {
                const int hi = hex_value(token[j]);
                const int lo = hex_value(token[j + 1]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                }
            }
        } else {
            out.push_back('\\');
            out.append(token.data(), token.size());
            out.push_back('\\');
        }

        i = end;
    }
    return out;
}

void append_hl7_field(std::string& segment, std::string_view value) {
    segment.push_back('|');
    segment.append(value.data(), value.size());
}

std::string join_with(std::string_view separator, const std::vector<std::string>& parts) {
    std::string out;
    bool first = true;
    for (const auto& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!first) {
            out.append(separator.data(), separator.size());
        }
        out += part;
        first = false;
    }
    return out;
}

std::string to_hl7_date(std::string_view yyyy_mm_dd) {
    std::string out;
    out.reserve(8);
    for (const char ch : yyyy_mm_dd) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            out.push_back(ch);
            if (out.size() == 8) {
                break;
            }
        }
    }
    return out;
}

std::string gender_to_hl7(const AdministrativeGender gender) {
    switch (gender) {
        case AdministrativeGender::Male:
            return "M";
        case AdministrativeGender::Female:
            return "F";
        case AdministrativeGender::Other:
            return "O";
        case AdministrativeGender::Unknown:
            return "U";
        default:
            return "U";
    }
}

std::string identifier_to_cx(const IdentifierData& identifier) {
    if (identifier.value.empty()) {
        return "";
    }

    std::string cx;
    cx.reserve(96);
    cx += hl7_escape(identifier.value);
    cx += "^^^";
    if (!identifier.system.empty()) {
        cx += hl7_escape(identifier.system);
    }
    if (identifier.type && !identifier.type->text.empty()) {
        cx += "^^";
        cx += hl7_escape(identifier.type->text);
    }
    return cx;
}

std::string human_name_to_xpn(const HumanNameData& name) {
    std::string xpn;
    xpn.reserve(128);
    xpn += hl7_escape(name.family);
    xpn += '^';
    if (!name.given.empty()) {
        xpn += hl7_escape(name.given.front());
    }
    xpn += '^';
    if (name.given.size() > 1) {
        xpn += hl7_escape(name.given[1]);
    }
    xpn += '^';
    if (!name.suffix.empty()) {
        xpn += hl7_escape(name.suffix.front());
    }
    xpn += '^';
    if (!name.prefix.empty()) {
        xpn += hl7_escape(name.prefix.front());
    }
    return xpn;
}

std::string address_to_xad(const AddressData& address) {
    std::string xad;
    xad.reserve(160);
    if (!address.line.empty()) {
        xad += hl7_escape(address.line.front());
    }
    xad += '^';
    if (address.line.size() > 1) {
        xad += hl7_escape(address.line[1]);
    }
    xad += '^';
    xad += hl7_escape(address.city);
    xad += '^';
    xad += hl7_escape(address.state);
    xad += '^';
    xad += hl7_escape(address.postalcode);
    xad += '^';
    xad += hl7_escape(address.country);
    return xad;
}

std::string telecom_to_xtn(const ContactPointData& telecom) {
    if (telecom.value.empty()) {
        return "";
    }

    std::string xtn;
    xtn.reserve(96);
    xtn += hl7_escape(telecom.value);
    xtn += '^';
    xtn += hl7_escape(enum_to_string(telecom.use));
    xtn += '^';
    xtn += hl7_escape(enum_to_string(telecom.system));
    return xtn;
}

std::string codeable_concept_primary_code(const CodeableConceptData* codeable_concept) {
    if (codeable_concept == nullptr) {
        return "";
    }
    if (!codeable_concept->coding.empty() && !codeable_concept->coding.front().code.empty()) {
        return hl7_escape(codeable_concept->coding.front().code);
    }
    return hl7_escape(codeable_concept->text);
}

std::string choice_kind(const ChoiceEntry& choice) {
    if (choice.is_empty()) {
        return "empty";
    }
    if (std::holds_alternative<bool>(choice.value)) {
        return "bool";
    }
    if (std::holds_alternative<int32_t>(choice.value)) {
        return "int32";
    }
    if (std::holds_alternative<uint32_t>(choice.value)) {
        return "uint32";
    }
    if (std::holds_alternative<int64_t>(choice.value)) {
        return "int64";
    }
    if (std::holds_alternative<uint64_t>(choice.value)) {
        return "uint64";
    }
    if (std::holds_alternative<double>(choice.value)) {
        return "double";
    }
    if (std::holds_alternative<std::string_view>(choice.value)) {
        return "string";
    }
    return "monostate";
}

std::string choice_value(const ChoiceEntry& choice) {
    if (choice.is_empty()) {
        return "";
    }
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "";
            } else {
                return value_to_string(value);
            }
        },
        choice.value);
}

std::string deceased_to_hl7(const ChoiceEntry& deceased) {
    if (deceased.is_empty()) {
        return "";
    }
    if (const auto* value = std::get_if<bool>(&deceased.value)) {
        return *value ? "Y" : "N";
    }
    return "";
}

void add_snapshot_value(std::vector<PathValue>& snapshot, std::string path, std::string value) {
    snapshot.push_back(PathValue{std::move(path), std::move(value)});
}

void add_snapshot_if_not_empty(std::vector<PathValue>& snapshot, const std::string& path, std::string_view value) {
    if (!value.empty()) {
        add_snapshot_value(snapshot, path, value_to_string(value));
    }
}

void add_snapshot_uint8(std::vector<PathValue>& snapshot, const std::string& path, const uint8_t value) {
    if (is_present_uint8(value)) {
        add_snapshot_value(snapshot, path, std::to_string(value));
    }
}

void add_snapshot_uint32(std::vector<PathValue>& snapshot, const std::string& path, const uint32_t value) {
    if (is_present_uint32(value)) {
        add_snapshot_value(snapshot, path, std::to_string(value));
    }
}

void add_snapshot_double(std::vector<PathValue>& snapshot, const std::string& path, const double value) {
    if (is_present_f64(value)) {
        add_snapshot_value(snapshot, path, value_to_string(value));
    }
}

template <typename EnumType>
void add_snapshot_enum(std::vector<PathValue>& snapshot, const std::string& path, const EnumType value) {
    const std::string text = enum_to_string(value);
    if (!text.empty()) {
        add_snapshot_value(snapshot, path, text);
    }
}

void append_period_snapshot(const std::string& path, const PeriodData& period, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", period.id);
    add_snapshot_if_not_empty(snapshot, path + ".start", period.start);
    add_snapshot_if_not_empty(snapshot, path + ".end", period.end);
}

void append_choice_snapshot(const std::string& path, const ChoiceEntry& choice, std::vector<PathValue>& snapshot) {
    if (choice.is_empty()) {
        return;
    }
    add_snapshot_value(snapshot, path + ".tag", std::to_string(static_cast<uint32_t>(choice.tag)));
    add_snapshot_value(snapshot, path + ".kind", choice_kind(choice));
    const std::string value = choice_value(choice);
    if (!value.empty()) {
        add_snapshot_value(snapshot, path + ".value", value);
    }
}

void append_coding_snapshot(const std::string& path, const CodingData& coding, std::vector<PathValue>& snapshot);
void append_codeable_concept_snapshot(const std::string& path, const CodeableConceptData& codeable_concept, std::vector<PathValue>& snapshot);
void append_reference_snapshot(const std::string& path, const ReferenceData& reference, std::vector<PathValue>& snapshot);
void append_extension_snapshot(const std::string& path, const ExtensionData& extension, std::vector<PathValue>& snapshot);

void append_meta_snapshot(const std::string& path, const MetaData& meta, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", meta.id);
    add_snapshot_if_not_empty(snapshot, path + ".versionid", meta.versionid);
    add_snapshot_if_not_empty(snapshot, path + ".lastupdated", meta.lastupdated);
    add_snapshot_if_not_empty(snapshot, path + ".source", meta.source);
    for (std::size_t i = 0; i < meta.profile.size(); ++i) {
        add_snapshot_if_not_empty(snapshot, path + ".profile[" + std::to_string(i) + "]", meta.profile[i]);
    }
    for (std::size_t i = 0; i < meta.security.size(); ++i) {
        append_coding_snapshot(path + ".security[" + std::to_string(i) + "]", meta.security[i], snapshot);
    }
    for (std::size_t i = 0; i < meta.tag.size(); ++i) {
        append_coding_snapshot(path + ".tag[" + std::to_string(i) + "]", meta.tag[i], snapshot);
    }
}

void append_narrative_snapshot(const std::string& path, const NarrativeData& narrative, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", narrative.id);
    add_snapshot_if_not_empty(snapshot, path + ".div", narrative.div);
    add_snapshot_value(snapshot, path + ".status", std::to_string(static_cast<uint32_t>(narrative.status)));
}

void append_coding_snapshot(const std::string& path, const CodingData& coding, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", coding.id);
    add_snapshot_if_not_empty(snapshot, path + ".system", coding.system);
    add_snapshot_if_not_empty(snapshot, path + ".version", coding.version);
    add_snapshot_if_not_empty(snapshot, path + ".code", coding.code);
    add_snapshot_if_not_empty(snapshot, path + ".display", coding.display);
    add_snapshot_uint8(snapshot, path + ".userselected", coding.userselected);
    for (std::size_t i = 0; i < coding.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", coding.extension[i], snapshot);
    }
}

void append_codeable_concept_snapshot(const std::string& path, const CodeableConceptData& codeable_concept, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", codeable_concept.id);
    add_snapshot_if_not_empty(snapshot, path + ".text", codeable_concept.text);
    for (std::size_t i = 0; i < codeable_concept.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", codeable_concept.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < codeable_concept.coding.size(); ++i) {
        append_coding_snapshot(path + ".coding[" + std::to_string(i) + "]", codeable_concept.coding[i], snapshot);
    }
}

void append_reference_snapshot(const std::string& path, const ReferenceData& reference, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", reference.id);
    add_snapshot_if_not_empty(snapshot, path + ".reference", reference.reference);
    add_snapshot_if_not_empty(snapshot, path + ".type", reference.type);
    add_snapshot_if_not_empty(snapshot, path + ".display", reference.display);
    for (std::size_t i = 0; i < reference.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", reference.extension[i], snapshot);
    }
    if (reference.identifier) {
        const IdentifierData& identifier = *reference.identifier;
        add_snapshot_if_not_empty(snapshot, path + ".identifier.id", identifier.id);
        add_snapshot_if_not_empty(snapshot, path + ".identifier.system", identifier.system);
        add_snapshot_if_not_empty(snapshot, path + ".identifier.value", identifier.value);
    }
}

void append_extension_snapshot(const std::string& path, const ExtensionData& extension, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", extension.id);
    add_snapshot_value(snapshot, path + ".ext_ref", std::to_string(extension.ext_ref));
    append_choice_snapshot(path + ".value", extension.value, snapshot);
    for (std::size_t i = 0; i < extension.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", extension.extension[i], snapshot);
    }
}

void append_identifier_snapshot(const std::string& path, const IdentifierData& identifier, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", identifier.id);
    add_snapshot_value(snapshot, path + ".use", std::to_string(static_cast<uint32_t>(identifier.use)));
    add_snapshot_if_not_empty(snapshot, path + ".system", identifier.system);
    add_snapshot_if_not_empty(snapshot, path + ".value", identifier.value);
    for (std::size_t i = 0; i < identifier.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", identifier.extension[i], snapshot);
    }
    if (identifier.type) {
        append_codeable_concept_snapshot(path + ".type", *identifier.type, snapshot);
    }
    if (identifier.period) {
        append_period_snapshot(path + ".period", *identifier.period, snapshot);
    }
    if (identifier.assigner) {
        append_reference_snapshot(path + ".assigner", *identifier.assigner, snapshot);
    }
}

void append_human_name_snapshot(const std::string& path, const HumanNameData& name, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", name.id);
    add_snapshot_enum(snapshot, path + ".use", name.use);
    add_snapshot_if_not_empty(snapshot, path + ".text", name.text);
    add_snapshot_if_not_empty(snapshot, path + ".family", name.family);
    for (std::size_t i = 0; i < name.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", name.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < name.given.size(); ++i) {
        add_snapshot_if_not_empty(snapshot, path + ".given[" + std::to_string(i) + "]", name.given[i]);
    }
    for (std::size_t i = 0; i < name.prefix.size(); ++i) {
        add_snapshot_if_not_empty(snapshot, path + ".prefix[" + std::to_string(i) + "]", name.prefix[i]);
    }
    for (std::size_t i = 0; i < name.suffix.size(); ++i) {
        add_snapshot_if_not_empty(snapshot, path + ".suffix[" + std::to_string(i) + "]", name.suffix[i]);
    }
    if (name.period) {
        append_period_snapshot(path + ".period", *name.period, snapshot);
    }
}

void append_contact_point_snapshot(const std::string& path, const ContactPointData& telecom, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", telecom.id);
    add_snapshot_enum(snapshot, path + ".system", telecom.system);
    add_snapshot_if_not_empty(snapshot, path + ".value", telecom.value);
    add_snapshot_enum(snapshot, path + ".use", telecom.use);
    add_snapshot_uint32(snapshot, path + ".rank", telecom.rank);
    for (std::size_t i = 0; i < telecom.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", telecom.extension[i], snapshot);
    }
    if (telecom.period) {
        append_period_snapshot(path + ".period", *telecom.period, snapshot);
    }
}

void append_address_snapshot(const std::string& path, const AddressData& address, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", address.id);
    add_snapshot_enum(snapshot, path + ".use", address.use);
    add_snapshot_enum(snapshot, path + ".type", address.type);
    add_snapshot_if_not_empty(snapshot, path + ".text", address.text);
    add_snapshot_if_not_empty(snapshot, path + ".city", address.city);
    add_snapshot_if_not_empty(snapshot, path + ".district", address.district);
    add_snapshot_if_not_empty(snapshot, path + ".state", address.state);
    add_snapshot_if_not_empty(snapshot, path + ".postalcode", address.postalcode);
    add_snapshot_if_not_empty(snapshot, path + ".country", address.country);
    for (std::size_t i = 0; i < address.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", address.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < address.line.size(); ++i) {
        add_snapshot_if_not_empty(snapshot, path + ".line[" + std::to_string(i) + "]", address.line[i]);
    }
    if (address.period) {
        append_period_snapshot(path + ".period", *address.period, snapshot);
    }
}

void append_attachment_snapshot(const std::string& path, const AttachmentData& attachment, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", attachment.id);
    add_snapshot_if_not_empty(snapshot, path + ".contenttype", attachment.contenttype);
    add_snapshot_if_not_empty(snapshot, path + ".language", attachment.language);
    add_snapshot_if_not_empty(snapshot, path + ".data", attachment.data);
    add_snapshot_if_not_empty(snapshot, path + ".url", attachment.url);
    add_snapshot_if_not_empty(snapshot, path + ".hash", attachment.hash);
    add_snapshot_if_not_empty(snapshot, path + ".title", attachment.title);
    add_snapshot_if_not_empty(snapshot, path + ".creation", attachment.creation);
    add_snapshot_uint32(snapshot, path + ".size", attachment.size);
    add_snapshot_uint32(snapshot, path + ".height", attachment.height);
    add_snapshot_uint32(snapshot, path + ".width", attachment.width);
    add_snapshot_uint32(snapshot, path + ".frames", attachment.frames);
    add_snapshot_double(snapshot, path + ".duration", attachment.duration);
    add_snapshot_uint32(snapshot, path + ".pages", attachment.pages);
    for (std::size_t i = 0; i < attachment.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", attachment.extension[i], snapshot);
    }
}

void append_patient_contact_snapshot(const std::string& path, const PatientcontactData& contact, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", contact.id);
    add_snapshot_enum(snapshot, path + ".gender", contact.gender);
    for (std::size_t i = 0; i < contact.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", contact.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < contact.modifierextension.size(); ++i) {
        append_extension_snapshot(path + ".modifierextension[" + std::to_string(i) + "]", contact.modifierextension[i], snapshot);
    }
    for (std::size_t i = 0; i < contact.relationship.size(); ++i) {
        append_codeable_concept_snapshot(path + ".relationship[" + std::to_string(i) + "]", contact.relationship[i], snapshot);
    }
    if (contact.name) {
        append_human_name_snapshot(path + ".name", *contact.name, snapshot);
    }
    for (std::size_t i = 0; i < contact.telecom.size(); ++i) {
        append_contact_point_snapshot(path + ".telecom[" + std::to_string(i) + "]", contact.telecom[i], snapshot);
    }
    if (contact.address) {
        append_address_snapshot(path + ".address", *contact.address, snapshot);
    }
    if (contact.organization) {
        append_reference_snapshot(path + ".organization", *contact.organization, snapshot);
    }
    if (contact.period) {
        append_period_snapshot(path + ".period", *contact.period, snapshot);
    }
}

void append_patient_communication_snapshot(const std::string& path, const PatientcommunicationData& communication, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", communication.id);
    add_snapshot_uint8(snapshot, path + ".preferred", communication.preferred);
    for (std::size_t i = 0; i < communication.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", communication.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < communication.modifierextension.size(); ++i) {
        append_extension_snapshot(path + ".modifierextension[" + std::to_string(i) + "]", communication.modifierextension[i], snapshot);
    }
    if (communication.language) {
        append_codeable_concept_snapshot(path + ".language", *communication.language, snapshot);
    }
}

void append_patient_link_snapshot(const std::string& path, const PatientlinkData& link, std::vector<PathValue>& snapshot) {
    add_snapshot_if_not_empty(snapshot, path + ".id", link.id);
    add_snapshot_enum(snapshot, path + ".type", link.type);
    for (std::size_t i = 0; i < link.extension.size(); ++i) {
        append_extension_snapshot(path + ".extension[" + std::to_string(i) + "]", link.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < link.modifierextension.size(); ++i) {
        append_extension_snapshot(path + ".modifierextension[" + std::to_string(i) + "]", link.modifierextension[i], snapshot);
    }
    if (link.other) {
        append_reference_snapshot(path + ".other", *link.other, snapshot);
    }
}

std::vector<PathValue> build_patient_snapshot(const PatientData& patient) {
    std::vector<PathValue> snapshot;
    add_snapshot_if_not_empty(snapshot, "id", patient.id);
    add_snapshot_if_not_empty(snapshot, "implicitrules", patient.implicitrules);
    add_snapshot_if_not_empty(snapshot, "language", patient.language);
    add_snapshot_uint8(snapshot, "active", patient.active);
    add_snapshot_enum(snapshot, "gender", patient.gender);
    add_snapshot_if_not_empty(snapshot, "birthdate", patient.birthdate);
    append_choice_snapshot("deceased", patient.deceased, snapshot);
    append_choice_snapshot("multiplebirth", patient.multiplebirth, snapshot);

    if (patient.meta) {
        append_meta_snapshot("meta", *patient.meta, snapshot);
    }
    if (patient.text) {
        append_narrative_snapshot("text", *patient.text, snapshot);
    }
    for (std::size_t i = 0; i < patient.contained.size(); ++i) {
        add_snapshot_value(snapshot, "contained[" + std::to_string(i) + "].offset", std::to_string(patient.contained[i].offset));
        add_snapshot_value(snapshot, "contained[" + std::to_string(i) + "].recovery", std::to_string(patient.contained[i].recovery));
    }
    for (std::size_t i = 0; i < patient.extension.size(); ++i) {
        append_extension_snapshot("extension[" + std::to_string(i) + "]", patient.extension[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.modifierextension.size(); ++i) {
        append_extension_snapshot("modifierextension[" + std::to_string(i) + "]", patient.modifierextension[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.identifier.size(); ++i) {
        append_identifier_snapshot("identifier[" + std::to_string(i) + "]", patient.identifier[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.name.size(); ++i) {
        append_human_name_snapshot("name[" + std::to_string(i) + "]", patient.name[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.telecom.size(); ++i) {
        append_contact_point_snapshot("telecom[" + std::to_string(i) + "]", patient.telecom[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.address.size(); ++i) {
        append_address_snapshot("address[" + std::to_string(i) + "]", patient.address[i], snapshot);
    }
    if (patient.maritalstatus) {
        append_codeable_concept_snapshot("maritalstatus", *patient.maritalstatus, snapshot);
    }
    for (std::size_t i = 0; i < patient.photo.size(); ++i) {
        append_attachment_snapshot("photo[" + std::to_string(i) + "]", patient.photo[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.contact.size(); ++i) {
        append_patient_contact_snapshot("contact[" + std::to_string(i) + "]", patient.contact[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.communication.size(); ++i) {
        append_patient_communication_snapshot("communication[" + std::to_string(i) + "]", patient.communication[i], snapshot);
    }
    for (std::size_t i = 0; i < patient.generalpractitioner.size(); ++i) {
        append_reference_snapshot("generalpractitioner[" + std::to_string(i) + "]", patient.generalpractitioner[i], snapshot);
    }
    if (patient.managingorganization) {
        append_reference_snapshot("managingorganization", *patient.managingorganization, snapshot);
    }
    for (std::size_t i = 0; i < patient.link.size(); ++i) {
        append_patient_link_snapshot("link[" + std::to_string(i) + "]", patient.link[i], snapshot);
    }

    std::sort(snapshot.begin(), snapshot.end(), [](const PathValue& lhs, const PathValue& rhs) {
        if (lhs.path != rhs.path) {
            return lhs.path < rhs.path;
        }
        return lhs.value < rhs.value;
    });
    return snapshot;
}

std::string build_pid_segment(const PatientData& patient) {
    std::vector<std::string> fields(30);
    fields[0] = "1";

    std::vector<std::string> identifiers;
    identifiers.reserve(patient.identifier.size());
    for (const auto& identifier : patient.identifier) {
        const auto cx = identifier_to_cx(identifier);
        if (!cx.empty()) {
            identifiers.push_back(cx);
        }
    }
    if (identifiers.empty() && !patient.id.empty()) {
        identifiers.push_back(hl7_escape(patient.id));
    }
    fields[2] = join_with("~", identifiers);

    std::vector<std::string> names;
    names.reserve(patient.name.size());
    for (const auto& name : patient.name) {
        const auto xpn = human_name_to_xpn(name);
        if (!xpn.empty()) {
            names.push_back(xpn);
        }
    }
    fields[4] = join_with("~", names);
    fields[6] = to_hl7_date(patient.birthdate);
    fields[7] = gender_to_hl7(patient.gender);

    std::vector<std::string> addresses;
    addresses.reserve(patient.address.size());
    for (const auto& address : patient.address) {
        const auto xad = address_to_xad(address);
        if (!xad.empty()) {
            addresses.push_back(xad);
        }
    }
    fields[10] = join_with("~", addresses);

    std::vector<std::string> home_phones;
    std::vector<std::string> work_phones;
    for (const auto& telecom : patient.telecom) {
        const auto xtn = telecom_to_xtn(telecom);
        if (xtn.empty()) {
            continue;
        }
        if (telecom.use == ContactPointUse::Work) {
            work_phones.push_back(xtn);
        } else {
            home_phones.push_back(xtn);
        }
    }
    fields[12] = join_with("~", home_phones);
    fields[13] = join_with("~", work_phones);
    fields[15] = codeable_concept_primary_code(patient.maritalstatus.get());
    fields[29] = deceased_to_hl7(patient.deceased);

    std::string pid = "PID";
    for (const auto& field : fields) {
        append_hl7_field(pid, field);
    }
    pid += "\r";
    return pid;
}

std::string build_pd1_segment(const PatientData& patient) {
    std::vector<std::string> fields(4);
    if (is_present_uint8(patient.active)) {
        fields[0] = patient.active == 0 ? "N" : "Y";
    }
    if (!patient.generalpractitioner.empty()) {
        fields[3] = hl7_escape(patient.generalpractitioner.front().display.empty()
            ? patient.generalpractitioner.front().reference
            : patient.generalpractitioner.front().display);
    }

    bool has_data = false;
    for (const auto& field : fields) {
        has_data = has_data || !field.empty();
    }
    if (!has_data) {
        return "";
    }

    std::string pd1 = "PD1";
    for (const auto& field : fields) {
        append_hl7_field(pd1, field);
    }
    pd1 += "\r";
    return pd1;
}

std::string build_nk1_segment(const PatientcontactData& contact, const std::size_t sequence) {
    std::vector<std::string> fields(8);
    fields[0] = std::to_string(sequence + 1);
    if (contact.name) {
        fields[1] = human_name_to_xpn(*contact.name);
    }
    if (!contact.relationship.empty() && !contact.relationship.front().coding.empty()) {
        fields[2] = hl7_escape(contact.relationship.front().coding.front().code);
    }
    if (contact.address) {
        fields[3] = address_to_xad(*contact.address);
    }
    std::vector<std::string> phones;
    for (const auto& telecom : contact.telecom) {
        const auto xtn = telecom_to_xtn(telecom);
        if (!xtn.empty()) {
            phones.push_back(xtn);
        }
    }
    fields[4] = join_with("~", phones);

    std::string nk1 = "NK1";
    for (const auto& field : fields) {
        append_hl7_field(nk1, field);
    }
    nk1 += "\r";
    return nk1;
}

std::vector<std::string> build_zpv_segments(const std::vector<PathValue>& snapshot) {
    std::vector<std::string> segments;
    segments.reserve(snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        std::string segment = "ZPV";
        append_hl7_field(segment, std::to_string(i + 1));
        append_hl7_field(segment, snapshot[i].path);
        append_hl7_field(segment, hl7_escape(snapshot[i].value));
        segment += "\r";
        segments.push_back(std::move(segment));
    }
    return segments;
}

SerializedPatientMessage serialize_patient_message(const PatientData& patient, const std::size_t index) {
    SerializedPatientMessage out;
    out.control_id = patient.id.empty()
        ? ("patient-" + std::to_string(index + 1))
        : value_to_string(patient.id);
    out.snapshot = build_patient_snapshot(patient);

    std::ostringstream message_stream;
    message_stream << "MSH|^~\\&|BENCH|BENCH|BENCH|BENCH|20260502170000||ADT^A08|"
                   << hl7_escape(out.control_id)
                   << "|P|2.5\r";
    message_stream << build_pid_segment(patient);

    const std::string pd1 = build_pd1_segment(patient);
    if (!pd1.empty()) {
        message_stream << pd1;
    }

    for (std::size_t i = 0; i < patient.contact.size(); ++i) {
        message_stream << build_nk1_segment(patient.contact[i], i);
    }

    const auto zpv_segments = build_zpv_segments(out.snapshot);
    for (const auto& segment : zpv_segments) {
        message_stream << segment;
    }

    out.message = message_stream.str();

    return out;
}

#ifdef HAVE_HL7PARSER
std::string element_to_string(HL7_Element* element) {
    if (element == nullptr || element->value == nullptr || element->length == 0) {
        return "";
    }
    return std::string(element->value, element->length);
}

ParsedMessageOutcome parse_patient_message(const std::string& message_text, const std::vector<PathValue>& expected_snapshot) {
    ParsedMessageOutcome outcome;

    HL7_Settings settings;
    HL7_Allocator allocator;
    HL7_Message message;
    HL7_Parser parser;
    HL7_Buffer input_buffer;
    std::vector<char> parse_bytes(message_text.begin(), message_text.end());

    hl7_settings_init(&settings);
    hl7_allocator_init(&allocator, malloc, free);
    hl7_message_init(&message, &settings, &allocator);
    hl7_parser_init(&parser, &settings);
    hl7_buffer_init(&input_buffer, parse_bytes.data(), parse_bytes.size());
    hl7_buffer_move_wr_ptr(&input_buffer, parse_bytes.size());

    const int parse_rc = hl7_parser_read(&parser, &message, &input_buffer);
    if (parse_rc == 0) {
        outcome.parsed = true;

        HL7_Segment pid{};
        if (hl7_message_segment(&message, &pid, "PID", 0) == 0) {
            if (HL7_Element* birth_date = hl7_segment_component(&pid, 6, 0)) {
                outcome.birthdate = element_to_string(birth_date);
            }
            if (HL7_Element* sex = hl7_segment_component(&pid, 7, 0)) {
                outcome.gender = element_to_string(sex);
            }
            if (HL7_Element* family = hl7_segment_component(&pid, 4, 0)) {
                outcome.family_name = element_to_string(family);
            }
            if (HL7_Element* given = hl7_segment_component(&pid, 4, 1)) {
                outcome.given_name = element_to_string(given);
            }
            if (HL7_Element* city = hl7_segment_component(&pid, 10, 2)) {
                outcome.city = element_to_string(city);
            }
            if (HL7_Element* home_phone = hl7_segment_component(&pid, 12, 0)) {
                outcome.home_phone = element_to_string(home_phone);
            }
            if (HL7_Element* marital = hl7_segment_component(&pid, 15, 0)) {
                outcome.marital = element_to_string(marital);
            }
            if (HL7_Element* deceased = hl7_segment_component(&pid, 29, 0)) {
                outcome.deceased = element_to_string(deceased);
            }

            for (std::size_t rep = 0;; ++rep) {
                HL7_Element* identifier = hl7_segment_component_rep(&pid, 2, rep, 0);
                if (identifier == nullptr || identifier->length == 0) {
                    break;
                }
                ++outcome.identifiers_found;
            }
        }

        for (std::size_t seq = 0;; ++seq) {
            HL7_Segment nk1{};
            if (hl7_message_segment(&message, &nk1, "NK1", seq) != 0) {
                break;
            }
            ++outcome.contacts_found;
        }

        for (std::size_t seq = 0;; ++seq) {
            HL7_Segment zpv{};
            if (hl7_message_segment(&message, &zpv, "ZPV", seq) != 0) {
                break;
            }
            ++outcome.z_segments_found;

            HL7_Element* path = hl7_segment_field(&zpv, 1);
            HL7_Element* value = hl7_segment_field(&zpv, 2);
            const std::string path_text = element_to_string(path);
            if (path_text.empty()) {
                continue;
            }
            outcome.snapshot.push_back(PathValue{path_text, hl7_unescape(element_to_string(value))});
        }

        std::sort(outcome.snapshot.begin(), outcome.snapshot.end(), [](const PathValue& lhs, const PathValue& rhs) {
            if (lhs.path != rhs.path) {
                return lhs.path < rhs.path;
            }
            return lhs.value < rhs.value;
        });

        outcome.parity_ok = outcome.snapshot.size() == expected_snapshot.size();
        if (outcome.parity_ok) {
            for (std::size_t i = 0; i < expected_snapshot.size(); ++i) {
                if (expected_snapshot[i].path != outcome.snapshot[i].path
                        || expected_snapshot[i].value != outcome.snapshot[i].value) {
                    outcome.parity_ok = false;
                    break;
                }
            }
        }
    }

    hl7_buffer_fini(&input_buffer);
    hl7_parser_fini(&parser);
    hl7_message_fini(&message);
    hl7_allocator_fini(&allocator);
    hl7_settings_fini(&settings);

    return outcome;
}
#endif

}  // namespace

ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture) {
    ArmRunResult result;
    result.metrics.reserve(3);

    Timer stage1;
    stage1.start();

    std::vector<SerializedPatientMessage> serialized_messages;
    serialized_messages.reserve(fixture.bundle.size());
    std::size_t total_bytes = 0;
    for (std::size_t i = 0; i < fixture.bundle.size(); ++i) {
        serialized_messages.push_back(serialize_patient_message(fixture.bundle[i].patient, i));
        total_bytes += serialized_messages.back().message.size();
    }
    (void)total_bytes;

    result.metrics.push_back(MetricEvent{
        "hl7v2",
        Stage::Stage1Serialize,
        std::max<std::int64_t>(stage1.stop_us(), 1),
    });
    result.metrics.push_back(MetricEvent{"hl7v2", Stage::Stage2Transport, 0});

    Timer stage3;
    stage3.start();

    int patients_found = 0;
    int identifiers_found = 0;
    int contacts_found = 0;
    int z_segments_found = 0;
    int parity_matches = 0;
    std::string found_birthdate;
    std::string found_gender;
    std::string found_family_name;
    std::string found_given_name;
    std::string found_city;
    std::string found_home_phone;
    std::string found_marital;
    std::string found_deceased;

#ifdef HAVE_HL7PARSER
    bool parse_failed = false;
    for (const auto& serialized : serialized_messages) {
        const ParsedMessageOutcome parsed = parse_patient_message(serialized.message, serialized.snapshot);
        if (!parsed.parsed) {
            parse_failed = true;
            break;
        }

        ++patients_found;
        identifiers_found += parsed.identifiers_found;
        contacts_found += parsed.contacts_found;
        z_segments_found += parsed.z_segments_found;
        if (parsed.parity_ok) {
            ++parity_matches;
        }

        if (found_birthdate.empty()) {
            found_birthdate = parsed.birthdate;
        }
        if (found_gender.empty()) {
            found_gender = parsed.gender;
        }
        if (found_family_name.empty()) {
            found_family_name = parsed.family_name;
        }
        if (found_given_name.empty()) {
            found_given_name = parsed.given_name;
        }
        if (found_city.empty()) {
            found_city = parsed.city;
        }
        if (found_home_phone.empty()) {
            found_home_phone = parsed.home_phone;
        }
        if (found_marital.empty()) {
            found_marital = parsed.marital;
        }
        if (found_deceased.empty()) {
            found_deceased = parsed.deceased;
        }
    }

    if (parse_failed) {
        result.queried_value = "hl7v2 parse error";
    }
#else
    result.queried_value = "blocked: hl7parser unavailable";
#endif

    if (result.queried_value.empty()) {
        result.queried_value = "patients=" + std::to_string(patients_found)
            + " identifiers=" + std::to_string(identifiers_found)
            + " contacts=" + std::to_string(contacts_found)
            + " zpv=" + std::to_string(z_segments_found)
            + " parity=" + std::to_string(parity_matches) + "/" + std::to_string(static_cast<int>(serialized_messages.size()))
            + " birthdate=" + (found_birthdate.empty() ? "none" : found_birthdate)
            + " gender=" + (found_gender.empty() ? "none" : found_gender)
            + " family=" + (found_family_name.empty() ? "none" : found_family_name)
            + " given=" + (found_given_name.empty() ? "none" : found_given_name)
            + " city=" + (found_city.empty() ? "none" : found_city)
            + " phone=" + (found_home_phone.empty() ? "none" : found_home_phone)
            + " marital=" + (found_marital.empty() ? "none" : found_marital)
            + " deceased=" + (found_deceased.empty() ? "none" : found_deceased);
    }

    result.metrics.push_back(MetricEvent{
        "hl7v2",
        Stage::Stage3Query,
        std::max<std::int64_t>(stage3.stop_us(), 1),
    });

    return result;
}

}  // namespace bench