# notes.md — what it actually took to make the benchmark run again

**Written 2026-08-25**, after porting the four-arm harness from the pre-`a9fd4e9`
FastFHIR API to the current one and getting it to produce numbers again.

This is the field report for the redesign. It is not a changelog — it is the set
of things that were **wrong in ways nothing caught**, and what that implies for
building a benchmark that is both equitable and performance-intensive.

Read [TASKS.md](TASKS.md) for the task list, [handoff.md](handoff.md) for what
to build next. Read this for the reasoning behind both.

---

## TL;DR — the five findings that matter

| # | Finding | Why it matters for the redesign |
|---|---|---|
| 1 | **ODR violation across the four arms.** The same type names had four different definitions in four translation units. | Root cause of every "impossible" crash. Any future shared-header arm design must namespace per arm. |
| 2 | **Test 2 walked 1 node in the FastFHIR arm and ~8,000 in the others.** | The headline "FFHR zero-parse is an architectural strength" was measured against a no-op. |
| 3 | **Test 1's array writes produced structurally invalid streams** — and `validate_FFHR_stream()` passed them. | The public API has no way to write an inline-block array. Field-by-field assembly is not viable for FastFHIR. |
| 4 | **No arm ever serialized `value[x]`.** Four arms, four different wrong behaviours. | The most clinically important field in an Observation was never in the benchmark. |
| 5 | **The bundle seed was `random_device`.** | Two runs of the same command measured different data. A corruption bug reproduced ~50 % of the time and looked like a heisenbug. |

---

## 1. The ODR violation — the one that made everything else undebuggable

### What happened

`bench_test_{1,2,3,4}.hpp` are included once per arm with a different `ARM_*`
macro. Inside, `bench::test_2::MaterializedTree` is:

| TU | Members |
|---|---|
| `arm_fastfhir.cpp` | `unique_ptr<FastFHIR::Parser>`, `Reflective::Node` |
| `arm_json_fhir.cpp` | `unique_ptr<simdjson::dom::parser>`, `dom::element` |
| `arm_google_fhir.cpp` | `vector<Patient>`, `vector<Observation>` |
| `arm_hl7v2.cpp` | `vector<hl7v2::ParsedMessage>` |

Four different layouts, one name, external linkage. Same for `StreamType`,
`test_2::materialize`, `test_3::query`, and all of `bench::assign::detail`.

The linker keeps one definition of each inline function and destructor and
discards the rest. So a `MaterializedTree` constructed with the Google layout
could be destroyed by the FastFHIR destructor.

### Why it took so long to find

It never looked like an ODR bug. It looked like a FastFHIR bug:

- `-c opt` crashed in `FF_CODEABLECONCEPT::deserialize`
- `-c dbg` crashed in `~vector<google::fhir::r4::core::Observation>`
- adding a debug print moved the crash somewhere else again
- stripping *any* large Patient field appeared to "fix" it

I spent several rounds bisecting Patient fields on the theory that one specific
field carried bad data. That was chasing layout noise.

**The tell was that the crash site moved with the build configuration.** That is
almost never a data bug and almost always a memory/linkage bug.

### What found it

`AddressSanitizer`, in one run:

```bash
bazel build -c dbg --copt=-fsanitize=address --copt=-fno-omit-frame-pointer --linkopt=-fsanitize=address --strip=never //bench:bench_harness
```

ASan named `bench::test_2::MaterializedTree::~MaterializedTree` destroying a
`vector<Observation>` with a garbage vtable. From there the cause was obvious.

### The fix

An inline namespace per arm, so each TU gets its own mangled symbols while every
call site stays spelled the same:

```cpp
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
...
#endif

namespace bench::test_2 {
inline namespace BENCH_ARM_NS {
  ...
}  // inline namespace
}  // namespace bench::test_2
```

### For the redesign

- **The macro-guarded shared header is not free.** It buys field-coverage parity
  and it costs you the ODR. If the redesign keeps that pattern, the per-arm
  namespace is mandatory, not hygiene.
