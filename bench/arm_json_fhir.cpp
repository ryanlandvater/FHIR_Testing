#include "harness.hpp"

#include <algorithm>
#include <string>

namespace bench {

std::vector<MetricEvent> run_json_fhir_smoke() {
  std::vector<MetricEvent> events;
  events.reserve(2);

  // Stage 1 start: immediately before the first JSON field write.
  Timer stage1;
  std::string json;
  json.reserve(256);
  stage1.start();
  json += "{";
  json += "\"resourceType\":\"Patient\",";
  json += "\"id\":\"patient-1\",";
  json += "\"gender\":\"male\",";
  json += "\"birthDate\":\"1990-03-21\",";
  json += "\"cholesterol_mg_dl\":183";
  json += "}";
  // Stage 1 end: complete UTF-8 JSON text is available.
  events.push_back(MetricEvent{"json_fhir", Stage::Stage1Serialize, std::max<std::int64_t>(stage1.stop_us(), 1)});

  // Stage 3 start: first read/traversal operation on received representation.
  Timer stage3;
  stage3.start();
  const std::string marker = "\"cholesterol_mg_dl\":";
  const auto marker_pos = json.find(marker);
  int cholesterol = -1;
  if (marker_pos != std::string::npos) {
    const auto start = marker_pos + marker.size();
    const auto end = json.find_first_of(",}", start);
    cholesterol = std::stoi(json.substr(start, end - start));
  }
  // Stage 3 end: target value extracted into result variable.
  events.push_back(MetricEvent{"json_fhir", Stage::Stage3Query, std::max<std::int64_t>(stage3.stop_us(), 1)});

  if (cholesterol < 0) {
    events.back().duration_us = std::max<std::int64_t>(events.back().duration_us, 1);
  }

  return events;
}

}  // namespace bench
