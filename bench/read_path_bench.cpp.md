# `read_path_bench.cpp` — read-path traversal validation

Ported from the validation instrument built 2026-08-19 during the FastFHIR
TASKS.md OPEN TOPIC investigation. Its job is to keep the whole-document
read path honest: every metric is reported as an **average per Bundle entry**
(µs ÷ `Bundle.entry` count) and the run **gates on `<= 50 µs/entry`** — exit 0
PASS, exit 1 FAIL, exit 2 usage/format error.

## Why per-entry

The README claim under test ("Reading a FastFHIR stream requires 0 heap
allocations… nanosecond read times") is about per-field access. Per-entry
averaging normalizes across bundle sizes so the gate means the same thing on a
100 KB fixture as on a 5 GB one.

## Build & run

```bash
bazelisk run //bench:read_path_bench -- /path/to/bundle.ffhr
```

- `.bazelrc` pins `--compilation_mode=opt` — required. A Debug build of
  FastFHIR measures ~10× slower on these paths (see README, "the Debug trap").
- The binary self-reports its build (`Release (NDEBUG, optimized)` vs
  `DEBUG (NOT optimized…)`) precisely so a mis-built run cannot be mistaken
  for a valid one.
- Timing is min-of-7 (TASKS.md OPEN TOPIC §E): run-to-run spread on the walk
  is a few ms, and one false conclusion has already been bought with a mean.

## Metrics

| Metric | What it measures | Unit |
|---|---|---|
| Parser construction | ctor cost (header + root bounds), absolute | µs |
| `validate_FFHR_stream()` | full graph walk (bounds/recovery per slot) | µs/entry |
| reflective walk (public API) | `fields()`/`entries()` recursion — the documented read API, allocations included | µs/entry |
| `print_json()` → null sink | JSON export walk (formatting included) | µs/entry |
| `Bundle.entry.entries()` | per-call vector materialization cost | µs/entry |
| `Compactor::archive()` | compaction walk (writes included) | µs/entry |

## Baseline (2026-08-19, Ryan's Mac, Bazel opt, 50.8 MiB Synthea bundle, 31,042 entries)

```
metric                                   avg us/entry   gate <= 50us
validate_FFHR_stream()                          0.344           PASS
reflective walk (public API)                    0.768           PASS
print_json() -> null sink                       6.324           PASS
Bundle.entry.entries() (materialize)            0.003           PASS
Compactor::archive()                            5.762           PASS
RESULT: PASS (max avg 6.324 us/entry, bound 50 us/entry)
```

The gate is a regression bound, not a Debug-vs-Release discriminator: even a
Debug build's walk (3.5 µs/entry) passes it. A violation means the read path
has gotten 10×+ slower than the optimized baseline, or per-entry work has
become pathological.
