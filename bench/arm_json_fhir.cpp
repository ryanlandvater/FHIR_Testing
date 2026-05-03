#include "harness.hpp"

#include <nlohmann/json.hpp>

#include <FF_Condition.hpp>
#include <FF_Encounter.hpp>
#include <FF_Observation.hpp>

#include <algorithm>
#include <atomic>
#include <execution>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

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

Json to_json_annotation(const AnnotationData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  write_choice_if(out, "author", data.author);
  put_string_if(out, "time", data.time);
  put_string_if(out, "text", data.text);
  return out;
}

Json to_json_range(const RangeData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_if(out, "low", data.low, to_json_quantity);
  put_object_if(out, "high", data.high, to_json_quantity);
  return out;
}

Json to_json_duration(const DurationData& data) {
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

Json to_json_codeable_reference(const CodeableReferenceData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_if(out, "concept", data.concept_, to_json_codeable_concept);
  put_object_if(out, "reference", data.reference, to_json_reference);
  return out;
}

Json to_json_encounter_status_history(const EncounterstatusHistoryData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_enum_if(out, "status", FF_EncounterStatusToString(data.status));
  put_object_if(out, "period", data.period, to_json_period);
  return out;
}

Json to_json_encounter_class_history(const EncounterclassHistoryData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "class", data.class_, to_json_coding);
  put_object_if(out, "period", data.period, to_json_period);
  return out;
}

Json to_json_encounter_participant(const EncounterparticipantData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "type", data.type, to_json_codeable_concept);
  put_object_if(out, "period", data.period, to_json_period);
  put_object_if(out, "individual", data.individual, to_json_reference);
  put_object_if(out, "actor", data.actor, to_json_reference);
  return out;
}

Json to_json_encounter_diagnosis(const EncounterdiagnosisData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "condition", data.condition, to_json_reference);
  put_object_if(out, "use", data.use, to_json_codeable_concept);
  put_u32_if(out, "rank", data.rank);
  return out;
}

Json to_json_encounter_hospitalization(const EncounterhospitalizationData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "preAdmissionIdentifier", data.preadmissionidentifier, to_json_identifier);
  put_object_if(out, "origin", data.origin, to_json_reference);
  put_object_if(out, "admitSource", data.admitsource, to_json_codeable_concept);
  put_object_if(out, "readmission", data.readmission, to_json_codeable_concept);
  put_object_array_if(out, "dietPreference", data.dietpreference, to_json_codeable_concept);
  put_object_array_if(out, "specialCourtesy", data.specialcourtesy, to_json_codeable_concept);
  put_object_array_if(out, "specialArrangement", data.specialarrangement, to_json_codeable_concept);
  put_object_if(out, "destination", data.destination, to_json_reference);
  put_object_if(out, "dischargeDisposition", data.dischargedisposition, to_json_codeable_concept);
  return out;
}

Json to_json_encounter_location(const EncounterlocationData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "location", data.location, to_json_reference);
  put_enum_if(out, "status", FF_EncounterLocationStatusToString(data.status));
  put_object_if(out, "physicalType", data.physicaltype, to_json_codeable_concept);
  put_object_if(out, "period", data.period, to_json_period);
  put_object_if(out, "form", data.form, to_json_codeable_concept);
  return out;
}

Json to_json_encounter_reason(const EncounterreasonData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "use", data.use, to_json_codeable_concept);
  put_object_array_if(out, "value", data.value, to_json_codeable_reference);
  return out;
}

Json to_json_encounter_admission(const EncounteradmissionData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "preAdmissionIdentifier", data.preadmissionidentifier, to_json_identifier);
  put_object_if(out, "origin", data.origin, to_json_reference);
  put_object_if(out, "admitSource", data.admitsource, to_json_codeable_concept);
  put_object_if(out, "readmission", data.readmission, to_json_codeable_concept);
  put_object_if(out, "destination", data.destination, to_json_reference);
  put_object_if(out, "dischargeDisposition", data.dischargedisposition, to_json_codeable_concept);
  return out;
}

