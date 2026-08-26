#include "harness.hpp"

#include <FF_Bundle.hpp>
#include <FF_Compactor.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
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
#include "walk_diagnostic.hpp"
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
    // Wire size is read AFTER the clock stops -- nothing goes between the last
    // real operation and stop_ns() (notes.md section 6).
    const std::int64_t test1_bytes = static_cast<std::int64_t>(payload_memory.view().size());
    out.metrics.push_back({"fastfhir", Stage::Test1Serialize, test1_ns, 0, test1_bytes});
    // --dump-artifacts input (see main.cpp): the sealed wire bytes.
    {
      const auto v = payload_memory.view();
      out.test1_payload.assign(v.data(), v.size());
    }

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

    // FastFHIR-native compact archive (WF-1.4 / IN-E). Compaction runs once
    // per sample; the arena and its losslessness verdict stay alive so the
    // same probes run over the compact stream (test_2_compact / test_3_compact
    // / test_4_compact) -- the claim under test is that the reader is
    // layout-agnostic, i.e. compact ≈ standard speed.
    //
    // Gated on losslessness: the compact stream must re-parse to JSON
    // semantically identical to the standard stream's, or no compact row is
    // emitted -- the upstream compactor silently dropped scalar arrays until
    // 459e8d8, and no compact number leaves without the gate (handoff.md
    // Instrument E, upstream I3.7).
    FastFHIR::Memory compact_mem;
    FastFHIR::Memory::View compact_view{};
    bool compact_lossless = false;
    {
      const FastFHIR::Memory::View standard_view = payload_memory.view();
      compact_mem =
          FastFHIR::Memory::create(std::max<std::size_t>(standard_view.size() * 2, 1));
      Timer compact_timer;
      compact_timer.start();
      compact_view = FastFHIR::Compactor::archive(FastFHIR::Parser(payload_memory), compact_mem,
                                                  FF_CHECKSUM_NONE);
      const std::int64_t compact_ns = compact_timer.stop_ns();

      // Semantic JSON equality (nlohmann), not string equality: the two
      // layouts may legitimately order fields differently, and a false-negative
      // gate would silently suppress every compact number.
      bool lossless = false;
      try
      {
        nlohmann::json std_json, cmp_json;
        {
          std::ostringstream sink;
          FastFHIR::Parser(payload_memory).print_json(sink);
          std_json = nlohmann::json::parse(sink.str());
        }
        {
          std::ostringstream sink;
          FastFHIR::Parser(compact_view.data(), compact_view.size()).print_json(sink);
          cmp_json = nlohmann::json::parse(sink.str());
        }
        lossless = (std_json == cmp_json);
      }
      catch (const std::exception &ex)
      {
        std::fprintf(stderr, "[warn] compact losslessness probe threw: %s\n", ex.what());
      }
      compact_lossless = lossless;

      if (lossless)
      {
        out.metrics.push_back({"fastfhir", Stage::Test1Compact, compact_ns, 0,
                               static_cast<std::int64_t>(compact_view.size())});
      }
      else
      {
        std::fprintf(
            stderr,
            "[warn] compact losslessness FAILED -- compact size not emitted (IN-E gate)\n");
      }
    }

    Timer test3_timer;
    test3_timer.start();
    const auto query_summary = test_3::query(payload_memory.view());
    out.metrics.push_back({"fastfhir", Stage::Test3Query, test3_timer.stop_ns()});
    out.queried_value = test_3::format_query_summary(query_summary);

    // Test 3 over the compact archive (test_3_compact). Same census, same
    // lens reads -- the reader dispatches on the stream layout, so this must
    // come out ~equal in speed and IDENTICAL in answers. A query-summary
    // mismatch would mean the compact stream lost content the print_json
    // gate could not see.
    if (compact_lossless)
    {
      try
      {
        Timer t3c_timer;
        t3c_timer.start();
        const auto compact_query =
            test_3::query(std::string_view(compact_view.data(), compact_view.size()));
        const std::int64_t t3c_ns = t3c_timer.stop_ns();
        if (test_3::format_query_summary(compact_query) != out.queried_value)
        {
          std::fprintf(stderr, "[warn] compact query summary differs from standard!\n");
        }
        out.metrics.push_back({"fastfhir", Stage::Test3QueryCompact, t3c_ns});
      }
      catch (const std::exception &ex)
      {
        std::fprintf(stderr, "[warn] test_3_compact failed: %s\n", ex.what());
      }
    }

  // Full-traversal walk -- DIAGNOSTIC ONLY, off unless BENCH_WALK=1, never a
  // reported stage. Retained so upstream CAPI-7 / CAPI-8 stay reproducible;
  // D4 retired the walk as a stage, not as evidence.
  (void)walk_diag::run(payload_memory);

  // Test 2 -- random access (IN-B / WF-1.1). Out-of-order reads, navigating
  // from the root each time; the retired materialize walk read in layout
  // order, and the two disagree by three orders of magnitude.
  //
  // MUST run BEFORE Test 4. FastFHIR::Memory is a shared_ptr handle, so the
  // enrich appends into this very arena (PA-9) -- running Test 2 afterwards
  // had the FastFHIR arm reading 1,474 entries while the other arms read
  // 1,473, and the cross-arm byte gate caught it.
  {
    const auto ra = test_2::random_access(payload_memory);
    out.metrics.push_back(test_2::random_access_metric("fastfhir", ra));
    out.random_access_summary = test_2::format_random_access_summary(ra);
  }

  // Random access over the compact archive (test_2_compact). Same lens reads;
  // the byte accumulators are NOT cross-arm-gated here (the gate covers the
  // standard stage) -- the reader is the same, which is what makes the two
  // comparable, and the query cross-check above guards content.
  if (compact_lossless)
  {
    try
    {
      Timer t2c_timer;
      t2c_timer.start();
      const auto compact_ra = test_2::random_access(compact_mem);
      const std::int64_t t2c_ns = t2c_timer.stop_ns();
      out.metrics.push_back({"fastfhir", Stage::Test2RandomAccessCompact, t2c_ns,
                             /*bytes_in=*/0, /*bytes_out=*/compact_ra.bytes_read,
                             /*ops=*/compact_ra.reads});
    }
    catch (const std::exception &ex)
    {
      std::fprintf(stderr, "[warn] test_2_compact failed: %s\n", ex.what());
    }
  }

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload_memory, enrichment_observation_fixture());
    out.metrics.push_back(test_4::enrich_metric("fastfhir", enrich_result.summary));
    out.enriched_stream = std::move(enrich_result.enriched_stream);
    out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

    // Enrich over the compact archive (test_4_compact). The API REFUSES this
    // by design -- "Cannot open Builder on a compact archive. Decompact to a
    // standard stream before append/mutation" -- so there is no row to emit
    // (0 would claim N/A per the MetricEvent contract but break the duration
    // gate). Attempting it anyway is the instrument: it verifies the
    // write-once property still holds. CAPI-10 tracks the undocumented
    // immutability.
    if (compact_lossless)
    {
      try
      {
        auto compact_enrich =
            test_4::BENCH_TEST_4_ENRICH_FN(compact_mem, enrichment_observation_fixture());
        out.metrics.push_back({"fastfhir", Stage::Test4EnrichCompact,
                               compact_enrich.summary.duration_ns,
                               static_cast<std::int64_t>(compact_enrich.summary.source_bytes),
                               static_cast<std::int64_t>(compact_enrich.summary.enriched_bytes)});
      }
      catch (const std::exception &ex)
      {
        static bool logged = false;
        if (!logged)
        {
          std::fprintf(stderr,
                       "[compact] test_4_compact: API refuses to open a Builder on a compact "
                       "archive (write-once format) -- no row emitted, by design: %s\n",
                       ex.what());
          logged = true;
        }
      }
    }
    return out;
  }

} // namespace bench
