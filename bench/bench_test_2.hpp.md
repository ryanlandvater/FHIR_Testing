# `bench_test_2.hpp` — Materialization (Test 2)

> ⚠️ **Corrected 2026-08-25 — this stage was measuring almost nothing.**
> `touch_tree()` walked objects with `entries()`, which returns elements of an
> *array*; blocks need `fields()`. The FastFHIR arm therefore visited **1 node**
> while the JSON, HL7v2 and Google arms visited ~8,000, and the reported duration
> was 333 ns.
>
> Now mirrors `read_path_bench.cpp::walk_node()` and visits 4,443 nodes
> (~108 µs). Node counts still differ per format for identical clinical content,
> so Test 2 is **not yet normalized**. See [notes.md](../notes.md) §2.

## Purpose

Implements **Test 2 (Materialize)**: taking each arm's serialized output and parsing it back into an in-memory tree or object model, then recursively walking every node to measure the full materialization cost.

This test represents the "receiver's cost" — how expensive is it to consume each format's output?

## Architecture

Defines one `materialize()` overload per arm, selected by the same `#define ARM_*` guard pattern as `bench_test_1.hpp`.

### Common Types

```cpp
struct MaterializedTree {
  // Arm-specific root storage (see below)
  std::size_t touched_nodes = 0;
  bool ok = false;
};
inline MetricEvent materialize_metric(arm, duration_ns);
```

### Per-Arm Implementations

#### FastFHIR (`ARM_FASTFHIR`)

```cpp
using StreamType = FastFHIR::Memory;
MaterializedTree materialize(const StreamType& payload);
```

- **Parser**: `FastFHIR::Parser(payload)` — zero-copy parser over the FFHR arena
- **Walk**: Recursively visits every node via `node.entries()`, incrementing `touched_nodes`
- **Cost model**: Pure pointer chasing — the FFHR binary is already in memory; this measures tree navigation speed

#### JSON (`ARM_JSON`)

```cpp
using StreamType = std::string;
MaterializedTree materialize(const StreamType& payload);
```

- **Parser**: `simdjson::dom::parser` — the fastest DOM parser for JSON
- **Walk**: Recursive object/array field enumeration via `simdjson::dom::object` / `array` iterators
- **Cost model**: Full parse + tree walk — simdjson must tokenize the string, build a DOM, and navigate it

#### Google FHIR (`ARM_GOOGLE_FHIR`)

```cpp
using StreamType = std::string;
MaterializedTree materialize(const StreamType& payload);
```

- **Parser**: Iterates the custom TLV record format, deserializes each protobuf via `ParseFromArray()`
- **Walk**: Uses protobuf reflection (`GetReflection()`, `ListFields()`, `FieldDescriptor::cpp_type()`) to recursively walk each message's fields
- **Cost model**: TLV iteration + protobuf deserialization + reflection tree walk — the most expensive materialization path

#### HL7v2 (`ARM_HL7V2`)

```cpp
using StreamType = std::string;
MaterializedTree materialize(const StreamType& payload);
```

- **Storage**: `MaterializedTree::messages` holds `std::vector<hl7v2::ParsedMessage>`, each containing a fully-parsed `MessageTree` (backed by an owning `storage` string for zero-copy `string_view` references).
- **Parser**: Calls `hl7v2::parse_batch()` which splits the concatenated HL7v2 stream on `MSH|` message boundaries, then builds a four-level `MessageTree` AST per message:
  - `Segment` level — split on `\r`
  - `Field` level (inside each Segment) — split on `|`
  - `Component` level (inside each Field) — split on `^`
  - `Subcomponent` level (inside each Component) — split on `&`
- **Walk**: Recursively visits every syntactic node in the tree:
  ```cpp
  inline void touch_tree(const hl7v2::ParsedMessage& msg, std::size_t& touched_nodes) {
    ++touched_nodes;                                      // message node
    for (const auto& seg : msg.tree.segments) {
      if (seg.name.empty()) continue;
      ++touched_nodes;                                    // segment node
      for (const auto& field : seg.fields) {
        ++touched_nodes;                                  // field node
        for (const auto& comp : field.components) {
          ++touched_nodes;                                // component node
          for (const auto& sub : comp.subcomponents) {
            ++touched_nodes;                              // subcomponent node
          }
        }
      }
    }
  }
  ```
- **Cost model**: Full HL7v2 parse (message boundary scan + `\r` → `|` → `^` → `&` hierarchical splitting) + 4-level tree walk. Heavier than segment-only counting but mirrors the node-granularity cost model used by FastFHIR and JSON arms.

## What Gets Measured

The timer starts **before** the `materialize()` call and stops **after** `touched_nodes` is populated. This captures the full cost of:

- **FastFHIR**: Walking a pre-built tree (zero deserialization — the FFHR binary IS the tree)
- **JSON**: simdjson DOM construction + tree walk
- **Google FHIR**: TLV parsing + protobuf deserialization + reflection walk
- **HL7v2**: Full HL7v2 string parse (message boundary scan + `\r` → `|` → `^` → `&` hierarchical splitting) + 4-level tree walk (segment → field → component → subcomponent)

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Recursive tree walk (`touch_tree`) | Guarantees all nodes are visited; prevents compiler from optimizing away unused parses |
| Separate parser instance per arm | Each arm's parser is heap-owned via `unique_ptr` to avoid move/copy complications |
| Protobuf reflection for Google FHIR | Generic walk without per-message-type visitor code |
| HL7v2 full 4-level parse tree | Hierarchical splitting (`\r` → `|` → `^` → `&`) builds `Segment`/`Field`/`Component`/`Subcomponent` AST — matches the node-granularity cost model of other arms |

## Dependencies

- `harness.hpp` — Types, `MetricEvent`, `Stage`
- `FastFHIR` headers (for FFHR arm)
- `simdjson.h` (for JSON arm)
- `google/protobuf/descriptor.h`, `google/protobuf/message.h` (for Google FHIR arm)
- `hl7v2_message.hpp` — `parse_batch()`, `ParsedMessage` (for HL7v2 arm)
