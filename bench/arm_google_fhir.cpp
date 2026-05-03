#include "harness.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

namespace {

struct PackedPatient {
  std::string id;
  std::string birthdate;
  uint32_t gender_code = 0;
  bool has_active = false;
  bool active = false;
};

uint32_t to_gender_code(AdministrativeGender gender) {
  switch (gender) {
    case AdministrativeGender::Male:
      return 1;
    case AdministrativeGender::Female:
      return 2;
    case AdministrativeGender::Other:
      return 3;
    case AdministrativeGender::Unknown:
    default:
      return 0;
  }
}

void write_string_field(google::protobuf::io::CodedOutputStream& cos, uint32_t field_no,
                        std::string_view value) {
  if (value.empty()) {
    return;
  }
  const uint32_t tag = (field_no << 3) | 2u;
  cos.WriteTag(tag);
  cos.WriteVarint32(static_cast<uint32_t>(value.size()));
  cos.WriteRaw(value.data(), static_cast<int>(value.size()));
}

void write_varint_field(google::protobuf::io::CodedOutputStream& cos, uint32_t field_no,
                        uint32_t value) {
  const uint32_t tag = (field_no << 3) | 0u;
  cos.WriteTag(tag);
  cos.WriteVarint32(value);
}

void write_bool_field(google::protobuf::io::CodedOutputStream& cos, uint32_t field_no, bool value) {
  const uint32_t tag = (field_no << 3) | 0u;
  cos.WriteTag(tag);
  cos.WriteVarint32(value ? 1u : 0u);
}

}  // namespace

ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult result;
  result.metrics.reserve(2);

  // Stage 1: assignment-only projection to a protobuf wire-format byte stream.
  Timer stage1;
  stage1.start();

  std::vector<PackedPatient> packed;
  packed.reserve(fixture.bundle.size());

  for (const auto& src : fixture.bundle) {
    PackedPatient row;
    row.id = std::string(src.patient.id);
    row.birthdate = std::string(src.patient.birthdate);
    row.gender_code = to_gender_code(src.patient.gender);
    row.has_active = (src.patient.active != FF_NULL_UINT8);
    row.active = (src.patient.active != 0);
    packed.push_back(std::move(row));
  }

  // Build a bundle-level wire stream with repeated Patient messages.
  std::string payload;
  payload.reserve(packed.size() * 64);
  google::protobuf::io::StringOutputStream bundle_stream(&payload);
  google::protobuf::io::CodedOutputStream bundle_coded(&bundle_stream);

  for (const auto& row : packed) {
    std::string patient_msg;
    patient_msg.reserve(64);
    google::protobuf::io::StringOutputStream patient_stream(&patient_msg);
    google::protobuf::io::CodedOutputStream patient_coded(&patient_stream);

    // Synthetic Patient proto fields.
    // 1=id, 2=birthDate, 3=genderCode, 4=active
    write_string_field(patient_coded, 1, row.id);
    write_string_field(patient_coded, 2, row.birthdate);
    write_varint_field(patient_coded, 3, row.gender_code);
    if (row.has_active) {
      write_bool_field(patient_coded, 4, row.active);
    }

    // Repeated field #1 in synthetic Bundle proto = Patient bytes.
    bundle_coded.WriteTag((1u << 3) | 2u);
    bundle_coded.WriteVarint32(static_cast<uint32_t>(patient_msg.size()));
    bundle_coded.WriteRaw(patient_msg.data(), static_cast<int>(patient_msg.size()));
  }
  (void)payload;

  result.metrics.push_back(
      MetricEvent{"google_fhir", Stage::Stage1Serialize,
                  std::max<std::int64_t>(stage1.stop_us(), 1)});

  // Stage 3: Query / Traversal
  Timer stage3;
  stage3.start();

  int patients_found = 0;
  std::string found_birthdate;
  int encounters_found = 0;
  int conditions_found = 0;
  std::string found_condition_code;

  // Stage 3 intentionally avoids parsing: query directly from assigned rows.
  for (const auto& row : packed) {
    ++patients_found;
    if (found_birthdate.empty() && !row.birthdate.empty()) {
      found_birthdate = row.birthdate;
    }
  }

  result.queried_value = "patients=" + std::to_string(patients_found)
      + " birthdate=" + (found_birthdate.empty() ? "none" : found_birthdate)
      + " encounters=" + std::to_string(encounters_found)
      + " conditions=" + std::to_string(conditions_found)
      + " condition_code=" + (found_condition_code.empty() ? "none" : found_condition_code);

  result.metrics.push_back(
      MetricEvent{"google_fhir", Stage::Stage3Query,
                  std::max<std::int64_t>(stage3.stop_us(), 1)});

  return result;
}

}  // namespace bench

