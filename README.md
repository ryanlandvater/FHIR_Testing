# FastFHIR Lightweight Benchmark

Four-arm comparative benchmark measuring serialization, materialization, query, and enrichment performance across **FastFHIR**, **JSON FHIR** (nlohmann::json + simdjson), **Google FHIR** (protobuf), and **HL7v2** (ORU^R01).

> ## ✅ Build status: GREEN (2026-08-25) — results not yet publishable
>
> The harness was ported to FastFHIR's `FF_*` façade API and **builds and runs
> again**. All four arms report all four stages.
>
> ```bash
> bazel build -c opt //bench:all                     # green
> bazel test  -c opt //bench:timing_conformance_test # PASSED
> ./bazel-bin/bench/bench_harness --runs 2 --bundle-max-mb 16
> ```
>
> **Exit code 2 is expected** — it signals a cross-arm parity mismatch, not a
> crash. The HL7v2 arm does not report `obs_issued_present` or
> `obs_component_value_*`.
>
> **The numbers it produces are proof the harness works, not results to
> publish.** Getting it running surfaced five defects that had been live and
> silent, including one that made every earlier crash undebuggable. Read
> **[notes.md](notes.md)** before quoting any figure, and
> **[handoff.md](handoff.md)** before designing new tests — the current 4×4 grid
> does not map onto the claims in FastFHIR's § Why FastFHIR?:
>
> | | |
> |---|---|
> | Test 1 | Not at parity — the FastFHIR arm serializes every POCO field, the others ~25. Excludes `value[x]` entirely. |
> | Test 2 | Random access (D4, 2026-08-26): N random entry reads, each navigating from the root. Replaced the materialize walk — a layout-order traversal that was a tape's best case and a query's worst. Cross-arm byte gate fails the run on mismatched reads. |
> | Test 3 | FastFHIR reads via node lenses — no whole-POCO materialization (2026-08-26); still pays a `print_json` penalty on `birthDate` only (PA-7). |
> | Provenance | Compiled profile and upstream SHA still unrecorded — [TASKS.md § PROFILE](TASKS.md). |

## FastFHIR build flags — the Debug trap (2026-08-19)

