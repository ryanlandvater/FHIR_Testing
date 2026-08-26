# Data Resilience Test Suite — TODO

Tests that exercise FastFHIR's unique structural integrity guarantees under
real-world corruption scenarios. Each test maps to a failure mode observed in
production FHIR/HL7v2 pipelines, and each exercises a capability that **no
other format in the benchmark** can provide.

> **Blocked on the API port.** These tests are written against the pre-`a9fd4e9`
> FastFHIR surface and none of them can be built today. The ingestion path
> below is the *old* one; the current equivalents are in
> [TASKS.md § PORT-1, PORT-2](TASKS.md). Land the port before implementing any
> test here, or you will write the whole suite against a dead API.

**Data source:** All tests use Synthea FHIR JSON files from `datasets/synthea/`
(119 real patient bundles with Observations, Conditions, Encounters, Procedures).
This ensures realistic payload sizes, nested structures, and resource diversity.
The directory is populated by `generate_repo.sh` and is **not** checked in — it
does not exist in a fresh clone until that script runs.

Ingestion path, in current API terms:
`make_bundle_patient_from_json()` → `FF_CreateIngestor` / `FF_Ingest`
→ `BundlePatient` struct → `FF_StreamAppendObject()` → `FF_StreamSetRoot()`
→ `FF_StreamFinalize()` → sealed `Memory::View`; same pipeline as the
benchmark harness.

**A resilience-specific note on the upstream change:** out-of-profile resources
are now retained as opaque JSON blocks rather than dropped. Opaque blocks carry
the same 10-byte universal header (`VALIDATION` + `RECOVERY_TAG`) as every
other block, so the truncation and bit-flip mechanisms below apply to them —
but their *interiors* are unparsed bytes, so corruption inside an opaque
payload is detectable only at block granularity, not field granularity. Any
test that asserts field-level damage detection must run on an in-profile
resource. Worth an explicit test either way: the Synthea corpus routes 1,444
`ImagingStudy` records through this path on every run.

---

## Test 1: Truncation Detection via VALIDATION Word

**Real-world analog:** Network streams drop mid-transmission — TCP RST, proxy
timeout, partial socket read. In HL7v2 this produces segment-boundary
misalignment (parser reads into garbage or interprets trailing bytes as a
phantom segment). In FHIR JSON it produces an unparseable trailing fragment
(best case) or a silently truncated array (worst case).

**FFHR-unique mechanism:** Every `DATA_BLOCK` stores its own absolute arena
offset at bytes 0–7 (`VALIDATION`). The `FF_HEADER` carries `STREAM_SIZE`.
After truncation the first incomplete block either has a `VALIDATION` word
pointing past EOF or one that doesn't match its arena position — both
detectable by `validate_offset()`. `STREAM_SIZE` also won't match actual byte
count — Parser catches this at construction.

**Implementation:**
1. Ingest a Synthea patient bundle → build & seal with `Builder::finalize()`.
2. Copy sealed bytes into `std::vector<uint8_t>`.
3. Truncate at N offsets bracketing structural boundaries: mid-`FF_HEADER`,
   mid-V-Table of a Patient block, mid-string-payload of a name field,
   mid-`FF_ARRAY` entry region, between two Bundle entries.
4. For each truncation attempt `Parser(buffer, truncated_size)`.
5. Assert: every truncation either (a) throws `std::runtime_error` at Parser
   construction, or (b) if Parser accepts, a recursive tree walk detects at
   least one block whose `VALIDATION` ≠ its offset.

**Key assertion:** No truncated Synthea stream is accepted as valid without
surfacing an error. FastFHIR never silently returns partial data.

---

## Test 2: Bit-Flip Detection — Structural vs Payload Damage

**Real-world analog:** Storage media degradation, faulty RAM, noisy
interconnects. In HL7v2 a single flipped bit in a segment ID (`OBX`→`OBY`)
silently changes semantic meaning of an entire segment group. In FHIR JSON a
flipped bit in `"resourceType"` or a numeric lab value produces no structural
error — the parser gives you wrong data with no indication.

**FFHR-unique mechanism:** Every block carries a 10-byte universal header:
8-byte self-referencing `VALIDATION` offset + 2-byte `RECOVERY_TAG`. A
random bit flip in this 10-byte region is detectable **without computing a
stream-level checksum**:
- Flip in `VALIDATION` → `validate_offset()` fails (stored offset ≠ actual).
- Flip in `RECOVERY_TAG` → tag won't match any known type, or matches the
  *wrong* type (caught by `TypeTraits<T>::read()`).

Crucially, FFHR **separates structural damage from payload damage**: a
flipped bit in a string payload (e.g. a patient name) leaves the block header
intact — the tree remains traversable even though the clinical value is
garbled. This localization is impossible in HL7v2 (where a delimiter flip
cascades) and in JSON (where a bracket flip destroys the entire document).

