#pragma once

// ---------------------------------------------------------------------------
// Test 2 -- random access (TASKS.md IN-B / WF-1.1)
// ---------------------------------------------------------------------------
// The receiver-side stage. It replaced the former Test 2 (materialize) on
// 2026-08-26 (TASKS.md D4): a full traversal in LAYOUT order is a contiguous
// tape's best case (sequential prefetch, no indirection) and an offset-indexed
// layout's worst, and no consumer reads a bundle in write order -- you jump to
// the resources you care about. Measured in layout order simdjson wins by ~22x
// per node; measured out of order FastFHIR wins by ~2,500x. Both are true, and
// publishing either alone misrepresents the result. The walk was retired
// because it is not how medical data is retrieved; this stage is.
//
// So: pick N random Bundle.entry ordinals, navigate to each ONE FROM THE ROOT,
// and read the resource's id. Every lookup pays its own path cost, which is
// exactly the asymmetry the claim is about:
//
//   FastFHIR   offset arithmetic from the root                  -> O(1)
//   simdjson   dom::array::at(i) iterates from element 0        -> O(i)
//   protobuf   scan i length-prefixed TLV records, then parse   -> O(i)
//   HL7v2      scan forward for the i-th MSH, then parse        -> O(i)
//
// GRANULARITY WARNING for the HL7v2 arm: a v2 batch has no resource-level
// index. Its addressable unit is the MESSAGE -- 5 ORU messages carry the same
// 1,473 resources the other three arms address individually. So its ns/read is
// a scan over a much smaller ordinal space and is NOT the same operation. That
// difference is a finding about the format, not a defect in the probe, and it
// has to be stated wherever this stage is reported.
//
// The three scan formats are not being sandbagged: none of them HAS an O(1)
// index into a serialized document. That is the point of the comparison.
//
// PARITY GATE: every arm returns the total bytes of id read, and the harness
// compares them across arms. Identical accumulators are what makes this a
// measurement rather than four unrelated loops -- without that check an arm
// that silently read nothing would look infinitely fast, which is precisely the
// failure notes.md section 2 documents.

#include "harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(ARM_FASTFHIR)
#include <FF_Bundle.hpp>
#elif defined(ARM_JSON)
#include <simdjson.h>
#elif defined(ARM_HL7V2)
#include "hl7v2_message.hpp"
#elif defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#endif

#include "bench_test_1.hpp"

// Per-arm namespace -- REQUIRED FOR CORRECTNESS. See bench_test_4.hpp for the
// full account of the ODR violation this prevents.
#ifndef BENCH_ARM_NS
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
#elif defined(ARM_HL7V2)
#define BENCH_ARM_NS arm_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#define BENCH_ARM_NS arm_google_fhir
#else
#define BENCH_ARM_NS arm_none
#endif
#endif

