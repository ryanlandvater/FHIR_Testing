#pragma once

/*
IMPORTANT BENCHMARK NOTE

This header is intentionally NOT representative of normal FastFHIR usage.

Normal FastFHIR application code should typically:
1) populate PatientData (or other resource structs) directly, then
2) call builder.append_obj(populated_struct).

In this benchmark harness, assignment is intentionally structured differently
to enforce per-line assignment parity across formats (FastFHIR vs JSON) for
fair, auditable comparisons. The shared assignment flow in this file is a
benchmark-control mechanism, not a recommended production integration pattern.
*/

#include <FF_Patient.hpp>
#include <FF_Observation.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bench::assign {

namespace detail {

using Json = nlohmann::json;

inline bool has_u8(uint8_t v) { return v != FF_NULL_UINT8; }
inline bool has_u32(uint32_t v) { return v != FF_NULL_UINT32; }
inline bool has_f64(double v) { return v != FF_NULL_F64; }

#if defined(ARM_HL7V2)
struct HL7v2Sink {
  bench::hl7v2::OruR01Message& message;
  bench::hl7v2::ObxSegment current_obx{};
  bool has_open_observation = false;

  void append_custom_field(std::string_view field_name, std::string payload) {
    message.append_custom_field(field_name, std::move(payload));
  }

  void begin_observation() {
    current_obx = bench::hl7v2::ObxSegment{};
    current_obx.set_id = static_cast<int>(message.obx.size() + 1);
    has_open_observation = true;
  }

  void finish_observation() {
    if (!has_open_observation) {
      return;
    }
    if (current_obx.observation_id.empty()) {
      current_obx.observation_id = "UNK^Observation^99LOCAL";
    }
    message.obx.push_back(std::move(current_obx));
    current_obx = bench::hl7v2::ObxSegment{};
    has_open_observation = false;
  }
};
#endif

inline std::string choice_suffix(RECOVERY_TAG tag) {
  switch (tag) {
    case RECOVER_FF_BOOL:
      return "Boolean";
    case RECOVER_FF_INT32:
    case RECOVER_FF_UINT32:
    case RECOVER_FF_INT64:
    case RECOVER_FF_UINT64:
      return "Integer";
    case RECOVER_FF_FLOAT64:
      return "Decimal";
    case RECOVER_FF_DATE:
      return "Date";
    case RECOVER_FF_DATETIME:
      return "DateTime";
    case RECOVER_FF_TIME:
      return "Time";
    case RECOVER_FF_INSTANT:
      return "Instant";
    case RECOVER_FF_CODE:
      return "Code";
    case RECOVER_FF_STRING:
      return "String";
    default:
      return "String";
  }
}

inline void write_choice(Json& out, const std::string_view base, const ChoiceEntry& choice) {
  if (choice.is_empty()) {
    return;
  }

  const auto key = std::string(base) + choice_suffix(choice.tag);
  if (std::holds_alternative<bool>(choice.value)) {
    out[key] = std::get<bool>(choice.value);
  } else if (std::holds_alternative<int32_t>(choice.value)) {
    out[key] = std::get<int32_t>(choice.value);
  } else if (std::holds_alternative<uint32_t>(choice.value)) {
    out[key] = std::get<uint32_t>(choice.value);
  } else if (std::holds_alternative<int64_t>(choice.value)) {
    out[key] = std::get<int64_t>(choice.value);
  } else if (std::holds_alternative<uint64_t>(choice.value)) {
    out[key] = std::get<uint64_t>(choice.value);
  } else if (std::holds_alternative<double>(choice.value)) {
    out[key] = std::get<double>(choice.value);
  } else if (std::holds_alternative<std::string_view>(choice.value)) {
    out[key] = std::string(std::get<std::string_view>(choice.value));
  }
}

inline void put_if_string(Json& out, const char* key, std::string_view value) {
  if (!value.empty()) out[key] = value;
}
inline void put_if_u32(Json& out, const char* key, uint32_t value) {
  if (has_u32(value)) out[key] = value;
}
inline void put_if_f64(Json& out, const char* key, double value) {
  if (has_f64(value)) out[key] = value;
}
inline void put_if_bool_flag(Json& out, const char* key, uint8_t flag) {
  if (has_u8(flag)) out[key] = (flag != 0);
}
inline void put_if_enum(Json& out, const char* key, int value) {
  if (value != 0) out[key] = value;
}

// ------- Datatypes -------

inline Json to_json_extension(const ExtensionData& src);
inline Json to_json_coding(const CodingData& src);
inline Json to_json_codeable_concept(const CodeableConceptData& src);
inline Json to_json_period(const PeriodData& src);
inline Json to_json_quantity(const QuantityData& src);
inline Json to_json_reference(const ReferenceData& src);
inline Json to_json_identifier(const IdentifierData& src);
inline Json to_json_meta(const MetaData& src);
inline Json to_json_narrative(const NarrativeData& src);
inline Json to_json_human_name(const HumanNameData& src);
inline Json to_json_address(const AddressData& src);
inline Json to_json_contact_point(const ContactPointData& src);
inline Json to_json_attachment(const AttachmentData& src);
inline Json to_json_range(const RangeData& src);
inline Json to_json_annotation(const AnnotationData& src);
inline Json to_json_observation_triggered_by(const ObservationtriggeredByData& src);
inline Json to_json_observation_reference_range(const ObservationreferenceRangeData& src);
inline Json to_json_observation_component(const ObservationcomponentData& src);
inline Json to_json_extension(const ExtensionData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.ext_ref != 0) out["urlIndex"] = src.ext_ref;
  write_choice(out, "value", src.value);
  return out;
}

inline Json to_json_coding(const CodingData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  put_if_string(out, "system", src.system);
  put_if_string(out, "version", src.version);
  put_if_string(out, "code", src.code);
  put_if_string(out, "display", src.display);
  put_if_bool_flag(out, "userSelected", src.userselected);
  return out;
}

inline Json to_json_codeable_concept(const CodeableConceptData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_string(out, "text", src.text);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.coding.empty()) {
    out["coding"] = Json::array();
    for (const auto& c : src.coding) out["coding"].push_back(to_json_coding(c));
  }
  return out;
}

inline Json to_json_period(const PeriodData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_string(out, "start", src.start);
  put_if_string(out, "end", src.end);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  return out;
}

inline Json to_json_quantity(const QuantityData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_f64(out, "value", src.value);
  put_if_enum(out, "comparator", static_cast<int>(src.comparator));
  put_if_string(out, "unit", src.unit);
  put_if_string(out, "system", src.system);
  put_if_string(out, "code", src.code);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  return out;
}

inline Json to_json_reference(const ReferenceData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_string(out, "reference", src.reference);
  put_if_string(out, "type", src.type);
  put_if_string(out, "display", src.display);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.identifier) out["identifier"] = to_json_identifier(*src.identifier);
  return out;
}

inline Json to_json_identifier(const IdentifierData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "use", static_cast<int>(src.use));
  put_if_string(out, "system", src.system);
  put_if_string(out, "value", src.value);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.type) out["type"] = to_json_codeable_concept(*src.type);
  if (src.period) out["period"] = to_json_period(*src.period);
  if (src.assigner) out["assigner"] = to_json_reference(*src.assigner);
  return out;
}

