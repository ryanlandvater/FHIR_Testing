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

## Test 5 (recovery comparison) — design, results, and KNOWN FLAWS

**Status: WORKING BUT METHODOLOGICALLY WRONG — do not cite.** The recovery
comparison across the four formats was built and runs, but the HL7v2 result is
inflated by flaws in the test, not by the format. Identified 2026-08-26; fixing
deferred to the next session. This section is everything known about the test.

### What it does (as built)

- **Artifacts**: the harness's `--dump-artifacts` writes one representative
  bundle's Test-1 wire payload per arm (FFHR / JSON / protobuf TLV / HL7v2
  batch) via `ArmRunResult.test1_payload`.
- **Corruptor** (`bench/corruption_probe.cpp --mode corrupt`): flips `k` random
  bits in the format's STRUCTURAL set only:
  - FFHR: FF_HEADER region + each resource block's 10-byte header
    (VALIDATION + RECOVERY_TAG).
  - JSON: brace / bracket / quote / colon / comma characters.
  - protobuf: TLV record headers (type byte + 4-byte length).
  - **HL7v2: `\r` segment terminators + the first 3 chars of each segment
    name. NOT pipes (`|`) or carrots (`^`)** — see Flaw D.
- **Recovery** (`--mode recover`, runs in a SEPARATE process from the
  corruptor — the recoverer sees only the corrupted bytes):
  - FFHR: `FastFHIR::Recovery` (library API) — VALIDATION-word resync;
    whole-stream fallback on header damage; adjacent-damage dedup.
  - JSON: full-parse fast path; on failure, entry-span reparse at each
    `"resource"` marker.
  - protobuf: TLV length resync + `ParseFromArray` confirmation.
  - **HL7v2: counts lines that still begin with a known 3-char segment name
    (MSH/PID/OBX/...).** — see Flaws A/B/C.
- **Driver** (`scripts/recovery_sweep.py`): subprocess pairs (corrupt →
  recover) per format × k ∈ {0..512} × 20 trials → `results/recovery_curve.csv`
  → `fig8_recovery`.

### Results (median % recoverable, 20 trials)

| bits | FastFHIR (entries) | JSON (entries) | protobuf TLV (entries) | HL7v2 (SEGMENTS) |
|---|---:|---:|---:|---:|
| 16 | 98.9 | 98.9 | 76.4 | 99.8 |
| 64 | 95.7 | 95.9 | 46.2 | 99.3 |
| 128 | 91.6 | 91.9 | 32.5 | 98.6 |
| 256 | 84.0 | 84.7 | 15.4 | 97.2 |
| 512 | 70.3 | 71.6 | 9.6 | 94.4 |

The protobuf collapse (one bad length header derails the walk) is real and
defensible. **The HL7v2 > FastFHIR result is NOT defensible** — it is a
measurement artifact. The user's read is correct: random delimiter corruption
with a naive recovery cannot legitimately beat a block-validated format at
recovering content.

### The flaws (in order of severity)

**A. Units are not comparable — HL7v2 counts SEGMENTS, the others count
ENTRIES.** The denominators: 8,939 segments (hl7v2) vs 1,473 entries (FFHR /
JSON / protobuf). A segment is a line; an entry is a whole resource. "94.4% of
segments still start with a known name" is not "94.4% of the clinical content
recovered." The figure's caveat says the units differ; that is not enough — the
numbers must not share an axis until they measure the same semantic.

**B. Damage density is not normalized.** `k` bits against each format's own
structural position count: 512 flips is ~5.7% of hl7v2's ~9k segment/terminator
positions but ~60% of FFHR's ~860 block-header positions. Same `k` = wildly
different damage intensity. The x-axis needs to be "fraction of structural
positions corrupted" or per-unit flips (e.g., flips per 1,000 segments), not an
absolute bit count.

**C. The HL7v2 recovery overcounts.** It counts a line as recovered if its
first 3 bytes are a known segment name — nothing else:
- A `\r` flip MERGES two segments; the merged line still starts with the first
  segment's name → counted recovered, though a boundary (and both segments'
  integrity) is destroyed.
- A name-char flip on, e.g., 'B' of OBX → "OCX" → uncounted (correct) — but a
  flip on the SEGMENT's field content (a `|` inside a field) is not even in the
  structural set, so the corrupted field is never tested.
