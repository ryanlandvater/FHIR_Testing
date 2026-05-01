#include "harness.hpp"

// HL7v2 arm — smoke test skeleton.
// Full implementation requires the jcomellas/hl7parser library.
// See BENCHMARK_IMPLEMENTATION_CHECKLIST.md §6 and FFHRnotes.md for integration plan.
// Comparisons are limited to workflows naturally representable in HL7v2 without
// nonstandard segments (see Study Design §1.3).
//
// TIMED SECTION BOUNDARIES (per Study Design §1.4):
//   Stage 1 start : immediately before first character written to the HL7v2 message string
//   Stage 1 end   : after final segment delimiter appended, before any transport
//   Stage 3 start : immediately before hl7parser parse call on message bytes
//   Stage 3 end   : after PID.7 birthdate + OBX.5 cholesterol value extracted

namespace bench {

ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture) {
    ArmRunResult result;
    result.metrics.reserve(2);

    const std::size_t n = fixture.bundle.size();

    // --- Stage 1: Serialization (smoke) ---
    // TODO(hl7v2): replace busy-loop with per-patient ORU^R01 message construction:
    //   std::string msg;
    //   msg += "MSH|^~\\&|BENCH|BENCH|BENCH|BENCH|<ts>||ORU^R01|<id>|P|2.5\r";
    //   msg += "PID|1||" + patient_id + "||||" + birthdate + "|" + gender + "\r";
    //   for each cholesterol observation parsed from the message payload:
    //     msg += "OBX|1|NM|2085-9^Total Cholesterol^LN||" + value + "|mg/dL||||F\r";
    // TODO(stage2): asio::async_write(socket, asio::buffer(msg));
    Timer stage1;
    stage1.start();
    volatile std::size_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // Simulate per-patient MSH + PID segment construction.
        for (std::size_t j = 0; j < 12; ++j) acc += (i + j);
    }
    (void)acc;
    result.metrics.push_back(
            MetricEvent{"hl7v2", Stage::Stage1Serialize,
                                    std::max<std::int64_t>(stage1.stop_us(), 1)});

    // Stage 2 stub — zero duration until Asio transport is implemented.
    result.metrics.push_back(MetricEvent{"hl7v2", Stage::Stage2Transport, 0});

    // --- Stage 3: Query / Traversal (smoke) ---
    // TODO(hl7v2): replace with hl7parser calls:
    //   hl7_msg_t* msg = hl7_msg_create(raw_bytes, length);
    //   const char* dob   = hl7_msg_get_field(msg, "PID", 7);  // date of birth
    //   iterate OBX segments: find OBX.3 == "2085-9", read OBX.5 (value)
    //   hl7_msg_destroy(msg);
    Timer stage3;
    stage3.start();
    volatile std::size_t acc3 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // Simulate per-message PID.7 + OBX segment traversal.
        for (std::size_t j = 0; j < 9; ++j) acc3 += (i + j);
    }
    (void)acc3;
    result.queried_value = "smoke:hl7v2 patients=" + std::to_string(n);
    result.metrics.push_back(
            MetricEvent{"hl7v2", Stage::Stage3Query,
                                    std::max<std::int64_t>(stage3.stop_us(), 1)});

    return result;
}

}  // namespace bench

