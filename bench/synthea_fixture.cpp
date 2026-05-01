#include "harness.hpp"

#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bench {

namespace {

AdministrativeGender gender_from_code(std::string_view sv) {
  if (sv == "male")   return AdministrativeGender::Male;
  if (sv == "female") return AdministrativeGender::Female;
  if (sv == "other")  return AdministrativeGender::Other;
  return AdministrativeGender::Unknown;
}

void extract_patient(const FastFHIR::Reflective::Node& node, SyntheaFixture& fixture) {
  auto id_entry = node[FastFHIR::Fields::PATIENT::ID];
  if (id_entry) {
    fixture.patient_id_storage = std::string(std::string_view(id_entry));
    fixture.patient.id = fixture.patient_id_storage;
  }
  auto bd_entry = node[FastFHIR::Fields::PATIENT::BIRTH_DATE];
  if (bd_entry) {
    fixture.patient_birthdate_storage = std::string(std::string_view(bd_entry));
    fixture.patient.birthdate = fixture.patient_birthdate_storage;
  }
  auto gender_entry = node[FastFHIR::Fields::PATIENT::GENDER];
  if (gender_entry) {
    fixture.patient.gender = gender_from_code(std::string_view(gender_entry));
  }
  auto active_entry = node[FastFHIR::Fields::PATIENT::ACTIVE];
  if (active_entry) {
    fixture.patient.active = active_entry.as<bool>() ? 1 : 0;
  }
}

}  // namespace

SyntheaFixture make_synthea_fixture(const std::filesystem::path& ffhr_path) {
  // Load the pre-generated .ffhr file into an owned buffer.
  std::ifstream file(ffhr_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open .ffhr file: " + ffhr_path.string());
  }
  const auto file_size = static_cast<std::streamsize>(file.tellg());
  if (file_size <= 0) {
    throw std::runtime_error("Empty .ffhr file: " + ffhr_path.string());
  }
  file.seekg(0);
  std::string ffhr_bytes(static_cast<std::size_t>(file_size), '\0');
  file.read(ffhr_bytes.data(), file_size);

  // Parse the FFHR stream.
  FastFHIR::Parser parser(ffhr_bytes.data(), ffhr_bytes.size());
  const auto root = parser.root();
  if (!root) {
    throw std::runtime_error("FFHR parse failed (no root): " + ffhr_path.string());
  }

  SyntheaFixture fixture{};
  fixture.ffhr_size_bytes = static_cast<int64_t>(file_size);

  if (root.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
    auto entries_entry = root[FastFHIR::Fields::BUNDLE::ENTRY];
    if (entries_entry) {
      for (const auto& entry_node : entries_entry.entries()) {
        auto resource_entry = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource_entry) continue;
        auto resource_node = resource_entry.as_node();
        if (!resource_node) continue;

        if (resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
          extract_patient(resource_node, fixture);
        }
      }
    }
  } else if (root.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    extract_patient(root, fixture);
  }

  if (fixture.patient_id_storage.empty() && fixture.patient_birthdate_storage.empty()) {
    throw std::runtime_error("No Patient found in: " + ffhr_path.string());
  }
  return fixture;
}

}  // namespace bench