inline Json to_json_meta(const MetaData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_string(out, "versionId", src.versionid);
  put_if_string(out, "lastUpdated", src.lastupdated);
  put_if_string(out, "source", src.source);
  if (!src.profile.empty()) {
    out["profile"] = Json::array();
    for (auto v : src.profile) if (!v.empty()) out["profile"].push_back(v);
  }
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.security.empty()) {
    out["security"] = Json::array();
    for (const auto& c : src.security) out["security"].push_back(to_json_coding(c));
  }
  if (!src.tag.empty()) {
    out["tag"] = Json::array();
    for (const auto& c : src.tag) out["tag"].push_back(to_json_coding(c));
  }
  return out;
}

inline Json to_json_narrative(const NarrativeData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "status", static_cast<int>(src.status));
  put_if_string(out, "div", src.div);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  return out;
}

inline Json to_json_human_name(const HumanNameData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "use", static_cast<int>(src.use));
  put_if_string(out, "text", src.text);
  put_if_string(out, "family", src.family);
  if (!src.given.empty()) {
    out["given"] = Json::array();
    for (auto v : src.given) if (!v.empty()) out["given"].push_back(v);
  }
  if (!src.prefix.empty()) {
    out["prefix"] = Json::array();
    for (auto v : src.prefix) if (!v.empty()) out["prefix"].push_back(v);
  }
  if (!src.suffix.empty()) {
    out["suffix"] = Json::array();
    for (auto v : src.suffix) if (!v.empty()) out["suffix"].push_back(v);
  }
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.period) out["period"] = to_json_period(*src.period);
  return out;
}

inline Json to_json_address(const AddressData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "use", static_cast<int>(src.use));
  put_if_enum(out, "type", static_cast<int>(src.type));
  put_if_string(out, "text", src.text);
  if (!src.line.empty()) {
    out["line"] = Json::array();
    for (auto v : src.line) if (!v.empty()) out["line"].push_back(v);
  }
  put_if_string(out, "city", src.city);
  put_if_string(out, "district", src.district);
  put_if_string(out, "state", src.state);
  put_if_string(out, "postalCode", src.postalcode);
  put_if_string(out, "country", src.country);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.period) out["period"] = to_json_period(*src.period);
  return out;
}

inline Json to_json_contact_point(const ContactPointData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "system", static_cast<int>(src.system));
  put_if_string(out, "value", src.value);
  put_if_enum(out, "use", static_cast<int>(src.use));
  put_if_u32(out, "rank", src.rank);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.period) out["period"] = to_json_period(*src.period);
  return out;
}

inline Json to_json_attachment(const AttachmentData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_string(out, "contentType", src.contenttype);
  put_if_string(out, "language", src.language);
  put_if_string(out, "data", src.data);
  put_if_string(out, "url", src.url);
  put_if_u32(out, "size", src.size);
  put_if_string(out, "hash", src.hash);
  put_if_string(out, "title", src.title);
  put_if_string(out, "creation", src.creation);
  put_if_u32(out, "height", src.height);
  put_if_u32(out, "width", src.width);
  put_if_u32(out, "frames", src.frames);
  put_if_f64(out, "duration", src.duration);
  put_if_u32(out, "pages", src.pages);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  return out;
}

inline Json to_json_range(const RangeData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (src.low) out["low"] = to_json_quantity(*src.low);
  if (src.high) out["high"] = to_json_quantity(*src.high);
  return out;
}

inline Json to_json_annotation(const AnnotationData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  write_choice(out, "author", src.author);
  put_if_string(out, "time", src.time);
  put_if_string(out, "text", src.text);
  return out;
}

inline Json to_json_observation_triggered_by(const ObservationtriggeredByData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.modifierextension.empty()) {
    out["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) out["modifierExtension"].push_back(to_json_extension(e));
  }
  if (src.observation) out["observation"] = to_json_reference(*src.observation);
  put_if_enum(out, "type", static_cast<int>(src.type));
  put_if_string(out, "reason", src.reason);
  return out;
}

inline Json to_json_observation_reference_range(const ObservationreferenceRangeData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.modifierextension.empty()) {
    out["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) out["modifierExtension"].push_back(to_json_extension(e));
  }
  if (src.low) out["low"] = to_json_quantity(*src.low);
  if (src.high) out["high"] = to_json_quantity(*src.high);
  if (src.type) out["type"] = to_json_codeable_concept(*src.type);
  if (!src.appliesto.empty()) {
    out["appliesTo"] = Json::array();
    for (const auto& c : src.appliesto) out["appliesTo"].push_back(to_json_codeable_concept(c));
  }
  if (src.age) out["age"] = to_json_range(*src.age);
  put_if_string(out, "text", src.text);
  if (src.normalvalue) out["normalValue"] = to_json_codeable_concept(*src.normalvalue);
  return out;
}

inline Json to_json_observation_component(const ObservationcomponentData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.modifierextension.empty()) {
    out["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) out["modifierExtension"].push_back(to_json_extension(e));
  }
  if (src.code) out["code"] = to_json_codeable_concept(*src.code);
  write_choice(out, "value", src.value);
  if (src.dataabsentreason) out["dataAbsentReason"] = to_json_codeable_concept(*src.dataabsentreason);
  if (!src.interpretation.empty()) {
    out["interpretation"] = Json::array();
    for (const auto& c : src.interpretation) out["interpretation"].push_back(to_json_codeable_concept(c));
  }
  if (!src.referencerange.empty()) {
    out["referenceRange"] = Json::array();
    for (const auto& rr : src.referencerange) out["referenceRange"].push_back(to_json_observation_reference_range(rr));
  }
  return out;
}

// ------- Patient nested -------
inline Json to_json_patient_contact(const PatientcontactData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "gender", static_cast<int>(src.gender));
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.modifierextension.empty()) {
    out["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) out["modifierExtension"].push_back(to_json_extension(e));
  }
  if (!src.relationship.empty()) {
    out["relationship"] = Json::array();
    for (const auto& c : src.relationship) out["relationship"].push_back(to_json_codeable_concept(c));
  }
  if (src.name) out["name"] = to_json_human_name(*src.name);
  if (!src.telecom.empty()) {
    out["telecom"] = Json::array();
    for (const auto& t : src.telecom) out["telecom"].push_back(to_json_contact_point(t));
  }
  if (src.address) out["address"] = to_json_address(*src.address);
  if (src.organization) out["organization"] = to_json_reference(*src.organization);
  if (src.period) out["period"] = to_json_period(*src.period);
  return out;
}

inline Json to_json_patient_communication(const PatientcommunicationData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_bool_flag(out, "preferred", src.preferred);
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.modifierextension.empty()) {
    out["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) out["modifierExtension"].push_back(to_json_extension(e));
  }
  if (src.language) out["language"] = to_json_codeable_concept(*src.language);
  return out;
}