**Implementation:**
1. Ingest a Synthea patient with at least 5 Observations → build & seal.
2. Walk the sealed stream; record every block's offset, header region, and
   payload region.
3. **Structural flip:** Flip one bit in the `VALIDATION` word of a mid-stream
   Observation block. Assert: `validate_offset()` returns false for that
   block. Assert: blocks *before* the damage are still reachable and their
   data is intact. Assert: the damaged block is flagged.
4. **Tag flip:** Flip one bit in the `RECOVERY_TAG` of a block. Assert:
   `node.as<ExpectedType>()` throws or returns error.
5. **Payload flip:** Flip one bit in an Observation's `valueQuantity.value`
   payload bytes. Assert: the block's `VALIDATION` word and `RECOVERY_TAG`
   are intact. The tree walk completes. The garbled value is accessible
   (structural integrity holds); only the clinical datum is corrupted.

**Key assertion:** Structural corruption is detected at block granularity.
Payload corruption is isolated — the block structure survives. This is the
property that lets a scanner recover from mid-stream damage by resynchronizing
at the next valid `VALIDATION` word.

---

## Test 3: Type Confusion Prevention via RECOVERY_TAG Dispatch

**Real-world analog:** A resource is mislabeled in a pipeline — a Condition
gets tagged as an Observation, or a Procedure's JSON body is pasted under a
wrong `resourceType`. HL7v2: segment types are 3-byte ASCII strings; `OBR`
vs `OBX` differ by one character, trivial to mangle. FHIR JSON: `resourceType`
is a string field with no structural barrier — a consumer that skips validation
reads Condition fields as if they were Observation fields.

**FFHR-unique mechanism:** The `RECOVERY_TAG` is a binary enum embedded in the
block header, not a payload string. `TypeTraits<T>::read()` checks it before
materializing any bytes. `Node::is<RESOURCETYPE>()` checks it at the node
level. There is no code path that interprets block bytes as type T without
first verifying the recovery tag. Cross-type reads are rejected at the binary
layer — this is not a "validate if you remember to" pattern; it's enforced by
the read API itself.

**Implementation:**
1. Ingest a Synthea bundle containing Patient + Observations + Conditions +
   Encounters + Procedures (a typical Synthea file has all 5).
2. Seal and parse. Assert `parser.root_type()` reports Bundle.
3. Iterate `Bundle.entry`. For each entry's resource node:
   - Call `node.is<RESOURCETYPE::PATIENT>()`, `::OBSERVATION()`, `::CONDITION()`,
     `::ENCOUNTER()`, `::PROCEDURE()`. Assert exactly one returns true.
   - Materialize with `node.as<T>()` for the matching type. Assert no throw.
   - Attempt `node.as<T>()` for at least one *non-matching* type. Assert it
     throws or returns an error sentinel.
4. Walk typed arrays (e.g. `Bundle.entry`). Verify every resource reference's
   inline recovery tag matches the target block's own header recovery tag.

**Key assertion:** RECOVERY_TAG-based dispatch prevents cross-type confusion
at the API level. It is impossible to read a Condition's bytes as an
Observation without the API rejecting it. This is a structural invariant, not
a best-practice convention.

---

## Test 4: Concurrent Build Integrity — Lock-Free Arena Under Contention

**Real-world analog:** Multiple ingestion threads writing to a shared FHIR
store simultaneously. HL7v2 has no concurrency model — parallel writes to a
file produce interleaved/corrupted output. FHIR JSON bundling has no atomic
multi-writer guarantee — two threads appending to the same NDJSON stream
produce broken records.

**FFHR-unique mechanism:** `Memory::claim_space()` uses a single `fetch_add`
on the atomic write-head — no mutex, no condition variable, no
producer/consumer queue. Each thread gets an exclusive byte range by
construction. `Builder::finalize()` sets `m_finalizing`, spins on
`m_active_mutators` until zero, then seals. The protocol guarantees that
concurrent appends cannot interleave and a finalizer cannot seal over
in-progress writes.

**Implementation:**
1. Ingest N Synthea patients (N=16, spread across hardware concurrency)
   into `BundlePatient` structs.
2. Create a single `Memory` arena + `Builder`. Spawn one thread per patient.
3. Each thread calls `builder.append_obj(patient)` on its own Patient.
4. All threads must complete before `finalize()` is called (join all, then
   finalize).
5. Assert: `Parser` accepts the sealed stream (no structural errors).
6. Assert: the root Bundle contains exactly N entries.
7. Assert: every entry's Patient `id` field matches one of the input ids
   (no lost writes, no torn writes, no duplicated entries).
8. Seal with SHA-256. Reopen with a fresh Parser. Assert: checksum
   validates (the concurrent build produced a deterministic, verifiable
   stream).

**Key assertion:** Lock-free concurrent arena building is correct under
contention. The final stream is structurally identical to a serial build —
every entry is present, no data is torn, and all integrity guarantees
(VALIDATION word, RECOVERY_TAG, checksum) hold.