- No parse verification: the recovery never checks that the segment splits into
  the expected field count or that any field value survived. It detects
  boundaries, not content.

**D. The HL7v2 structural set excludes the real-world cascade triggers.**
Per FastFHIR's own README §3: "HL7v2 ... a delimiter flip cascades." The
cascade triggers are `|` (field), `^` (component), `&` (sub-component), `~`
(repetition). The test corrupts only `\r` + names — the LOWEST-blast-radius
bytes in the format. Pipe/carrot corruption is both the real failure mode and
the damaging one; excluding it systematically understates v2's vulnerability.
(FFHR and JSON get their syntax-critical bytes; v2 does not.)

**E. Blast-radius asymmetry.** Each `\r`/name flip damages at most one hl7v2
line. FFHR header flips can invalidate the root (whole-stream fallback) and
adjacent block damage chains. The formats are not under symmetric attack.
**F. The y-axis semantic is ASYMMETRIC — and the curves are identical by
construction.** JSON's "recovered" is CONTENT-VERIFIED (a full nlohmann parse
of the entry span must succeed); FFHR's "recovered" is HEADER-ONLY
(`FastFHIR::Recovery` counts blocks with a valid VALIDATION word + known
RECOVERY_TAG — the interior fields are never walked). Measured 2026-08-26:
at one flip, JSON loses 1–2 entries, FFHR loses exactly 1. Both formats have
~one-unit blast radius per structural flip, and both recovery routines resync
past the damaged unit — so the curves track each other BY CONSTRUCTION, not
because the formats recover comparably. Two consequences:
- The near-identical JSON/FFHR curves do NOT demonstrate the recovery
  routine's value — nothing in the current metric isolates what differs.
- The asymmetry may even FAVOR FFHR: JSON does strictly more work per unit
  (full parse) and still matches FFHR's looser count. FFHR's true
  content-recovery rate is lower than measured.
What the fixed test must isolate instead: (1) detection WITHOUT parsing —
FFHR validates offsets O(1) per block; JSON discovers damage only by parsing;
(2) self-delimiting boundaries — FFHR's VALIDATION is positional (a flip
cannot move a block boundary), JSON's resync marker (`"resource"`) is itself
corruptible content; (3) the cost of a flip at DENSITY (multiple flips
interacting) rather than the per-unit rate.
### What is sound in the test (keep)

- Corruption and recovery are INDEPENDENT processes (subprocess pairs) — the
  recoverer genuinely sees only corrupted bytes.
- The FFHR recovery lives in the library (`FastFHIR::Recovery`) with the
  CAPI-13 ctor fix (throws, not SEGV) and the adjacent-damage dedup guard.
- The protobuf collapse and the FastFHIR≈JSON mid-damage tracking are honest.
- The per-format recovery semantics (resync at the next self-consistent unit)
  are the right idea; the UNITS and the DENSITY are the problem.

### Fix directions for next session (not started)

0. **The recovery semantic that can legitimately differentiate FFHR —
   edge/relationship recovery via cross-validation (Ryan, 2026-08-26; full
   spec in TASKS.md IN-G2).** FastFHIR encodes every parent→child edge
   TWICE: the parent's slot (offset + expected RECOVERY_TAG) and the child's
   header (VALIDATION + actual tag). Corrupt the parent's pointer → the child
   is orphaned but still self-consistent; an offset sweep finds it and its
   tag matches the parent slot's declared type → the edge is reconstructible.
   Corrupt the child's header → the parent's intact pointer still names the
   location → the edge's existence is provable from the parent alone. Two
   sets of correct information for each set of mistakes. **The metric should
   count RESTORED EDGES, not surviving blocks** — and only FFHR has the
   redundancy (JSON/protobuf values are inline; no orphan to sweep, no
   slot-type to match). Build `Recovery::recover_edges()` per IN-G2.
1. **One semantic for the y-axis**: "% of the bundle's clinical units a scanner
   can still extract INTACT" — with content verification in every format:
   FFHR entries whose block parses; JSON entries whose span parses; protobuf
   records whose `ParseFromArray` succeeds; **HL7v2 segments that parse to
   their expected field count AND whose name is intact** (merged-line
   detection: a `\r`-joined line has too many fields or an embedded `\r`).
