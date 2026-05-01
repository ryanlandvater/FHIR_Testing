# **FastFHIR vs. JSON-FHIR: End-to-End Clinical Workflow Benchmarking Study**

**Objective:** Quantify the performance delta between binary FastFHIR, text-based JSON FHIR, Google's protobuf-backed FHIR implementation, and HL7v2-style clinical messaging in high-throughput clinical environments, with primary emphasis on the **Transformation Gap**: the compute and memory cost required to move from native EHR objects to wire-ready representations and then back into clinically useful access patterns.

## **1. Benchmarking Methodology**

The study is organized as a staged pipeline so that transformation cost, wire cost, and downstream access cost can be measured independently rather than collapsed into a single opaque throughput number.

### **1.1 Research Questions**

1. How much latency and allocation overhead is incurred when serializing native clinical objects into FastFHIR versus JSON FHIR versus Google FHIR protobuf representations versus HL7v2 messages?
2. How much of the total end-to-end cost is attributable to the transport payload itself versus pre-wire and post-wire processing?
3. For downstream consumers, how much work is required to answer common clinical queries after receipt of each format?
4. When eager reconstruction into native structs is required, how does that cost compare across representations?

### **1.2 Native EHR Source of Truth**

All benchmark arms start from the same in-memory source objects represented as generated C++ structs such as `PatientData` and `ObservationData` \[cite: FASTFHIR\_CANONICAL\_API\_REFERENCE.md\]. The source objects define the semantic ground truth for every run. No benchmark arm is permitted to begin from another arm's already-serialized output.

### **1.3 Benchmark Arms**

* **FastFHIR arm:** Uses `FastFHIR::Builder` writing into `FastFHIR::Memory`, with wire egress exposed through `Memory::View` and receive-side ingress through `Memory::StreamHead` \[cite: FASTFHIR\_CANONICAL\_API\_REFERENCE.md\].
* **JSON FHIR arm:** Serializes the same native objects into standards-compliant FHIR JSON and uses a query-oriented JSON reader such as `simdjson::ondemand` for receive-side traversal.
* **Google FHIR arm:** Uses the `google/fhir` C++ libraries as a protobuf-backed typed FHIR baseline. This arm should measure JSON-to-protobuf parse, protobuf-to-JSON print, and protobuf-native access patterns using the repository's `JsonFhirStringToProto`, `MergeJsonFhirStringIntoProto`, and `PrintFhirToJsonString` style APIs.
* **HL7v2 arm:** Uses `jcomellas/hl7parser` as the legacy baseline for workflows with a natural HL7v2 mapping. This arm is message-profile-specific and must be limited to workflows that are naturally representable in HL7v2 without inventing nonstandard segments.

### **1.4 Timed Stages: Precise Measurement Boundaries**

To ensure measurement accuracy, the start and end points for each timed stage are strictly defined to isolate the work being performed and to exclude one-time setup costs from per-operation latency metrics.

#### **Stage 1: Serialization / Transformation**

This stage measures the cost of producing a wire-ready payload from native in-memory clinical objects. One-time setup costs, such as allocating the initial memory arena (`FastFHIR::Memory::create`) or the initial capacity for a JSON string buffer, are performed *before* timing begins.

*   **Start:** Immediately before the first field of the source object is written to the destination representation (e.g., before the first call to `FastFHIR::Builder::set` or the first append to a JSON object).
*   **End:** Immediately after the payload is fully serialized and sealed, but *before* any transport-specific preparation begins. For FastFHIR, this is when the `Memory::View` is obtained. For JSON, it is when the final JSON string is complete.

* **FastFHIR:** Time the write path from native structs into a `Builder`/`Memory` arena until a sealed `Memory::View` is available.
* **JSON FHIR:** Time conversion from native structs into UTF-8 JSON text.
* **Google FHIR:** Time conversion from native structs into the Google FHIR protobuf representation, and separately time protobuf-to-FHIR-JSON print when a JSON wire artifact is required.
* **HL7v2:** Time conversion from native structs into delimited HL7v2 message text, using `jcomellas/hl7parser` as the legacy parsing/materialization baseline where applicable.