FastFHIR is a Bazel module dependency of this repo (see `generate_repo.sh`: "Bazel
handles building FastFHIR from source as a module dependency"), so it inherits
this repo's `.bazelrc` `--compilation_mode=opt`. That is load-bearing: FastFHIR's
own CMake presets configure **Debug (`-O0`)** builds, and its published numbers —
including TASKS.md's OPEN TOPIC §A/§B tables, labeled "-O2" — were measured on a
Debug build. The same code is ~10× faster optimized:

| Path (50.8 MiB Synthea bundle, min of 7, same machine) | Debug `-O0` | Release `-O3` |
|---|---|---|
| `validate_FFHR_stream()` full graph walk | 107.5 ms / 0.46 GiB/s | **10.4 ms / 4.9 GiB/s** |
| `Bundle.entry.entries()` (31,042 elements) | 856 µs | **36 µs** |
| `print_json()` export walk | 734 ms | 197 ms |

> ⚠ **These absolute numbers are void.** They were measured before the
> opaque-JSON change (2026-08-24) and describe a stream that did not contain
> out-of-profile resources. The Debug-vs-Release *ratio* is still the point of
> the table and still holds; the milliseconds do not. Re-measure after the
> port — [TASKS.md § PORT-9](TASKS.md).

Rules:

- **Keep `--compilation_mode=opt` in `.bazelrc`.** Never build or run the
  benchmark with `-c fastbuild`/`-c dbg`: the cross-format comparison would stay
  fair (all arms share flags) but every absolute number and every
  FastFHIR-vs-orjson ratio would be Debug-inflated — the exact trap that
  produced FastFHIR's §A table. If you ever re-run the published 2.4–3.4×
  orjson ratios, confirm the build was `opt`.
- When FastFHIR is needed outside Bazel (fixture prep, local profiling),
  configure it with `-DCMAKE_BUILD_TYPE=Release`; its `ninja`/`xcode` CMake
  presets are Debug.
- Do not adopt FastFHIR "read-path optimizations" measured only at Debug: a
  one-shot-fill `entries()` variant won 856→525 µs at `-O0` but **regressed
  36→58 µs at `-O3`** (per-element `push_back` writes each 48-byte `Node` once;
  the fill writes twice once libc++'s container annotations inline away).
  See FastFHIR TASKS.md OPEN TOPIC correction, 2026-08-19.

---

## Result provenance

Two things silently change what this benchmark measures. Neither is visible to
Bazel, and both must be recorded alongside any published number.

### 1. The compiled profile

`.external/FastFHIR` is a **symlink to the live working tree**
(`../FastFHIR`) — not a pinned checkout. Whatever state that tree is in is what
you measure. Worse, Bazel does not run FastFHIR's generator: its `BUILD.bazel`
globs `generated_src/*.cpp`, and that directory is produced by **CMake at
configure time** and is **profile-dependent**.

**So the profile you benchmark is whatever CMake last generated.** A profile
change swaps the binary under test with no Bazel-visible signal.

Current upstream state (verified 2026-08-24, head `a9fd4e9`):

| | |
|---|---|
| profile | `us-core,billing,medication-admin,supply` |
| code-system enums | 80 |
| generated `.cpp` | 44 |
| resource types compiled | 37 |
| `ImagingStudy` compiled | no — takes the opaque-JSON path |

**Record the profile and the upstream git SHA with every result you publish.**

After any profile change, `rm -rf ../FastFHIR/generated_src` before
regenerating — the generator never deletes output it no longer emits, so a
stale tree survives the change and produces confusing compile errors.

### 2. Out-of-profile resources are opaque, not absent

FastFHIR used to **silently drop** any resource type outside the compiled
profile — one Synthea bundle lost 41 of 250 records and still returned
success. That is fixed: the raw JSON is now retained verbatim in the stream as
an opaque block and re-emitted byte-for-byte on export.

This changes the benchmark in three ways:

- **Size and throughput comparisons are apples-to-apples for the first time.**
  Any earlier FastFHIR-vs-JSON size or throughput win was measured against a
  FastFHIR stream that did not contain data the other arms carried. That
  favoured FastFHIR, invisibly. Treat all pre-2026-08-24 numbers as void.
- **Streams are larger and ingest does more work** for any document containing
  out-of-profile types — which Synthea does, heavily.
- **An opaque resource round-trips perfectly but is not typed-navigable.** No
  V-Table means no `Node` field access, no query, and no interior compaction.

Because the shipped preset deliberately excludes the `imaging` grouping, the
Synthea corpus's **1,444 `ImagingStudy` resources take the opaque path on
every run**. That is intentional coverage — it is what continuously proves the
fallback is lossless. **Do not enable `imaging` to "fix" it.**

The consequence for stage 3: a "query every resource" benchmark is not
querying every resource. Either restrict the corpus to in-profile types or
build with a wider profile — and **say which you did**, because it changes
both the binary and the workload. Report the opaque fraction (resource count
and bytes) next to any query result. Tracked in
[TASKS.md § CORPUS](TASKS.md).

---

## Repository documents

| Document | What it is | Freshness |
|---|---|---|
| [`TASKS.md`](TASKS.md) | **The only backlog**, and the standing **Decisions** (D1 macro parity · D2 `value[x]` tiering · D3 serialization model). **Start here.** | ✅ current (2026-08-26) |
| [`handoff.md`](handoff.md) | **Claim register + instrument design.** Maps every § Why FastFHIR? claim to an instrument, an artifact, and a citable number. Amendments marked ✎. | ✅ current (2026-08-26) |
| [`notes.md`](notes.md) | **Field report from the 2026-08-25 port.** What was silently broken and how it was found. Read before designing new tests. | ✅ current (2026-08-26) |
| [`BENCHMARK_IMPLEMENTATION_CHECKLIST.md`](BENCHMARK_IMPLEMENTATION_CHECKLIST.md) | Per-component design vs. build status | ✅ current |
| [`IMPLEMENTATION_SUMMARY.md`](IMPLEMENTATION_SUMMARY.md) | Curated architecture + parity overview | ✅ current |
| [`PARITY_ASSESSMENT.md`](PARITY_ASSESSMENT.md) | Fairness audit of all four arms | ⚠️ 2026-05-07; reasoning stands, verification stale |
| [`RESOURCE_COVERAGE_ANALYSIS.md`](RESOURCE_COVERAGE_ANALYSIS.md) | Which FHIR resources are exercised | ⚠️ denominator corrected in-place |
| [`MESSAGE_SURFACE_PARITY_AUDIT.md`](MESSAGE_SURFACE_PARITY_AUDIT.md) | Google FHIR proto coverage audit | ⚠️ 2026-05-06; Stage 2/3 findings still stand |
| [`MESSAGE_SURFACE_PARITY_QUICK_REFERENCE.md`](MESSAGE_SURFACE_PARITY_QUICK_REFERENCE.md) | TL;DR of the above | ⚠️ same vintage |
| [`FFHRnotes.md`](FFHRnotes.md) | Upstream API friction log | ⚠️ mostly resolved; see its status banner |
| [`TODO.md`](TODO.md) | **Design spec for Instrument G** — the four resilience tests. Not a second backlog. | ⚠️ unblocked 2026-08-25; API references need a pass |
| [`bench/*.md`](bench/) | Per-file architecture docs | ⚠️ describe pre-port code; each carries a banner |

Upstream context lives in `../FastFHIR/handoff.md` (why the API moved and what
it invalidates) and `../FastFHIR/CLAUDE.md` (**not** auto-loaded — read it
before editing that tree).

Our asks against FastFHIR's public API are filed in `../FastFHIR/TASKS.md` as
**CAPI-1…CAPI-6**, plus claims-alignment items **I3.6** (the orjson citation)
and **I3.7** (the compact-size figures). Tracked here in TASKS.md § UP. When
this benchmark hits an API that is unclear, unsafe to misuse, or missing, file it
there — this repo is FastFHIR's first external consumer, and that friction is a
finding, not an inconvenience.

Both repos carry a chunk-level `.arbiter/` index (`repo_map.md` + per-file
JSON). Navigate with it rather than reading trees wholesale — and remember a
correction banner at the top of a document **does not travel with a retrieved
chunk**, so stale body text must be struck in place (TASKS.md HY-5).

---

## Architecture

```
Synthea JSON files (119 patients)
        │
        ▼
  synthea_fixture.cpp ── FastFHIR Ingest ──► BundlePatient (POCO + arena)
        │
        ▼
  BundleBenchFixture ── identical C++ structs fed to all 4 arms
        │
   ┌────┼────┬────┐
   ▼    ▼    ▼    ▼
 arm_   arm_ arm_ arm_
 fastfhir.json hl7v2 google_
 .cpp  .cpp .cpp fhir.cpp
  │     │     │     │
  ▼     ▼     ▼     ▼
  bench_test_1.hpp — shared assign_patient() / assign_observation()
  bench_test_2.hpp — shared random_access() per format
  bench_test_3.hpp — shared query() per format
  bench_test_4.hpp — shared enrich() per format
  │     │     │     │
  ▼     ▼     ▼     ▼
  4× MetricEvent per arm → stdout CSV + optional PostgreSQL
```

Each arm receives the **exact same in-memory `PatientData`/`ObservationData` structs** and serializes them into its respective wire format. See the per-file docs:

| Doc File | What It Describes |
|---|---|
| [`arm_fastfhir.cpp.md`](bench/arm_fastfhir.cpp.md) | FastFHIR binary arena serialization — the primary arm |
| [`arm_json_fhir.cpp.md`](bench/arm_json_fhir.cpp.md) | nlohmann::json → FHIR JSON bundle |
| [`arm_hl7v2.cpp.md`](bench/arm_hl7v2.cpp.md) | HL7v2 ORU^R01 pipe-delimited messages |
| [`arm_google_fhir.cpp.md`](bench/arm_google_fhir.cpp.md) | Google protobuf FHIR — all 4 stages live |
| [`harness.hpp.md`](bench/harness.hpp.md) | Core types, Timer, Stage enum, ArmRunResult |
| [`main.cpp.md`](bench/main.cpp.md) | CLI args, Synthea discovery, bundle sweep, DB persistence |
| [`synthea_fixture.cpp.md`](bench/synthea_fixture.cpp.md) | JSON → BundlePatient ingestion pipeline |
| [`bench_test_1.hpp.md`](bench/bench_test_1.hpp.md) | Shared assignment layer (the fairness guarantee) |
| [`bench_test_2.hpp.md`](bench/bench_test_2.hpp.md) | Random access: N reads from the root (replaced materialize, D4) |
| [`bench_test_3.hpp.md`](bench/bench_test_3.hpp.md) | Query: LOINC 2085-9 search + value classification |
| [`bench_test_4.hpp.md`](bench/bench_test_4.hpp.md) | Enrichment: append observation to existing bundle |
| [`hl7v2_message.hpp.md`](bench/hl7v2_message.hpp.md) | HL7v2 segment types, builder, batch parser |
| [`timing_conformance_test.cpp.md`](bench/timing_conformance_test.cpp.md) | Build-time parity validation |
| [`read_path_bench.cpp.md`](bench/read_path_bench.cpp.md) | Read-path traversal validation — per-Bundle-entry throughput gate (`<= 50 µs/entry`) |
| [`enrich.json.md`](bench/enrich.json.md) | BMP panel test fixture (LOINC 24321-3) |
| [`BUILD.bazel.md`](bench/BUILD.bazel.md) | Bazel target graph and conditional arms |

---

## Code Parity: The Fundamental Requirement

This benchmark's validity depends on one axiom: **every arm serializes bit-identical source data through structurally identical assignment code.** Any deviation — a missing field, a different code path, a skipped edge case — invalidates the comparison.

### How Parity Is Enforced

The entire assignment layer lives in a **single header per test stage**, guarded by `#define ARM_*` macros:

```cpp
// arm_fastfhir.cpp:
#define ARM_FASTFHIR
#include "bench_test_1.hpp"   // assign_patient() → FastFHIR ObjectHandle
#include "bench_test_2.hpp"   // random_access() → N root-navigated reads
#include "bench_test_3.hpp"   // query()          → FFHR node walk
#include "bench_test_4.hpp"   // enrich()         → FFHR arena append
#undef ARM_FASTFHIR

// arm_json_fhir.cpp:
#define ARM_JSON
#include "bench_test_1.hpp"   // assign_patient() → nlohmann::json
#include "bench_test_2.hpp"   // materialize()    → simdjson DOM
#include "bench_test_3.hpp"   // query()          → simdjson walk
#include "bench_test_4.hpp"   // enrich()         → JSON parse→modify→dump
#undef ARM_JSON
```

Inside each `bench_test_N.hpp`, `#if defined(ARM_FASTFHIR) / #elif defined(ARM_JSON) / #elif defined(ARM_HL7V2) / #elif defined(ARM_GOOGLE_FHIR)` blocks expose the format-specific implementation of the *same logical operation*. The call sites in the arm files are identical — only the backend changes.

This pattern guarantees:
- **Same loop structure** — no arm gets an algorithmic advantage
- **Same field coverage** — every field assigned by one arm is assigned by all arms
- **Same query logic** — LOINC matching, value type classification, component traversal are line-for-line equivalent
- **One audit point** — add a field to the shared assignment, and all four arms get it simultaneously

### Critical Open TODOs for Full Parity

> These are *parity* gaps — they assume a benchmark that builds. The build
> break itself is [TASKS.md § PORT](TASKS.md) and comes first.

| # | TODO | Impact | Effort |
|---|---|---|---|
| 1 | ~~**Google FHIR Stage 2**~~ — **already implemented** (verified 2026-08-25): parses the TLV records, `ParseFromArray` per message, protobuf-reflection walk. 9,539 nodes, ~1.6 ms/MB. | ✅ resolved | — |
| 2 | ~~**Google FHIR Stage 3**~~ — **already implemented**: returns `patients=1 observations=316 obs_issued_present=316`, matching the JSON baseline. **But** it reports `birthdate` as a microsecond epoch where every other arm reports ISO, and the arm is excluded from `validate_parity()`. | 🟡 Normalization + cross-check | ~4h |
| 3 | **Google FHIR Stage 4 (Enrich)** — functional but re-parses *all* records. Verify parity with other arms' enrich semantics (FastFHIR appends without re-parse; JSON re-parses all). | 🟡 Enrich cost model differs fundamentally | ~8h doc |
| 4 | **HL7v2 birthdate normalization mismatch** — `normalize_birthdate()` strips non-digits (`1990-03-21` → `19900321`). All other arms preserve ISO format. The conformance checker accounts for this, but the serialization output differs. | 🟡 Cross-arm query parity | ~2h fix |
| 5 | **Only 2/37 FHIR resources tested** — Patient + Observation are active. Encounter, Condition, Procedure, and 32 others are hydrated in fixture structs but never serialized. The benchmark measures a tiny fraction of real-world EHR workload. | 🟡 Generalizability | ~200h per phase |
| 6 | **No multi-bundle enrichment test** — Test 4 enriches once. Real EHRs enrich millions of times. The one-shot cost profile may not extrapolate. | 🟡 Realism | ~4h config |
| 7 | **Opaque resources are not query-navigable** — 1,444 Synthea `ImagingStudy` records take the opaque-JSON path, so stage 3 silently skips them. See [Result provenance](#result-provenance). | 🔴 Stage 3 measures less than it appears to | ~8h doc + corpus decision |

---

## Setup

```bash
export FASTFHIR_REPO=https://github.com/<org>/FastFHIR.git
./generate_repo.sh
```

Google FHIR routine is enabled by default (separate Bazel build in `.external/google-fhir`).
Useful environment overrides:

```bash
# Google FHIR controls
export GOOGLE_FHIR_ENABLE=1
export GOOGLE_FHIR_REPO=https://github.com/google/fhir.git
export GOOGLE_FHIR_SYNC_REMOTE=0
export FORCE_GOOGLE_FHIR_REBUILD=0
export GOOGLE_FHIR_BAZEL_VERSION=7.7.1
export GOOGLE_FHIR_BAZELISK_VERSION=v1.22.1
export TEST_GOOGLE_FHIR_COMPONENTS=1
export TEST_BENCH_COMPONENTS=1
export GOOGLE_FHIR_CLEAN_ARTIFACTS=1

# Data source (this is the script's default; override only if you need a pinned release)
export SYNTHEA_DATA_URL=https://synthetichealth.github.io/synthea-sample-data/downloads/latest/synthea_sample_data_fhir_latest.zip
```

Set `GOOGLE_FHIR_ENABLE=0` to skip the Google FHIR routine entirely.

### Corpus location

`generate_repo.sh` downloads and extracts Synthea FHIR JSON into
**`datasets/synthea/`** at the repo root (`generate_repo.sh:21`). The harness
looks there; if the directory is missing or empty it exits with
`Synthea data not found`.

> ⚠ `bench/main.cpp:31` still hardcodes an absolute developer-machine path as
> its *primary* corpus location and only falls back to `datasets/synthea`. On
> any other machine the fallback is what runs. Removing the hardcoded path and
> adding a `--synthea-dir` flag is tracked in
> [TASKS.md § INFRA](TASKS.md).

FastFHIR itself is **not** vendored: `.external/FastFHIR` is a symlink to
`../FastFHIR`, resolved by `local_path_override` in `MODULE.bazel`. Bazel
builds it from source as a module dependency, which is how it inherits this
repo's `--compilation_mode=opt`.

---

## Build

```bash
bazel build -c opt //bench:bench_harness
```

PostgreSQL persistence is opt-in — the default target links no libpq, so it
builds anywhere. For `--db` support build `//bench:bench_harness_pg`, which
needs libpq on the host (`brew install libpq`).

## Run Benchmark

```bash
./bazel-bin/bench/bench_harness
```

With PostgreSQL persistence:

```bash
./bazel-bin/bench/bench_harness \
  --db "host=localhost port=5432 dbname=fhir_benchmark user=postgres password=postgres"
```

### CLI Arguments

| Argument | Default | Description |
|---|---|---|
| `--iterations N` | 1 | Measurement iterations per bundle run |
| `--warmup-iterations N` | 1 | Warmup rounds (0 to disable) |
| `--runs N` | 10 | Number of bundle build + arm execution repetitions |
| `--bundle-max-mb N` | 256 | Cap on largest target bundle size (MB) |
| `--bundle-max-mb-explicit` | off | If set, only the specified max is used (no sweep) |
| `--bundle-targets-mb a,b,c` | 1,2,4,8,16,32,64,256 | Explicit sweep ladder |
| `--seed N` | 20260825 | Bundle composition seed. `--seed 0` is random — and a random workload cannot become an artifact |
| `--results-dir DIR` | none | Write the release artifact (`provenance.json`) here. **Refuses, and exits 3, if the provenance record is incomplete** |
| `--profile STR` | auto | Pin `FASTFHIR_PRODUCTION_PROFILE`. Needed when several CMake caches disagree — see below |
| `--db connstr` | none | PostgreSQL connection string (`//bench:bench_harness_pg` only) |

### Output Columns (stdout CSV)

```text
arm,test,duration_ns,bytes_in,bytes_out,target_mb,patients_in_bundle
```

Example:
```
fastfhir,test_1_serialize,76834,0,232534,1,1
fastfhir,test_2_random_access,105541,2000,0,72000,1,1
json_fhir,test_1_serialize,402500,0,105637,1,1
```

`bytes_in` / `bytes_out` are **wire bytes** crossing that stage's boundary, and
they are what makes any size or per-byte throughput claim measurable at all —
before them the harness emitted only nanoseconds:

| Stage | `bytes_in` | `bytes_out` |
|---|---|---|
| `test_1_serialize` | 0 — the input is POCOs, not wire | sealed payload size |
| `test_1_compact` | 0 | compact archive size (FastFHIR arm only — withheld unless the IN-E losslessness gate passes) |
| `test_2_random_access` | 0 | id bytes read — the cross-arm parity accumulator (`ops` = reads) |
| `test_2_compact` | 0 | id bytes read over the compact archive (FF arm only) |
| `test_3_query` | 0 | 0 |
| `test_3_compact` | 0 | 0 — same census over the compact archive (FF arm only) |
| `test_4_enrich` | source stream | enriched stream |
| `test_4_compact` | — | **no row**: the API refuses to open a Builder on a compact archive (write-once, CAPI-10) |

**0 means "not applicable to this stage", never "measured zero".** A serialize
stage reporting 0 bytes out is a defect and the harness warns about it.

`target_mb` is the *requested* corpus size and is not a measurement — do not use
it as a denominator.

### Result provenance and the artifact gate

Every run prints a provenance block to stderr, because the two things that most
change these numbers are invisible in them: the compiled profile and the
compilation mode.

```text
[provenance] fastfhir a9fd4e9 @ a9fd4e9a7430 DIRTY
[provenance] profile us-core,billing,medication-admin,supply (operator) -- 80 code-system enums, 44 generated .cpp, 36 resource types
[provenance] build opt clang 21.0.0 on macos/arm64 (Apple M5 Pro)
[provenance] corpus 342 docs, 1292 MB, sha256 958b1ceb2642
[provenance] benchmark @ 38705db50262 DIRTY, seed 20260825
```

With `--results-dir` the same record is written as `provenance.json` — and the
harness **refuses to write it** (exit 3) if any required field is unestablished.
Two refusals are deliberate and will bite:

- **`compilation_mode` must be `opt`.** The same code runs ~10x faster at -O2
  than -O0, and nothing in a results row says which you built. This is the Debug
  trap, enforced.
- **An ambiguous profile is rejected.** There is no runtime signal for
  `FASTFHIR_PRODUCTION_PROFILE`, so it is read from the CMake caches under
  `../FastFHIR/*/` — and on a working machine those disagree (three build trees,
  two different values here). The newest wins for the stderr summary, but an
  artifact requires `--profile` to pin it. `codesystem_enums`, `generated_cpp`
  and the resource list are recorded alongside as corroboration: the profile
  string is a claim, those are evidence from the tree actually compiled in.

The corpus digest is a SHA-256 over a manifest of every file's name, size and
content hash — so a regenerated corpus is never mistaken for the same one. It is
memoized in `datasets/.corpus_sha256.cache` keyed on (file count, total bytes,
newest mtime); the first run after a corpus change re-hashes ~1.3 GB.

---

## Validate

```bash
# Bazel test
bazel test //bench:timing_conformance_test

# Direct execution
./bazel-bin/bench/bench_timing_conformance
```

The conformance test verifies:
1. All metrics have positive duration values
2. FastFHIR and JSON arms both report exactly `patients=1`
3. Both arms report matching birthdate values

### Read-path gate

The one validation target that builds today. It reports whole-document
traversal cost as an average per Bundle entry and **fails (exit 1) if any
average exceeds 50 µs/entry**:

```bash
bazel run //bench:read_path_bench -- /path/to/bundle.ffhr
```

The binary self-reports its optimization mode. The 50 µs/entry bound is a
regression gate for *optimized* builds — a Debug FastFHIR measures ~10× slower
on these paths and will fail the gate for the wrong reason. See
[`read_path_bench.cpp.md`](bench/read_path_bench.cpp.md).

---

## Analyze

Use `notebooks/benchmark_results.ipynb` to read from PostgreSQL.

---

## Known Issues & Incident Log

### Open

| Issue | Document | Summary |
|---|---|---|
| **Harness does not compile** | [`TASKS.md`](TASKS.md) | Upstream FastFHIR API redesign; all four arms affected |
| Hand-rolled CODE encoding is wire-invalid | [`TASKS.md`](TASKS.md) § PORT-7 | `bench_test_1.hpp` open-codes a flag scheme that upstream moved from bit 30 to bit 31 |
| Opaque resources not query-navigable | [Result provenance](#result-provenance) | 1,444 Synthea `ImagingStudy` records skip stage 3 |
| No build provenance in results | [`TASKS.md`](TASKS.md) § PROFILE | Profile + upstream SHA are invisible to Bazel and unrecorded |
| Google FHIR static link failure | [`arm_google_fhir.cpp.md`](bench/arm_google_fhir.cpp.md) | Static archives cause undefined symbol explosions from absl/protobuf/utf8_range |
| Google arm `birthdate` unit mismatch | [`notes.md`](notes.md) | Microsecond epoch vs ISO in every other arm; arm also excluded from `validate_parity()` |
| Resource coverage gap | [`RESOURCE_COVERAGE_ANALYSIS.md`](RESOURCE_COVERAGE_ANALYSIS.md) | Only 2/37 compiled FHIR resources tested |
| CODE assignment type expectations | [`FFHRnotes.md`](FFHRnotes.md) § 4 | Now a hard compile error: `TypeTraits<std::string>` is undefined |

### Resolved upstream (kept for history)

| Issue | Resolution |
|---|---|
| Missing installed headers | Moot — consumption is a Bazel module, not an install tree. CMake also now installs an explicit header set. |
| Ingestor symbols not exported | Misdiagnosed. `fastfhir_ingestor` is a *separate target*, which this repo links via `//:fastfhir_runtime`. |
| Relative `../generated_src/` includes | Fixed upstream — no such includes remain in `include/`. |
| `Fields` vs `FieldKeys` namespace | Fixed — both namespaces coexist in `FF_FieldKeys.hpp`. |
| README examples not compile-tested | Fixed — upstream `py_readme_examples` runs them verbatim. |
| FastFHIR ingest CAPACITY errors | CMake-era issue (`SIMDJSON_THREADS_ENABLED` parity with `ff_ingest`). Not reachable under the Bazel module build. |