2. **Normalize the x-axis**: flips per 1,000 recoverable units (or % of
   structural positions), not an absolute `k` shared across formats.
3. **Add pipes/carrots/amps/tildes to the HL7v2 structural set** — the
   cascade triggers — and let the recovery attempt real re-delimiting (v2
   scanners resync at the next MSH; field-level recovery is genuinely harder).
4. **Reconsider the hl7v2 unit**: messages (5 per bundle — too coarse) or
   OBX observations (the v2 analogue of entries) with verified fields.
5. Re-run, re-render, and re-examine whether the HL7v2 curve still claims a
   win. If it does after content verification + pipe/carrot corruption, THAT
   would be a real finding; today it is an artifact.

---

## SESSION CONTINUITY — test 5 restructure to macro parity (START HERE)

**Written 2026-08-26 (end of session, context window low).** This section is
the complete handoff for the NEXT session: what the plan is, what exists, what
is broken mid-flight, and the exact next steps. Read this first.

### The goal

Restructure the corruption/recovery test (test 5) into the repo's macro-parity
architecture, per Ryan's design:

- **`bench/bench_test_5.hpp`** — shared, macro-guarded header (D1: per-arm
  inline namespace; each arm TU defines `ARM_*` and includes it), with THREE
  polymorphic functions per format:
  1. `calc_stream_hash(wire) → StreamFingerprint` — structural fingerprint of
     a CLEAN stream: every recoverable unit's `(offset, tag)` + a SHA-256
     digest of the unit list (the report-integrity stamp).
  2. `corrupt_stream(wire, k, seed) → damaged wire` — flip k random STRUCTURAL
     bits in the format's own syntax region. **FFHR branch (Ryan 2026-08-26):
     SYNTACTIC ELEMENTS ONLY** — FF_HEADER, block VALIDATION words +
     RECOVERY_TAGs, and the POINTER SLOTS (vtable fields holding child
     offsets — block/array/resource/choice references, the edges the recovery
     cross-validates) plus the entry array's resource slots. Scalar VALUES
     (string payloads, numbers, codes) and leaf-data slots (string/code
     slots) are NEVER corrupted — they are not part of the self-corrective
     scheme, and a broken leaf reference has no cross-validation to heal it.
  3. `recover_stream(corrupted) → StreamFingerprint` — resync from the
     corrupted bytes ONLY; returns the recovered units.
- **`bench/bench_test_5.cpp`** — the driver: links the four arm TUs and
  dispatches FOUR INDEPENDENT process modes: `--hash`, `--corrupt`,
  `--recover`, `--check`.
- **The check** (third process, holds the baseline): (a) report integrity —
  re-derive `recover_stream`'s digest from its reported units; (b) content
  verification — every recovered unit must exist in the baseline with the SAME
  offset and tag (`recovered ⊆ baseline`); (c) `% = 100 · |recovered ∩
  baseline| / |baseline|`. This fixes the recovery-test flaws C/F (content-
  verified, not header-only) and is where the cross-validated EDGE recovery
  (IN-G2) plugs in.

`bench/bench_test_5.hpp` is ALREADY WRITTEN (this session) — the design in
code, per-arm implementations for all four formats, `StreamFingerprint`,
`UnitRef`, the three functions. It is not yet wired into the arms and not yet
compiled.

### Current state — what is committed vs broken

**Committed (benchmark repo, clean at `0acb8ab` + `8dd2463`):**
- Instrument G suite (`bench/resilience_test.cpp`, 4 tests + recovery probe).
- Recovery comparison across 4 formats (`bench/corruption_probe.cpp`,
  `scripts/recovery_sweep.py`, `results/recovery_curve.csv`, `fig8_recovery`).
- The recovery-test methodology flaws A–F documented (do-not-cite; fig8
  carries the caveat). The cross-validated edge-recovery design (IN-G2).
- Upstream findings filed in `../FastFHIR/TASKS.md`: CAPI-9 (datetime
  as<string_view> contract), CAPI-10 (compact write-once), CAPI-11 (raw
  packed slot in ChoiceEntry), CAPI-12 (no sealed-array append), CAPI-13
  (Parser ctor SEGV — **FIXED**: FF_HEADER::validate_full bounds-checks
  CHECKSUM_OFFSET; the `FastFHIR::Recovery` class shipped), CAPI-14 (POCO
  string_view dangles), plus the top-of-file P0 (abstraction-typed
  amend/append).