- ~~**A serious alternative:** make the arms templates or explicit classes over a
  shared interface, compiled once, rather than one header compiled four ways.~~
  ✎ **Rejected 2026-08-26 — TASKS.md D1.** The `#if` ladder is not incidental:
  it puts all four implementations of the same field on adjacent lines, which is
  how a reviewer audits parity. Compiling once behind an interface hides exactly
  what needs to be visible. The ODR surface is closed by the per-arm inline
  namespace; ASan in CI (HY-1) keeps it closed.
- **Add ASan to CI.** This bug was live for the entire life of the benchmark and
  no build ever complained. One ASan run finds it in seconds.

---

## 2. Test 2 was measuring a no-op in the FastFHIR arm

`touch_tree()` walked the tree like this:

```cpp
++touched_nodes;
if (!node.is_array() && !node.is_object()) return;
for (auto& child : node.entries()) touch_tree(child, touched_nodes);
```

`entries()` returns elements of an **array**. For a **block** (object) you need
`fields()` plus an owner-keyed lookup. So the walk visited the Bundle root and
stopped.

Measured node counts on a 1 MB bundle, before and after:

| Arm | Nodes touched (before) | Nodes touched (after) |
|---|---|---|
| fastfhir | **1** | 4,443 |
| json_fhir | 8,327 | 8,327 |
| hl7v2 | 8,008 | 8,008 |
| google_fhir | 9,539 | 9,539 |

Reported Test 2 duration for the FastFHIR arm went from **333 ns to ~108 µs** —
and FastFHIR is now *slower* than the JSON arm on that stage, which is an honest
result rather than an artifact.

The correct traversal already existed in
[`read_path_bench.cpp`](bench/read_path_bench.cpp) (`walk_node`), which is the
validated public-API walk. `bench_test_2.hpp` predated it and never adopted it.

### Compounding problem: dead-code elimination

Each arm did `const auto materialized = test_2::materialize(...); (void)materialized;`.
Nothing observed `touched_nodes`, so the optimizer was free to delete the walk.
Part of the original 83 ns was exactly that. The arms now read the counter.

### For the redesign

- **Every timed stage must produce a value that escapes**, checked against an
  expected magnitude. A stage that can be optimized away eventually will be.
- **Node counts are not comparable across formats** (4,443 vs 9,539 for the same
  clinical content). If Test 2 is meant to measure traversal throughput,
  normalize — per byte, or per FHIR element, not per node.
- Assert the counts are within a band of each other and fail the run otherwise.
  A 1-vs-8000 divergence should have been a hard error, not a fast number.

---

## 3. Field-by-field assembly cannot write FastFHIR arrays

### What happened

`Observation.category` is stored as `FF_ARRAY::INLINE_BLOCK`: entry *i* is a
fixed-size block header at `entries_start + i*HEADER_SIZE`, with its
variable-length tail elsewhere in child space. The generator emits this with a
**four-argument** `STORE_FF_<TYPE>(base, header_offset, child_offset, data)`.

The bench's `stream_assign_array_offsets()` wrote a `vector<Offset>` instead. The
reader then walked those 8-byte offsets as inline CodeableConcept headers and
dereferenced payload text as a string offset:

```
SEGV in FF_STRING::read_view
  <- FF_CODEABLECONCEPT::deserialize
  <- FF_OBSERVATION::deserialize
  <- Node::as<ObservationData>()
  <- bench::test_3::query
```

### The part that should worry us

**`validate_FFHR_stream()` returned `FF_SUCCESS` on that stream.** The array
header and offsets are self-consistent by the validator's rules; only the
generated deserializer walks the entries as blocks and discovers they are not.

So "the stream validates" was not sufficient evidence that Test 1 produced
correct output — and it had been the only check.

### Why it can't be fixed in place

