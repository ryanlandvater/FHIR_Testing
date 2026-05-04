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
#endif

#if defined(ARM_JSON)
inline void assign_patient_id(const PatientData& src, Json& dst) { put_if_string(dst, "id", src.id); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_id(const PatientData& src, PatientStreamSink& dst) {
  if (!src.id.empty()) dst.handle[FastFHIR::Fields::PATIENT::ID] = src.id;
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
#endif

#if defined(ARM_JSON)
inline void assign_patient_language(const PatientData& src, Json& dst) { put_if_string(dst, "language", src.language); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_language(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_code_field(dst, FastFHIR::Fields::PATIENT::LANGUAGE, src.language);
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_active(const PatientData& src, Json& dst) { put_if_bool_flag(dst, "active", src.active); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_active(const PatientData& src, PatientStreamSink& dst) {
  if (src.active != FF_NULL_UINT8) dst.handle[FastFHIR::Fields::PATIENT::ACTIVE] = (src.active != 0);
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
#endif

#if defined(ARM_JSON)
inline void assign_patient_birth_date(const PatientData& src, Json& dst) {
  put_if_string(dst, "birthDate", src.birthdate);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_birth_date(const PatientData& src, PatientStreamSink& dst) {
  if (!src.birthdate.empty()) dst.handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = src.birthdate;
}
#endif

#if defined(ARM_JSON)
inline void assign_patient_deceased(const PatientData& src, Json& dst) { write_choice(dst, "deceased", src.deceased); }
#elif defined(ARM_FASTFHIR)
inline void assign_patient_deceased(const PatientData& src, PatientStreamSink& dst) {
  stream_assign_choice_field(dst, FastFHIR::Fields::PATIENT::DECEASED, src.deceased, "Patient.deceased");
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
#endif

#if defined(ARM_JSON)
inline void assign_patient_meta(const PatientData& src, Json& dst) {
  if (src.meta) dst["meta"] = to_json_meta(*src.meta);
}
#elif defined(ARM_FASTFHIR)
inline void assign_patient_meta(const PatientData& src, PatientStreamSink& dst) {
  if (src.meta) stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::META, *src.meta);
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
#endif

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
#endif

}  // namespace bench::assign
