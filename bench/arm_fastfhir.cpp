#include "harness.hpp"

#include <FF_Bundle.hpp>

#include <algorithm>
#include <execution>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_FASTFHIR
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#undef ARM_FASTFHIR

namespace bench
{
  namespace
  {

    const ObservationData &enrichment_observation_fixture()
    {
      static const EnrichmentObservationFixture fixture =
          load_enrichment_observation_from_json("bench/enrich.json");
      return fixture.observation;
    }

#if defined(__APPLE__)
    struct EntryBuildContext
    {
      FastFHIR::Builder *builder;
      const std::vector<BundlePatient> *bundle;
      std::vector<BundleentryData> *entries;
    };

    struct ObservationBuildContext
    {
      FastFHIR::Builder *builder;
      const std::vector<const ObservationData *> *observations;
      std::vector<BundleentryData> *entries;
    };

    static inline void build_bundle_entry(void *raw_context, std::size_t idx)
    {
      auto *context = static_cast<EntryBuildContext *>(raw_context);
      const auto &item = (*context->bundle)[idx];
      auto patient_handle = context->builder->append_obj(PatientData{});
      assign::assign_patient(item.patient, patient_handle);
      (*context->entries)[idx] = BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
    }

    static inline void build_observation_entry(void *raw_context, std::size_t idx)
    {
      auto *context = static_cast<ObservationBuildContext *>(raw_context);
      const auto &observation = *(*context->observations)[idx];
      auto observation_handle = context->builder->append_obj(ObservationData{});
      assign::assign_observation(observation, observation_handle);
      (*context->entries)[idx] = BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)};
    }
#endif

  } // namespace

  ArmRunResult run_fastfhir_bundle(const BundleBenchFixture &fixture)
  {
    ArmRunResult out;

    std::size_t arena_hint = 4096;
    std::size_t total_observation_count = 0;
    for (const auto &p : fixture.bundle)
    {
      arena_hint += p.memory.capacity();
      total_observation_count += p.observations.size();
    }
    FastFHIR::Memory payload_memory = FastFHIR::Memory::create(arena_hint);
    FastFHIR::Builder builder(payload_memory, FHIR_VERSION_R5);

    Timer test1_timer;
    test1_timer.start();

    BundleData bundle{};
    bundle.type = BundleType::Collection;

    std::vector<BundleentryData> entries(fixture.bundle.size());
    // #if defined(__APPLE__)
    //   EntryBuildContext context{&builder, &fixture.bundle, &entries};
    //   dispatch_apply_f(entries.size(), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &context,
    //                    build_bundle_entry);
    // #elif defined(__cpp_lib_execution) && (__cpp_lib_execution >= 201603L)
    //   std::transform(
    //       std::execution::par_unseq,
    //       fixture.bundle.begin(),
    //       fixture.bundle.end(),
    //       entries.begin(),
    //       [&builder](const BundlePatient& item) -> BundleentryData {
    //         auto patient_handle = builder.append_obj(PatientData{});
    //         assign::assign_patient(item.patient, patient_handle);
    //         return BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
    //       });
    // #else
    std::transform(
        fixture.bundle.begin(),
        fixture.bundle.end(),
        entries.begin(),
        [&builder](const BundlePatient &item) -> BundleentryData
        {
          auto patient_handle = builder.append_obj(PatientData{});
          assign::assign_patient(item.patient, patient_handle);
          return BundleentryData{.resource = static_cast<ResourceReference>(patient_handle)};
        });
    // #endif
    std::vector<const ObservationData *> observation_ptrs;
    observation_ptrs.reserve(total_observation_count);
    for (const auto &item : fixture.bundle)
    {
      for (const auto &observation : item.observations)
      {
        observation_ptrs.push_back(&observation);
      }
    }

    std::vector<BundleentryData> observation_entries(observation_ptrs.size());
    // #if defined(__APPLE__)
    //   ObservationBuildContext observation_context{&builder, &observation_ptrs, &observation_entries};
    //   dispatch_apply_f(observation_entries.size(),
    //                    dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
    //                    &observation_context,
    //                    build_observation_entry);
    // #elif defined(__cpp_lib_execution) && (__cpp_lib_execution >= 201603L)
    //   std::transform(
    //       std::execution::par_unseq,
    //       observation_ptrs.begin(),
    //       observation_ptrs.end(),
    //       observation_entries.begin(),
    //       [&builder](const ObservationData* observation) -> BundleentryData {
    //         auto observation_handle = builder.append_obj(ObservationData{});
    //         assign::assign_observation(*observation, observation_handle);
    //         return BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)};
    //       });
    // #else
    std::transform(
        observation_ptrs.begin(),
        observation_ptrs.end(),
        observation_entries.begin(),
        [&builder](const ObservationData *observation) -> BundleentryData
        {
          auto observation_handle = builder.append_obj(ObservationData{});
          assign::assign_observation(*observation, observation_handle);
          return BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)};
        });
    // #endif

    bundle.entry = std::move(entries);
    if (!observation_entries.empty())
    {
      bundle.entry.insert(bundle.entry.end(),
                          std::make_move_iterator(observation_entries.begin()),
                          std::make_move_iterator(observation_entries.end()));
    }

    const auto root = builder.append_obj(bundle);
    builder.set_root(root);
    (void)builder.finalize(FF_CHECKSUM_SHA256,
                           [](const unsigned char *, size_t) -> std::vector<BYTE>
                           {
                             return std::vector<BYTE>(32);
                           });
    out.metrics.push_back({"fastfhir", Stage::Test1Serialize, test1_timer.stop_ns()});

    Timer test2_timer;
    test2_timer.start();
    const auto materialized = test_2::materialize(payload_memory);
    out.metrics.push_back(test_2::materialize_metric("fastfhir", test2_timer.stop_ns()));
    (void)materialized;

    Timer test3_timer;
    test3_timer.start();
    const auto query_summary = test_3::query(payload_memory.view());
    out.metrics.push_back({"fastfhir", Stage::Test3Query, test3_timer.stop_ns()});
    out.queried_value = test_3::format_query_summary(query_summary);

    auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload_memory, enrichment_observation_fixture());
    out.metrics.push_back(test_4::enrich_metric("fastfhir", enrich_result.summary.duration_ns));
    out.enriched_stream = std::move(enrich_result.enriched_stream);
    out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);
    return out;
  }

} // namespace bench