`TypeTraits<T>` exposes only the self-contained three-argument
`store(base, off, data, ver)`. There is **no public API** for writing an
inline-block array of a FHIR datatype. Doing it properly means per-type dispatch
to the four-argument overloads — re-implementing a slice of the generator.

### What I did instead

The FastFHIR arm now hands the whole POCO to the generated STORE:

```cpp
auto patient_handle     = builder.append_obj(item.patient);   // was: append_obj(PatientData{}) + assign
auto observation_handle = builder.append_obj(*observation);   // was: append_obj(ObservationData{}) + assign
```

Serialized output went from 2,215 bytes to 107,463 bytes for the same bundle —
i.e. the field-by-field path had been writing almost nothing.

### The parity cost — read this before quoting Test 1

The FastFHIR arm now serializes **every field of the POCO**. The other three arms
serialize only the ~25 fields the shared assignment layer covers. That is *more*
work for FastFHIR, so the bias runs against it rather than for it — but it is
**not parity**, and Test 1 numbers should not be published in this state.

### For the redesign

Pick one, explicitly:

1. **Whole-object serialization for every arm.** Each arm serializes the complete
   POCO using its library's native path (`append_obj`, `json::dump`,
   `SerializeToString`, full ORU build). This is what a real EHR does, it is the
   fairest comparison, and it removes the assignment layer entirely. My
   recommendation.
2. **Keep field-by-field**, but then FastFHIR needs an upstream public API for
   inline-block arrays. Worth filing regardless — see §7.

Option 1 also deletes the largest source of complexity in the repo.

✎ **Decided 2026-08-26: option 2 — TASKS.md D3.** Option 1 turned out to be
impossible for one of the four arms: **HL7v2 has no whole-object serializer.**
There is no generic "serialize this FHIR resource" call in v2.x — an ORU^R01 is
assembled segment by segment by definition — so whole-object parity is a model
only three arms can adopt, which is not a parity model. It also conflicts with
D1. The upstream API was filed as **CAPI-1** and is what unblocks PA-1.

A separate, clearly labelled *native whole-object* row may still be reported per
arm that offers one. It never shares a table cell with a parity row.

---

## 4. `value[x]` was never serialized by any arm

`ChoiceEntry` stores a block-typed variant (`valueQuantity`,
`valueCodeableConcept`, …) as the raw child **offset** of that block:

```cpp
// generated_src/FF_Observation.cpp:319
else data.value.value = child_off;   // offset into the SOURCE arena
```

The POCO carries no base pointer, so that offset is meaningless outside the arena
it was read from — and this benchmark hydrates from one arena and serializes into
another. What each arm did with it:

| Arm | Behaviour |
|---|---|
| FastFHIR | Wrote the foreign offset into a slot tagged as a block → **corrupt stream** |
| JSON | Emitted `{"valueQuantity": 55683}` — a raw arena offset as a JSON number |
| Google | Mocked a hardcoded `"1.0"` |
| HL7v2 | Mocked a hardcoded `"1"` with units `{qty}` |

Four arms, four different wrong answers, for the single most clinically important
field in an Observation.

### What I did

Sanitized these at hydration (`sanitize_choice` in
[`harness.hpp`](bench/harness.hpp)) so every arm receives byte-identical POCOs
and no arm corrupts its stream. **Test 1 does not measure `value[x]` at all.**

### The subtlety that cost three failed attempts

My first predicate only treated `FF_FIELD_BLOCK` as non-portable. Three shapes
reach the `uint64_t` alternative and only some are portable:

| Kind | Payload | Portable? |
|---|---|---|
| `FF_FIELD_BLOCK` | child offset | **no** |
| `FF_FIELD_CODE` | dictionary index (MSB clear) | yes |
| `FF_FIELD_CODE` | packed `FF_CODEABLE_CONCEPT` offset (MSB set) | **no** |
| `FF_FIELD_DATETIME` | packed civil value (bit 63 clear) | yes |
| `FF_FIELD_DATETIME` | fallback offset to an `FF_STRING` (bit 63 set) | **no** |