#### **Stage 2: Transport / Wire Movement**

This stage measures the transfer of the already-serialized payload through a consistent transport path (e.g., an Asio TCP socket). It specifically evaluates the efficiency of moving the bytes, including any benefits from zero-copy buffer representations.

*   **Start:** Immediately upon initiating the send operation (e.g., calling `asio::async_write`).
*   **End:** Upon confirmation from the transport that the send operation has completed (e.g., the Asio completion handler is invoked).

* The same framing strategy, socket configuration, and payload boundaries must be used across all formats.
* **FastFHIR:** Egress must use the `Memory::View` directly as a native Asio buffer, highlighting its zero-copy transport advantage.
* For the Google FHIR arm, protobuf-native transport and JSON-wire transport should be reported as separate conditions rather than blended into a single result.
* **Receive-Side Start:** The receive-side clock for the subsequent stage begins when the receiver is notified of incoming data and has access to the first byte of the payload. Timing must stop before any semantic parsing or query work begins.

#### **Stage 3: Receive-Side Access**

This stage is split into two distinct benchmark classes because they answer different questions and should not be conflated.

* **Stage 3A: Query/Traversal.** Measure the cost to answer fixed clinical questions directly from the received representation.
	* **FastFHIR:** Use `FastFHIR::Parser`, `parser.root()`, typed field keys, and direct node traversal.
	* **JSON FHIR:** Use a streaming/query-oriented JSON reader over the received JSON bytes.
	* **Google FHIR:** Measure protobuf-native field access over the parsed FHIR proto object graph. If a JSON-wire condition is used, the JSON-to-protobuf parse cost must be reported separately from protobuf-native traversal.
	* **HL7v2:** Use direct segment and field extraction from the receive buffer, with `jcomellas/hl7parser` as the legacy baseline implementation.
* **Stage 3B: Eager materialization.** Measure the cost to reconstruct native C++ structs when a consumer explicitly requires that representation.
	* **FastFHIR:** Materialize from `Reflective::Node` into generated native structs.
	* **JSON FHIR:** Materialize from parsed JSON into equivalent native structs.
	* **Google FHIR:** Materialize from FHIR protobuf messages into equivalent native structs.
	* **HL7v2:** Materialize from parsed HL7v2 fields into equivalent native structs where the workflow supports a valid mapping, again using `jcomellas/hl7parser` as the legacy baseline.

### **1.5 Fairness Guardrails**

The following rules are mandatory for any published result set:

1. **Field-by-field assignment parity.** All serialization arms must construct their output representations by assigning data on a field-by-field basis from the native C++ source-of-truth objects. Bulk assignment or whole-struct serialization routines (e.g., assigning a `PatientData` struct directly) are disallowed in this primary comparison to ensure a fair measure of the transformation gap. A separate ablation study may compare field-by-field vs. bulk assignment for formats that support it.
2. **No cross-format bridge inside timed sections.** FastFHIR must not be converted to JSON and then benchmarked through a JSON parser as a proxy for FastFHIR query performance.
3. **Equivalent semantic work only.** Each arm must answer the same clinical questions and reconstruct the same data elements.
4. **Stage separation is strict.** Serialization, transport, query/traversal, and eager materialization are reported separately as well as in aggregate.
5. **Cold and warm runs are reported separately.** Initial page faults and first-touch mapping effects must not be mixed with steady-state latency.
6. **Checksum, compression, and optional enrichment features are disclosed.** If enabled, they must be held constant or reported as a separate experimental factor.
7. **Representation layers are not silently collapsed.** Google FHIR protobuf-native results, Google FHIR JSON parse/print results, and plain JSON traversal results must be reported as distinct paths.
8. **Unsupported mappings are excluded, not approximated.** If a workflow is not naturally comparable across FastFHIR, JSON FHIR, Google FHIR, and HL7v2, it should be removed from the cross-format comparison set.

