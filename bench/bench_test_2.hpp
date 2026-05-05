#pragma once

#include "harness.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(ARM_JSON)
#include <simdjson.h>
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