Synthea's `us-core-race` / `us-core-ethnicity` extensions carry `valueCode`, so
~337 slots per corpus slipped through a BLOCK-only test and corrupted the stream
anyway. The fix has to test the **flag bit**, not just the kind.

### For the redesign

- **Deep-copy is the real fix**: ~~plumb the source arena base into the assignment
  sink~~ ✎ *do it at **hydration*** — deserialize the block from the source
  `Parser` and append it into the destination — so the fix does not depend on
  which serialization model D3 picks. Filed upstream as **CAPI-3**; the POCO has
  no base pointer, so the library has to either carry it or do the copy.
- Until then, **say in the results that `value[x]` is excluded.** A cross-format
  serialization benchmark that omits Observation values is measuring a materially
  easier problem than the one it claims.
- ✎ **The exclusion is now tiered, not total (TASKS.md D2, 2026-08-26).** Two of
  the four arms turn out to have a native choice representation — protobuf a
  `oneof` in a nested `ValueX`, HL7v2 the OBX-2 value type — so the *scalar*
  variants can be measured at four-arm parity today; only the block-typed ones
  wait on CAPI-3. The HL7v2 arm's hardcoded `"1"` above is Tier S work with no
  upstream dependency (PA-2). **Do not present choice support as a capability
  the other formats lack** — they have it; our offset is what cannot cross.

---

## 5. Non-deterministic bundle composition