Json to_json_encounter(const EncounterData& data) {
  Json out = Json::object();
  out["resourceType"] = "Encounter";
  put_string_if(out, "id", data.id);
  put_object_if(out, "meta", data.meta, to_json_meta);
  put_string_if(out, "implicitRules", data.implicitrules);
  put_string_if(out, "language", data.language);
  put_object_if(out, "text", data.text, to_json_narrative);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "identifier", data.identifier, to_json_identifier);
  put_enum_if(out, "status", FF_EncounterStatusToString(data.status));
  put_object_array_if(out, "statusHistory", data.statushistory, to_json_encounter_status_history);
  put_object_if(out, "class", data.class_, to_json_coding);
  put_object_array_if(out, "classHistory", data.classhistory, to_json_encounter_class_history);
  put_object_array_if(out, "type", data.type, to_json_codeable_concept);
  put_object_if(out, "serviceType", data.servicetype, to_json_codeable_concept);
  put_object_if(out, "priority", data.priority, to_json_codeable_concept);
  put_object_if(out, "subject", data.subject, to_json_reference);
  put_object_array_if(out, "episodeOfCare", data.episodeofcare, to_json_reference);
  put_object_array_if(out, "basedOn", data.basedon, to_json_reference);
  put_object_array_if(out, "participant", data.participant, to_json_encounter_participant);
  put_object_array_if(out, "appointment", data.appointment, to_json_reference);
  put_object_if(out, "period", data.period, to_json_period);
  put_object_if(out, "length", data.length, to_json_duration);
  put_object_array_if(out, "reasonCode", data.reasoncode, to_json_codeable_concept);
  put_object_array_if(out, "reasonReference", data.reasonreference, to_json_reference);
  put_object_array_if(out, "diagnosis", data.diagnosis, to_json_encounter_diagnosis);
  put_object_array_if(out, "account", data.account, to_json_reference);
  put_object_if(out, "hospitalization", data.hospitalization, to_json_encounter_hospitalization);
  put_object_array_if(out, "location", data.location, to_json_encounter_location);
  put_object_if(out, "serviceProvider", data.serviceprovider, to_json_reference);
  put_object_if(out, "partOf", data.partof, to_json_reference);
  put_object_if(out, "subjectStatus", data.subjectstatus, to_json_codeable_concept);
  put_object_array_if(out, "careTeam", data.careteam, to_json_reference);
  put_string_if(out, "plannedStartDate", data.plannedstartdate);
  put_string_if(out, "plannedEndDate", data.plannedenddate);
  put_object_array_if(out, "reason", data.reason, to_json_encounter_reason);
  put_object_array_if(out, "dietPreference", data.dietpreference, to_json_codeable_concept);
  put_object_array_if(out, "specialArrangement", data.specialarrangement, to_json_codeable_concept);
  put_object_array_if(out, "specialCourtesy", data.specialcourtesy, to_json_codeable_concept);
  put_object_if(out, "admission", data.admission, to_json_encounter_admission);
  return out;
}

Json to_json_condition_stage(const ConditionstageData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "summary", data.summary, to_json_codeable_concept);
  put_object_array_if(out, "assessment", data.assessment, to_json_reference);
  put_object_if(out, "type", data.type, to_json_codeable_concept);
  return out;
}

Json to_json_condition_evidence(const ConditionevidenceData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "code", data.code, to_json_codeable_concept);
  put_object_array_if(out, "detail", data.detail, to_json_reference);
  return out;
}

Json to_json_condition_participant(const ConditionparticipantData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "function", data.function, to_json_codeable_concept);
  put_object_if(out, "actor", data.actor, to_json_reference);
  return out;
}