**Uncommitted / broken mid-flight (START HERE):**
- **`../FastFHIR` does not compile.** Ryan's in-flight refactor of
  `src/FF_Parser.cpp` `recover_bundle_entries()` uses undeclared members —
  `tag_at`, `next_valid_resource_of`, `header_pointers_safe`,
  `scan_all_resources` — and `s.tag_conflicts`, none of which exist in
  `include/FF_Parser.hpp`'s `Recovery` declaration (which currently has
  `next_valid_resource`, `valid_resources`, `recover_bundle_entries`; the
  `Stats` struct has entries/recovered/resyncs/**units** — `units` was added,
  `tag_conflicts` was NOT). The refactor's intent: tag-constrained resync —
  sweep for a block whose tag matches the PARENT slot's expected type (the
  cross-validation design). DO NOT rewrite it — declare the missing members
  to match Ryan's in-progress intent.
- **`bench/bench_test_5.hpp`** — created, unwired, uncompiled.
- **`src/FF_Parser.cpp`** — my `s.units.emplace_back(...)` additions are in
  the intact + resync paths (compile-blocked by the refactor; the fallback
  whole-stream scan does NOT yet record units).
- The four arm `.cpp` files — NOT yet edited (need `#include
  "bench_test_5.hpp"` after `bench_test_4.hpp`).
- `bench_test_5.cpp` (driver), BUILD target, `recovery_sweep.py` migration —
  NOT started.

### Next steps (in order)

1. **Get `../FastFHIR` compiling**: add the missing declarations to
   `Recovery` in `include/FF_Parser.hpp` matching the in-progress
   `FF_Parser.cpp` — `tag_at(Offset)`, `next_valid_resource_of(Offset,
   RECOVERY_TAG)` (tag-constrained sweep), `header_pointers_safe()`,
   `scan_all_resources()`, and the `tag_conflicts` field in `Stats`. Build
   `bazel build -c opt //bench:corruption_probe` until green.
2. Wire `bench_test_5.hpp` into the four arm TUs (`#include "bench_test_5.hpp"`
   after `bench_test_4.hpp`, before `#undef ARM_*`).
3. Write `bench/bench_test_5.cpp` — the driver: `--hash` (calc + write
   fingerprint file: count + units + digest), `--corrupt` (write damaged),
   `--recover` (report recovered fingerprint), `--check` (baseline vs
   recovered: subset + digest-integrity + %). It calls the four
   `bench::test_5::<arm_ns>::` function sets; links `:bench_core_harness` +
   `:bench_provenance`.
4. `bench/BUILD.bazel`: add `bench_test_5.hpp` to the shared hdrs; new
   `bench_test_5` binary target (replaces `corruption_probe`).
5. Migrate `scripts/recovery_sweep.py` to the new modes: `--hash` once per
   format; per (format, k, trial): `--corrupt` → `--recover` → `--check`
   (three subprocesses; the recoverer never sees the baseline).
6. Re-run the sweep, re-render `fig8_recovery`, re-examine the curves —
   this is where the FFHR vs JSON comparison gets its honest content-verified
   numbers and where the edge-recovery differentiator shows.

### Facts to re-anchor (commands)

- Artifacts: `./bazel-bin/bench/bench_harness --dump-artifacts artifacts` →
  `artifacts/{fastfhir,json,google_fhir,hl7v2}.bin` (one bundle's Test-1 wire
  payload per arm; `ArmRunResult.test1_payload`).
- Recovery units: entries for FFHR/JSON/protobuf; segments for HL7v2.
- Sweep: `.venv/bin/python scripts/recovery_sweep.py` →
  `results/recovery_curve.csv`; figures via `scripts/plot_benchmarks.py`.
- Standing instructions: **do NOT call rebuild_index** (user's repeated
  instruction; the `.arbiter` trees are stale by choice). `../FastFHIR` is its
  own git repo, kept uncommitted by documented convention. Qualifiers for any
  resilience artifact: malformed-not-hostile, integrity-not-authenticity.
- The recovery-test flaws (A–F) and the edge-recovery design are documented
  in the § Test 5 section above — re-read before touching the recovery code.

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
