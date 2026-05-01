#include "harness.hpp"

// Google FHIR arm — smoke test skeleton.
// Full implementation requires the google/fhir C++ library (Abseil + Protobuf).
// See BENCHMARK_IMPLEMENTATION_CHECKLIST.md §5 and FFHRnotes.md for integration plan.
//
// TIMED SECTION BOUNDARIES (per Study Design §1.4):
//   Stage 1 start : immediately before first field written to proto representation
//   Stage 1 end   : immediately after bundle proto is fully populated / wire bytes available
//   Stage 3 start : immediately before first parse / proto access call
//   Stage 3 end   : after birthDate + cholesterol value extracted into local variables

namespace bench {

ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture) {
    ArmRunResult result;
    result.metrics.reserve(2);

    const std::size_t n = fixture.patients.size();

    // --- Stage 1: Serialization (smoke) ---
    // TODO(google_fhir): replace busy-loop with per-patient proto construction:
    //   google::fhir::r4::Patient proto_patient;
    //   proto_patient.set_id(p.patient.id);
    //   proto_patient.mutable_birth_date()->...
    //   for each obs: add Observation submessage with coding + valueQuantity
    //   google::fhir::PrintFhirToJsonString(bundle_proto, &wire_json);
    // TODO(stage2): asio::async_write(socket, asio::buffer(wire_json));
    Timer stage1;
    stage1.start();
    volatile std::size_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // Simulate per-patient proto field assignments.
        for (std::size_t j = 0; j < 10; ++j) acc += (i + j);
    }
    (void)acc;
    result.metrics.push_back(
            MetricEvent{"google_fhir", Stage::Stage1Serialize,
                                    std::max<std::int64_t>(stage1.stop_us(), 1)});

    // Stage 2 stub — zero duration until Asio transport is implemented.
    result.metrics.push_back(MetricEvent{"google_fhir", Stage::Stage2Transport, 0});

    // --- Stage 3: Query / Traversal (smoke) ---
    // TODO(google_fhir): replace with:
    //   auto bundle_proto = JsonFhirStringToProto<r4::Bundle>(wire_json);
    //   for each entry: if Patient → access birth_date()
    //                   if Observation → match coding.code() == "2085-9" → value_quantity().value()
    Timer stage3;
    stage3.start();
    volatile std::size_t acc3 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // Simulate per-patient proto field reads.
        for (std::size_t j = 0; j < 8; ++j) acc3 += (i + j);
    }
    (void)acc3;
    result.queried_value = "smoke:google_fhir patients=" + std::to_string(n);
    result.metrics.push_back(
            MetricEvent{"google_fhir", Stage::Stage3Query,
                                    std::max<std::int64_t>(stage3.stop_us(), 1)});

    return result;
}

}  // namespace bench

