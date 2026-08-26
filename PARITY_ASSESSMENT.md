# FastFHIR 4-Arm Benchmark — Parity Assessment & Hardening Recommendations

**Date**: 2026-05-07
**Scope**: Fairness audit of all four benchmark arms across all four test stages, with concrete hardening recommendations
**Conclusion**: Test 3 fixed. Tests 1, 2, and 4 are structurally fair — format-inherent differences are the findings, not flaws. Hardening recommendations provided for rebuttal-proof publication.

> **Reviewed 2026-08-24 — reasoning stands, verification does not.** This audit
> predates the upstream FastFHIR change, and its "✅ Fixed" markers describe a
> benchmark that does not currently build ([TASKS.md § PORT](TASKS.md)). The
> structural fairness arguments below are unaffected by the port and remain the
> right frame; the claims that a given behaviour *was verified* need re-running
> once the build is green.
>
> One finding is materially changed and should be read before the rest:
>
> **Test 1's size comparison was not apples-to-apples.** FastFHIR used to
> silently drop resource types outside the compiled profile — one Synthea
> bundle lost 41 of 250 records and still reported success. The FastFHIR arm
> was therefore serializing *less data* than the other three arms, invisibly,
> in exactly the direction that flattered it. That is fixed (out-of-profile
> resources are retained as opaque JSON), but it means **every pre-2026-08-24
> size and throughput figure in this repo is void**, and the Test 1 verdict
> below was reached on numbers that no longer apply.
>
> A second consequence lands on Test 3: opaque resources are not
> typed-navigable, so a "query every resource" stage silently skips them —
> 1,444 `ImagingStudy` records in the Synthea corpus. That is a new fairness
> question this audit never considered. See
> [README § Result provenance](README.md#result-provenance) and
> [TASKS.md § CORPUS](TASKS.md).

---

## Assessment Summary

| Test | Status | Key Finding | Hardening |
|---|---|---|---|
| Test 1 (Serialize) | ⚠️ HL7v2 ZFX overhead | HL7v2 does extra work preserving non-mappable FHIR fields | Add lossy HL7v2 variant; report bytes-per-arm |
| Test 2 (Materialize) | ✅ Fair | FFHR zero-parse is an architectural strength, not an advantage | Report bytes-per-touched-node; add memory metrics |
| Test 3 (Query) | ✅ Fixed | HL7v2 now pays full parse cost like every other arm | Add cross-arm QuerySummary assertion; extract ZFX fields or document gap |
| Test 4 (Enrich) | ✅ Fair | Append cost IS the architectural finding | Add byte-level validation; add optional round-trip companion test |

---

## Section 1: Test 1 (Serialization)

### How It Works

All four arms share the same `assign_patient_common()` and `assign_observation_common()` functions in `bench_test_1.hpp`. These call the exact same ~25 field-assignment functions per resource, macro-dispatched to each arm's output target. The loop structure, source data, and field coverage are identical.

### Fairness Assessment

**Structurally fair.** Every arm writes every field from the source POCO. The macro-guarded header guarantees this.

**Cost skews against HL7v2.** The HL7v2 arm has a dual-write pattern for fields that exceed native HL7v2 fidelity:

| Example field | HL7v2 native write | HL7v2 overflow write | Other arms |
|---|---|---|---|
| `patient_name` | `hl7_name_xpn()` → PID-5 (`Family^Given`) | ZFX `"patient.name[0].details"` (JSON with id, extension, use, text, prefix, suffix, period) | Single native write |
| `patient_name` (multiple) | N/A (only first name in PID-5) | ZFX `"patient.name[*]"` (JSON array of all names) | Single native write |
| `patient_telecom` | `hl7_phone_xtn()` → PID-13 (first number) | ZFX `"patient.telecom[*]"` if multiple or extra fields present | Single native write |
| `observation_code` | `observation_code_id()` → OBX-3 (`code^display^LN`) | ZFX `"observation.code.details"` + `"observation.code.coding[0].details"` if extra fields present | Single native write |
| ~20 non-mappable fields | None | ZFX custom segment (`hl7_append_json_field`) | Single native write |

The ZFX overflow costs string escaping (`hl7_escape`: `|`→`\F\`, `^`→`\S\`, `&`→`\T\`, `~`→`\R\`) and `nlohmann::json::dump()` for structured types. The other arms pay none of this.

### Verdict

✅ **Fair.** The ZFX mechanism preserves FHIR fields that have no HL7v2 equivalent. Removing it would mean HL7v2 serializes *less data* — a worse fairness violation. This is format-inherent serialization overhead.

### Hardening Recommendations

| # | Recommendation | Effort | Rationale |
|---|---|---|---|
| H1.1 | **Add "lossy HL7v2" variant** — a second HL7v2 serialization path that *omits* all ZFX custom segments. This measures the speed/fidelity trade-off: how much faster is HL7v2 if you accept data loss? | ~4 hours | Neutralizes the "HL7v2 does extra work" criticism by providing both data points. The lossy variant represents real-world HL7v2 integration (fields without mappings are dropped). |
| H1.2 | **Report output byte counts per arm** — add `serialized_bytes` to `ArmRunResult` metrics. Normalize timing as `ns/byte` and `ns/field`. | ~2 hours | Prevents the "HL7v2 produces larger output" rebuttal. If HL7v2 is slower but produces more bytes, `ns/byte` normalizes for that. |
| H1.3 | **Add serialized-output validation** — after Test 1, parse each arm's output back and verify that key fields (patient ID, birthdate, LOINC codes) are present and correct. | ~3 hours | Prevents "arm X serialized garbage" criticism. Every arm's output must round-trip through its own parser and produce the expected data. |
| H1.4 | **Document ZFX overhead in published results** — include a footnote or table row showing the byte overhead of ZFX segments vs. native segments. | ~1 hour | Transparency neutralizes the criticism. Readers can see exactly what the overhead is. |

---

## Section 2: Test 2 (Materialization)

### How It Works

Each arm takes its serialized payload and reconstructs an in-memory model, then recursively walks every node:

| Arm | Parse phase | Walk phase | Node count granularity |
|---|---|---|---|
| FastFHIR | None (arena IS the tree — zero deserialization cost in timer) | `node.entries()` recursive | 1 per entry/child |
| JSON | `simdjson::dom::parser::parse()` — full DOM construction | `object`/`array` iterators | 1 per key, 1 per value |
| Google FHIR | TLV scan + `ParseFromArray()` — full proto deserialize | `GetReflection()->ListFields()` | 1 per message, 1 per field, 1 per sub-message |
| HL7v2 | `parse_batch()` — `\r`→`\|`→`^`→`&` hierarchical split | 4-level `touch_tree` | 1 per segment, field, component, subcomponent |

### Fairness Assessment

**Structurally fair.** All arms build comparable in-memory structures and walk them at comparable granularity. Each arm's node count reflects its format's natural structural units — this is expected and correct.

**FFHR zero-parse is the headline finding.** The FFHR arena IS the tree — `materialize()` pays zero deserialization cost. This is the architectural advantage the benchmark exists to demonstrate. It is not a design flaw. The timer captures the full tree-walk cost; the parse cost is genuinely zero because the binary format is self-describing.

**Node counts are arm-specific.** JSON counts keys and values separately, HL7v2 counts subcomponents, protobuf counts repeated sub-messages. Do not compare `touched_nodes` across arms. Use timing as the cross-arm metric.

### Verdict

✅ **Fair.** The differing parse costs are the result, not the problem.

### Hardening Recommendations

| # | Recommendation | Effort | Rationale |
|---|---|---|---|
| H2.1 | **Report `bytes_per_touched_node`** — divide input bytes by `touched_nodes` for each arm. This normalizes walk cost by structural density: JSON has many small nodes (keys), HL7v2 has fewer but larger nodes (segments). | ~1 hour | Neutralizes "arm X has more/fewer nodes" criticism. Cost per structural unit is a defensible normalized metric. |
| H2.2 | **Track peak heap allocation during materialize** — wrap `materialize()` in a scope that records `malloc`/`free` delta (or use a custom allocator). | ~4 hours | Surfaces memory cost of each format's in-memory representation. JSON and protobuf allocate; FFHR and HL7v2 use views. This is an architectural finding worth measuring. |
| H2.3 | **Add `touched_nodes` cross-arm warning to output** — if the harness detects it's printing `touched_nodes` for multiple arms, emit: `WARNING: touched_nodes is arm-specific and not comparable across formats. Use timing metrics for cross-arm comparison.` | ~0.5 hours | Prevents misinterpretation by readers who see different node counts and assume unfairness. |
| H2.4 | **Document FFHR zero-parse prominently** — in the README and any publication, state: "FastFHIR Test 2 measures tree-walk cost only. The arena format eliminates the deserialize step entirely. This is the primary architectural advantage being demonstrated." | ~1 hour | Preempts the "you're not measuring the same thing" criticism by being explicit about it. |

---

## Section 3: Test 3 (Query)

### Previous State (Fixed 2026-05-07)

The HL7v2 arm used a raw `\r`-delimited line scan with `segment_field()` for on-demand `|` splitting. No parse tree was built. This skipped the parsing cost that every other arm pays.

### Current State (Fixed)

The HL7v2 arm now calls `hl7v2::parse_batch(payload)` to build the full `MessageTree` AST, then uses `PidView`/`ObxView` typed views for field access. This matches the parse-before-query cost model of all other arms.

```cpp
// Current HL7v2 test 3 (bench_test_3.hpp, lines 532-580):
auto messages = hl7v2::parse_batch(payload);    // full parse cost paid here
detail::QueryAccumulator acc;
for (const auto& msg : messages) {
  for (const auto& seg : msg.tree.segments) {
    if (seg.name == "PID") {
      hl7v2::PidView pid(seg);
      acc.note_patient(pid.birth_date());
    } else if (seg.name == "OBX") {
      hl7v2::ObxView obx(seg);
      acc.note_observation();
      if (obx.observation_id() == kCholesterolLoincCode)
        acc.note_loinc_2085_9();
      // value type classification via obx.value_type()
    }
  }
}
```

### Known Gap: Missing QuerySummary Fields

The `QuerySummary` struct has 16 counters. The HL7v2 arm populates only 8:

| Field | HL7v2 | Reason |
|---|---|---|
| `patients`, `birthdate`, `observations`, `loinc_2085_9_matches` | ✅ | Native PID/OBX fields |
| `obs_value_present`, `obs_value_quantity`, `obs_value_codeableconcept`, `obs_value_string`, `obs_value_code` | ✅ | OBX-2/OBX-5 |
| `obs_effective_datetime`, `obs_effective_period` | ❌ | Stored in ZFX `"observation.effective[x]"` |
| `obs_issued_present` | ❌ | Stored in ZFX `"observation.issued"` |
| `obs_component_value_*` (4 fields) | ❌ | Stored in ZFX `"observation.component"` |

This is a data fidelity gap, not a performance fairness issue. The HL7v2 arm does not do *less work* — it does the *same parse work* but cannot extract fields that have no native HL7v2 representation.

### Verdict

✅ **Fair.** All arms pay parse cost before query. The QuerySummary gap is a format fidelity limitation, not a performance cheat.

### Hardening Recommendations

| # | Recommendation | Effort | Rationale |
|---|---|---|---|
| H3.1 | **Add cross-arm QuerySummary assertion** — after all arms run Test 3, verify that the *intersection* of populated fields matches. Specifically: `patients`, `observations`, `loinc_2085_9_matches`, and `birthdate` (digit-normalized) must be identical across all arms. Fail the benchmark if they diverge. | ~3 hours | This is the single strongest hardening: it proves every arm processes the same data and finds the same results. If counts match, no arm can be accused of skipping work. |
| H3.2 | **Extract ZFX fields in HL7v2 Test 3** — after the main segment loop, do a second pass over ZFX segments to populate `effective`/`issued`/`component` counters. Parse the JSON payload in each ZFX segment to extract value types. | ~6 hours | Closes the QuerySummary gap entirely. All 16 counters populated by all arms. Eliminates the "HL7v2 reports incomplete data" criticism. |
| H3.3 | **Alternative to H3.2: Document the gap explicitly** — if ZFX extraction is too expensive for the query benchmark, declare the 8 shared fields as the "canonical query surface" and mark the other 8 as format-specific. Only compare the canonical fields in cross-arm assertions. | ~1 hour | Less work, defensible position: "We compare what every format can represent." |
| H3.4 | **Report QuerySummary in benchmark output** — print the full QuerySummary for each arm alongside timing. Readers can verify parity themselves. | ~1 hour | Transparency. A reviewer can see that `patients=119` for every arm and know the query is correct. |

---

## Section 4: Test 4 (Enrich)

### Current State

Each arm uses its **format-native approach** to appending a new Observation to an existing serialized payload:

| Arm | Approach | Key property |
|---|---|---|
| FastFHIR | Copy VMA handle, append to arena, re-finalize root | Zero-copy enrichment within the same VMA that came off the wire. Arena IS the tree — only root pointer and checksum updated. |
| HL7v2 | String concatenation (`payload + message.dump()`) | Stream-oriented protocol. Each ORU^R01 message is self-contained with its own MSH header. |
| JSON | `nlohmann::json::parse()` + mutate + `dump()` | Document model. No partial-update primitive in JSON spec. Full parse + re-dump required. |
| Google FHIR | TLV deserialize all records + mutate + TLV serialize all | Envelope format. Record boundaries are length-prefixed. Adding one record requires re-emitting the stream. |

### Why This Is Fair

The test asks: *"Append an observation to an existing serialized bundle."* Each arm answers with its format-native approach. The differences in cost are **not a benchmark design flaw** — they surface the architectural properties the benchmark is designed to measure:

- **FFHR's zero-copy VMA enrichment is a core architectural strength**, not an unfair advantage. The format was designed so that the binary representation IS the in-memory tree. Mutations append to the same arena. There is no separate "deserialize" step because the data never leaves its native form. **This is the result the benchmark exists to demonstrate.**

- **HL7v2's stream-orientation** is why it dominated healthcare messaging for decades. Self-contained messages make concatenation valid and safe.

- **JSON's document model** requires full round-trip because JSON has no append primitive. This is a well-known trade-off of text-based interchange formats.

- **Protobuf TLV** requires full re-serialization because record boundaries are length-prefixed and interleaved.

### Verdict

✅ **Fair.** Each arm does the format-native thing. The differences are findings, not flaws.

### Hardening Recommendations

| # | Recommendation | Effort | Rationale |
|---|---|---|---|
| H4.1 | **Add byte-level validation** — after enrichment, parse the enriched payload and verify: (a) it is valid/parseable, (b) it contains exactly one more observation than the source, (c) the new observation's LOINC code matches the enrichment fixture. | ~3 hours | Proves that every arm actually added the observation. Prevents "arm X produced invalid output" criticism. |
| H4.2 | **Add Test 4b (Round-Trip Enrich)** — a companion test where every arm does full deserialize → mutate → re-serialize. FFHR parses the arena into POCOs and re-serializes from scratch. HL7v2 parses the stream and re-dumps all messages. This is the "structurally identical" comparison. | ~17 hours | Provides a second data point for readers who want a same-operation comparison. The append-based Test 4a remains as the format-native primary test. Both results published side by side. |
| H4.3 | **Report byte delta** — show `source_bytes → enriched_bytes` with the delta and percentage. | ~0.5 hours | Shows the per-arm byte cost of adding one observation. Normalizes the timing: `ns per added byte`. |
| H4.4 | **Document the append asymmetry in published results** — include a sentence like: "Test 4 measures each format's native append cost. FFHR and HL7v2 support zero-copy append; JSON and protobuf require full round-trips. These differences reflect real architectural properties, not benchmark design choices." | ~0.5 hours | Preempts criticism by stating the asymmetry explicitly. |

---

## Section 5: Cross-Cutting Hardening

These recommendations apply to all tests and arms.

### H5.1 — Warm-Up Runs

**Problem**: First-run timing includes cold caches, page faults, and (for JIT'd/VM languages) compilation overhead.

**Fix**: Run one un-timed warm-up iteration of the full 4-test pipeline before recording any metrics. Discard warm-up results.

**Effort**: ~1 hour. Add a `--warmup` flag to `main.cpp` or hardcode one warm-up iteration before the timed loop.

### H5.2 — Statistical Reporting

**Problem**: Single-run timing is vulnerable to outlier noise.

**Fix**: Report `mean`, `stddev`, `min`, `max`, `p50`, `p99` across N runs. The harness already supports `--runs N`. Extend `ArmRunResult` or `main.cpp` to compute statistics across runs.

**Effort**: ~4 hours. Accumulate per-arm per-stage metrics across run iterations, compute statistics at end.

### H5.3 — Memory Metrics

**Problem**: Timing alone doesn't capture memory cost. FFHR uses a single arena; JSON allocates many small objects; protobuf allocates per-message.

**Fix**: Track peak heap allocation per test per arm. On Linux/macOS: `malloc_stats()` or `getrusage(RUSAGE_SELF)`. On Windows: `GetProcessMemoryInfo()`. Wrap each test in a scope that records memory before/after.

**Effort**: ~6 hours. Platform-specific, but only needs to work on the benchmark's target platform.

### H5.4 — Output Size Normalization

**Problem**: Different formats produce different byte counts for the same data. HL7v2 with ZFX segments produces more bytes than HL7v2 without. JSON is more verbose than FFHR binary.

**Fix**: Always report `serialized_bytes` alongside timing. Report derived metrics: `ns/byte`, `bytes/field`, `ns/touched_node`. These normalize timing for output size differences.

**Effort**: ~2 hours. Add byte counters to `ArmRunResult` and compute normalized metrics in `main.cpp`.

### H5.5 — Correctness Assertion Framework

**Problem**: Without output validation, a fast arm could be fast because it's wrong.

**Fix**: After each test, assert correctness before recording timing:

| Test | Assertion |
|---|---|
| Test 1 | Serialized output is parseable by its own format's parser. Key fields (patient ID, LOINC codes) are present. |
| Test 2 | `touched_nodes > 0` and `ok == true`. Output tree is structurally valid. |
| Test 3 | Cross-arm `QuerySummary` canonical fields match: `patients`, `observations`, `loinc_2085_9_matches`, `birthdate` (digit-normalized). |
| Test 4 | Enriched output is parseable. Contains one more observation than source. New observation has correct LOINC code. |

**Effort**: ~8 hours total (some assertions already exist in timing_conformance_test.cpp).

### H5.6 — No Dead-Code Elimination

**Problem**: The compiler could optimize away parse/walk work if results are unused.

**Fix**: Verify that `touched_nodes` and `QuerySummary` values are consumed after timing (logged, printed, or `volatile`-written). The current code already does this for most paths — audit the remaining ones.

**Effort**: ~2 hours. Review each arm's test path for unused results. Add `volatile` writes or log statements where needed.

---

## Section 6: Hardening Priority Matrix

| Priority | ID | What | Why first |
|---|---|---|---|
| 🔴 P0 | H3.1 | Cross-arm QuerySummary assertion | Single strongest hardening: proves all arms process same data correctly |
| 🔴 P0 | H5.5 | Correctness assertion framework | Without this, fast results could be wrong results |
| 🔴 P0 | H5.2 | Statistical reporting | Single-run timing is not publishable |
| 🟡 P1 | H1.1 | Lossy HL7v2 variant | Neutralizes the most visible fairness criticism |
| 🟡 P1 | H5.1 | Warm-up runs | Standard benchmarking practice, easy to implement |
| 🟡 P1 | H5.4 | Output size normalization | Enables ns/byte comparisons that neutralize format verbosity differences |
| 🟡 P1 | H4.1 | Enrich byte-level validation | Proves Test 4 correctness across all arms |
| 🟢 P2 | H2.3 | touched_nodes cross-arm warning | Prevents misinterpretation, very cheap |
| 🟢 P2 | H3.4 | Report QuerySummary in output | Transparency, very cheap |
| 🟢 P2 | H4.3 | Report byte delta for enrich | Transparency, very cheap |
| 🟢 P2 | H4.4 | Document append asymmetry | Preempts criticism, very cheap |
| 🔵 P3 | H1.2 | Bytes-per-arm metrics | Requires plumbing but enables ns/byte normalization |
| 🔵 P3 | H2.1 | bytes_per_touched_node | Normalizes walk cost |
| 🔵 P3 | H2.2 | Peak heap tracking | Surfaces memory cost, platform-specific |
| 🔵 P3 | H3.2 | ZFX field extraction in HL7v2 query | Closes QuerySummary gap completely |
| 🔵 P3 | H4.2 | Test 4b round-trip enrich | Large effort, complementary to existing Test 4 |
| 🔵 P3 | H5.3 | Memory metrics | Platform-specific, large effort |
| ⚪ P4 | H1.3 | Serialized-output validation | Covered by H5.5 correctness framework |
| ⚪ P4 | H1.4 | Document ZFX overhead | Paper-level task, not code |
| ⚪ P4 | H2.4 | Document FFHR zero-parse | Paper-level task, not code |
| ⚪ P4 | H3.3 | Document QuerySummary gap | Paper-level task, not code |
| ⚪ P4 | H5.6 | No dead-code elimination audit | Already mostly handled |

---

## Section 7: Documentation Status

| Document | Status |
|---|---|
| `hl7v2_message.hpp.md` | ✅ Current — 4-level parse tree, typed views, `parse_batch()` |
| `bench_test_1.hpp.md` | ✅ Current — macro-guarded assignment layer |
| `bench_test_2.hpp.md` | ✅ Current — 4-level tree walk |
| `bench_test_3.hpp.md` | ✅ Current — `parse_batch()` + typed views |
| `bench_test_4.hpp.md` | ✅ Current — format-native append documented |
| `arm_hl7v2.cpp.md` | ✅ Current |
| `arm_fastfhir.cpp.md` | ✅ Current |
| `arm_json_fhir.cpp.md` | ✅ Current |
| `arm_google_fhir.cpp.md` | ✅ Current |
| `PARITY_ASSESSMENT.md` | ✅ Current — this document |

---

## Section 8: Action Items

### Completed

| # | Item | Date |
|---|---|---|
| ✅ | Fix HL7v2 Test 3 to use `parse_batch()` + typed views | 2026-05-07 |
| ✅ | Update all `.md` documentation to reflect current implementation | 2026-05-07 |
| ✅ | Write parity assessment with hardening recommendations | 2026-05-07 |

### Recommended Implementation Order

| # | Priority | Item | Effort |
|---|---|---|---|
| 1 | P0 | H3.1 — Cross-arm QuerySummary assertion | ~3h |
| 2 | P0 | H5.5 — Correctness assertion framework | ~8h |
| 3 | P0 | H5.2 — Statistical reporting (mean/stddev/p50/p99) | ~4h |
| 4 | P1 | H1.1 — Lossy HL7v2 variant (no ZFX segments) | ~4h |
| 5 | P1 | H5.1 — Warm-up runs | ~1h |
| 6 | P1 | H5.4 — Output size normalization (ns/byte, bytes/field) | ~2h |
| 7 | P1 | H4.1 — Enrich byte-level validation | ~3h |
| 8 | P2 | H2.3, H3.4, H4.3, H4.4 — Quick transparency fixes | ~3h |
| 9 | P3 | H1.2, H2.1 — Byte and node normalization metrics | ~3h |
| 10 | P3 | H3.2 — ZFX field extraction in HL7v2 query | ~6h |
| 11 | P3 | H4.2 — Test 4b round-trip enrich | ~17h |
| 12 | P3 | H2.2, H5.3 — Memory metrics | ~10h |
| 13 | P4 | H1.3, H1.4, H2.4, H3.3, H5.6 — Documentation and audit | ~5h |

**Total P0+P1 effort to reach publishable state**: ~25 hours
**Total all recommendations**: ~70 hours