### **1.6 Primary Metrics**

The benchmark suite should report, at minimum:

* Per-stage latency in microseconds and milliseconds.
* End-to-end latency across Stages 1 through 3.
* Throughput in resources/second and bundles/second.
* Payload size in bytes.
* Allocation count and allocated bytes, where practical.
* Tail latency statistics including P50, P95, and P99.

### **1.7 Statistical Treatment**

* Each benchmark condition should be repeated enough times to stabilize tail estimates, with warm-up iterations excluded from the primary sample.
* Report median and interquartile range for central tendency, plus P95 and P99 for tail behavior.
* Confidence intervals should be computed for the primary comparisons, preferably via bootstrap resampling when distributions are non-normal.

## **2. Implementation Mapping by Baseline**

The current FastFHIR documentation in this workspace supports the following benchmarkable primitives:

* `FastFHIR::Memory::create()` and `FastFHIR::Memory::createFromFile()` for anonymous and file-backed arenas.
* `FastFHIR::Builder` for native-object to FastFHIR transformation.
* `Memory::View` for zero-copy sealed-wire egress.
* `Memory::StreamHead` for direct receive-side streaming into an arena.
* `FastFHIR::Parser` and typed field traversal for query-side access.
* Eager reconstruction from `Reflective::Node` into generated structs such as `PatientData`.

These are the primitives that should anchor the first benchmark implementation. Any additional benchmark component should be justified against documented APIs or added to the workspace as explicit implementation evidence.

The Google FHIR repository provides a second typed-FHIR baseline with the following benchmark-relevant API families:

* `JsonFhirStringToProto` and `MergeJsonFhirStringIntoProto` for JSON-to-protobuf parse.
* `PrintFhirToJsonString` and `PrettyPrintFhirToJsonString` for protobuf-to-JSON print.
* Protobuf-native field access for typed traversal after parse.

This baseline is important because it separates two questions that should not be conflated in the paper:

* Whether FastFHIR outperforms text JSON processing.
* Whether FastFHIR still outperforms a strongly typed protobuf-backed FHIR implementation after parse/materialization has already occurred.

The HL7v2 baseline should be described as a legacy interoperability comparator rather than a schema-isomorphic peer. For Stage 1 and Stage 3, `jcomellas/hl7parser` is the named baseline library for parsing and materialization-oriented comparisons.

## **3. Data Persistence & Schema**
* **Storage Backend:** Managed relational database (AWS RDS or Google Cloud SQL) replaces flat CSV/S3 storage for real-time querying and structured analysis.
* **Schema Design:**
    * `raw_metrics_table`: Captures individual run timestamps (Serialization Start/End, Transport Start/End, Materialization Start/End) linked by `test_id`.
    * `aggregate_metrics_table`: Stores post-processed statistics (Median, P95, P99) per test batch.
    * `manifest_table`: Stores run metadata, including instance specs, container characteristics, CPU pinning status, and test scale (e.g., 100-patient Synthea bundle).

## **4. Cloud Infrastructure & Provisioning**
* **Infrastructure as Code (IaC):** Terraform will provision identical sending and receiving environments across AWS and Google Cloud to ensure structural parity.
* **Instance Selection:** Compute-optimized or memory-optimized instances (e.g., AWS c6in/m6in or GCP c3/m3) must be used to maximize network bandwidth and minimize baseline network latency during the Asio transfer stage.
* **Network Topology:** Sending and receiving instances must reside within the same region and availability zone/VPC to isolate protocol performance from geographic routing noise.

## **5. Dataset Design**

The initial dataset should use Synthea-generated clinical resources and bundles at multiple scales, for example 10, 100, and 1000 patient cohorts. The dataset plan should additionally stratify by workload shape rather than only by bundle size:

