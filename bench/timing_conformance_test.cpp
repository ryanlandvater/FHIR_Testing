#include "harness.hpp"

#include <FF_Patient.hpp>
#include <cctype>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string extract_field(const std::string& text, const std::string& key) {
  const std::string needle = key + "=";
  const auto start = text.find(needle);
  if (start == std::string::npos) {
    return "";
  }
  const auto value_start = start + needle.size();
  const auto value_end = text.find(' ', value_start);
  if (value_end == std::string::npos) {
    return text.substr(value_start);
  }
  return text.substr(value_start, value_end - value_start);
}

int extract_patients(const std::string& text) {
  const auto raw = extract_field(text, "patients");
  if (raw.empty()) {
    return -1;
  }
  try {
    return std::stoi(raw);
  } catch (...) {
    return -1;
  }
}

std::string normalize_birthdate(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      out.push_back(ch);
    }
  }
  return out;
}

bool metrics_are_valid(const bench::ArmRunResult& run, bool allow_stage2_zero) {
  for (const auto& metric : run.metrics) {
    if (metric.duration_us > 0) {
      continue;
    }
    if (allow_stage2_zero && metric.stage == bench::Stage::Stage2Transport && metric.duration_us == 0) {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bench::BundlePatient bp{};
  bp.memory = FastFHIR::Memory::create(4096);
  FastFHIR::Builder builder(bp.memory, FHIR_VERSION_R5);

  PatientData patient{};
  patient.id = "patient-conformance";
  patient.birthdate = "1990-03-21";
  patient.gender = AdministrativeGender::Male;
  patient.active = 1;

  auto patient_handle = builder.append_obj(patient);
  builder.set_root(patient_handle);
  const auto patient_view = builder.finalize();
  const int64_t patient_ffhr_bytes = static_cast<int64_t>(patient_view.size());

  bp.patient.id = patient.id;
  bp.patient.birthdate = patient.birthdate;
  bp.patient.gender = patient.gender;
  bp.patient.active = patient.active;

  bench::BundleBenchFixture fixture{};
  fixture.bundle.push_back(std::move(bp));
  fixture.target_size_bytes = patient_ffhr_bytes;
  fixture.actual_ingested_bytes = patient_ffhr_bytes;

  const auto fastfhir = bench::run_fastfhir_bundle(fixture);
  const auto json = bench::run_json_bundle(fixture);
  const auto google = bench::run_google_fhir_bundle(fixture);
  const auto hl7 = bench::run_hl7v2_bundle(fixture);

  if (fastfhir.metrics.size() < 2 || json.metrics.size() < 2
      || google.metrics.size() < 2 || hl7.metrics.size() < 3) {
    std::cerr << "timing conformance failed: expected stage metrics from all arms\n";
    return 1;
  }

  if (!metrics_are_valid(fastfhir, false)) {
    std::cerr << "timing conformance failed: FastFHIR metric duration invalid\n";
    return 1;
  }
  if (!metrics_are_valid(json, false)) {
    std::cerr << "timing conformance failed: JSON metric duration invalid\n";
    return 1;
  }
  if (!metrics_are_valid(google, false)) {
    std::cerr << "timing conformance failed: Google metric duration invalid\n";
    return 1;
  }
  if (!metrics_are_valid(hl7, true)) {
    std::cerr << "timing conformance failed: HL7 metric duration invalid\n";
    return 1;
  }

  const int fast_patients = extract_patients(fastfhir.queried_value);
  const int json_patients = extract_patients(json.queried_value);
  const int google_patients = extract_patients(google.queried_value);
  const int hl7_patients = extract_patients(hl7.queried_value);

  if (fast_patients != 1 || json_patients != 1 || google_patients != 1 || hl7_patients != 1) {
    std::cerr << "timing conformance failed: expected patients=1 for all arms\n"
              << "  fastfhir: " << fastfhir.queried_value << "\n"
              << "  json:     " << json.queried_value << "\n"
              << "  google:   " << google.queried_value << "\n"
              << "  hl7v2:    " << hl7.queried_value << "\n";
    return 1;
  }

  const auto fast_birth = normalize_birthdate(extract_field(fastfhir.queried_value, "birthdate"));
  const auto json_birth = normalize_birthdate(extract_field(json.queried_value, "birthdate"));
  const auto google_birth = normalize_birthdate(extract_field(google.queried_value, "birthdate"));
  const auto hl7_birth = normalize_birthdate(extract_field(hl7.queried_value, "birthdate"));

  if (fast_birth.empty() || json_birth.empty() || google_birth.empty() || hl7_birth.empty()
      || fast_birth != json_birth || fast_birth != google_birth || fast_birth != hl7_birth) {
    std::cerr << "timing conformance failed: birthdate mismatch across arms\n"
              << "  fastfhir: " << fastfhir.queried_value << "\n"
              << "  json:     " << json.queried_value << "\n"
              << "  google:   " << google.queried_value << "\n"
              << "  hl7v2:    " << hl7.queried_value << "\n";
    return 1;
  }

  const auto hl7_parity = extract_field(hl7.queried_value, "parity");
  if (hl7_parity != "1/1") {
    std::cerr << "timing conformance failed: hl7 parity expected 1/1, got: "
              << hl7.queried_value << "\n";
    return 1;
  }

  // ── 2. HL7v2 field-level encode/decode check ─────────────────────────────
  // Build a patient with names, address, telecom, identifier, and contact so
  // the ZPV snapshot parity covers all those fields and the PID decode is
  // fully exercised.  String literals have static duration; string_view is safe.
  {
    PatientData rich{};
    rich.id        = "patient-rich";
    rich.birthdate = "1985-07-15";
    rich.gender    = AdministrativeGender::Female;
    rich.active    = 1;

    // PID-5 — patient name (XPN)
    HumanNameData nm{};
    nm.family = "Smith";
    nm.given  = {"Jane", "Marie"};
    nm.use    = NameUse::Official;
    rich.name.push_back(std::move(nm));

    // PID-11 — patient address (XAD)
    AddressData addr{};
    addr.line       = {"123 Main St"};
    addr.city       = "Boston";
    addr.state      = "MA";
    addr.postalcode = "02101";
    addr.country    = "US";
    addr.use        = AddressUse::Home;
    rich.address.push_back(std::move(addr));

    // PID-13 — home phone (XTN)
    ContactPointData cp{};
    cp.system = ContactPointSystem::Phone;
    cp.use    = ContactPointUse::Home;
    cp.value  = "617-555-0100";
    rich.telecom.push_back(std::move(cp));

    // PID-3 — patient identifier (CX)
    IdentifierData ident{};
    ident.system = "http://hospital.example.org";
    ident.value  = "PAT-001";
    rich.identifier.push_back(std::move(ident));

    // NK1 — patient contact
    PatientcontactData ctct{};
    ctct.name = std::make_unique<HumanNameData>();
    ctct.name->family = "Jones";
    ctct.name->given  = {"Bob"};
    rich.contact.push_back(std::move(ctct));

    bench::BundlePatient rich_bp{};
    rich_bp.patient = std::move(rich);

    bench::BundleBenchFixture rich_fixture{};
    rich_fixture.actual_ingested_bytes = 1024;
    rich_fixture.bundle.push_back(std::move(rich_bp));

    const auto rich_hl7 = bench::run_hl7v2_bundle(rich_fixture);

    if (!metrics_are_valid(rich_hl7, true)) {
      std::cerr << "hl7v2 field parity failed: invalid metrics\n";
      return 1;
    }

    const int rich_patients = extract_patients(rich_hl7.queried_value);
    if (rich_patients != 1) {
      std::cerr << "hl7v2 field parity failed: expected patients=1, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // Full ZPV snapshot round-trip
    const auto rich_parity = extract_field(rich_hl7.queried_value, "parity");
    if (rich_parity != "1/1") {
      std::cerr << "hl7v2 field parity failed: ZPV snapshot parity expected 1/1, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // PID-7 birthdate (YYYYMMDD after decode)
    const auto rich_birth = normalize_birthdate(extract_field(rich_hl7.queried_value, "birthdate"));
    if (rich_birth != "19850715") {
      std::cerr << "hl7v2 field parity failed: birthdate expected 19850715, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // PID-8 administrative sex (HL7 code: F for Female)
    const auto rich_gender = extract_field(rich_hl7.queried_value, "gender");
    if (rich_gender != "F") {
      std::cerr << "hl7v2 field parity failed: gender expected F, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // PID-5 family name (XPN component 0)
    const auto rich_family = extract_field(rich_hl7.queried_value, "family");
    if (rich_family != "Smith") {
      std::cerr << "hl7v2 field parity failed: family expected Smith, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // PID-11 city (XAD component 2)
    const auto rich_city = extract_field(rich_hl7.queried_value, "city");
    if (rich_city != "Boston") {
      std::cerr << "hl7v2 field parity failed: city expected Boston, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // PID-3 identifier repetition count
    const auto rich_ids = extract_field(rich_hl7.queried_value, "identifiers");
    if (rich_ids != "1") {
      std::cerr << "hl7v2 field parity failed: identifiers expected 1, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }

    // NK1 segment count
    const auto rich_ctcts = extract_field(rich_hl7.queried_value, "contacts");
    if (rich_ctcts != "1") {
      std::cerr << "hl7v2 field parity failed: contacts expected 1, got: "
                << rich_hl7.queried_value << "\n";
      return 1;
    }
  }

  std::cout << "timing conformance passed\n";
  return 0;
}
