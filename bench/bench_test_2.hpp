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
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#elif defined(ARM_HL7V2)
#include "hl7v2_message.hpp"
#endif

namespace bench::test_2 {

inline MetricEvent materialize_metric(std::string_view arm, std::int64_t duration_ns) {
  return MetricEvent{std::string(arm), Stage::Test2Materialize, duration_ns};
}

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
  std::vector<hl7v2::ParsedMessage> messages;
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

// Unified dynamic tree walk for Protobuf
inline void touch_tree(const google::protobuf::Message& msg, std::size_t& touched_nodes) {
  ++touched_nodes;
  const auto* reflection = msg.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(msg, &fields);

  for (const auto* field : fields) {
    ++touched_nodes;
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (field->is_repeated()) {
        const int count = reflection->FieldSize(msg, field);
        for (int i = 0; i < count; ++i) {
          touch_tree(reflection->GetRepeatedMessage(msg, field, i), touched_nodes);
        }
      } else {
        touch_tree(reflection->GetMessage(msg, field), touched_nodes);
      }
    }
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
      touch_tree(patient, tree.touched_nodes);
      tree.patients.push_back(std::move(patient));
      continue;
    }

    if (record_type == 'O') {
      google::fhir::r4::core::Observation observation;
      if (!observation.ParseFromArray(record_data, static_cast<int>(record_len))) {
        return tree;
      }
      touch_tree(observation, tree.touched_nodes);
      tree.observations.push_back(std::move(observation));
      continue;
    }

    return tree;
  }

  tree.ok = !tree.patients.empty() || !tree.observations.empty();
  return tree;
}

#elif defined(ARM_HL7V2)

using StreamType = std::string;

// Pure AST walk without string allocations
inline void touch_tree(const hl7v2::ParsedMessage& msg, std::size_t& touched_nodes) {
  ++touched_nodes;
  for (const auto& seg : msg.tree.segments) {
    if (seg.name.empty()) {
      continue;
    }
    ++touched_nodes;

    for (const auto& field : seg.fields) {
      ++touched_nodes;

      for (const auto& comp : field.components) {
        ++touched_nodes;

        for (const auto& sub : comp.subcomponents) {
          ++touched_nodes;
        }
      }
    }
  }
}

inline MaterializedTree materialize(const StreamType& payload) {
  MaterializedTree tree;

  auto parsed_messages = hl7v2::parse_batch(payload);
  if (parsed_messages.empty()) {
    return tree;
  }

  for (auto& msg : parsed_messages) {
    touch_tree(msg, tree.touched_nodes);
    tree.messages.push_back(std::move(msg));
  }

  tree.ok = !tree.messages.empty();
  return tree;
}

#endif

}  // namespace bench::test_2