inline Json to_json_patient_link(const PatientlinkData& src) {
  Json out = Json::object();
  put_if_string(out, "id", src.id);
  put_if_enum(out, "type", static_cast<int>(src.type));
  if (!src.extension.empty()) {
    out["extension"] = Json::array();
    for (const auto& e : src.extension) out["extension"].push_back(to_json_extension(e));
  }
  if (!src.modifierextension.empty()) {
    out["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) out["modifierExtension"].push_back(to_json_extension(e));
  }
  if (src.other) out["other"] = to_json_reference(*src.other);
  return out;
}

#if defined(ARM_HL7V2)
inline Json hl7_json_value(std::string_view src) { return std::string(src); }
inline Json hl7_json_value(const std::string& src) { return src; }
inline Json hl7_json_value(bool src) { return src; }
inline Json hl7_json_value(uint8_t src) { return src != 0; }
inline Json hl7_json_value(uint32_t src) { return src; }
inline Json hl7_json_value(int src) { return src; }
inline Json hl7_json_value(double src) { return src; }
inline Json hl7_json_value(const ResourceReference& src) {
  return Json{{"offset", src.offset}, {"recovery", static_cast<int>(src.recovery)}};
}
inline Json hl7_json_value(const ExtensionData& src) { return to_json_extension(src); }
inline Json hl7_json_value(const CodingData& src) { return to_json_coding(src); }
inline Json hl7_json_value(const CodeableConceptData& src) { return to_json_codeable_concept(src); }
inline Json hl7_json_value(const PeriodData& src) { return to_json_period(src); }
inline Json hl7_json_value(const QuantityData& src) { return to_json_quantity(src); }
inline Json hl7_json_value(const ReferenceData& src) { return to_json_reference(src); }
inline Json hl7_json_value(const IdentifierData& src) { return to_json_identifier(src); }
inline Json hl7_json_value(const MetaData& src) { return to_json_meta(src); }
inline Json hl7_json_value(const NarrativeData& src) { return to_json_narrative(src); }
inline Json hl7_json_value(const HumanNameData& src) { return to_json_human_name(src); }
inline Json hl7_json_value(const AddressData& src) { return to_json_address(src); }
inline Json hl7_json_value(const ContactPointData& src) { return to_json_contact_point(src); }
inline Json hl7_json_value(const AttachmentData& src) { return to_json_attachment(src); }
inline Json hl7_json_value(const RangeData& src) { return to_json_range(src); }
inline Json hl7_json_value(const AnnotationData& src) { return to_json_annotation(src); }
inline Json hl7_json_value(const ObservationtriggeredByData& src) {
  return to_json_observation_triggered_by(src);
}
inline Json hl7_json_value(const ObservationreferenceRangeData& src) {
  return to_json_observation_reference_range(src);
}
inline Json hl7_json_value(const ObservationcomponentData& src) {
  return to_json_observation_component(src);
}
inline Json hl7_json_value(const PatientcontactData& src) { return to_json_patient_contact(src); }
inline Json hl7_json_value(const PatientcommunicationData& src) {
  return to_json_patient_communication(src);
}
inline Json hl7_json_value(const PatientlinkData& src) { return to_json_patient_link(src); }
inline Json hl7_json_value(const ChoiceEntry& choice) {
  Json out = Json::object();
  write_choice(out, "value", choice);
  return out;
}

template <typename T>
inline Json hl7_json_array(const std::vector<T>& values) {
  Json out = Json::array();
  for (const auto& value : values) {
    out.push_back(hl7_json_value(value));
  }
  return out;
}

template <typename Target>
inline void hl7_append_json_field(Target& dst, const char* field_name, const Json& payload) {
  dst.append_custom_field(field_name, payload.dump());
}

template <typename Target>
inline void hl7_mark_if_string(Target& dst, const char* field_name, std::string_view value) {
  if (!value.empty()) {
    hl7_append_json_field(dst, field_name, hl7_json_value(value));
  }
}

template <typename Target>
inline void hl7_mark_if_bool(Target& dst, const char* field_name, uint8_t value) {
  if (has_u8(value)) {
    hl7_append_json_field(dst, field_name, hl7_json_value(value));
  }
}

template <typename Target, typename Pointer>
inline void hl7_mark_if_pointer(Target& dst, const char* field_name, const Pointer& value) {
  if (value) {
    hl7_append_json_field(dst, field_name, hl7_json_value(*value));
  }
}

template <typename Target, typename T>
inline void hl7_mark_if_vector(Target& dst, const char* field_name, const std::vector<T>& values) {
  if (!values.empty()) {
    hl7_append_json_field(dst, field_name, hl7_json_array(values));
  }
}

template <typename Target>
inline void hl7_mark_if_choice(Target& dst, const char* field_name, const ChoiceEntry& choice) {
  if (!choice.is_empty()) {
    hl7_append_json_field(dst, field_name, hl7_json_value(choice));
  }
}
#endif

// ------- Unified Patient field assignment (JSON + FFHR stream) -------

#if defined(ARM_FASTFHIR)
struct PatientStreamSink {
  FastFHIR::Builder& builder;
  FastFHIR::Reflective::ObjectHandle handle;
};

inline void stream_assign_code_field(PatientStreamSink& dst, FF_FieldKey key, std::string_view code_value) {
  if (code_value.empty()) {
    dst.builder.amend_scalar<uint32_t>(dst.handle.offset(), key.field_offset, FF_CODE_NULL);
    return;
  }

  uint32_t raw_code = FF_GetDictionaryCode(std::string(code_value), dst.builder.FhirVersion());
  if (raw_code == FF_CODE_NULL) {
    const auto string_offset = dst.builder.append(std::string_view(code_value));
    const auto code_slot_offset = dst.handle.offset() + key.field_offset;
    if (string_offset < code_slot_offset) {
      throw std::runtime_error("FastFHIR benchmark assignment: custom CODE string offset is before code slot");
    }
    const auto relative_offset = string_offset - code_slot_offset;
    if (relative_offset > 0x7FFFFFFF) {
      throw std::runtime_error("FastFHIR benchmark assignment: custom CODE relative offset exceeds 31-bit range");
    }
    raw_code = static_cast<uint32_t>(relative_offset) | FF_CUSTOM_STRING_FLAG;
  }
  dst.builder.amend_scalar<uint32_t>(dst.handle.offset(), key.field_offset, raw_code);
}

inline void stream_assign_choice_field(PatientStreamSink& dst, FF_FieldKey key, const ChoiceEntry& choice,
                                       const char* field_name) {
  if (choice.is_empty()) return;

  auto assign_raw_variant = [&](uint64_t raw_bits) {
    dst.builder.amend_variant(dst.handle.offset(), key.field_offset, raw_bits, choice.tag);
  };

  if (std::holds_alternative<bool>(choice.value)) {
    assign_raw_variant(static_cast<uint64_t>(std::get<bool>(choice.value) ? 1 : 0));
  } else if (std::holds_alternative<int32_t>(choice.value)) {
    assign_raw_variant(static_cast<uint64_t>(std::get<int32_t>(choice.value)));
  } else if (std::holds_alternative<uint32_t>(choice.value)) {
    assign_raw_variant(static_cast<uint64_t>(std::get<uint32_t>(choice.value)));
  } else if (std::holds_alternative<int64_t>(choice.value)) {
    assign_raw_variant(static_cast<uint64_t>(std::get<int64_t>(choice.value)));
  } else if (std::holds_alternative<uint64_t>(choice.value)) {
    assign_raw_variant(std::get<uint64_t>(choice.value));
  } else if (std::holds_alternative<double>(choice.value)) {
    const double val = std::get<double>(choice.value);
    uint64_t raw = 0;
    std::memcpy(&raw, &val, sizeof(double));
    assign_raw_variant(raw);
  } else if (std::holds_alternative<std::string_view>(choice.value)) {
    const auto s = std::get<std::string_view>(choice.value);
    const auto off = dst.builder.append(s);
    assign_raw_variant(off);
  } else {
    throw std::runtime_error(std::string("FastFHIR benchmark assignment: unsupported CHOICE variant for ") + field_name);
  }
}

template <typename T>
inline void stream_append_assigned_single(PatientStreamSink& dst, FF_FieldKey key, const T& value) {
  dst.handle[key] = dst.builder.append_obj(value);
}

template <typename T>
inline void stream_assign_array_offsets(PatientStreamSink& dst, FF_FieldKey key,
                                        const std::vector<T>& values) {
  if (values.empty()) return;
  std::vector<Offset> offsets;
  offsets.reserve(values.size());
  for (const auto& value : values) {
    offsets.push_back(dst.builder.append(value));
  }
  dst.handle[key] = offsets;
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_contained(const PatientData& src, Json& dst) {
  if (!src.contained.empty()) {
    dst["contained"] = Json::array();
    for (const auto& c : src.contained) {
      dst["contained"].push_back({{"offset", c.offset}, {"recovery", static_cast<int>(c.recovery)}});
    }
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_contained(const PatientData& src, PatientStreamSink&) {
  if (!src.contained.empty()) {
    throw std::runtime_error("FastFHIR benchmark assignment: Patient.contained remap is not implemented for stream assignment");
  }
}
#elif defined(ARM_HL7V2)
inline void assign_patient_contained(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.contained", src.contained);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_id(const PatientData& src, Json& dst) { put_if_string(dst, "id", src.id); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_id(const PatientData& src, PatientStreamSink& dst) {
  if (!src.id.empty()) dst.handle[FastFHIR::Fields::PATIENT::ID] = src.id;
}
#elif defined(ARM_HL7V2)
inline void assign_patient_id(const PatientData& src, HL7v2Sink& dst) {
  dst.message.pid.patient_id = std::string(src.id);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_implicit_rules(const PatientData& src, Json& dst) {
  put_if_string(dst, "implicitRules", src.implicitrules);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_implicit_rules(const PatientData& src, PatientStreamSink& dst) {
  if (!src.implicitrules.empty()) dst.handle[FastFHIR::Fields::PATIENT::IMPLICIT_RULES] = src.implicitrules;
}
#elif defined(ARM_HL7V2)
inline void assign_patient_implicit_rules(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_string(dst, "patient.implicitRules", src.implicitrules);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_language(const PatientData& src, Json& dst) { put_if_string(dst, "language", src.language); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_language(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_code_field(dst, FastFHIR::Fields::PATIENT::LANGUAGE, src.language);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_language(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_string(dst, "patient.language", src.language);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_active(const PatientData& src, Json& dst) { put_if_bool_flag(dst, "active", src.active); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_active(const PatientData& src, PatientStreamSink& dst) {
  if (src.active != FF_NULL_UINT8) dst.handle[FastFHIR::Fields::PATIENT::ACTIVE] = (src.active != 0);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_active(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_bool(dst, "patient.active", src.active);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_gender(const PatientData& src, Json& dst) {
  put_if_enum(dst, "gender", static_cast<int>(src.gender));
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_gender(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_code_field(dst, FastFHIR::Fields::PATIENT::GENDER, FF_AdministrativeGenderToString(src.gender));
}
#elif defined(ARM_HL7V2)
inline void assign_patient_gender(const PatientData& src, HL7v2Sink& dst) {
  dst.message.pid.administrative_sex = bench::hl7v2::sex_code(src);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_birth_date(const PatientData& src, Json& dst) {
  put_if_string(dst, "birthDate", src.birthdate);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_birth_date(const PatientData& src, PatientStreamSink& dst) {
  if (!src.birthdate.empty()) dst.handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = src.birthdate;
}
#elif defined(ARM_HL7V2)
inline void assign_patient_birth_date(const PatientData& src, HL7v2Sink& dst) {
  dst.message.pid.birth_date = bench::hl7v2::normalize_birthdate(src.birthdate);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_deceased(const PatientData& src, Json& dst) { write_choice(dst, "deceased", src.deceased); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_deceased(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_choice_field(dst, FastFHIR::Fields::PATIENT::DECEASED, src.deceased, "Patient.deceased");
}
#elif defined(ARM_HL7V2)
inline void assign_patient_deceased(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_choice(dst, "patient.deceased[x]", src.deceased);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_multiple_birth(const PatientData& src, Json& dst) {
  write_choice(dst, "multipleBirth", src.multiplebirth);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_multiple_birth(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_choice_field(dst, FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH, src.multiplebirth, "Patient.multipleBirth");
}
#elif defined(ARM_HL7V2)
inline void assign_patient_multiple_birth(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_choice(dst, "patient.multipleBirth[x]", src.multiplebirth);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_meta(const PatientData& src, Json& dst) {
  if (src.meta) dst["meta"] = to_json_meta(*src.meta);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_meta(const PatientData& src, PatientStreamSink& dst) {
  if (src.meta) stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::META, *src.meta);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_meta(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "patient.meta", src.meta);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_text(const PatientData& src, Json& dst) {
  if (src.text) dst["text"] = to_json_narrative(*src.text);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_text(const PatientData& src, PatientStreamSink& dst) {
  if (src.text) stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::TEXT, *src.text);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_text(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "patient.text", src.text);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_extension(const PatientData& src, Json& dst) {
  if (!src.extension.empty()) {
    dst["extension"] = Json::array();
    for (const auto& e : src.extension) dst["extension"].push_back(to_json_extension(e));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_extension(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::EXTENSION, src.extension);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_extension(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.extension", src.extension);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_modifier_extension(const PatientData& src, Json& dst) {
  if (!src.modifierextension.empty()) {
    dst["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) dst["modifierExtension"].push_back(to_json_extension(e));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_modifier_extension(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::MODIFIER_EXTENSION, src.modifierextension);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_modifier_extension(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.modifierExtension", src.modifierextension);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_identifier(const PatientData& src, Json& dst) {
  if (!src.identifier.empty()) {
    dst["identifier"] = Json::array();
    for (const auto& i : src.identifier) dst["identifier"].push_back(to_json_identifier(i));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_identifier(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::IDENTIFIER, src.identifier);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_identifier(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.identifier", src.identifier);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_name(const PatientData& src, Json& dst) {
  if (!src.name.empty()) {
    dst["name"] = Json::array();
    for (const auto& n : src.name) dst["name"].push_back(to_json_human_name(n));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_name(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::NAME, src.name);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_name(const PatientData& src, HL7v2Sink& dst) {
  dst.message.pid.patient_name = bench::hl7v2::hl7_name_xpn(src);
  if (!src.name.empty()) {
    const auto& name = src.name.front();
    if (!name.id.empty() || !name.extension.empty() || static_cast<int>(name.use) != 0 ||
        !name.text.empty() || name.given.size() > 1 || !name.prefix.empty() ||
        !name.suffix.empty() || name.period) {
      hl7_append_json_field(dst, "patient.name[0].details", hl7_json_value(name));
    }
  }
  if (src.name.size() > 1) {
    hl7_append_json_field(dst, "patient.name[*]", hl7_json_array(src.name));
  }
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_telecom(const PatientData& src, Json& dst) {
  if (!src.telecom.empty()) {
    dst["telecom"] = Json::array();
    for (const auto& t : src.telecom) dst["telecom"].push_back(to_json_contact_point(t));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_telecom(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::TELECOM, src.telecom);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_telecom(const PatientData& src, HL7v2Sink& dst) {
  dst.message.pid.home_phone = bench::hl7v2::hl7_phone_xtn(src);
  for (const auto& telecom : src.telecom) {
    if (!telecom.id.empty() || !telecom.extension.empty() ||
        static_cast<int>(telecom.system) != 0 || static_cast<int>(telecom.use) != 0 ||
        has_u32(telecom.rank) || telecom.period) {
      hl7_append_json_field(dst, "patient.telecom.details", hl7_json_array(src.telecom));
      break;
    }
  }
  if (src.telecom.size() > 1) {
    hl7_append_json_field(dst, "patient.telecom[*]", hl7_json_array(src.telecom));
  }
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_address(const PatientData& src, Json& dst) {
  if (!src.address.empty()) {
    dst["address"] = Json::array();
    for (const auto& a : src.address) dst["address"].push_back(to_json_address(a));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_address(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::ADDRESS, src.address);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_address(const PatientData& src, HL7v2Sink& dst) {
  dst.message.pid.patient_address = bench::hl7v2::hl7_address_xad(src);
  if (!src.address.empty()) {
    const auto& address = src.address.front();
    if (!address.id.empty() || !address.extension.empty() || static_cast<int>(address.use) != 0 ||
        static_cast<int>(address.type) != 0 || !address.text.empty() || address.line.size() > 1 ||
        !address.district.empty() || address.period) {
      hl7_append_json_field(dst, "patient.address[0].details", hl7_json_value(address));
    }
  }
  if (src.address.size() > 1) {
    hl7_append_json_field(dst, "patient.address[*]", hl7_json_array(src.address));
  }
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_marital_status(const PatientData& src, Json& dst) {
  if (src.maritalstatus) dst["maritalStatus"] = to_json_codeable_concept(*src.maritalstatus);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_marital_status(const PatientData& src, PatientStreamSink& dst) {
  if (src.maritalstatus) {
    stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::MARITAL_STATUS, *src.maritalstatus);
  }
}
#elif defined(ARM_HL7V2)
inline void assign_patient_marital_status(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "patient.maritalStatus", src.maritalstatus);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_photo(const PatientData& src, Json& dst) {
  if (!src.photo.empty()) {
    dst["photo"] = Json::array();
    for (const auto& p : src.photo) dst["photo"].push_back(to_json_attachment(p));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_photo(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::PHOTO, src.photo);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_photo(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.photo", src.photo);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_contact(const PatientData& src, Json& dst) {
  if (!src.contact.empty()) {
    dst["contact"] = Json::array();
    for (const auto& c : src.contact) dst["contact"].push_back(to_json_patient_contact(c));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_contact(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::CONTACT, src.contact);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_contact(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.contact", src.contact);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_communication(const PatientData& src, Json& dst) {
  if (!src.communication.empty()) {
    dst["communication"] = Json::array();
    for (const auto& c : src.communication) dst["communication"].push_back(to_json_patient_communication(c));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_communication(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::COMMUNICATION, src.communication);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_communication(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.communication", src.communication);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_general_practitioner(const PatientData& src, Json& dst) {
  if (!src.generalpractitioner.empty()) {
    dst["generalPractitioner"] = Json::array();
    for (const auto& gp : src.generalpractitioner) dst["generalPractitioner"].push_back(to_json_reference(gp));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_general_practitioner(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::GENERAL_PRACTITIONER,
                              src.generalpractitioner);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_general_practitioner(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.generalPractitioner", src.generalpractitioner);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_managing_organization(const PatientData& src, Json& dst) {
  if (src.managingorganization) dst["managingOrganization"] = to_json_reference(*src.managingorganization);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_managing_organization(const PatientData& src, PatientStreamSink& dst) {
  if (src.managingorganization) {
    stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::MANAGING_ORGANIZATION,
                                  *src.managingorganization);
  }
}
#elif defined(ARM_HL7V2)
inline void assign_patient_managing_organization(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "patient.managingOrganization", src.managingorganization);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_link(const PatientData& src, Json& dst) {
  if (!src.link.empty()) {
    dst["link"] = Json::array();
    for (const auto& l : src.link) dst["link"].push_back(to_json_patient_link(l));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_link(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::LINK, src.link);
}
#elif defined(ARM_HL7V2)
inline void assign_patient_link(const PatientData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "patient.link", src.link);
}
#endif

// ------- Observation field assignment (JSON + FFHR stream) -------

#if defined(ARM_JSON)
inline void assign_observation_contained(const ObservationData& src, Json& dst) {
  if (!src.contained.empty()) {
    dst["contained"] = Json::array();
    for (const auto& c : src.contained) {
      dst["contained"].push_back({{"offset", c.offset}, {"recovery", static_cast<int>(c.recovery)}});
    }
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_contained(const ObservationData& src, PatientStreamSink&) {
  if (!src.contained.empty()) {
    throw std::runtime_error("FastFHIR benchmark assignment: Observation.contained remap is not implemented for stream assignment");
  }
}
#elif defined(ARM_HL7V2)
inline void assign_observation_contained(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.contained", src.contained);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_id(const ObservationData& src, Json& dst) { put_if_string(dst, "id", src.id); }
#elif defined(ARM_FASTFHIR)
inline void assign_observation_id(const ObservationData& src, PatientStreamSink& dst) {
  if (!src.id.empty()) dst.handle[FastFHIR::Fields::OBSERVATION::ID] = src.id;
}
#elif defined(ARM_HL7V2)
inline void assign_observation_id(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_string(dst, "observation.id", src.id);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_meta(const ObservationData& src, Json& dst) {
  if (src.meta) dst["meta"] = to_json_meta(*src.meta);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_meta(const ObservationData& src, PatientStreamSink& dst) {
  if (src.meta) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::META, *src.meta);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_meta(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.meta", src.meta);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_implicit_rules(const ObservationData& src, Json& dst) {
  put_if_string(dst, "implicitRules", src.implicitrules);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_implicit_rules(const ObservationData& src, PatientStreamSink& dst) {
  if (!src.implicitrules.empty()) dst.handle[FastFHIR::Fields::OBSERVATION::IMPLICIT_RULES] = src.implicitrules;
}
#elif defined(ARM_HL7V2)
inline void assign_observation_implicit_rules(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_string(dst, "observation.implicitRules", src.implicitrules);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_language(const ObservationData& src, Json& dst) { put_if_string(dst, "language", src.language); }
#elif defined(ARM_FASTFHIR)
inline void assign_observation_language(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_code_field(dst, FastFHIR::Fields::OBSERVATION::LANGUAGE, src.language);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_language(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_string(dst, "observation.language", src.language);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_text(const ObservationData& src, Json& dst) {
  if (src.text) dst["text"] = to_json_narrative(*src.text);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_text(const ObservationData& src, PatientStreamSink& dst) {
  if (src.text) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::TEXT, *src.text);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_text(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.text", src.text);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_extension(const ObservationData& src, Json& dst) {
  if (!src.extension.empty()) {
    dst["extension"] = Json::array();
    for (const auto& e : src.extension) dst["extension"].push_back(to_json_extension(e));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_extension(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::EXTENSION, src.extension);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_extension(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.extension", src.extension);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_modifier_extension(const ObservationData& src, Json& dst) {
  if (!src.modifierextension.empty()) {
    dst["modifierExtension"] = Json::array();
    for (const auto& e : src.modifierextension) dst["modifierExtension"].push_back(to_json_extension(e));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_modifier_extension(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::MODIFIER_EXTENSION, src.modifierextension);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_modifier_extension(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.modifierExtension", src.modifierextension);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_identifier(const ObservationData& src, Json& dst) {
  if (!src.identifier.empty()) {
    dst["identifier"] = Json::array();
    for (const auto& v : src.identifier) dst["identifier"].push_back(to_json_identifier(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_identifier(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::IDENTIFIER, src.identifier);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_identifier(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.identifier", src.identifier);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_based_on(const ObservationData& src, Json& dst) {
  if (!src.basedon.empty()) {
    dst["basedOn"] = Json::array();
    for (const auto& v : src.basedon) dst["basedOn"].push_back(to_json_reference(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_based_on(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::BASED_ON, src.basedon);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_based_on(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.basedOn", src.basedon);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_part_of(const ObservationData& src, Json& dst) {
  if (!src.partof.empty()) {
    dst["partOf"] = Json::array();
    for (const auto& v : src.partof) dst["partOf"].push_back(to_json_reference(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_part_of(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::PART_OF, src.partof);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_part_of(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.partOf", src.partof);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_status(const ObservationData& src, Json& dst) {
  put_if_string(dst, "status", FF_ObservationStatusToString(src.status));
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_status(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_code_field(dst, FastFHIR::Fields::OBSERVATION::STATUS, FF_ObservationStatusToString(src.status));
}
#elif defined(ARM_HL7V2)
inline void assign_observation_status(const ObservationData& src, HL7v2Sink& dst) {
  if (static_cast<int>(src.status) != 0) {
    hl7_append_json_field(dst, "observation.status", hl7_json_value(static_cast<int>(src.status)));
  }
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_category(const ObservationData& src, Json& dst) {
  if (!src.category.empty()) {
    dst["category"] = Json::array();
    for (const auto& v : src.category) dst["category"].push_back(to_json_codeable_concept(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_category(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::CATEGORY, src.category);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_category(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.category", src.category);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_code(const ObservationData& src, Json& dst) {
  if (src.code) dst["code"] = to_json_codeable_concept(*src.code);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_code(const ObservationData& src, PatientStreamSink& dst) {
  if (src.code) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::CODE, *src.code);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_code(const ObservationData& src, HL7v2Sink& dst) {
  dst.current_obx.observation_id = bench::hl7v2::observation_code_id(src);
  if (src.code) {
    if (!src.code->id.empty() || !src.code->extension.empty() || !src.code->text.empty() ||
        src.code->coding.size() > 1) {
      hl7_append_json_field(dst, "observation.code.details", hl7_json_value(*src.code));
    }
    if (!src.code->coding.empty()) {
      const auto& coding = src.code->coding.front();
      if (!coding.id.empty() || !coding.extension.empty() || !coding.version.empty() ||
          has_u8(coding.userselected)) {
        hl7_append_json_field(dst, "observation.code.coding[0].details", hl7_json_value(coding));
      }
    }
  }
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_subject(const ObservationData& src, Json& dst) {
  if (src.subject) dst["subject"] = to_json_reference(*src.subject);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_subject(const ObservationData& src, PatientStreamSink& dst) {
  if (src.subject) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::SUBJECT, *src.subject);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_subject(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.subject", src.subject);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_focus(const ObservationData& src, Json& dst) {
  if (!src.focus.empty()) {
    dst["focus"] = Json::array();
    for (const auto& v : src.focus) dst["focus"].push_back(to_json_reference(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_focus(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::FOCUS, src.focus);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_focus(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.focus", src.focus);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_encounter(const ObservationData& src, Json& dst) {
  if (src.encounter) dst["encounter"] = to_json_reference(*src.encounter);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_encounter(const ObservationData& src, PatientStreamSink& dst) {
  if (src.encounter) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::ENCOUNTER, *src.encounter);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_encounter(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.encounter", src.encounter);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_effective(const ObservationData& src, Json& dst) { write_choice(dst, "effective", src.effective); }
#elif defined(ARM_FASTFHIR)
inline void assign_observation_effective(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_choice_field(dst, FastFHIR::Fields::OBSERVATION::EFFECTIVE, src.effective, "Observation.effective");
}
#elif defined(ARM_HL7V2)
inline void assign_observation_effective(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_choice(dst, "observation.effective[x]", src.effective);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_issued(const ObservationData& src, Json& dst) { put_if_string(dst, "issued", src.issued); }
#elif defined(ARM_FASTFHIR)
inline void assign_observation_issued(const ObservationData& src, PatientStreamSink& dst) {
  if (!src.issued.empty()) dst.handle[FastFHIR::Fields::OBSERVATION::ISSUED] = src.issued;
}
#elif defined(ARM_HL7V2)
inline void assign_observation_issued(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_string(dst, "observation.issued", src.issued);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_performer(const ObservationData& src, Json& dst) {
  if (!src.performer.empty()) {
    dst["performer"] = Json::array();
    for (const auto& v : src.performer) dst["performer"].push_back(to_json_reference(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_performer(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::PERFORMER, src.performer);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_performer(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.performer", src.performer);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_value(const ObservationData& src, Json& dst) { write_choice(dst, "value", src.value); }
#elif defined(ARM_FASTFHIR)
inline void assign_observation_value(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_choice_field(dst, FastFHIR::Fields::OBSERVATION::VALUE, src.value, "Observation.value");
}
#elif defined(ARM_HL7V2)
inline void assign_observation_value(const ObservationData& src, HL7v2Sink& dst) {
  switch (src.value.tag) {
    case RECOVER_FF_QUANTITY:
      dst.current_obx.value_type = "NM";
      dst.current_obx.value = "1";
      dst.current_obx.units = "{qty}";
      break;
    case RECOVER_FF_CODEABLECONCEPT:
      dst.current_obx.value_type = "CE";
      dst.current_obx.value = "1";
      dst.current_obx.units.clear();
      break;
    case RECOVER_FF_CODE:
      dst.current_obx.value_type = "CWE";
      dst.current_obx.value = "1";
      dst.current_obx.units.clear();
      break;
    case RECOVER_FF_STRING:
      dst.current_obx.value_type = "ST";
      dst.current_obx.value = "1";
      dst.current_obx.units.clear();
      break;
    default:
      dst.current_obx.value_type = "ST";
      dst.current_obx.value = src.value.is_empty() ? "" : "1";
      dst.current_obx.units.clear();
      break;
  }
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_data_absent_reason(const ObservationData& src, Json& dst) {
  if (src.dataabsentreason) dst["dataAbsentReason"] = to_json_codeable_concept(*src.dataabsentreason);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_data_absent_reason(const ObservationData& src, PatientStreamSink& dst) {
  if (src.dataabsentreason) {
    stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::DATA_ABSENT_REASON, *src.dataabsentreason);
  }
}
#elif defined(ARM_HL7V2)
inline void assign_observation_data_absent_reason(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.dataAbsentReason", src.dataabsentreason);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_interpretation(const ObservationData& src, Json& dst) {
  if (!src.interpretation.empty()) {
    dst["interpretation"] = Json::array();
    for (const auto& v : src.interpretation) dst["interpretation"].push_back(to_json_codeable_concept(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_interpretation(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::INTERPRETATION, src.interpretation);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_interpretation(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.interpretation", src.interpretation);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_note(const ObservationData& src, Json& dst) {
  if (!src.note.empty()) {
    dst["note"] = Json::array();
    for (const auto& v : src.note) dst["note"].push_back(to_json_annotation(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_note(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::NOTE, src.note);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_note(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.note", src.note);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_body_site(const ObservationData& src, Json& dst) {
  if (src.bodysite) dst["bodySite"] = to_json_codeable_concept(*src.bodysite);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_body_site(const ObservationData& src, PatientStreamSink& dst) {
  if (src.bodysite) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::BODY_SITE, *src.bodysite);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_body_site(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.bodySite", src.bodysite);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_method(const ObservationData& src, Json& dst) {
  if (src.method) dst["method"] = to_json_codeable_concept(*src.method);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_method(const ObservationData& src, PatientStreamSink& dst) {
  if (src.method) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::METHOD, *src.method);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_method(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.method", src.method);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_specimen(const ObservationData& src, Json& dst) {
  if (src.specimen) dst["specimen"] = to_json_reference(*src.specimen);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_specimen(const ObservationData& src, PatientStreamSink& dst) {
  if (src.specimen) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::SPECIMEN, *src.specimen);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_specimen(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.specimen", src.specimen);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_device(const ObservationData& src, Json& dst) {
  if (src.device) dst["device"] = to_json_reference(*src.device);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_device(const ObservationData& src, PatientStreamSink& dst) {
  if (src.device) stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::DEVICE, *src.device);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_device(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.device", src.device);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_reference_range(const ObservationData& src, Json& dst) {
  if (!src.referencerange.empty()) {
    dst["referenceRange"] = Json::array();
    for (const auto& v : src.referencerange) dst["referenceRange"].push_back(to_json_observation_reference_range(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_reference_range(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::REFERENCE_RANGE, src.referencerange);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_reference_range(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.referenceRange", src.referencerange);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_has_member(const ObservationData& src, Json& dst) {
  if (!src.hasmember.empty()) {
    dst["hasMember"] = Json::array();
    for (const auto& v : src.hasmember) dst["hasMember"].push_back(to_json_reference(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_has_member(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::HAS_MEMBER, src.hasmember);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_has_member(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.hasMember", src.hasmember);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_derived_from(const ObservationData& src, Json& dst) {
  if (!src.derivedfrom.empty()) {
    dst["derivedFrom"] = Json::array();
    for (const auto& v : src.derivedfrom) dst["derivedFrom"].push_back(to_json_reference(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_derived_from(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::DERIVED_FROM, src.derivedfrom);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_derived_from(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.derivedFrom", src.derivedfrom);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_component(const ObservationData& src, Json& dst) {
  if (!src.component.empty()) {
    dst["component"] = Json::array();
    for (const auto& v : src.component) dst["component"].push_back(to_json_observation_component(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_component(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::COMPONENT, src.component);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_component(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.component", src.component);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_instantiates(const ObservationData& src, Json& dst) {
  write_choice(dst, "instantiates", src.instantiates);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_instantiates(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_choice_field(dst, FastFHIR::Fields::OBSERVATION::INSTANTIATES, src.instantiates, "Observation.instantiates");
}
#elif defined(ARM_HL7V2)
inline void assign_observation_instantiates(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_choice(dst, "observation.instantiates[x]", src.instantiates);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_triggered_by(const ObservationData& src, Json& dst) {
  if (!src.triggeredby.empty()) {
    dst["triggeredBy"] = Json::array();
    for (const auto& v : src.triggeredby) dst["triggeredBy"].push_back(to_json_observation_triggered_by(v));
  }
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_triggered_by(const ObservationData& src, PatientStreamSink& dst) {
  stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::TRIGGERED_BY, src.triggeredby);
}
#elif defined(ARM_HL7V2)
inline void assign_observation_triggered_by(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_vector(dst, "observation.triggeredBy", src.triggeredby);
}
#endif

#if defined(ARM_JSON)
inline void assign_observation_body_structure(const ObservationData& src, Json& dst) {
  if (src.bodystructure) dst["bodyStructure"] = to_json_reference(*src.bodystructure);
}
#elif defined(ARM_FASTFHIR)
inline void assign_observation_body_structure(const ObservationData& src, PatientStreamSink& dst) {
  if (src.bodystructure) {
    stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::BODY_STRUCTURE, *src.bodystructure);
  }
}
#elif defined(ARM_HL7V2)
inline void assign_observation_body_structure(const ObservationData& src, HL7v2Sink& dst) {
  hl7_mark_if_pointer(dst, "observation.bodyStructure", src.bodystructure);
}
#endif

template <typename Sink>
inline void assign_observation_common(const ObservationData& src, Sink& dst) {
  assign_observation_contained(src, dst);
  assign_observation_id(src, dst);
  assign_observation_meta(src, dst);
  assign_observation_implicit_rules(src, dst);
  assign_observation_language(src, dst);
  assign_observation_text(src, dst);
  assign_observation_extension(src, dst);
  assign_observation_modifier_extension(src, dst);
  assign_observation_identifier(src, dst);
  assign_observation_based_on(src, dst);
  assign_observation_part_of(src, dst);
  assign_observation_status(src, dst);
  assign_observation_category(src, dst);
  assign_observation_code(src, dst);
  assign_observation_subject(src, dst);
  assign_observation_focus(src, dst);
  assign_observation_encounter(src, dst);
  assign_observation_effective(src, dst);
  assign_observation_issued(src, dst);
  assign_observation_performer(src, dst);
  assign_observation_value(src, dst);
  assign_observation_data_absent_reason(src, dst);
  assign_observation_interpretation(src, dst);
  assign_observation_note(src, dst);
  assign_observation_body_site(src, dst);
  assign_observation_method(src, dst);
  assign_observation_specimen(src, dst);
  assign_observation_device(src, dst);
  assign_observation_reference_range(src, dst);
  assign_observation_has_member(src, dst);
  assign_observation_derived_from(src, dst);
  assign_observation_component(src, dst);
  assign_observation_instantiates(src, dst);
  assign_observation_triggered_by(src, dst);
  assign_observation_body_structure(src, dst);
}

template <typename Dst>
inline void assign_patient_contained_t(const PatientData& src, Dst& dst) { assign_patient_contained(src, dst); }
template <typename Dst>
inline void assign_patient_id_t(const PatientData& src, Dst& dst) { assign_patient_id(src, dst); }
template <typename Dst>
inline void assign_patient_implicit_rules_t(const PatientData& src, Dst& dst) { assign_patient_implicit_rules(src, dst); }
template <typename Dst>
inline void assign_patient_language_t(const PatientData& src, Dst& dst) { assign_patient_language(src, dst); }
template <typename Dst>
inline void assign_patient_active_t(const PatientData& src, Dst& dst) { assign_patient_active(src, dst); }
template <typename Dst>
inline void assign_patient_gender_t(const PatientData& src, Dst& dst) { assign_patient_gender(src, dst); }
template <typename Dst>
inline void assign_patient_birth_date_t(const PatientData& src, Dst& dst) { assign_patient_birth_date(src, dst); }
template <typename Dst>
inline void assign_patient_deceased_t(const PatientData& src, Dst& dst) { assign_patient_deceased(src, dst); }
template <typename Dst>
inline void assign_patient_multiple_birth_t(const PatientData& src, Dst& dst) { assign_patient_multiple_birth(src, dst); }
template <typename Dst>
inline void assign_patient_meta_t(const PatientData& src, Dst& dst) { assign_patient_meta(src, dst); }
template <typename Dst>
inline void assign_patient_text_t(const PatientData& src, Dst& dst) { assign_patient_text(src, dst); }
template <typename Dst>
inline void assign_patient_extension_t(const PatientData& src, Dst& dst) { assign_patient_extension(src, dst); }
template <typename Dst>
inline void assign_patient_modifier_extension_t(const PatientData& src, Dst& dst) { assign_patient_modifier_extension(src, dst); }
template <typename Dst>
inline void assign_patient_identifier_t(const PatientData& src, Dst& dst) { assign_patient_identifier(src, dst); }
template <typename Dst>
inline void assign_patient_name_t(const PatientData& src, Dst& dst) { assign_patient_name(src, dst); }
template <typename Dst>
inline void assign_patient_telecom_t(const PatientData& src, Dst& dst) { assign_patient_telecom(src, dst); }
template <typename Dst>
inline void assign_patient_address_t(const PatientData& src, Dst& dst) { assign_patient_address(src, dst); }
template <typename Dst>
inline void assign_patient_marital_status_t(const PatientData& src, Dst& dst) { assign_patient_marital_status(src, dst); }
template <typename Dst>
inline void assign_patient_photo_t(const PatientData& src, Dst& dst) { assign_patient_photo(src, dst); }
template <typename Dst>
inline void assign_patient_contact_t(const PatientData& src, Dst& dst) { assign_patient_contact(src, dst); }
template <typename Dst>
inline void assign_patient_communication_t(const PatientData& src, Dst& dst) { assign_patient_communication(src, dst); }
template <typename Dst>
inline void assign_patient_general_practitioner_t(const PatientData& src, Dst& dst) { assign_patient_general_practitioner(src, dst); }
template <typename Dst>
inline void assign_patient_managing_organization_t(const PatientData& src, Dst& dst) { assign_patient_managing_organization(src, dst); }
template <typename Dst>
inline void assign_patient_link_t(const PatientData& src, Dst& dst) { assign_patient_link(src, dst); }

template <typename Sink>
inline void assign_patient_common(const PatientData& src, Sink& dst) {
  assign_patient_contained_t(src, dst);
  assign_patient_id_t(src, dst);
  assign_patient_implicit_rules_t(src, dst);
  assign_patient_language_t(src, dst);
  assign_patient_active_t(src, dst);
  assign_patient_gender_t(src, dst);
  assign_patient_birth_date_t(src, dst);
  assign_patient_deceased_t(src, dst);
  assign_patient_multiple_birth_t(src, dst);
  assign_patient_meta_t(src, dst);
  assign_patient_text_t(src, dst);
  assign_patient_extension_t(src, dst);
  assign_patient_modifier_extension_t(src, dst);
  assign_patient_identifier_t(src, dst);
  assign_patient_name_t(src, dst);
  assign_patient_telecom_t(src, dst);
  assign_patient_address_t(src, dst);
  assign_patient_marital_status_t(src, dst);
  assign_patient_photo_t(src, dst);
  assign_patient_contact_t(src, dst);
  assign_patient_communication_t(src, dst);
  assign_patient_general_practitioner_t(src, dst);
  assign_patient_managing_organization_t(src, dst);
  assign_patient_link_t(src, dst);
}

}  // namespace detail

// ------- Resource assignment entrypoints -------

template <typename Target>
inline void assign_patient(const PatientData& src, Target& dst) {
#if defined(ARM_JSON)
  dst = detail::Json::object();
  dst["resourceType"] = "Patient";
  detail::assign_patient_common(src, dst);
#elif defined(ARM_FASTFHIR)
  auto* builder = dst.get_builder();
  if (builder == nullptr) {
    throw std::runtime_error("assign_patient: ObjectHandle has null builder");
  }
  detail::PatientStreamSink sink{*builder, dst};
  detail::assign_patient_common(src, sink);
#elif defined(ARM_HL7V2)
  detail::HL7v2Sink sink{dst};
  detail::assign_patient_common(src, sink);
#else
  static_assert(sizeof(Target) == 0, "Unsupported benchmark assignment arm");
#endif
}

template <typename Target>
inline void assign_observation(const ObservationData& src, Target& dst) {
#if defined(ARM_JSON)
  dst = detail::Json::object();
  dst["resourceType"] = "Observation";
  detail::assign_observation_common(src, dst);
#elif defined(ARM_FASTFHIR)
  auto* builder = dst.get_builder();
  if (builder == nullptr) {
    throw std::runtime_error("assign_observation: ObjectHandle has null builder");
  }
  detail::PatientStreamSink sink{*builder, dst};
  detail::assign_observation_common(src, sink);
#elif defined(ARM_HL7V2)
  detail::HL7v2Sink sink{dst};
  sink.begin_observation();
  detail::assign_observation_common(src, sink);
  sink.finish_observation();
#else
  static_assert(sizeof(Target) == 0, "Unsupported benchmark assignment arm");
#endif
}

#if defined(ARM_FASTFHIR)
inline FastFHIR::Reflective::ObjectHandle append_patient_stream(FastFHIR::Builder& builder,
                                                                const PatientData& src) {
  auto handle = builder.append_obj(PatientData{});
  assign_patient(src, handle);
  return handle;
}

inline FastFHIR::Reflective::ObjectHandle append_observation_stream(FastFHIR::Builder& builder,
                                                                    const ObservationData& src) {
  auto handle = builder.append_obj(ObservationData{});
  assign_observation(src, handle);
  return handle;
}
#endif

}  // namespace bench::assign
