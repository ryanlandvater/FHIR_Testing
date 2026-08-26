# `bench_test_2.hpp` — Random Access (Test 2)

> ⚠️ **Replaced the materialize walk 2026-08-26 (TASKS.md D4).** The former
> Test 2 walked every node in LAYOUT order — a contiguous tape's best case and
> an offset-indexed layout's worst — and no consumer reads a bundle in write
> order. Read in layout order simdjson wins ~22x per node; read out of order
> FastFHIR wins up to ~2,500x. Both are true; the walk chart is retired.

## Purpose

Implements **Test 2 (Random Access)** — the receiver-side stage: pick N random
`Bundle.entry` ordinals (deterministic seed `20260826u`, default 2000 reads,
`BENCH_RANDOM_READS` overrides), navigate to each ONE FROM THE ROOT, and read
the resource's `id`. Every lookup pays its own path cost — that asymmetry is
the WF-1.1 claim:

| Arm | Per-read cost |
|---|---|
| FastFHIR | offset arithmetic from the root — O(1) |
| simdjson | `dom::array::at(i)` iterates from element 0 — O(i) |
| protobuf | scan i length-prefixed TLV records, then parse — O(i) |
| HL7v2 | scan forward for the i-th MSH, then parse — O(i) |

The three scan formats are not being sandbagged: none of them HAS an O(1)
index into a serialized document. That is the point of the comparison.

## Parity Gate

Every arm accumulates the **total bytes of `id` read**; the harness compares
the accumulators across arms and **fails the run (exit 2)** on any mismatch —
an arm that silently read nothing would look infinitely fast. The gate caught
three real bugs during development (TASKS.md IN-B2: wrong HL7v2 segment
terminator, wrong PID field index, and the probe running after Test 4's
in-place enrich).

## What Gets Measured

The timer starts after the target list is built and stops when all reads
complete. `MetricEvent` carries `ops` = reads and `bytes_out` = id bytes read
(the parity accumulator). The read is `as<std::string_view>()` — the call that
actually touches the arena; cached `Node` members would measure nothing (an
earlier probe reported a 26x speedup that was entirely that artefact).

## Granularity Warning (HL7v2)

A v2 batch has no resource-level index — its addressable unit is the MESSAGE
(5 ORU messages carry the same 1,473 resources the other arms address
individually). Its ns/read is a scan over a much smaller ordinal space and is
NOT the same operation. That difference is a finding about the format, not a
probe defect, and is captioned wherever this stage is reported.

## Dependencies

- `harness.hpp` — Types, `MetricEvent`, `Stage`
- `FastFHIR` headers (for FFHR arm)
- `simdjson.h` (for JSON arm)
- `google/protobuf/descriptor.h`, `google/protobuf/message.h` (for Google FHIR arm)
- `hl7v2_message.hpp` — `parse_batch()`, `ParsedMessage` (for HL7v2 arm)
