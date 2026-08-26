#pragma once

// ---------------------------------------------------------------------------
// Full-traversal walk -- DIAGNOSTIC ONLY, never a reported stage
// ---------------------------------------------------------------------------
// D4 retired the layout-order walk as Test 2, and this does not bring it back:
// nothing here emits a MetricEvent, appears in the CSV, or reaches a figure.
// It exists because two upstream asks cite measurements taken with it, and a
// citation whose measurement cannot be reproduced is the exact defect this repo
// exists to prevent (cf. I3.6, the orjson ratio in FastFHIR's README):
//
//   ../FastFHIR/TASKS.md CAPI-7 (P2)  "~13% faster with the strlen removed;
//                                      BENCH_NO_STRLEN=1 toggles it"
//   ../FastFHIR/TASKS.md CAPI-8 (P1)  "2,396 allocations for 15,920 nodes
//                                      (BENCH_ARRAYS=1)"; the entries() vs
//                                      node[i] table
//
// Whoever picks up CAPI-7 or CAPI-8 needs to re-run those numbers before and
// after a fix. That is the whole job of this file.
//
// Run:
//   BENCH_WALK=1                      full traversal, reports ns and node count
//   BENCH_WALK=1 BENCH_ARRAYS=1       + array-node count (= vector<Node> allocations)
//   BENCH_WALK=1 BENCH_NO_STRLEN=1    + skip the from_cstr strlen (CAPI-7)
//   BENCH_WALK=1 BENCH_INDEX_WALK=1   + node[i] instead of entries()  (CAPI-8)
//
// What it measures, and why it is not evidence about the format: a full walk in
// layout order is a contiguous tape's best case and an offset-indexed layout's
// worst. It says nothing about random access (that is Test 2) and § Why
// FastFHIR? makes no full-traversal claim. Read it as an API-cost probe only.

#include "harness.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace bench::walk_diag {

// Read once, outside the walk: a getenv per node would measure getenv
// (notes.md section 6).
inline const bool g_enabled = std::getenv("BENCH_WALK") != nullptr;
inline const bool g_skip_key_name = std::getenv("BENCH_NO_STRLEN") != nullptr;
inline const bool g_index_walk = std::getenv("BENCH_INDEX_WALK") != nullptr;
inline const bool g_count_arrays = std::getenv("BENCH_ARRAYS") != nullptr;
inline std::size_t g_array_nodes = 0;

inline void touch_tree(const FastFHIR::Reflective::Node& node, std::size_t& touched_nodes) {
  if (!node) {
    return;
  }

  ++touched_nodes;

  switch (node.kind()) {
    case FF_FIELD_ARRAY: {
      // entries() returns an owning std::vector<Node>: one heap allocation per
      // array node, every element copied into it. This is the documented
      // exception to the zero-allocation read path (README § 1), and a full
      // walk is nothing but array materialization, so it maximises the
      // exception -- 2,396 allocations for a 15,920-node bundle. CAPI-8.
      ++g_array_nodes;
      if (g_index_walk) {
        // Same traversal, same public API, no vector: Node::operator[](size_t)
        // resolves one element in place. If this is materially faster then the
        // allocation is the cost, not the walk. Measured -18.5%.
        const std::size_t n = node.size();
        for (std::size_t i = 0; i < n; ++i) {
          touch_tree(node[i], touched_nodes);
        }
      } else {
        for (const auto& child : node.entries()) {
          touch_tree(child, touched_nodes);
        }
      }
      break;
    }
    case FF_FIELD_BLOCK: {
      for (const auto& field : node.fields()) {
        // from_cstr() strlen()s field.name for every field of every block, but
        // standard_node_lookup_field never reads name/name_len -- it dispatches
        // on owner_recovery, kind and field_offset. FF_FieldInfo carries no
        // name_len, so the strlen is forced on any caller doing reflection.
        // Measured ~13% of this walk. CAPI-7. The default path KEEPS it,
        // because that is what the public API costs a real consumer.
        const FF_FieldKey key =
            g_skip_key_name ? FF_FieldKey(node.recovery(), field.kind, field.field_offset,
                                          field.child_recovery, field.array_entries_are_offsets,
                                          nullptr, 0)
                            : FF_FieldKey::from_cstr(
                                  node.recovery(), field.kind, field.field_offset,
                                  field.child_recovery, field.array_entries_are_offsets,
                                  field.name);
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

// No-op unless BENCH_WALK is set. Returns the node count so a caller can make
// the result escape -- a walk whose result nothing observes can be deleted
// wholesale by the optimizer, which is how the original Test 2 came to report
// 83 ns for a 317-entry bundle (notes.md section 2).
inline std::size_t run(const FastFHIR::Memory& payload) {
  if (!g_enabled) return 0;

  FastFHIR::Parser parser(payload);
  auto root = parser.root();
  if (!root) return 0;

  g_array_nodes = 0;
  std::size_t touched = 0;
  Timer timer;
  timer.start();
  touch_tree(root, touched);
  const std::int64_t ns = timer.stop_ns();

  std::fprintf(stderr,
               "[walk] DIAGNOSTIC (not a stage): %zu nodes in %lld ns (%.2f ns/node)%s%s\n",
               touched, (long long)ns, touched ? double(ns) / double(touched) : 0.0,
               g_index_walk ? "  [node[i], no vector]" : "",
               g_skip_key_name ? "  [no strlen]" : "");
  if (g_count_arrays) {
    std::fprintf(stderr, "[walk] %zu array nodes = %zu vector<Node> heap allocations\n",
                 g_array_nodes, g_array_nodes);
  }
  return touched;
}

}  // namespace bench::walk_diag
