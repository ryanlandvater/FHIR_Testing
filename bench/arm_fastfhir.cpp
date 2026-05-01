#include "harness.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace bench {

std::vector<MetricEvent> run_fastfhir_smoke() {
  std::vector<MetricEvent> events;
  events.reserve(2);

  const std::array<std::pair<const char*, const char*>, 5> fields = {{{"resourceType", "Patient"},
                                                                       {"id", "patient-1"},
                                                                       {"gender", "male"},
                                                                       {"birthDate", "1990-03-21"},
                                                                       {"cholesterol_mg_dl", "183"}}};

  // Stage 1 start: immediately before the first field write.
  Timer stage1;
  std::string payload;
  payload.reserve(256);
  stage1.start();
  payload += "FFHR|";
  for (const auto& [key, value] : fields) {
    payload += key;
    payload += "=";
    payload += value;
    payload += ";";
  }
  // Stage 1 end: payload is sealed for read-side use.
  events.push_back(MetricEvent{"fastfhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  // Stage 3 start: first parser/read call that consumes bytes for query.
  Timer stage3;
  stage3.start();
  const std::string marker = "cholesterol_mg_dl=";
  const auto marker_pos = payload.find(marker);
  std::string extracted;
  if (marker_pos != std::string::npos) {
    const auto start = marker_pos + marker.size();
    const auto end = payload.find(';', start);
    extracted = payload.substr(start, end - start);
  }
  // Stage 3 end: target value extracted.
  events.push_back(MetricEvent{"fastfhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  if (extracted.empty()) {
    // Keep the extracted value visible to the optimizer while preserving timing output.
    events.back().duration_us = std::max<std::int64_t>(events.back().duration_us, 1);
  }

  return events;
}

}  // namespace bench