namespace bench::test_2 {
inline namespace BENCH_ARM_NS {

struct RandomAccessSummary {
  std::int64_t duration_ns = 0;
  std::int64_t reads = 0;
  std::int64_t bytes_read = 0;   // cross-arm parity gate
  std::int64_t entries_seen = 0;
};

// Deterministic by construction: two runs must probe the same ordinals or the
// numbers are not comparable (notes.md section 5).
inline std::vector<std::size_t> pick_targets(std::size_t entry_count, std::size_t n_reads) {
  std::vector<std::size_t> targets;
  if (entry_count == 0) return targets;
  std::mt19937 rng(20260826u);
  std::uniform_int_distribution<std::size_t> pick(0, entry_count - 1);
  targets.resize(n_reads);
  for (auto& t : targets) t = pick(rng);
  return targets;
}

inline std::size_t default_reads() {
  if (const char* n = std::getenv("BENCH_RANDOM_READS")) {
    const auto v = std::strtoul(n, nullptr, 10);
    if (v > 0) return v;
  }
  return 2000;
}

inline MetricEvent random_access_metric(std::string_view arm, const RandomAccessSummary& s) {
  // Field order is {arm, stage, duration_ns, bytes_in, bytes_out, ops, entries}.
  return MetricEvent{std::string(arm), Stage::Test2RandomAccess, s.duration_ns,
                     /*bytes_in=*/0, /*bytes_out=*/s.bytes_read, /*ops=*/s.reads,
                     /*entries=*/s.entries_seen};
}

inline std::string format_random_access_summary(const RandomAccessSummary& s) {
  if (std::getenv("BENCH_RA_DEBUG"))
    std::fprintf(stderr, "[ra] entries_seen=%lld reads=%lld bytes=%lld\n",
                 (long long)s.entries_seen, (long long)s.reads, (long long)s.bytes_read);
  return "reads=" + std::to_string(s.reads) + " entries=" + std::to_string(s.entries_seen) +
         " bytes_read=" + std::to_string(s.bytes_read) + " ns_per_read=" +
         std::to_string(s.reads ? s.duration_ns / s.reads : 0);
}

#if defined(ARM_FASTFHIR)

inline RandomAccessSummary random_access(const FastFHIR::Memory& payload) {
  RandomAccessSummary summary;
  FastFHIR::Parser parser(payload);
  auto root = parser.root();
  if (!root) return summary;

  auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY].as_node();
  if (!entries) return summary;
  summary.entries_seen = static_cast<std::int64_t>(entries.size());

  const auto targets = pick_targets(entries.size(), default_reads());
  if (targets.empty()) return summary;

  Timer timer;
  timer.start();
  std::size_t acc = 0;
  for (const std::size_t i : targets) {
    auto entry = entries[i];
    if (!entry) continue;
    auto resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE].as_node();
    if (!resource) continue;
    // as<std::string_view>() is the call that touches the arena. Node::size()
    // and Node::kind() return cached members and would measure nothing -- an
    // earlier version of this probe did exactly that and reported a 26x speedup
    // that was entirely an artefact.
    auto id = resource.is<FastFHIR::RESOURCETYPE::PATIENT>()
                  ? resource[FastFHIR::Fields::PATIENT::ID]
                  : resource[FastFHIR::Fields::OBSERVATION::ID];
    if (!id) {
      if (std::getenv("BENCH_RA_DEBUG"))
        std::fprintf(stderr, "[ra] entry %zu: no id (recovery=%u)\n", i,
                     (unsigned)resource.recovery());
      continue;
    }
    const auto sv = id.as_node().as<std::string_view>();
    if (std::getenv("BENCH_RA_DEBUG") && sv.size() != 36)
      std::fprintf(stderr, "[ra] entry %zu: id len=%zu '%.*s' recovery=%u\n", i, sv.size(),
                   (int)sv.size(), sv.data(), (unsigned)resource.recovery());
    acc += sv.size();
  }
  summary.duration_ns = timer.stop_ns();
  summary.reads = static_cast<std::int64_t>(targets.size());
  summary.bytes_read = static_cast<std::int64_t>(acc);
  return summary;
}

#elif defined(ARM_JSON)

inline RandomAccessSummary random_access(const std::string& payload) {
  RandomAccessSummary summary;
  simdjson::dom::parser parser;
  auto doc = parser.parse(payload);
  if (doc.error()) return summary;

  auto root = doc.value_unsafe();
  auto entries = root["entry"];
  if (entries.error()) return summary;

  std::size_t count = 0;
  for (auto e : entries.get_array()) { (void)e; ++count; }
  summary.entries_seen = static_cast<std::int64_t>(count);

  const auto targets = pick_targets(count, default_reads());
  if (targets.empty()) return summary;

  // Hoisted out of the loop: this is the fairest implementation available to a
  // real consumer. at(i) remains O(i) because a simdjson DOM has no O(1) index
  // -- that is the property under test, not a handicap imposed by the harness.
  auto entry_array = entries;

  Timer timer;
  timer.start();
  std::size_t acc = 0;
  for (const std::size_t i : targets) {
    auto entry = entry_array.at(i);
    if (entry.error()) continue;
    auto resource = entry["resource"];
    if (resource.error()) continue;
    std::string_view sv;
    if (!resource["id"].get_string().get(sv)) acc += sv.size();
  }
  summary.duration_ns = timer.stop_ns();
  summary.reads = static_cast<std::int64_t>(targets.size());
  summary.bytes_read = static_cast<std::int64_t>(acc);
  return summary;
}

#elif defined(ARM_GOOGLE_FHIR)

