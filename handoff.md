# benchmark → benchmark handoff: retooling around the "Why FastFHIR?" claims

**Written 2026-08-25**, from `FastFHIR-benchmark`, after getting the four-arm
harness building and running again. **Amended 2026-08-26** — see the amendment
notes marked ✎ below; the decisions they record now live in
[`TASKS.md` § Decisions](TASKS.md) (D1 macro parity, D2 `value[x]` tiering,
D3 serialization model) and the upstream asks are filed as
`../FastFHIR/TASKS.md` CAPI-1…CAPI-6, I3.6, I3.7.

**You are reading this from `../FastFHIR-benchmark`.** FastFHIR itself is
`../FastFHIR/`; its `CLAUDE.md` is not auto-loaded — read it before touching
that tree.

Companion documents, in reading order:

1. [`notes.md`](notes.md) — what was silently broken in the old harness. Read it
   first; several of its findings are *why* the retool is necessary.
2. This document — what to build instead, and why.
3. [`TASKS.md`](TASKS.md) — the working backlog.

---

## The job

`../FastFHIR/README.md` § **Why FastFHIR?** makes ~20 discrete claims. This repo
is the thing that is supposed to prove them, and FastFHIR's own `CLAUDE.md`
already forbids asserting a performance claim there without citing a result from
here.

The retool is therefore not "make the benchmark better." It is:

> **Make every claim in § Why FastFHIR? map to exactly one instrument here, one
> release artifact, and one citable number — or delete the claim.**

Everything below follows from that.

---

## Two things to fix before anything else

### A. The README already cites a number this repo cannot produce

> *"Measured at 2.4–3.4x the receiver-side throughput of an `orjson` JSON
> pipeline across 1.7–162 MB bundles (FastFHIR-benchmark, Test 1)."*
> — `../FastFHIR/README.md`, § 1, Zero-Copy Engine

**There has never been an `orjson` arm in this repository.** I grepped the whole
tree: the only occurrences of the string "orjson" are in text I wrote in
`README.md` last week. The four arms are FastFHIR, **nlohmann::json + simdjson**
(C++), HL7v2, and Google protobuf. No Python, no orjson, anywhere.

Two further mismatches in the same sentence:

- **"receiver-side throughput" is not Test 1.** Test 1 here is *serialize* — the
  sender side. The receiver-side stage is Test 2 — since D4 (2026-08-26)
  random access, i.e. time-to-value, which is exactly receiver-side work. The
  citation points at the wrong test.
- **"1.7–162 MB" are not sweep points.** ✎ *Corrected 2026-08-26:* the default
  ladder is powers of two — 1/2/4/8/16/32/64/256 MB
  ([`bench/main.cpp:134-143`](bench/main.cpp:134)) — and is truncated only when
  `--bundle-max-mb` is passed explicitly ([`:190`](bench/main.cpp:190)), so the
  default does reach 256 MB. The real objection is narrower and still fatal:
  1.7 and 162 are not points on that ladder, and nothing has been **run** above
  16 MB (notes.md §8). Filed upstream as I3.6.

So the headline performance claim in FastFHIR's README is currently uncited in
substance while appearing to be cited. That is the single highest-priority item
in the retool, and it is the reason an **orjson arm** leads the work below.

Until it exists, either soften the claim upstream or mark it provisional. Do not
leave a specific ratio attributed to a test that did not produce it.

### B. Nothing in this repo measures size

The CSV is `arm,test,duration_ns,target_mb,patients_in_bundle`. The PostgreSQL
schema carries the same seven columns. `target_mb` is the *requested* corpus
size, not the produced wire size.

`EnrichMetricsSummary` does compute `source_bytes` / `enriched_bytes` — and they
are never printed or stored.

So **two of the four § 1 claims have no instrument at all**: "Fraction of Size on
Disk" (the 66 % compact-archive figure) and any size dimension of the zero-copy
claim. A benchmark that only emits nanoseconds cannot validate a claim about
bytes.

---

## Claim register

IDs are `WF-<section>.<item>`, matching § Why FastFHIR? order. **Status** is
what this repo can support *today*.

