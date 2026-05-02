#include "harness.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace bench {

namespace {

using Json = nlohmann::json;

void put_string_if(Json& out, const char* key, std::string_view value) {
  if (!value.empty()) {
    out[key] = value;
  }
}

void put_bool_if(Json& out, const char* key, uint8_t value) {
  if (value != FF_NULL_UINT8) {
    out[key] = (value != 0);
  }
}

void put_u32_if(Json& out, const char* key, uint32_t value) {
  if (value != FF_NULL_UINT32) {
    out[key] = value;
  }
}

void put_f64_if(Json& out, const char* key, double value) {
  if (value != FF_NULL_F64) {
    out[key] = value;
  }
}

void put_enum_if(Json& out, const char* key, const char* value) {
  if (value != nullptr && value[0] != '\0') {
    out[key] = value;
  }
}

void put_string_array_if(Json& out, const char* key, const std::vector<std::string_view>& values) {
  if (values.empty()) {
    return;
  }

  Json arr = Json::array();
  for (const auto& value : values) {
    if (!value.empty()) {
      arr.push_back(value);
    }
  }
  if (!arr.empty()) {
    out[key] = std::move(arr);
  }
}

template <typename T, typename Converter>
void put_object_array_if(Json& out, const char* key, const std::vector<T>& values, Converter&& convert) {
  if (values.empty()) {
    return;
  }

  Json arr = Json::array();
  for (const auto& value : values) {
    arr.push_back(convert(value));
  }
  if (!arr.empty()) {
    out[key] = std::move(arr);
  }
}

template <typename T, typename Converter>
void put_object_if(Json& out, const char* key, const std::unique_ptr<T>& value, Converter&& convert) {
  if (value) {
    out[key] = convert(*value);
  }
}

void write_choice_if(Json& out, const std::string_view base_name, const ChoiceEntry& choice) {
  if (choice.is_empty()) {
    return;
  }

  const auto key_for = [&](const char* suffix) {
    return std::string(base_name) + suffix;
  };

  switch (choice.tag) {
    case RECOVER_FF_BOOL: {
      if (const auto* value = std::get_if<bool>(&choice.value)) {
        out[key_for("Boolean")] = *value;
      }
      break;
    }
    case RECOVER_FF_INT32: {
      if (const auto* value = std::get_if<int32_t>(&choice.value)) {
        out[key_for("Integer")] = *value;
      }
      break;
    }
    case RECOVER_FF_UINT32: {
      if (const auto* value = std::get_if<uint32_t>(&choice.value)) {
        out[key_for("Integer")] = *value;
      }
      break;
    }
    case RECOVER_FF_INT64: {
      if (const auto* value = std::get_if<int64_t>(&choice.value)) {
        out[key_for("Integer")] = *value;
      }
      break;
    }
    case RECOVER_FF_UINT64: {
      if (const auto* value = std::get_if<uint64_t>(&choice.value)) {
        out[key_for("Integer")] = *value;
      }
      break;
    }
    case RECOVER_FF_FLOAT64: {
      if (const auto* value = std::get_if<double>(&choice.value)) {
        out[key_for("Decimal")] = *value;
      }
      break;
    }
    case RECOVER_FF_DATE:
    case RECOVER_FF_DATETIME:
    case RECOVER_FF_TIME:
    case RECOVER_FF_INSTANT:
    case RECOVER_FF_STRING:
    case RECOVER_FF_CODE: {
      if (const auto* value = std::get_if<std::string_view>(&choice.value)) {
        if (value->empty()) {
          break;
        }

        if (choice.tag == RECOVER_FF_DATE) {
          out[key_for("Date")] = *value;
        } else if (choice.tag == RECOVER_FF_DATETIME) {
          out[key_for("DateTime")] = *value;
        } else if (choice.tag == RECOVER_FF_TIME) {
          out[key_for("Time")] = *value;
        } else if (choice.tag == RECOVER_FF_INSTANT) {
          out[key_for("Instant")] = *value;
        } else if (choice.tag == RECOVER_FF_CODE) {
          out[key_for("Code")] = *value;
        } else {
          out[key_for("String")] = *value;
        }
      }
      break;
    }
    default:
      break;
  }
}

Json to_json_extension(const ExtensionData& data);

Json to_json_coding(const CodingData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_string_if(out, "system", data.system);
  put_string_if(out, "version", data.version);
  put_string_if(out, "code", data.code);
  put_string_if(out, "display", data.display);
  put_bool_if(out, "userSelected", data.userselected);
  return out;
}

Json to_json_codeable_concept(const CodeableConceptData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "coding", data.coding, to_json_coding);
  put_string_if(out, "text", data.text);
  return out;
}