Json to_json_condition(const ConditionData& data) {
  Json out = Json::object();
  out["resourceType"] = "Condition";
  put_string_if(out, "id", data.id);
  put_object_if(out, "meta", data.meta, to_json_meta);
  put_string_if(out, "implicitRules", data.implicitrules);
  put_string_if(out, "language", data.language);
  put_object_if(out, "text", data.text, to_json_narrative);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "identifier", data.identifier, to_json_identifier);
  put_object_if(out, "clinicalStatus", data.clinicalstatus, to_json_codeable_concept);
  put_object_if(out, "verificationStatus", data.verificationstatus, to_json_codeable_concept);
  put_object_array_if(out, "category", data.category, to_json_codeable_concept);
  put_object_if(out, "severity", data.severity, to_json_codeable_concept);
  put_object_if(out, "code", data.code, to_json_codeable_concept);
  put_object_array_if(out, "bodySite", data.bodysite, to_json_codeable_concept);
  put_object_if(out, "subject", data.subject, to_json_reference);
  put_object_if(out, "encounter", data.encounter, to_json_reference);
  write_choice_if(out, "onset", data.onset);
  write_choice_if(out, "abatement", data.abatement);
  put_string_if(out, "recordedDate", data.recordeddate);
  put_object_if(out, "recorder", data.recorder, to_json_reference);
  put_object_if(out, "asserter", data.asserter, to_json_reference);
  put_object_array_if(out, "stage", data.stage, to_json_condition_stage);
  put_object_array_if(out, "evidence", data.evidence, to_json_condition_evidence);
  put_object_array_if(out, "note", data.note, to_json_annotation);
  put_object_array_if(out, "participant", data.participant, to_json_condition_participant);
  return out;
}

Json to_json_observation_reference_range(const ObservationreferenceRangeData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "low", data.low, to_json_quantity);
  put_object_if(out, "high", data.high, to_json_quantity);
  put_object_if(out, "type", data.type, to_json_codeable_concept);
  put_object_array_if(out, "appliesTo", data.appliesto, to_json_codeable_concept);
  put_object_if(out, "age", data.age, to_json_range);
  put_string_if(out, "text", data.text);
  put_object_if(out, "normalValue", data.normalvalue, to_json_codeable_concept);
  return out;
}

Json to_json_observation_component(const ObservationcomponentData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "code", data.code, to_json_codeable_concept);
  write_choice_if(out, "value", data.value);
  put_object_if(out, "dataAbsentReason", data.dataabsentreason, to_json_codeable_concept);
  put_object_array_if(out, "interpretation", data.interpretation, to_json_codeable_concept);
  put_object_array_if(out, "referenceRange", data.referencerange, to_json_observation_reference_range);
  return out;
}

Json to_json_observation_triggered_by(const ObservationtriggeredByData& data) {
  Json out = Json::object();
  put_string_if(out, "id", data.id);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_if(out, "observation", data.observation, to_json_reference);
  put_enum_if(out, "type", FF_TriggeredBytypeToString(data.type));
  put_string_if(out, "reason", data.reason);
  return out;
}