* Single-resource reads such as `Patient` and `Observation`.
* Multi-resource bundle traversal.
* Query-heavy access patterns such as extracting identifiers, demographics, and observation values.
* Materialization-heavy workflows that require reconstruction into native structs.

Every dataset used in the paper should have a fixed generation recipe and a retained manifest so that exact payloads can be regenerated.

## **6. Publication Readiness Criteria**

Before submission, the benchmark package should satisfy the following:

1. Every claim in the study design maps to version-controlled code or a documented manual procedure.
2. Each reported comparison identifies whether it is a serialization, transport, query, or eager-materialization result.
3. The paper includes at least one ablation showing where FastFHIR's advantage originates: transformation, payload size, traversal cost, or total end-to-end path.
4. Threats to validity are stated explicitly, especially around HL7v2 comparability and dataset representativeness.

## **7. Ancillary Studies**

### **7.1 FastFHIR Internal Serialization Methods: Field-by-Field vs. Bulk Assignment**

This ancillary test is a FastFHIR-only comparison designed to quantify the performance delta between two different serialization strategies within the FastFHIR library itself. It is intended to be run separately from the primary cross-format benchmark.

*   **Objective:** Measure the latency and allocation difference between constructing a FastFHIR representation using field-by-field assignment versus using optimized bulk assignment routines (e.g., assigning a complete `PatientData` struct in a single operation).
*   **Methodology:**
    *   **Arm 1 (Field-by-Field):** Use `FastFHIR::Builder` to serialize a native C++ struct (e.g., `PatientData`) by iterating through each field and assigning its value individually. This mirrors the methodology required for the main cross-format comparison.
    *   **Arm 2 (Bulk Assignment):** Use any available higher-level `FastFHIR::Builder` routine that accepts the entire native C++ struct directly.
*   **Metrics:** The test will measure and compare Stage 1 (Serialization) latency, allocation counts, and throughput for both arms.
*   **Purpose:** This study isolates the performance impact of the serialization strategy itself, providing data on the efficiency gains achievable through FastFHIR's specialized bulk operations, independent of wire formats or other vendors.

### **7.2 Time and Memory Efficiency for Clinical Queries (Low-RAM Modes)**

This ancillary study compares each format using its best available low-memory access mode and reports both speed and memory cost for the same clinical query.

*   **Objective:** Measure the time-vs-RAM tradeoff while locating and extracting a cholesterol value from large datasets, ensuring each arm is configured for minimal memory overhead.
*   **Methodology:**
    *   Use a large, multi-gigabyte dataset (e.g., a Synthea-generated cohort with 100,000+ patients) and the same query target for every arm.
    *   Use memory-mapped file access where supported, and otherwise the lowest-copy streaming mode available.
    *   **FastFHIR Arm:** Use `FastFHIR::Memory::createFromFile()` and `FastFHIR::Parser` for zero-copy traversal.
    *   **JSON FHIR Arm:** Use `simdjson::ondemand` (or equivalent streaming/on-demand parser) over mapped bytes to avoid DOM materialization.
    *   **Google FHIR Arm:** Use Protobuf streaming parse (`google::protobuf::io::CodedInputStream` or equivalent low-copy path) from mapped/streamed input.
    *   **HL7v2 Arm:** Use streaming segment/message traversal with `jcomellas/hl7parser` without loading the full corpus into a contiguous heap buffer.
    *   **Timed boundary (per query):**
        *   **Start:** First parser/read call that begins consumption of payload bytes for this query.
        *   **End:** Target cholesterol value is found and extracted into the result variable.
    *   Parse and traversal work are intentionally included in timed latency for this test.
*   **Metrics:**
    *   Query latency (P50/P95/P99) including parse + traversal + extraction.
    *   Queries/second under fixed concurrency.
    *   Peak RSS and RSS delta from pre-query baseline.
    *   Report a time-vs-memory frontier plot for each format/mode.