inline uint32_t decode_u32_le_t5(const char* p) {
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

inline RandomAccessSummary random_access(const std::string& payload) {
  RandomAccessSummary summary;

  // Count records first (untimed) so the probe knows the ordinal range.
  std::size_t count = 0;
  for (std::size_t pos = 0; pos + 5 <= payload.size();) {
    const uint32_t len = decode_u32_le_t5(payload.data() + pos + 1);
    pos += 5;
    if (pos + len > payload.size()) break;
    pos += len;
    ++count;
  }
  summary.entries_seen = static_cast<std::int64_t>(count);

  const auto targets = pick_targets(count, default_reads());
  if (targets.empty()) return summary;

  google::fhir::r4::core::Patient patient;
  google::fhir::r4::core::Observation observation;

  Timer timer;
  timer.start();
  std::size_t acc = 0;
  for (const std::size_t target : targets) {
    // No index into a length-prefixed stream: reaching record i means walking
    // the i preceding length prefixes. O(i), same as every other scan format.
    std::size_t pos = 0;
    std::size_t idx = 0;
    while (pos + 5 <= payload.size()) {
      const char record_type = payload[pos];
      const uint32_t len = decode_u32_le_t5(payload.data() + pos + 1);
      pos += 5;
      if (pos + len > payload.size()) break;
      if (idx == target) {
        const char* data = payload.data() + pos;
        if (record_type == 'P') {
          patient.Clear();
          if (patient.ParseFromArray(data, static_cast<int>(len)) && patient.has_id()) {
            acc += patient.id().value().size();
          }
        } else {
          observation.Clear();
          if (observation.ParseFromArray(data, static_cast<int>(len)) && observation.has_id()) {
            acc += observation.id().value().size();
          }
        }
        break;
      }
      pos += len;
      ++idx;
    }
  }
  summary.duration_ns = timer.stop_ns();
  summary.reads = static_cast<std::int64_t>(targets.size());
  summary.bytes_read = static_cast<std::int64_t>(acc);
  return summary;
}

#elif defined(ARM_HL7V2)

inline RandomAccessSummary random_access(const std::string& payload) {
  RandomAccessSummary summary;

  // HL7v2 terminates segments with \r, not \n -- a first cut of this probe
  // split on \n, found no PID, and read 0 bytes. The cross-arm accumulator
  // check is what caught it: three arms agreed on 18,000 bytes and this one
  // reported 0, which would have been a fast, meaningless number.
  //
  // Boundaries are located untimed; a reader that already has the batch in
  // memory would know them. The timed part is the scan to the target message,
  // which is the O(i) cost under test.
  const auto starts = hl7v2::find_message_starts(payload);

  // entries_seen counts RESOURCES, not messages. HL7v2 batches one ORU^R01 per
  // patient with that patient's results as OBX segments inside it, so
  // starts.size() is 1 where every other arm reports 317 -- an addressing
  // difference, not a content one, but the cross-arm entry gate compares
  // content and would read it as this arm having lost 316 resources.
  // Random access still ADDRESSES messages (that is the format's unit); this
  // number says what the batch contains.
  std::int64_t obx = 0;
  for (std::size_t at = payload.find("\rOBX|"); at != std::string::npos;
       at = payload.find("\rOBX|", at + 1))
    ++obx;
  summary.entries_seen = static_cast<std::int64_t>(starts.size()) + obx;

  const auto targets = pick_targets(starts.size(), default_reads());
  if (targets.empty()) return summary;

  Timer timer;
  timer.start();
  std::size_t acc = 0;
  for (const std::size_t target : targets) {
    // Walk forward to the target message rather than indexing `starts`: a
    // reader consuming a serialized batch has no index into it.
    std::size_t idx = 0;
    std::size_t pos = 0;
    while (pos + 3 < payload.size()) {
      if (!hl7v2::is_message_start(payload, pos)) {
        ++pos;
        continue;
      }
      if (idx == target) {
        const std::size_t msg_end =
            (target + 1 < starts.size()) ? starts[target + 1] : payload.size();
        std::string_view msg(payload.data() + pos, msg_end - pos);
        for (std::size_t p = 0; p < msg.size();) {
          const std::size_t e = msg.find('\r', p);
          const std::size_t line_end = (e == std::string_view::npos ? msg.size() : e);
          const std::string_view seg = msg.substr(p, line_end - p);
          if (seg.rfind("PID", 0) == 0) {
            // Use the arm's own PidView rather than a hand-rolled field index:
            // parse_segment_line() keeps the segment name in Segment::name, so
            // fields[] is shifted by one and PID-3 is NOT fields[3]. Getting
            // that wrong is what made this probe read 0 bytes twice.
            const auto parsed = hl7v2::parse_segment_line(seg);
            acc += hl7v2::PidView(parsed).patient_id().size();
            break;
          }
          if (e == std::string_view::npos) break;
          p = e + 1;
        }
        break;
      }
      ++idx;
      ++pos;
    }
  }
  summary.duration_ns = timer.stop_ns();
  summary.reads = static_cast<std::int64_t>(targets.size());
  summary.bytes_read = static_cast<std::int64_t>(acc);
  return summary;
}

#endif

}  // inline namespace BENCH_ARM_NS
}  // namespace bench::test_2
