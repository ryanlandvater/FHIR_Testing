#include "harness.hpp"

#include "proto/google/fhir/proto/r4/core/codes.pb.h"
#include "proto/google/fhir/proto/r4/core/datatypes.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"

#include <ctime>
#include <optional>
#include <string>

namespace bench {

namespace {

using google::fhir::r4::core::AdministrativeGenderCode_Value;
using google::fhir::r4::core::Date_Precision_DAY;
using google::fhir::r4::core::ObservationStatusCode_Value;

std::optional<int64_t> parse_birthdate_us(std::string_view birthdate) {
  if (birthdate.size() < 10) {
    return std::nullopt;
  }

  // Expected canonical date format from fixtures: YYYY-MM-DD.
  std::tm tm{};
  tm.tm_year = std::atoi(std::string(birthdate.substr(0, 4)).c_str()) - 1900;
  tm.tm_mon = std::atoi(std::string(birthdate.substr(5, 2)).c_str()) - 1;
  tm.tm_mday = std::atoi(std::string(birthdate.substr(8, 2)).c_str());
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;

  const std::time_t epoch_seconds = timegm(&tm);
  if (epoch_seconds < 0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(epoch_seconds) * 1000000LL;
}

AdministrativeGenderCode_Value map_gender(AdministrativeGender gender) {
  switch (gender) {
    case AdministrativeGender::Male:
      return google::fhir::r4::core::AdministrativeGenderCode_Value_MALE;
    case AdministrativeGender::Female:
      return google::fhir::r4::core::AdministrativeGenderCode_Value_FEMALE;
    case AdministrativeGender::Other:
      return google::fhir::r4::core::AdministrativeGenderCode_Value_OTHER;
    case AdministrativeGender::Unknown:
    default:
      return google::fhir::r4::core::AdministrativeGenderCode_Value_UNKNOWN;
  }
}

ObservationStatusCode_Value map_observation_status(ObservationStatus status) {
  switch (status) {
    case ObservationStatus::Registered:
      return google::fhir::r4::core::ObservationStatusCode_Value_REGISTERED;
    case ObservationStatus::Preliminary:
      return google::fhir::r4::core::ObservationStatusCode_Value_PRELIMINARY;
    case ObservationStatus::Final:
      return google::fhir::r4::core::ObservationStatusCode_Value_FINAL;
    case ObservationStatus::Amended:
      return google::fhir::r4::core::ObservationStatusCode_Value_AMENDED;
    case ObservationStatus::Corrected:
      return google::fhir::r4::core::ObservationStatusCode_Value_CORRECTED;
    case ObservationStatus::Cancelled:
      return google::fhir::r4::core::ObservationStatusCode_Value_CANCELLED;
    case ObservationStatus::EnteredInError:
      return google::fhir::r4::core::ObservationStatusCode_Value_ENTERED_IN_ERROR;
    case ObservationStatus::Unknown:
    default:
      return google::fhir::r4::core::ObservationStatusCode_Value_UNKNOWN;
  }
}

}  // namespace

ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer test1_timer;
  test1_timer.start();

  std::string payload;
  payload.reserve(fixture.bundle.size() * 256);

  for (const auto& item : fixture.bundle) {
    google::fhir::r4::core::Patient patient;
    if (!item.patient.id.empty()) {
      patient.mutable_id()->set_value(std::string(item.patient.id));
    }
    if (item.patient.active != FF_NULL_UINT8) {
      patient.mutable_active()->set_value(item.patient.active != 0);
    }
    patient.mutable_gender()->set_value(map_gender(item.patient.gender));
    if (const auto birth_us = parse_birthdate_us(item.patient.birthdate)) {
      auto* birth_date = patient.mutable_birth_date();
      birth_date->set_value_us(*birth_us);
      birth_date->set_timezone("UTC");
      birth_date->set_precision(Date_Precision_DAY);
    }
    if (!item.patient.name.empty() && !item.patient.name.front().text.empty()) {
      patient.add_name()->mutable_text()->set_value(std::string(item.patient.name.front().text));
    }

    std::string patient_bytes;
    if (patient.SerializeToString(&patient_bytes)) {
      payload.append(patient_bytes);
    }

    for (const auto& observation : item.observations) {
      google::fhir::r4::core::Observation obs;
      if (!observation.id.empty()) {
        obs.mutable_id()->set_value(std::string(observation.id));
      }
      obs.mutable_status()->set_value(map_observation_status(observation.status));
      if (observation.code && !observation.code->text.empty()) {
        obs.mutable_code()->mutable_text()->set_value(std::string(observation.code->text));
      }

      std::string subject_patient_id;
      if (observation.subject && !observation.subject->reference.empty()) {
        const std::string_view ref = observation.subject->reference;
        constexpr std::string_view kPatientPrefix = "Patient/";
        if (ref.substr(0, kPatientPrefix.size()) == kPatientPrefix && ref.size() > kPatientPrefix.size()) {
          subject_patient_id = std::string(ref.substr(kPatientPrefix.size()));
        } else {
          subject_patient_id = std::string(ref);
        }
      } else if (!item.patient.id.empty()) {
        subject_patient_id = std::string(item.patient.id);
      }
      if (!subject_patient_id.empty()) {
        obs.mutable_subject()->mutable_patient_id()->set_value(subject_patient_id);
      }

      std::string obs_bytes;
      if (obs.SerializeToString(&obs_bytes)) {
        payload.append(obs_bytes);
      }
    }
  }

  out.metrics.push_back({"google_fhir", Stage::Test1Serialize, test1_timer.stop_ns()});

  // Stage 2 and Stage 3 are intentionally stubbed in this first protobuf arm pass.
  out.metrics.push_back({"google_fhir", Stage::Test2Materialize, 0});
  out.metrics.push_back({"google_fhir", Stage::Test3Query, 0});

  out.queried_value = "google_fhir_protobuf_stage1";
  out.reconstructed_bundle_json = "protobuf_payload_bytes=" + std::to_string(payload.size());
  return out;
}

}  // namespace bench