*   **Purpose:** Provide a fair comparison of practical low-RAM operation by showing how much memory each format saves and what latency is paid (or avoided) to achieve that memory profile.

## **8. Implementation Checklist**

This checklist defines the required engineering deliverables to implement, run, and review the benchmark package across local and cloud environments.

### **8.1 Containerized Local Stack**

1. Create Dockerfiles for each benchmark component:
    * Sender service container.
    * Receiver service container.
    * Database container integration (or official image binding with init scripts).
    * Profiling and analysis container (Jupyter notebook environment plus profiling tools).
2. Create a `docker-compose.yml` that starts all required services together:
    * Database service.
    * Sender service.
    * Receiver service.
    * Profiling/Jupyter service.
3. Define explicit network topology, health checks, startup ordering, and mounted volumes for datasets and run artifacts.
4. Add deterministic image tagging and pinned base image versions to improve reproducibility.

### **8.2 Local Bring-Up and Teardown Scripts**

1. Add local orchestration scripts for repeatable test execution:
    * Stack startup script (build + compose up + health wait).
    * Stack teardown script (compose down + optional volume cleanup).
    * Reset script for clearing prior run state and DB tables.
2. Add a one-command local benchmark driver script that:
    * Validates prerequisites.
    * Starts services.
    * Runs benchmark scenarios.
    * Collects logs, profiles, and metrics into a run directory.
3. Add a local smoke-test script that performs a minimal end-to-end run for CI and developer sanity checks.

### **8.3 Cloud Provisioning and Execution Scripts**

1. Add Terraform modules and environment definitions for AWS and GCP parity:
    * Compute instances for sender and receiver.
    * Managed relational database.
    * VPC/subnet/security rules and region/AZ placement.
2. Add deployment scripts for provisioning and workload launch:
    * `plan` / `apply` / `destroy` wrappers.
    * Artifact upload and remote bootstrap scripts.
    * Remote benchmark execution and result collection scripts.
3. Add environment manifest generation that records instance types, OS images, kernel settings, CPU pinning policy, and benchmark commit SHA.

### **8.4 Benchmark Harness Code**

1. Implement a shared benchmark harness that runs all timed stages and ancillary studies with identical scenario definitions.
2. Ensure stage timing boundaries match Section 1.4 and Section 7.2 definitions exactly.
3. Implement all benchmark arms:
    * FastFHIR.
    * JSON FHIR.
    * Google FHIR protobuf.
    * HL7v2 baseline.
4. Persist raw and aggregate metrics to the relational schema defined in Section 3.
5. Emit per-run manifests, machine metadata, and reproducibility artifacts.

### **8.5 Manual Review Requirement for Timing Code**

1. In each benchmark source file, place the timed-section declarations and start/stop boundaries at the top of the file for manual reviewer visibility.
2. Keep timing helper wrappers minimal and explicit so reviewers can verify that setup work is excluded and only intended work is timed.
3. Require code review sign-off that the visible top-of-file timed boundaries match the study definitions before publishing results.

### **8.6 CMake Targets and Tests**

1. Add CMake targets for:
    * Benchmark harness binaries.
    * Format-specific benchmark modules.
    * Smoke/integration test binaries.
2. Register CTest entries that cover:
    * Minimal local end-to-end benchmark execution.
    * Timing-boundary conformance checks.
    * Output schema/manifest validation checks.
3. Add a dedicated test target for generating and validating benchmark fixture data used by the harness.

### **8.7 CI and Quality Gates**

1. Add CI jobs that build benchmark targets, run smoke tests, and validate schema outputs.
2. Add a reproducibility gate that fails when required manifests, environment metadata, or timing-boundary validations are missing.
3. Add artifact publication for benchmark logs, profiles, and summary metrics to support audit and peer review.