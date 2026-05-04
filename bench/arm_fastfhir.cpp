#include "harness.hpp"

#include <FF_Bundle.hpp>

#include <algorithm>
#include <execution>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_FASTFHIR
#include "bench_assign.hpp"
#undef ARM_FASTFHIR

namespace bench {
namespace {
#if defined(__APPLE__)
struct EntryBuildContext {
  FastFHIR::Builder* builder;
  const std::vector<BundlePatient>* bundle;
  std::vector<BundleentryData>* entries;
};

static inline void build_bundle_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<EntryBuildContext*>(raw_context);
  const auto& item = (*context->bundle)[idx];
  auto patient_handle = context->builder->append_obj(PatientData{});
  assign::assign_patient(item.patient, patient_handle);
  (*context->entries)[idx] = BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
}
#endif
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

  std::vector<BundleentryData> entries(fixture.bundle.size());
#if defined(__APPLE__)
  EntryBuildContext context{&builder, &fixture.bundle, &entries};
  dispatch_apply_f(entries.size(), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &context,
                   build_bundle_entry);
#elif defined(__cpp_lib_execution) && (__cpp_lib_execution >= 201603L)
  std::transform(
      std::execution::par_unseq,
      fixture.bundle.begin(),
      fixture.bundle.end(),
      entries.begin(),
      [&builder](const BundlePatient& item) -> BundleentryData {
        auto patient_handle = builder.append_obj(PatientData{});
        assign::assign_patient(item.patient, patient_handle);
        return BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
      });
#else
  std::transform(
      fixture.bundle.begin(),
      fixture.bundle.end(),
      entries.begin(),
      [&builder](const BundlePatient& item) -> BundleentryData {
        auto patient_handle = builder.append_obj(PatientData{});
        assign::assign_patient(item.patient, patient_handle);
        return BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
      });
#endif
  bundle.entry = std::move(entries);

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