### § 1 — Extreme Performance & Compact Size

| ID | Claim | Instrument needed | Status |
|---|---|---|---|
| **WF-1.1** | O(1) random access; bypasses O(N) HL7v2 scan and O(N) JSON hashing + DOM build | Depth/ordinal sweep — time-to-first-value vs field position | ✅ **Test 2 (D4)** — see ✎ below |
| **WF-1.2** | Zero heap allocation for navigation + reflection; `entries()` is the one exception, ~1 ns/element | Allocation counter, pass/fail gate | ❌ no instrument |
| **WF-1.3** | 2.4–3.4× receiver-side throughput vs orjson, 1.7–162 MB | **orjson arm** + receiver-side stage + large corpus | ❌ arm does not exist — but see ✎ below: the *mechanism* now has a number |
| **WF-1.4** | Compact archive up to −66 % on sparse resources | Size table across formats, sparse vs dense | � **compact size measured + gated 2026-08-26 (IN-E partial)** — full table still open, and the published figure may itself be lossy — see ✎ below |

✎ **WF-1.1 — measured, and it is the headline, 2026-08-26.** Test 2's walk reads
nodes in *layout order*, which is a contiguous tape's best case and an
offset-indexed layout's worst — and no consumer reads a bundle in write order.
`BENCH_RANDOM_ACCESS=N` does what a query does: navigate from the root to a
random `Bundle.entry`, read the resource's `id`. Identical accumulators in both
arms at every size.

| bundle entries | FastFHIR ns/read | simdjson ns/read | ratio |
|---:|---:|---:|---:|
| 5,844 | 92.6 | 5,913 | 64× |
| 24,661 | 342.0 | 38,225 | 112× |
| 105,202 | 929.2 | 366,602 | 395× |
| 226,925 | 791.5 | 2,030,842 | **2,566×** |

**simdjson's cost grows linearly with entry count; FastFHIR's does not.** Read in
layout order the tape wins 22×; read out of order FastFHIR wins 2,566×. Both are
true, and reporting either alone is a misrepresentation — which is why the
existing Test 2 chart, taken alone, argued against the very claim the library
leads with. Instrument B must ship **both orders**, and with the indexed-JSON
counterpoint (a real consumer would build an index once; the crossover is
estimated near 50k lookups and is **not yet measured**).

✎ **2026-08-26 (later) — Instrument B IS Test 2 now (TASKS.md D4).** The
materialize walk is retired; [`bench/bench_test_2.hpp`](bench/bench_test_2.hpp)
runs the random-access probe on all four arms with a cross-arm byte parity
gate, emitting `test_2_random_access` rows. The walk chart is gone with it.
Still open: the depth sweep (this instrument probes ordinal position only) and
the indexed-JSON counterpoint (**IN-B3**).

✎ **WF-1.3 — first real evidence, 2026-08-26.** Splitting Test 2 into "make the
bytes addressable" and "walk every node" (`BENCH_SPLIT=1`, target 2048, ~141 MiB
FastFHIR stream / ~66 MiB JSON):

| | FastFHIR | simdjson |
|---|---:|---:|
| open / parse | **7.4 µs** | **27.4 ms** |
| walk | 84.78 ms | 7.14 ms |
| ns per node | 27.39 | 1.22 |

**The receiver-side step the claim is actually about is 3,696× faster.** That is
the zero-copy engine doing exactly what § 1 says. It is not yet WF-1.3 — the
claim names orjson, a ratio, and a corpus range, and this is simdjson at one
size — but it is the first number in this repo that bears on the claim at all,
and it is far larger than the 2.4–3.4× the README quotes. Instrument A should
report **both** halves: time-to-addressable and time-to-all-nodes. Reporting only
the second (which is what the former Test 2 did) inverts the result.