Json to_json_period(const PeriodData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_string_if(out, "start", data.start);
  put_string_if(out, "end", data.end);
  return out;
}

Json to_json_quantity(const QuantityData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_f64_if(out, "value", data.value);
  put_enum_if(out, "comparator", FF_QuantityComparatorToString(data.comparator));
  put_string_if(out, "unit", data.unit);
  put_string_if(out, "system", data.system);
  put_string_if(out, "code", data.code);
  return out;
}

Json to_json_reference(const ReferenceData& data);

Json to_json_identifier(const IdentifierData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_enum_if(out, "use", FF_IdentifierUseToString(data.use));
  put_object_if(out, "type", data.type, to_json_codeable_concept);
  put_string_if(out, "system", data.system);
  put_string_if(out, "value", data.value);
  put_object_if(out, "period", data.period, to_json_period);
  put_object_if(out, "assigner", data.assigner, to_json_reference);
  return out;
}

Json to_json_reference(const ReferenceData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_string_if(out, "reference", data.reference);
  put_string_if(out, "type", data.type);
  put_object_if(out, "identifier", data.identifier, to_json_identifier);
  put_string_if(out, "display", data.display);
  return out;
}

Json to_json_meta(const MetaData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_string_if(out, "versionId", data.versionid);
  put_string_if(out, "lastUpdated", data.lastupdated);
  put_string_if(out, "source", data.source);
  put_string_array_if(out, "profile", data.profile);
  put_object_array_if(out, "security", data.security, to_json_coding);
  put_object_array_if(out, "tag", data.tag, to_json_coding);
  return out;
}

Json to_json_narrative(const NarrativeData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_enum_if(out, "status", FF_NarrativeStatusToString(data.status));
  put_string_if(out, "div", data.div);
  return out;
}

Json to_json_human_name(const HumanNameData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_enum_if(out, "use", FF_NameUseToString(data.use));
  put_string_if(out, "text", data.text);
  put_string_if(out, "family", data.family);
  put_string_array_if(out, "given", data.given);
  put_string_array_if(out, "prefix", data.prefix);
  put_string_array_if(out, "suffix", data.suffix);
  put_object_if(out, "period", data.period, to_json_period);
  return out;
}

Json to_json_address(const AddressData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_enum_if(out, "use", FF_AddressUseToString(data.use));
  put_enum_if(out, "type", FF_AddressTypeToString(data.type));
  put_string_if(out, "text", data.text);
  put_string_array_if(out, "line", data.line);
  put_string_if(out, "city", data.city);
  put_string_if(out, "district", data.district);
  put_string_if(out, "state", data.state);
  put_string_if(out, "postalCode", data.postalcode);
  put_string_if(out, "country", data.country);
  put_object_if(out, "period", data.period, to_json_period);
  return out;
}

Json to_json_contact_point(const ContactPointData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_enum_if(out, "system", FF_ContactPointSystemToString(data.system));
  put_string_if(out, "value", data.value);
  put_enum_if(out, "use", FF_ContactPointUseToString(data.use));
  put_u32_if(out, "rank", data.rank);
  put_object_if(out, "period", data.period, to_json_period);
  return out;
}

Json to_json_attachment(const AttachmentData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_string_if(out, "contentType", data.contenttype);
  put_string_if(out, "language", data.language);
  put_string_if(out, "data", data.data);
  put_string_if(out, "url", data.url);
  put_u32_if(out, "size", data.size);
  put_string_if(out, "hash", data.hash);
  put_string_if(out, "title", data.title);
  put_string_if(out, "creation", data.creation);
  put_u32_if(out, "height", data.height);
  put_u32_if(out, "width", data.width);
  put_u32_if(out, "frames", data.frames);
  put_f64_if(out, "duration", data.duration);
  put_u32_if(out, "pages", data.pages);
  return out;
}

Json to_json_extension(const ExtensionData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  if (data.ext_ref != 0) {
    // URL intern table index from FastFHIR stream (used when URL text is not materialized).
    out["urlIndex"] = data.ext_ref;
  }
  write_choice_if(out, "value", data.value);
  return out;
}

Json to_json_patient_contact(const PatientcontactData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "relationship", data.relationship, to_json_codeable_concept);
  put_object_if(out, "name", data.name, to_json_human_name);
  put_object_array_if(out, "telecom", data.telecom, to_json_contact_point);
  put_object_if(out, "address", data.address, to_json_address);
  put_enum_if(out, "gender", FF_AdministrativeGenderToString(data.gender));
  put_object_if(out, "organization", data.organization, to_json_reference);
  put_object_if(out, "period", data.period, to_json_period);
  return out;
}