Json to_json_observation(const ObservationData& data) {
  Json out = Json::object();
  out["resourceType"] = "Observation";
  put_string_if(out, "id", data.id);
  put_object_if(out, "meta", data.meta, to_json_meta);
  put_string_if(out, "implicitRules", data.implicitrules);
  put_string_if(out, "language", data.language);
  put_object_if(out, "text", data.text, to_json_narrative);
  put_object_array_if(out, "extension", data.extension, to_json_extension);
  put_object_array_if(out, "modifierExtension", data.modifierextension, to_json_extension);
  put_object_array_if(out, "identifier", data.identifier, to_json_identifier);
  put_object_array_if(out, "basedOn", data.basedon, to_json_reference);
  put_object_array_if(out, "partOf", data.partof, to_json_reference);
  put_enum_if(out, "status", FF_ObservationStatusToString(data.status));
  put_object_array_if(out, "category", data.category, to_json_codeable_concept);
  put_object_if(out, "code", data.code, to_json_codeable_concept);
  put_object_if(out, "subject", data.subject, to_json_reference);
  put_object_array_if(out, "focus", data.focus, to_json_reference);
  put_object_if(out, "encounter", data.encounter, to_json_reference);
  write_choice_if(out, "effective", data.effective);
  put_string_if(out, "issued", data.issued);
  put_object_array_if(out, "performer", data.performer, to_json_reference);
  write_choice_if(out, "value", data.value);
  put_object_if(out, "dataAbsentReason", data.dataabsentreason, to_json_codeable_concept);
  put_object_array_if(out, "interpretation", data.interpretation, to_json_codeable_concept);
  put_object_array_if(out, "note", data.note, to_json_annotation);
  put_object_if(out, "bodySite", data.bodysite, to_json_codeable_concept);
  put_object_if(out, "method", data.method, to_json_codeable_concept);
  put_object_if(out, "specimen", data.specimen, to_json_reference);
  put_object_if(out, "device", data.device, to_json_reference);
  put_object_array_if(out, "referenceRange", data.referencerange, to_json_observation_reference_range);
  put_object_array_if(out, "hasMember", data.hasmember, to_json_reference);
  put_object_array_if(out, "derivedFrom", data.derivedfrom, to_json_reference);
  put_object_array_if(out, "component", data.component, to_json_observation_component);
  write_choice_if(out, "instantiates", data.instantiates);
  put_object_array_if(out, "triggeredBy", data.triggeredby, to_json_observation_triggered_by);
  put_object_if(out, "bodyStructure", data.bodystructure, to_json_reference);
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

  // Phase 1: Serialize directly from in-memory PatientData values.
  std::vector<nlohmann::json> merged_entries;
  merged_entries.reserve(fixture.bundle.size());

  for (const auto& p : fixture.bundle) {
    nlohmann::json patient_entry;
    patient_entry["resource"] = to_json_patient(p);
    merged_entries.push_back(std::move(patient_entry));
  }

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  // Phase 2: Assemble bundle entry array sequentially.
  for (auto& entry : merged_entries) {
    if (!entry.is_null()) {
      bundle["entry"].push_back(std::move(entry));
    }
  }

  // Phase 3: Final stringification remains the serialization bottleneck.
  const std::string payload = bundle.dump();

  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  Timer stage3;
  stage3.start();

  const auto parsed = nlohmann::json::parse(payload);
  int patients_found = 0;
  int encounters_found = 0;
  int conditions_found = 0;
  std::string found_birthdate;
  std::string found_condition_code;

  if (parsed.contains("entry") && parsed["entry"].is_array()) {
    for (const auto& json_entry : parsed["entry"]) {
      if (!json_entry.contains("resource") || !json_entry["resource"].is_object()) continue;
      const auto& resource = json_entry["resource"];
      const auto resource_type = resource.value("resourceType", "");

      if (resource_type == "Patient") {
        ++patients_found;
        if (resource.contains("birthDate") && found_birthdate.empty()) {
          found_birthdate = resource["birthDate"].get<std::string>();
        }
      } else if (resource_type == "Encounter") {
        ++encounters_found;
      } else if (resource_type == "Condition") {
        ++conditions_found;
        if (resource.contains("code") && resource["code"].contains("coding")
            && resource["code"]["coding"].is_array()) {
          for (const auto& coding : resource["code"]["coding"]) {
            if (found_condition_code.empty() && coding.contains("code")) {
              found_condition_code = coding["code"].get<std::string>();
              break;
            }
          }
        }
      }
    }
  }

  result.queried_value = "patients=" + std::to_string(patients_found)
      + " birthdate=" + (found_birthdate.empty() ? "none" : found_birthdate)
      + " encounters=" + std::to_string(encounters_found)
      + " conditions=" + std::to_string(conditions_found)
      + " condition_code=" + (found_condition_code.empty() ? "none" : found_condition_code);

  result.metrics.push_back(
      MetricEvent{"json_fhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench
