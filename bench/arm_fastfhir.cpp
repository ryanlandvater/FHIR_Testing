#include "harness.hpp"

#include <FF_Bundle.hpp>

#include <algorithm>
#include <sstream>
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

  // ---------------------------------------------------------------------
  // Why this arm calls append_obj(POCO) instead of the shared assignment layer
  // ---------------------------------------------------------------------
  // The shared, macro-guarded layer assembles a resource field-by-field:
  // append an EMPTY resource, then amend each slot. For arrays of FHIR
  // datatypes that is not expressible through FastFHIR's public API.
  //
  // Observation.category (and identifier, interpretation, note, ...) is stored
  // as FF_ARRAY::INLINE_BLOCK: entry i is a fixed-size block header at
  // entries_start + i*HEADER_SIZE, with its variable-length tail written
  // elsewhere in child space. The generator emits that with the four-argument
  // STORE_FF_<TYPE>(base, header_offset, child_offset, data) overload.
  //
  // The bench's stream_assign_array_offsets() instead wrote a vector<Offset>.
  // The reader then walked those 8-byte offsets as if they were inline
  // CodeableConcept headers and dereferenced payload text as a string offset:
  //   ASan: SEGV in FF_STRING::read_view <- FF_CODEABLECONCEPT::deserialize
  //         <- FF_OBSERVATION::deserialize <- Node::as<ObservationData>()
  // validate_FFHR_stream() did NOT catch it -- the stream is self-consistent by
  // its rules; only the generated deserializer walks the array as blocks.
  //
  // TypeTraits<T> exposes only the self-contained three-argument store, so a
  // generic inline-block array writer cannot be built on the public API. Doing
  // it properly means per-type dispatch to the four-argument overloads, i.e.
  // re-implementing part of the generator.
  //
  // So this arm hands the whole POCO to the generated STORE, which lays out
  // every array correctly. PARITY COST: this arm now serializes EVERY field of
  // the POCO, while the other three arms serialize only the ~25 fields the
  // shared assignment layer covers. That is more work, not less, so it biases
  // against FastFHIR -- but it is not parity. See notes.md.
  // ---------------------------------------------------------------------
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
    const FastFHIR::FF_Stream stream = make_stream(payload_memory, FHIR_VERSION_R5);
    FastFHIR::Builder &builder = *stream;

    Timer test1_timer;
    test1_timer.start();

    BundleData bundle{};
    bundle.type = FF_BundleType::Collection;

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
          auto patient_handle = builder.append_obj(item.patient);
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
          auto observation_handle = builder.append_obj(*observation);
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
    (void)seal_stream(stream, root, "fastfhir arm bundle");
    const std::int64_t test1_ns = test1_timer.stop_ns();
    out.metrics.push_back({"fastfhir", Stage::Test1Serialize, test1_ns});

    if (std::getenv("BENCH_VALIDATE"))
    {
      FastFHIR::Parser check(payload_memory);
      const FF_Result vr = check.validate_FFHR_stream();
      std::fprintf(stderr, "[validate] fastfhir arm stream: code=%d %s\n",
                   (int)vr.code, vr.message.c_str());
      // Does the REFLECTIVE reader survive this stream, even though the
      // generated deserializer does not?
      std::ostringstream sink;
      try {
        check.print_json(sink);
        std::fprintf(stderr, "[validate] print_json ok, %zu bytes\n", sink.str().size());
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "[validate] print_json threw: %s\n", ex.what());
      }
      // And what does the bundle actually contain?
      auto rn = check.root();
      if (auto es = rn[FastFHIR::Fields::BUNDLE::ENTRY]) {
        std::size_t n_pat = 0, n_obs = 0, n_other = 0, n_null = 0;
        for (auto& e : es.entries()) {
          auto r = e[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
          if (!r) { ++n_null; continue; }
          auto node = r.as_node();
          if (!node) { ++n_null; continue; }
          if (node.is<FastFHIR::RESOURCETYPE::PATIENT>()) ++n_pat;
          else if (node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) ++n_obs;
          else ++n_other;
        }
        std::fprintf(stderr, "[validate] entries: patient=%zu observation=%zu other=%zu null=%zu\n",
                     n_pat, n_obs, n_other, n_null);
      }
    }


    Timer test2_timer;
    test2_timer.start();
    const auto materialized = test_2::materialize(payload_memory);
    out.metrics.push_back(test_2::materialize_metric("fastfhir", test2_timer.stop_ns()));
    // Observe the walk result. Without this the compiler is free to elide
  // touch_tree() entirely -- the FastFHIR arm reported 83 ns for a 317-entry
  // bundle before this check existed, which was a dead-code artefact rather
  // than a zero-copy result. See notes.md section 2.
  if (materialized.touched_nodes == 0 && materialized.ok) {
    std::cerr << "[warn] materialize touched 0 nodes\n";
  }
  if (std::getenv("BENCH_TOUCHED")) {
    std::cerr << "[touched] " << out.metrics.back().arm << " nodes="
              << materialized.touched_nodes << " ok=" << materialized.ok << "\n";
  }

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