Json to_json_patient_communication(const PatientcommunicationData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "language", data.language, to_json_codeable_concept);
  put_bool_if(out, "preferred", data.preferred);
  return out;
}

Json to_json_patient_link(const PatientlinkData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "other", data.other, to_json_reference);
  put_enum_if(out, "type", FF_LinkTypeToString(data.type));
  return out;
}

Json to_json_patient(const BundlePatient& bundle_patient) {
  const auto& patient = bundle_patient.patient;
  Json out = Json::object();

  out["resourceType"] = "Patient";
  put_string_if(out, "id", patient.id);
  put_object_if(out, "meta", patient.meta, to_json_meta);
  put_string_if(out, "implicitRules", patient.implicitrules);
  put_string_if(out, "language", patient.language);
  put_object_if(out, "text", patient.text, to_json_narrative);

  if (!patient.contained.empty()) {
    FastFHIR::Parser parser(bundle_patient.memory);
    auto root = parser.root();
    auto contained_node = root[FastFHIR::Fields::PATIENT::CONTAINED];
    if (contained_node && contained_node.is_array()) {
      Json contained = Json::array();
      for (const auto& entry : contained_node.entries()) {
        std::ostringstream raw;
        entry.print_json(raw);
        Json parsed = Json::parse(raw.str(), nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
          contained.push_back(std::move(parsed));
        }
      }
      if (!contained.empty()) {
        out["contained"] = std::move(contained);
      }
    }
  }

  put_object_array_if(out, "extension", patient.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", patient.modifierextension, to_json_extension);
  put_object_array_if(out, "identifier", patient.identifier, to_json_identifier);
  put_bool_if(out, "active", patient.active);
  put_object_array_if(out, "name", patient.name, to_json_human_name);
  put_object_array_if(out, "telecom", patient.telecom, to_json_contact_point);
  put_enum_if(out, "gender", FF_AdministrativeGenderToString(patient.gender));
  put_string_if(out, "birthDate", patient.birthdate);
  write_choice_if(out, "deceased", patient.deceased);
  put_object_array_if(out, "address", patient.address, to_json_address);
  put_object_if(out, "maritalStatus", patient.maritalstatus, to_json_codeable_concept);
  write_choice_if(out, "multipleBirth", patient.multiplebirth);
  put_object_array_if(out, "photo", patient.photo, to_json_attachment);
  put_object_array_if(out, "contact", patient.contact, to_json_patient_contact);
  put_object_array_if(out, "communication", patient.communication, to_json_patient_communication);
  put_object_array_if(
      out, "generalPractitioner", patient.generalpractitioner, to_json_reference);
  put_object_if(out, "managingOrganization", patient.managingorganization, to_json_reference);
  put_object_array_if(out, "link", patient.link, to_json_patient_link);

  return out;
}

}  // namespace

ArmRunResult run_json_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  Timer stage1;
  stage1.start();

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  for (const auto& p : fixture.bundle) {
    nlohmann::json patient_resource = to_json_patient(p);

    nlohmann::json patient_entry;
    patient_entry["resource"] = std::move(patient_resource);
    bundle["entry"].push_back(std::move(patient_entry));
  }

  const std::string payload = bundle.dump();

  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  Timer stage3;
  stage3.start();

  const auto parsed = nlohmann::json::parse(payload);
  int patients_found = 0;
  std::string found_birthdate;
  double found_cholesterol = 0.0;
  bool found_cholesterol_value = false;

  if (parsed.contains("entry") && parsed["entry"].is_array()) {
    for (const auto& json_entry : parsed["entry"]) {
      if (!json_entry.contains("resource") || !json_entry["resource"].is_object()) continue;
      const auto& resource = json_entry["resource"];
      const auto resource_type = resource.value("resourceType", "");

      if (resource_type == "Patient") {
        if (resource.contains("birthDate")) {
          ++patients_found;
          if (found_birthdate.empty())
            found_birthdate = resource["birthDate"].get<std::string>();
        }
      } else if (resource_type == "Observation") {
        if (resource.contains("code") && resource["code"].contains("coding")
            && resource["code"]["coding"].is_array()) {
          for (const auto& coding : resource["code"]["coding"]) {
            if (coding.value("code", "") == std::string(kCholesterolLoincCode)) {
              if (resource.contains("valueQuantity")
                  && resource["valueQuantity"].contains("value")) {
                found_cholesterol = resource["valueQuantity"]["value"].get<double>();
                found_cholesterol_value = true;
              }
              break;
            }
          }
        }
      }
    }
  }

  result.queried_value = "patients=" + std::to_string(patients_found)
      + " birthdate=" + found_birthdate
      + " cholesterol=" + (found_cholesterol_value ? std::to_string(found_cholesterol) : "N/A");

  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