`std::mt19937 rng(std::random_device{}())`. Two runs of the identical command
measured different bundles. During debugging this produced a corruption that
reproduced roughly half the time, which sent me down a completely wrong path
(I concluded arena size mattered; it didn't — the seed did).

Now defaults to a fixed seed, `--seed 0` restores random, and the seed is printed
on every run.

### For the redesign

- **Deterministic by default is the right choice for a benchmark**, not just for
  debugging: it makes two runs comparable and makes a regression attributable.
- The seed belongs in the results metadata alongside the profile and git SHA.

---

## 5b. A documented "P1 blocker" that did not exist

Five documents state that the Google FHIR arm's Stages 2 and 3 are stubbed and
return 0 ns, scheduling ~80 h to implement them. **Both are fully implemented.**

Verified by running the harness:

| | Documented | Measured |
|---|---|---|
| Stage 2 | "returns 0 ns" | TLV parse + `ParseFromArray` + protobuf-reflection walk; **9,539 nodes**, ~1.6 ms on a 1 MB bundle |
| Stage 3 | "returns 0 ns" | 42 accumulator calls; returns `patients=1 observations=316 obs_issued_present=316` — matching the JSON baseline exactly |

There is no zero-duration metric push anywhere in `arm_google_fhir.cpp`.

**The two real Google-arm gaps are different ones**, and neither was documented:

- **`birthdate` is a microsecond epoch** (`194140800000000`) where every other
  arm reports ISO (`1976-02-26`).
- **The Google arm is excluded from `validate_parity()`**, which only compares
  FastFHIR-vs-JSON and JSON-vs-HL7v2. Nothing has ever checked its query results
  against another arm — which is presumably how the unit mismatch survived.

### For the redesign

Cross-arm validation has to cover **every** arm. An arm that nothing checks will
drift, and the docs will describe a problem it does not have while missing the
one it does.

---

## 6. Smaller things that were still wrong

- **A diagnostic inside the timing window.** My own `getenv("BENCH_VALIDATE")`
  check initially sat between the last write and `stop_ns()`. Caught it by
  reading the diff, not by measurement. Worth an explicit rule: *nothing* between
  the last real operation and the clock stop.
- **libpq was a hard build dependency** via `copts = ["-I/opt/homebrew/..."]`,
  which Bazel rejects as a path outside the execution root regardless of whether
  libpq is installed. PostgreSQL persistence is now opt-in
  (`//bench:bench_harness_pg`); the default target builds anywhere.
- **The no-libpq code path had never been compiled.** Its `#else` branch declared
  `PGconn *db_conn = nullptr;` — a libpq type. It could not have worked.
- **`--db` was silently ignored** when built without libpq. Now warns.
- **`bench_harness_win` is a four-arm target including Google FHIR** — correct,
  since dropping an arm on one platform would make the platforms
  non-comparable. Blocked only by Google FHIR's Bazel build being macOS-only
  here.
- **Corpus discovery** still prefers a hardcoded developer path
  (`bench/main.cpp:31`). Left alone; tracked in TASKS.md § INFRA.

---

## 7. Worth raising upstream with FastFHIR

✎ **Filed 2026-08-26** into `../FastFHIR/TASKS.md` as **CAPI-1…CAPI-6**, in that
order, each with a *Locate* command and quoted current state per that file's
execution contract. Not committed — the tree is a symlinked live checkout.
Tracked here as TASKS.md § UP.

Collected while porting. None of these are bugs in FastFHIR's own correctness
gates — they are gaps in what a *consumer* can do through the public API.

1. **No public API for inline-block arrays.** `TypeTraits<T>` exposes only the
   self-contained store. Anything assembling a resource field-by-field cannot
   write `Observation.category` correctly, and gets no compile error for trying.
2. **`validate_FFHR_stream()` passes streams the generated deserializer
   segfaults on.** Worth either strengthening the validator to walk arrays as the
   deserializer does, or documenting precisely what it does and does not cover.
3. **Block-typed `ChoiceEntry` cannot round-trip across arenas.** Deserialize
   writes a source-arena offset into the `uint64_t` alternative; store writes it
   back out as an integer. Deserialize→store is not an identity for these. Either
   document it, or carry the base pointer in the POCO.
4. **No zero-copy reader for packed date/time.** `Node::as<std::string_view>()`
   throws "Node is not a string or code" on `Patient.birthDate` since DT-2.
   `print_json` is the only public path, which costs a stream plus JSON escaping
   the string case does not pay — a real distortion in a query benchmark.
   See `read_text_field()` in [`harness.hpp`](bench/harness.hpp).
5. **`TypeTraits<std::string>` is undefined** — only `string_view` is
   specialised, so assigning a `std::string` fails to compile. The date/time POCO
   fields (`birthdate`, `issued`, …) *are* `std::string`, so the mismatch is
   internal to the generated types. This is FFHRnotes item 4, now a hard error.
6. **Stale doc comment**: `include/FF_Ingestor.hpp:69` still shows
   `SourceType::FHIR_JSON`, a name that no longer exists.

---

## 8. Current state

```bash
bazel build -c opt //bench:all          # green
bazel test  -c opt //bench:timing_conformance_test   # PASSED
./bazel-bin/bench/bench_harness --runs 2 --bundle-max-mb 16   # 161 metric rows
```

Exit code **2** is expected: it signals a cross-arm parity mismatch, not a crash.
The HL7v2 arm does not report `obs_issued_present` or `obs_component_value_*`,
which the JSON baseline does. That is a genuine, pre-existing coverage gap in the
HL7v2 arm, now visible because the harness runs far enough to check it.

### What these numbers are and are not

They are **proof the harness works end to end**. They are **not publishable**:

- Test 1 is not at parity (§3) and excludes `value[x]` (§4).
- Test 2 node counts are not normalized across formats (§2).
- Test 3 pays a `print_json` penalty in the FastFHIR arm for packed dates (§7.4).
- Nothing here records the compiled profile or upstream SHA yet
  (TASKS.md § PROFILE).

### Debug hooks left in place

| Env var | Effect |
|---|---|
| `BENCH_VALIDATE=1` | After Test 1, validate the FastFHIR arm's stream, `print_json` it, and census the bundle entries by resource type. Outside the timing window. |
| `BENCH_TOUCHED=1` | Report nodes touched per arm in Test 2 — the check that caught §2. |
| `--seed N` | Fix bundle composition. `--seed 0` for random. |

Keep these. Both were what turned an unfalsifiable crash into a specific one.
