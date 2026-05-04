#include "harness.hpp"

#include <FF_Bundle.hpp>

#define ARM_FASTFHIR
#include "bench_assign.hpp"
#undef ARM_FASTFHIR

namespace bench {
namespace {
}  // namespace

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;


  std::size_t arena_hint = 4096;
  for (const auto& p : fixture.bundle) {
    arena_hint += p.memory.size();
  }
  FastFHIR::Memory payload_memory = FastFHIR::Memory::create(arena_hint);
  FastFHIR::Builder builder(payload_memory, FHIR_VERSION_R5);

  Timer stage1_timer;
  stage1_timer.start();

  BundleData bundle{};
  bundle.type = BundleType::Collection;

  for (const auto& item : fixture.bundle) {
    auto patient_handle = builder.append_obj(PatientData{});
    assign::assign_patient(item.patient, patient_handle);
    bundle.entry.push_back(BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)});
  }

  const auto root = builder.append_obj(bundle);
  builder.set_root(root);
  (void)builder.finalize(FF_CHECKSUM_SHA256,
                         [](const unsigned char*, size_t) -> std::vector<BYTE> {
                           return std::vector<BYTE>(32);
                         });
  out.metrics.push_back({"fastfhir", Stage::Stage1Serialize, stage1_timer.stop_us()});

  Timer stage3_timer;
  stage3_timer.start();
  std::string birthdate;
  std::size_t cholesterol_matches = 0;
  std::size_t patient_count = 0;

  if (payload_memory) {
    FastFHIR::Parser parser(payload_memory);
    const auto root_node = parser.root();
    if (root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
      if (auto entries = root_node[FastFHIR::Fields::BUNDLE::ENTRY]) {
        for (auto& entry : entries.entries()) {
          auto resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
          if (!resource) {
            continue;
          }
          auto resource_node = resource.as_node();
          if (!resource_node) {
            continue;
          }
          if (resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
            ++patient_count;
            if (birthdate.empty()) {
#if defined(BENCH_ASSIGN_FORCE_FULL_CAST)
              const auto patient = resource_node.as<PatientData>();
              birthdate = std::string(patient.birthdate);
#else
              auto birth_date_entry = resource_node[FastFHIR::Fields::PATIENT::BIRTH_DATE];
              if (birth_date_entry) {
                birthdate = std::string(birth_date_entry.as<std::string_view>());
              }
#endif
            }
          }
        }
      }
    }
  }

  out.metrics.push_back({"fastfhir", Stage::Stage3Query, stage3_timer.stop_us()});
  out.queried_value =
      "patients=" + std::to_string(patient_count) +
      " birthdate=" + birthdate +
      " loinc_2085_9_matches=" + std::to_string(cholesterol_matches);
  return out;
}

}  // namespace bench
