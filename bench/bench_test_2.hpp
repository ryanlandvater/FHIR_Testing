#pragma once

#include "harness.hpp"

#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(ARM_JSON)
#include <simdjson.h>
#elif defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#endif

namespace bench::test_2 {

inline MetricEvent materialize_metric(std::string_view arm, std::int64_t duration_ns) {
  return MetricEvent{std::string(arm), Stage::Test2Materialize, duration_ns};
}

#if defined(ARM_HL7V2)
struct Hl7Field {
  std::string value;
};

struct Hl7Segment {
  std::string id;
  std::vector<Hl7Field> fields;
};
#endif

struct MaterializedTree {
  #if defined(ARM_FASTFHIR)
  std::unique_ptr<FastFHIR::Parser> parser;
  FastFHIR::Reflective::Node root;
  #elif defined(ARM_JSON)
  std::unique_ptr<simdjson::dom::parser> parser;
  simdjson::dom::element root;
  #elif defined(ARM_GOOGLE_FHIR)
  std::vector<google::fhir::r4::core::Patient> patients;
  std::vector<google::fhir::r4::core::Observation> observations;
  #elif defined(ARM_HL7V2)
  std::vector<Hl7Segment> segments;
  #endif  
  std::size_t touched_nodes = 0;
  bool ok = false;
};



#if defined(ARM_FASTFHIR)

using StreamType = FastFHIR::Memory;

inline void touch_tree(const FastFHIR::Reflective::Node& node, std::size_t& touched_nodes) {
  if (!node) {
    return;
  }

  ++touched_nodes;
  if (!node.is_array() && !node.is_object()) {
    return;
  }

  for (auto& child : node.entries()) {
    touch_tree(child, touched_nodes);
  }
}

inline MaterializedTree materialize(const StreamType& payload) {
  MaterializedTree tree;
  tree.parser = std::make_unique<FastFHIR::Parser>(payload);
  tree.root = tree.parser->root();
  if (!tree.root) {
    return tree;
  }

  touch_tree(tree.root, tree.touched_nodes);
  tree.ok = true;
  return tree;
}

#elif defined(ARM_JSON)

using StreamType = std::string;

inline void touch_tree(simdjson::dom::element node, std::size_t& touched_nodes) {
  ++touched_nodes;

  if (node.is_object()) {
    auto object = node.get_object();
    if (object.error()) {
      return;
    }
    for (auto field : object.value_unsafe()) {
      ++touched_nodes;
      touch_tree(field.value, touched_nodes);
    }
    return;
  }

  if (node.is_array()) {
    auto array = node.get_array();
    if (array.error()) {
      return;
    }
    for (auto item : array.value_unsafe()) {
      touch_tree(item, touched_nodes);
    }
  }
}

inline MaterializedTree materialize(const StreamType& payload) {
  MaterializedTree tree;
  tree.parser = std::make_unique<simdjson::dom::parser>();
  auto doc = tree.parser->parse(payload);
  if (doc.error()) {
    return tree;
  }

  tree.root = doc.value_unsafe();
  touch_tree(tree.root, tree.touched_nodes);
  tree.ok = true;
  return tree;
}

#elif defined(ARM_GOOGLE_FHIR)

using StreamType = std::string;

inline uint32_t decode_u32_le(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

inline void touch_patient_tree(const google::fhir::r4::core::Patient& patient,
                               std::size_t& touched_nodes) {
  if (patient.has_id()) {
    ++touched_nodes;
  }
  if (patient.has_active()) {
    ++touched_nodes;
  }
  if (patient.has_gender()) {
    ++touched_nodes;
  }
  if (patient.has_birth_date()) {
    ++touched_nodes;
  }
  for (const auto& name : patient.name()) {
    ++touched_nodes;
    if (name.has_text()) {
      ++touched_nodes;
    }
    if (name.has_family()) {
      ++touched_nodes;
    }
    touched_nodes += static_cast<std::size_t>(name.given_size());
  }
}

inline void touch_observation_tree(const google::fhir::r4::core::Observation& observation,
                                   std::size_t& touched_nodes) {
  if (observation.has_id()) {
    ++touched_nodes;
  }
  if (observation.has_status()) {
    ++touched_nodes;
  }
  if (observation.has_code() && observation.code().has_text()) {
    ++touched_nodes;
  }
  if (observation.has_subject() && observation.subject().has_patient_id()) {
    ++touched_nodes;
  }
}

inline MaterializedTree materialize(const StreamType& payload) {
  MaterializedTree tree;

  std::size_t pos = 0;
  while (pos + 5 <= payload.size()) {
    const char record_type = payload[pos];
    const uint32_t record_len = decode_u32_le(payload.data() + pos + 1);
    pos += 5;

    if (pos + record_len > payload.size()) {
      return tree;
    }

    const char* record_data = payload.data() + pos;
    pos += record_len;

    if (record_type == 'P') {
      google::fhir::r4::core::Patient patient;
      if (!patient.ParseFromArray(record_data, static_cast<int>(record_len))) {
        return tree;
      }
      touch_patient_tree(patient, tree.touched_nodes);
      tree.patients.push_back(std::move(patient));
      continue;
    }

    if (record_type == 'O') {
      google::fhir::r4::core::Observation observation;
      if (!observation.ParseFromArray(record_data, static_cast<int>(record_len))) {
        return tree;
      }
      touch_observation_tree(observation, tree.touched_nodes);
      tree.observations.push_back(std::move(observation));
      continue;
    }

    return tree;
  }

  if (pos != payload.size()) {
    return tree;
  }

  tree.ok = !tree.patients.empty() || !tree.observations.empty();
  return tree;
}

#elif defined(ARM_HL7V2)

#include "hl7v2_message.hpp"

using StreamType = std::string;

inline void append_field_leaf_values(const hl7v2::Field& field, std::vector<Hl7Field>& out,
                                     std::size_t& touched_nodes) {
  ++touched_nodes;

  if (field.components.empty()) {
    out.push_back({std::string(field.val)});
    return;
  }

  for (const auto& component : field.components) {
    ++touched_nodes;
    if (component.subcomponents.empty()) {
      out.push_back({std::string(component.val)});
      continue;
    }

    for (const auto subcomponent : component.subcomponents) {
      ++touched_nodes;
      out.push_back({std::string(subcomponent)});
    }
  }
}

inline MaterializedTree materialize(const StreamType& payload) {
  MaterializedTree tree;

  const auto parsed_messages = hl7v2::parse_batch(payload);
  if (parsed_messages.empty()) {
    return tree;
  }

  for (const auto& parsed : parsed_messages) {
    for (const auto& seg_node : parsed.tree.segments) {
      if (seg_node.name.empty()) {
        continue;
      }

      ++tree.touched_nodes;
      Hl7Segment seg;
      seg.id.assign(seg_node.name);

      for (const auto& field : seg_node.fields) {
        append_field_leaf_values(field, seg.fields, tree.touched_nodes);
      }

      tree.segments.push_back(std::move(seg));
    }
  }

  tree.ok = !tree.segments.empty();
  return tree;
}

#endif

}  // namespace bench::test_2