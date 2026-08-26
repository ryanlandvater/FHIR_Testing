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


// ---------------------------------------------------------------------------
// Per-arm namespace -- REQUIRED FOR CORRECTNESS, not style.
// ---------------------------------------------------------------------------
// Each arm compiles these headers with a different ARM_* macro, so the SAME
// type and function names get four DIFFERENT definitions across four
// translation units: bench::test_2::MaterializedTree holds a
// unique_ptr<FastFHIR::Parser> in one TU, a simdjson element in another, and
// two protobuf vectors in a third.
//
// That is a One Definition Rule violation. The linker keeps one definition of
// each inline function and destructor and discards the rest, so an object built
// with one layout gets destroyed with another. It manifests as heap corruption
// far from the cause -- ASan caught it as a SEGV inside
// ~vector<google::fhir::r4::core::Observation> from
// bench::test_2::MaterializedTree::~MaterializedTree, and it also moved the
// apparent crash site around between -c opt and -c dbg builds, which is the
// classic signature.
//
// An inline namespace gives each arm its own mangled symbols while leaving
// every existing call site (bench::test_2::query, bench::assign::assign_patient)
// spelled exactly as before.
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

inline MetricEvent materialize_metric(std::string_view arm, std::int64_t duration_ns) {
  return MetricEvent{std::string(arm), Stage::Test2Materialize, duration_ns};
}

struct MaterializedTree {
  #if defined(ARM_FASTFHIR)
  // Keep parser heap-owned so Node lifetimes stay valid with minimal copy/move constraints.
  std::unique_ptr<FastFHIR::Parser> parser;
  FastFHIR::Reflective::Node root;
  #elif defined(ARM_JSON)
  std::unique_ptr<simdjson::dom::parser> parser;
  simdjson::dom::element root;
  #elif defined(ARM_GOOGLE_FHIR)
  std::vector<google::fhir::r4::core::Patient> patients;
  std::vector<google::fhir::r4::core::Observation> observations;
  #elif defined(ARM_HL7V2)
  // parse_batch returns ParsedMessage; preserve parsed ASTs without re-materializing strings.
  std::vector<hl7v2::ParsedMessage> messages;
  #endif  
  std::size_t touched_nodes = 0;
  bool ok = false;
};



#if defined(ARM_FASTFHIR)

using StreamType = FastFHIR::Memory;

// Arrays are walked with entries(); OBJECTS are walked with fields() plus an
// owner-keyed lookup. The previous version called entries() for both, which
// returns nothing for a block -- so this walk visited exactly ONE node (the
// Bundle root) while the JSON, HL7v2 and Google arms visited 8-9k. Test 2 was
// comparing a full tree walk against a no-op.
//
// This mirrors read_path_bench.cpp::walk_node(), which is the validated
// public-API traversal (and the one FastFHIR's own is_empty()/print use).
inline void touch_tree(const FastFHIR::Reflective::Node& node, std::size_t& touched_nodes) {
  if (!node) {
    return;
  }

  ++touched_nodes;

  switch (node.kind()) {
    case FF_FIELD_ARRAY: {
      for (const auto& child : node.entries()) {
        touch_tree(child, touched_nodes);
      }
      break;
    }
    case FF_FIELD_BLOCK: {
      for (const auto& field : node.fields()) {
        const FF_FieldKey key = FF_FieldKey::from_cstr(
            node.recovery(), field.kind, field.field_offset, field.child_recovery,
            field.array_entries_are_offsets, field.name);
        FastFHIR::Reflective::Entry entry = node[key];
        if (!entry) {
          continue;
        }
        FastFHIR::Reflective::Node child = entry.as_node();
        if (child) {
          touch_tree(child, touched_nodes);
        }
      }
      break;
    }
    default:
      // Strings, codes and scalars: count the node, do not descend.
      break;
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

  // Move parsed messages into tree storage to avoid extra copies.
  for (auto& msg : parsed_messages) {
    touch_tree(msg, tree.touched_nodes);
    tree.messages.push_back(std::move(msg));
  }

  tree.ok = !tree.messages.empty();
  return tree;
}

#endif

}  // inline namespace BENCH_ARM_NS
}  // namespace bench::test_2