✎ **WF-1.4 is worse than "no instrument" (2026-08-26).** The −66 % / −41 %
figures entered `../FastFHIR/README.md` on 2026-04-27 (`d9b086f`) and the bytes
have never changed since. Compaction was **dropping every scalar array, the URL
intern table and every `Attachment.data`** until `459e8d8` (2026-08-23). The
table's own sparse example is `Patient (id, gender, active, name/given/family)`
— and `given` is a scalar string array, i.e. one of the dropped categories. So
part of that 66 % may be data the compactor deleted rather than packed. Nothing
pins the numbers today: `tests/cpp/test_compactor.cpp:147` only asserts
`compact < standard`. Filed upstream as I3.7. **Consequence for Instrument E:
do not adopt −66 % as a baseline to reproduce.** Measure from zero.

### § 2 — Type Safety & Validated FHIR Format

These are **correctness** claims. They need a differential conformance harness,
not a timer — see [Instrument F](#f--preservation-matrix-wf-2x-wf-54).

| ID | Claim | Status |
|---|---|---|
| **WF-2.1** | `RECOVERY_TAG` gives safe polymorphic resolution; prevents garbage reads / overflows | ⚠️ partly covered upstream (342/342 round-trip); nothing here |
| **WF-2.2** | Native polymorphic support — `value[x]` **and** `Bundle.entry.resource` — without lossy adapters | 🔴 **`value[x]` excluded from every arm** (notes.md §4) — now **tiered**, see ✎ below and TASKS.md **D2** |
| **WF-2.3** | Registered extensions → typed binary fields (WASM codecs); unknown extensions preserved with URL tracking | ❌ untested here |
| **WF-2.4** | Primitive extensions (`_`-prefixed) survive round-trip; protobuf JSON serializers do not implement this | ❌ untested here — and this is the strongest differentiator in the list |

**WF-2.2 is the uncomfortable one.** The claim singles out `valueQuantity` /
`valueString` / `valueCodeableConcept`, and those are exactly the fields the
current harness skips, because a block-typed `ChoiceEntry` carries a source-arena
offset that cannot cross arenas (notes.md §4). The benchmark cannot presently
demonstrate the feature the README leads with.

✎ **Resolved as a design question, 2026-08-26 (TASKS.md D2).** The open
temptation was to drop `value[x]` and claim choice polymorphism as a
FastFHIR-only capability. **Checked, and that would have been false:** protobuf
carries it natively as a `oneof` inside a nested `ValueX` message
(`third_party/google_fhir/.../observation.proto:159-176`), and HL7v2 carries it
natively as OBX-2 value type selecting the type of OBX-5
([`bench/hl7v2_message.hpp:71`](bench/hl7v2_message.hpp:71)). **The blocker is
ours, not theirs** — filed upstream as CAPI-3. So the field is *tiered*, not
stripped:

- **Tier S** (scalar variants) — all four arms, unblocked today; only the HL7v2
  arm still writes a mocked `"1"` (TASKS.md **PA-2**). Fix that and Tier S is at
  four-arm parity with no upstream dependency.
- **Tier B** (block variants) — blocked on CAPI-3; HL7v2 is partially lossy here
  (`Quantity`→NM+OBX-6, `CodeableConcept`→CWE; `Range`/`Ratio`/`SampledData`
  degrade), and **that lossiness is a result to report, not a reason to skip**.
- **Tier X** (`_`-prefixed primitive extensions on choice values) — the genuine
  differentiator, and a **conformance** result: it belongs to Instrument F, never
  to a timing row.

No `value[x]` number is published without its tier.

### § 3 — Memory Safety & Integrity

| ID | Claim | Instrument needed | Status |
|---|---|---|---|
| **WF-3.1** | OS-protected memory (mmap/VirtualAlloc); stable pointers; no heap fragmentation vector | RSS + fragmentation trace; pointer-stability test under growth | 🟡 truncation half ✅ (IN-G test 1); full instrument still IN-D |
| **WF-3.2** | `RECOVERY_TAG` catches type confusion at runtime | Mis-typed-read corpus; corruption suite | ✅ **IN-G test 3 (2026-08-26)** — 560 typed + 297 opaque entries; wrong-type reads refused |
| **WF-3.3** | Checksum footers detect corruption (integrity, not authenticity) | Bit-flip detection-rate sweep | ✅ **IN-G tests 2 (2026-08-26)** — structural vs payload vs real-SHA-256 footer |
| **WF-3.4** | Deterministic layout; no dynamic allocation, no surprise reallocations | Same counter as WF-1.2 | ❌ IN-C |

The README is already careful here — it scopes 3.2 to *malformed* not *hostile*
data and 3.3 to integrity not authenticity. **Preserve those qualifiers in any
artifact.** Do not let a benchmark headline round them off.

### § 4 — Clinical Informatics

| ID | Claim | Instrument needed | Status |
|---|---|---|---|
| **WF-4.1** | Lazy enrichment — "you only pay for the exact bytes you traverse"; append without touching other bytes | **Bytes/pages-touched counter**, not a timer | ❌ — see [Instrument D](#d--bytes-touched-wf-41) |
| **WF-4.2** | Lock-free concurrent generation across a thread pool | Thread-scaling curve | 🔴 the parallel path in `arm_fastfhir.cpp` is **commented out** |
| **WF-4.3** | Pluggable profiles; out-of-profile resources still round-trip intact | Opaque-path cost + opaque-fraction reporting | ⚠️ upstream proves losslessness; cost unmeasured |

**WF-4.2 needs a look before you design for it.** `arm_fastfhir.cpp` contains
disabled `dispatch_apply_f` and `std::execution::par_unseq` blocks. The
concurrency claim is real in the library, but this repo currently exercises only
the serial path, so it proves nothing either way.

### § 5 — Developer Ergonomics

Mostly not benchmarkable, and that is fine — but two are:

| ID | Claim | Instrument needed | Status |
|---|---|---|---|
| **WF-5.1** | Static typed keys bypass runtime string hashing | Micro-benchmark: `node[Fields::PATIENT::ACTIVE]` vs `node["active"]` | ❌ both APIs exist; never compared |
| **WF-5.4** | FHIR-accurate JSON round-trip vs `MessageToJsonString` | Same preservation matrix as WF-2.x | ❌ |

WF-5.2 and WF-5.3 are API-shape claims. Cover them with compiled examples (as
upstream's `py_readme_examples` does), not with timings.

---

## Instruments to build

Six instruments cover the whole register. Each is a separate binary with a
narrow job — **not** another stage bolted onto the 4×4 grid.

The existing four-arm × four-stage harness stays as a **smoke and regression
rig**. It is the wrong shape for claim validation: a claim like "O(1) random
access" is a *curve*, and the current harness only ever reports a scalar per
(arm, stage, size).

### A — Receiver-side throughput (WF-1.3)

The one that unblocks the existing citation.

- Add an **orjson arm**. It is cross-language, so define the boundary
  explicitly: measure a *process* that receives bytes and makes all fields
  addressable. For orjson that is `orjson.loads()` → dict. For FastFHIR it is
  mmap + header validation. For protobuf it is `ParseFromArray`.
- Run at the claimed corpus range — **1.7 MB to 162 MB** — not the current
  1–16 MB default.
- Report throughput as MB/s of *input wire bytes*, plus the ratio the README
  quotes.
- Keep the existing C++ `nlohmann`/`simdjson` arm alongside it. orjson answers
  the README's claim; simdjson answers "what if the competitor is fast C++?",
  which is the harder and more honest question.

⚠️ Do not let the Python process-start cost land inside the measurement. Warm
the interpreter, measure only the receive+parse call, and say so in the artifact.

### B — Random-access curve (WF-1.1)

✅ **SHIPPED 2026-08-26 as Test 2 (TASKS.md D4)** —
[`bench/bench_test_2.hpp`](bench/bench_test_2.hpp), all four arms, cross-arm
byte parity gate (exit 2 on mismatch). What shipped probes random **ordinals**
at one depth (resource id); the depth sweep below is the remaining half.

- Pick target fields at increasing **depth** (Patient.id → …name[0].given[1] →
  …extension[3].valueCodeableConcept.coding[0].code) and increasing **ordinal
  position** (entry[0] … entry[N-1]) in a large bundle.
- Measure **cold time-to-value**: from "bytes are in RAM" to "value in hand",
  once, per field, without a preceding full walk.
- The comparison is only meaningful if each arm pays its own real entry cost:
  FastFHIR pays ~0 setup, JSON pays DOM construction, HL7v2 pays segment
  scanning. **That asymmetry is the claim** — do not amortize it away by
  pre-parsing for the other arms.
- Output is two curves per arm (time vs depth, time vs ordinal). The claim holds
  if FastFHIR's slope is flat and the others' are not.

### C — Allocation gate (WF-1.2, WF-3.4)

- Count allocations, do not time them. Override global `operator new`/`delete`
  (and `malloc` on macOS via zone hooks) around a navigation sequence.
- **Assert exactly 0** for field navigation and reflection.
- **Assert exactly 1 per `entries()` call**, and measure ns/element to check the
  ~1 ns figure at `-O3`.
- This is a **pass/fail test target**, not a metric row. It belongs in
  `bazel test`, and it should fail the build when violated.

### D — Bytes touched (WF-4.1)

The most valuable new instrument, and the one with no equivalent today.

"You only pay for the exact bytes you traverse" is a claim about **residency**,
not speed. Measure it directly:

- `mmap` the stream, read one `Patient.id` from a large bundle, then count
  resident pages (`mincore`) and minor faults (`getrusage`).
- Expect FastFHIR to touch O(1) pages regardless of bundle size, while JSON and
  protobuf must fault in the whole document.
- Plot pages-touched vs bundle size. A flat line against a linear one is a far
  stronger artifact than a nanosecond ratio, and it is much harder to argue with.

Same instrument covers "append without touching any other byte": count pages
dirtied by an enrich.

### E — Size table (WF-1.4)

🟡 **PARTIAL 2026-08-26**: the harness emits `test_1_compact` — the FastFHIR
compact archive size — gated on losslessness: the row is withheld unless the
compact stream re-parses to JSON semantically identical to the standard
stream's. `fig2_wire_size` plots it against the JSON baseline. The remaining
rows below (gzip columns, protobuf, sparse/dense split) are still open.

- Per corpus document, emit: raw JSON, gzip(JSON), FastFHIR standard, FastFHIR
  compact, protobuf, gzip(protobuf), HL7v2.
- **Honor the qualifier.** The claim is "up to 66 % *on sparse resources*".
  Report sparse and dense corpora as separate rows and never blend them into one
  headline.
- **Gate on losslessness first.** Compaction was silently dropping scalar
  arrays, the URL intern table and `Attachment.data` until recently
  (`../FastFHIR/handoff.md` §3). Quote no compact size until the compact export
  is byte-identical to the standard one for that document.

### F — Preservation matrix (WF-2.x, WF-5.4)

Not a benchmark. A **differential conformance table**, and probably the most
publishable artifact in the whole set.

- Round-trip each corpus document through each library: JSON → library → JSON.
- Diff against the input and classify every difference: preserved / reordered /
  coerced / **lost**.
- Break out the rows the claims name specifically: `value[x]` choice fields,
  `Bundle.entry.resource` polymorphic slots, `_`-prefixed primitive extensions,
  unknown extensions, code systems.
- Include `google::protobuf::util::MessageToJsonString` as a column, since § 5.4
  calls it out by name.

This is where FastFHIR's §2 claims either land or don't, and unlike a timing
number it cannot be attacked on methodology grounds — a field is either in the
output or it isn't.

### G — Resilience & integrity (WF-3.1, 3.2, 3.3; WF-4.2 correctness half)

✎ *Added 2026-08-26.* This instrument already had a full design spec — it was
sitting in [`TODO.md`](TODO.md), written before the port and marked blocked on
it, which is why it never appeared in this list. The blocker closed on
2026-08-25. `TODO.md` is now scoped as **the design spec for this instrument**
and is not a second backlog; the tracker entry is TASKS.md **IN-G**.

Four tests, all pass/fail, none of them timers: truncation detection via the
`VALIDATION` word, bit-flip detection separating structural from payload damage,
type-confusion prevention via `RECOVERY_TAG` dispatch, and concurrent build
integrity under contention.

Two constraints carry into it from this document:

- **Preserve README § 3's qualifiers verbatim.** 3.2 is scoped to *malformed*
  data, not *hostile* (unfuzzed — upstream G1); 3.3 is *integrity*, not
  *authenticity*. A resilience artifact is exactly the place those get rounded
  off, so they are restated at the top of the spec.
- **Say what the other arms do, not what they lack.** Protobuf type-checks at
  the message level; the FastFHIR claim is about field-level dispatch inside a
  block and about detecting a mislabelled but well-formed resource. Measure that
  narrower thing. A differentiator that survives review beats a broad one that
  does not.

Test 4 is additionally blocked on the commented-out parallel path
([`bench/arm_fastfhir.cpp:124-172`](bench/arm_fastfhir.cpp:124)) — same blocker
as WF-4.2's timing half.

---

## Release artifacts

The user-facing requirement: **the FastFHIR README should cite an artifact, not
a number typed by hand.**

### Shape

```
results/
  <fastfhir-tag>/                 # e.g. v2026.1.0
    provenance.json               # see below — REQUIRED, artifact is void without it
    claims.json                   # claim ID -> measured value -> pass/fail
    receiver_throughput.csv       # instrument A
    random_access.csv             # instrument B
    allocations.txt               # instrument C (pass/fail)
    bytes_touched.csv             # instrument D
    size_table.csv                # instrument E
    preservation_matrix.csv       # instrument F
    resilience.csv                # instrument G (pass/fail per scenario)
```

### `provenance.json` — non-negotiable fields

Every one of these silently changes the result:

| Field | Why |
|---|---|
| `fastfhir_sha`, `fastfhir_tag` | `.external/FastFHIR` is a **symlink to a live tree**, not a pinned checkout |
| `fastfhir_dirty` | if the tree had uncommitted changes, the artifact is not reproducible |
| `production_profile` | e.g. `us-core,billing,medication-admin,supply` — decides which resources are typed vs opaque |
| `codesystem_enums`, `generated_cpp` | cheap corroboration that the profile is what you think |
| `compilation_mode` | the Debug trap: same code is ~10× slower at `-O0` |
| `compiler`, `compiler_version`, `os`, `arch`, `cpu_model` | |
| `corpus_id`, `corpus_sha256`, `corpus_doc_count` | |
| `benchmark_sha`, `seed` | |

**Refuse to emit an artifact if any of these is missing.** A number without
provenance is not evidence, and the profile in particular is invisible to Bazel
— see [README § Result provenance](README.md#result-provenance).

### `claims.json`

The contract between the two repos:

```json
{
  "WF-1.3": {
    "claim": "2.4-3.4x receiver-side throughput vs orjson, 1.7-162 MB",
    "instrument": "receiver_throughput.csv",
    "measured": { "ratio_min": 0.0, "ratio_max": 0.0, "corpus_mb": [1.7, 162] },
    "status": "not_yet_measured",
    "asserted_in": "FastFHIR/README.md#why-fastfhir"
  }
}
```

Then upstream cites `claims.json#WF-1.3` at a release tag rather than restating
a ratio. When a claim regresses, the artifact changes and the citation stays
valid — which is the whole point.

### Rules

- **A claim with no `claims.json` entry does not belong in § Why FastFHIR?**
  That is the enforcement mechanism for FastFHIR's existing CLAUDE.md rule.
- `status` is one of `validated` / `not_yet_measured` / `refuted`. **Ship
  `refuted` honestly** — a benchmark that can only confirm is not a benchmark.
- Artifacts are generated on release tags, never edited by hand.

---

## Sequencing

Ordered by what unblocks the most, not by difficulty.

✎ *Reordered 2026-08-26.* The original list opened with `value[x]`, which is
blocked on an upstream change (CAPI-3) that this repo is not allowed to make.
Nothing that leads the critical path should be blocked on another tree, so the
unblocked half of `value[x]` moved to the front and the blocked half moved to
where its dependency can land.

1. **`value[x]` Tier S** (TASKS.md PA-2). Unblocked, small, and it removes the
   blanket statement that no arm serializes `value[x]`. The HL7v2 arm already
   switches on the tag — it just writes `"1"` instead of the value. ⚠️ **It is
   4.8 % of the choice surface**, not a solution: a full-corpus census
   (2026-08-26) puts `valueQuantity` at 57.7 % and `valueCodeableConcept` at
   35.0 %, both Tier B. Ship it, label it, and keep CAPI-3 as the item that
   actually matters.
2. **Emit size and provenance** (IN-0). Cheap; instrument E and the entire
   artifact story depend on it. Nothing else should ship first.
3. **Instrument F, preservation matrix.** No new arms, no timing methodology to
   defend, validates four claims at once (WF-2.2, 2.3, 2.4, 5.4), and it is
   where Tier X — the real differentiator — gets proven.
4. **Instrument A, orjson arm.** Retires the uncited citation (I3.6).
5. **Instruments B, C, D.** The performance-character claims. D is the strongest
   of the three and the best candidate to cite upstream.
6. **Instrument G, resilience** (TODO.md). Independent of the timing work; can
   run in parallel with 3–5 if there is capacity.
7. **`value[x]` Tier B** (PA-3), **when CAPI-3 lands upstream**. The fix belongs
   at **hydration** — deep-copy the block from the source `Parser` into the
   destination builder — **not** in the assignment sink as originally written
   here. Putting it in the sink would tie it to the serialization-model decision
   (TASKS.md D3) for no reason.
8. **Instrument E.** Last: gated on compact-losslessness, and on I3.7 resolving
   what the −66 % figure actually measured.
9. **Re-enable the concurrent path** before attempting WF-4.2 (IN-H).

---

## Rules carried forward

From [`notes.md`](notes.md) — these are not style preferences, they are the
things that already went wrong:

- **The macro-guarded shared header survives the retool, and per-arm inline
  namespaces are mandatory.** ✎ *Decided 2026-08-26 — TASKS.md **D1**.* The
  four side-by-side `#if defined(ARM_*)` implementations of each field are the
  auditable artifact of parity, and that outranks line count. notes.md §1's
  alternative — arms as classes or templates over a shared interface — is
  **rejected**; it trades the audit for an ODR surface the namespaces already
  close. ASan in CI (TASKS.md HY-1) is the compensating control.
- **ASan in CI.** An ODR violation was live for the life of the benchmark and no
  ordinary build ever complained.
- **Every timed stage must produce a value that escapes.** `(void)result` let the
  optimizer delete a whole traversal. (notes.md §2)
- **Nothing between the last real operation and `stop_ns()`.**
- **Cross-arm validation must cover every arm.** The Google arm is excluded from
  `validate_parity()`, which is how a `birthdate` unit mismatch survived
  undetected while the docs described a stubbed-stages problem that did not
  exist. (notes.md §5b)
- **`validate_FFHR_stream()` passing is not proof the stream is readable.** It
  accepted a stream the generated deserializer segfaulted on. Gate Test 1 output
  on a real round-trip, not on the validator. (notes.md §3)
- **Keep `--compilation_mode=opt`**, keep `--seed` deterministic, and record both.
- **Treat all pre-2026-08-24 FastFHIR numbers as void** — they predate the
  opaque-JSON change (`../FastFHIR/handoff.md` §1).

## Do not

- **Do not enable the `imaging` grouping** in `../FastFHIR/CMakePresets.json`.
  The 1,444 opaque `ImagingStudy` resources are deliberate coverage proving the
  fallback is lossless.
- **Do not commit into `../FastFHIR` from here.** It is a symlinked live tree.
- **Do not publish a number without its `provenance.json`.**
- **Do not quote a compact size before the compact export is verified
  byte-identical** for that document.
- **Do not blend sparse and dense corpora** into one size headline; the claim is
  scoped to sparse resources and the scope is load-bearing.
