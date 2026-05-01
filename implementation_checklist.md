# FastFHIR Benchmark Implementation Checklist

Use this as the execution tracker for building, validating, and publishing the benchmark system.

## 1) Repository and Project Setup

- [x] Create and commit top-level directories:
  - [x] `bench/` for benchmark harness source.
  - [x] `services/` for sender and receiver implementations.
  - [x] `infra/` for Terraform and cloud scripts.
  - [x] `docker/` for container assets.
  - [x] `scripts/` for local and cloud orchestration scripts.
  - [x] `notebooks/` for analysis and reporting notebooks.
  - [x] `sql/` for schema and helper views.
- [ ] Add a top-level `README` section documenting benchmark run order (build -> up -> run -> collect -> analyze).
- [ ] Define a stable artifact layout for each run:
  - [ ] `artifacts/<run_id>/logs/`
  - [ ] `artifacts/<run_id>/metrics/`
  - [ ] `artifacts/<run_id>/profiles/`
  - [ ] `artifacts/<run_id>/manifest/`
- [x] Add `.gitignore` entries for generated artifacts, notebooks checkpoints, and temporary DB dumps.

## 2) Database Schema and Data Contracts

- [x] Implement DDL for required tables:
  - [x] `raw_metrics_table`
  - [x] `aggregate_metrics_table`
  - [x] `manifest_table`
- [x] Add migration scripts under `sql/migrations/` with monotonic versioning.
- [x] Define mandatory columns and constraints:
  - [x] Unique run key (`test_id` / `run_id`).
  - [x] Benchmark arm (`fastfhir`, `json_fhir`, `google_fhir_proto`, `hl7v2`).
  - [x] Stage and substage identifiers.
  - [x] Start/end timestamps and computed duration fields.
  - [x] Environment metadata linkage to manifest.
- [x] Add indexes for expected query patterns:
  - [x] By run id.
  - [x] By stage.
  - [x] By format arm.
  - [x] By timestamp.
- [x] Add SQL views for notebook stability:
  - [x] `v_stage_latency_summary`
  - [x] `v_time_memory_frontier`
  - [x] `v_latest_run_status`
- [ ] Add high-throughput ingestion path requirements (to avoid DB round-trip bottlenecks in timed code):
  - [ ] Implement an in-process lock-free queue for metric events emitted from the benchmark runner.
  - [ ] Ensure timed execution paths only enqueue metric payloads (no synchronous SQL in timed sections).
  - [ ] Implement a background async batch-writer thread/process for DB flushes.
  - [ ] Support bulk insert mode (`COPY` for Postgres-compatible backends) for large raw metric batches.
  - [ ] Define backpressure policy (bounded queue, drop/flush strategy, shutdown drain behavior).
  - [ ] Add ingestion latency and queue-depth observability metrics.

## 3) Dockerfiles (Service Images)

- [x] Create `docker/Dockerfile.sender`:
  - [x] Pin base image by tag.
  - [x] Install runtime deps only.
  - [x] Add non-root user.
  - [x] Include healthcheck endpoint command.
- [x] Create `docker/Dockerfile.receiver`:
  - [x] Pin base image by tag.
  - [x] Configure receiver port and env defaults.
  - [x] Add non-root user and healthcheck.
- [x] Create `docker/Dockerfile.profiler`:
  - [x] Include Python + Jupyter + analysis libs.
  - [x] Include optional profiling tools.
  - [x] Configure working directory and notebook startup command.
- [x] Database image strategy:
  - [x] Use official DB image for local dev.
  - [x] Mount `sql/init/` bootstrap scripts.
- [ ] Add image labels for version, commit SHA, and build time.

## 4) Docker Compose (Local Integration)

- [x] Create `docker-compose.yml` with services:
  - [x] `db`
  - [x] `sender`
  - [x] `receiver`
  - [x] `profiler`
- [x] Configure deterministic network topology:
  - [x] Dedicated compose network.
  - [x] Explicit service DNS names.
- [x] Configure startup dependencies and health gates:
  - [x] `db` healthy before `sender` and `receiver` start.
  - [x] `profiler` starts after DB is reachable.
- [x] Define volume mounts:
  - [x] Dataset volume.
  - [x] Artifacts volume.
  - [x] Notebook volume.
- [ ] Add resource limits for local reproducibility (CPU and memory quotas).
- [x] Externalize credentials via `.env` and document required variables.

## 5) Local Scripts (Bring-Up, Run, Teardown)

- [x] Create `scripts/local_up.sh`:
  - [x] Build images.
  - [x] Start compose stack.
  - [x] Wait for health checks.
- [x] Create `scripts/local_down.sh`:
  - [x] Stop stack.
  - [x] Optional volume cleanup flag.
- [x] Create `scripts/local_reset.sh`:
  - [x] Truncate benchmark tables.
  - [ ] Reset transient artifact directories.
- [x] Create `scripts/local_smoke.sh`:
  - [x] Run minimal dataset path.
  - [x] Assert DB writes occurred.
  - [x] Assert artifacts were generated.
- [x] Create `scripts/local_benchmark.sh`:
  - [ ] Validate prerequisites.
  - [ ] Execute benchmark matrix.
  - [ ] Export logs/profiles/metrics with run id.
- [x] Add ingestion validation script `scripts/local_ingest_stress.sh`:
  - [x] Run high-rate metric emission without clinical workload to validate queue/batch writer throughput.
  - [ ] Assert no synchronous DB writes occur on timed worker threads.
  - [ ] Assert queue drains cleanly on shutdown with no metric loss beyond configured policy.

## 6) Cloud Infrastructure and Automation

- [x] Create Terraform root modules:
  - [x] `infra/aws/`
  - [x] `infra/gcp/`
- [ ] Provision parity resources for each cloud:
  - [ ] Sender instance.
  - [ ] Receiver instance.
  - [ ] Managed relational DB.
  - [ ] Network/VPC/subnet/security policies.
- [ ] Enforce same-region and same-zone placement for sender/receiver when required.
- [ ] Enforce database network locality and private routing:
  - [ ] Provision RDS/Cloud SQL explicitly via Terraform in the same private VPC/subnet domain as sender/receiver.
  - [ ] Disable public ingress for metrics DB except controlled admin paths.
  - [ ] Prohibit benchmark-path DB traffic through public internet or NAT egress when private routing is available.
  - [ ] Add security group / firewall rules that only permit runner and analysis service access.
  - [ ] Add Terraform assertions/outputs validating DB endpoint is private and region/zone aligned with runners.
- [x] Create script wrappers:
  - [x] `scripts/cloud_plan.sh`
  - [x] `scripts/cloud_apply.sh`
  - [x] `scripts/cloud_destroy.sh`
- [x] Create deployment and execution scripts:
  - [x] `scripts/cloud_bootstrap.sh` for host preparation.
  - [x] `scripts/cloud_deploy_artifacts.sh` for binary and config sync.
  - [x] `scripts/cloud_run_benchmark.sh` for remote execution.
  - [x] `scripts/cloud_collect_results.sh` for artifact retrieval.
- [ ] Produce per-run cloud manifest:
  - [ ] Instance type.
  - [ ] OS image.
  - [ ] Kernel and sysctl tuning.
  - [ ] CPU pinning policy.
  - [ ] Git commit SHA.

## 7) Benchmark Harness Code

- [x] Implement shared harness structure in `bench/`:
  - [ ] Common scenario loader.
  - [x] Common timer utilities.
  - [ ] Common metrics writer.
- [x] Implement format modules:
  - [x] FastFHIR arm.
  - [x] JSON FHIR arm.
  - [x] Google FHIR protobuf arm.
  - [x] HL7v2 arm.
- [ ] Enforce Stage 1, Stage 2, Stage 3 boundaries exactly as study doc defines.
- [ ] Implement Section 7.2 low-RAM mode pathways for all arms.
- [ ] Implement checksum/logging configuration controls to avoid timed I/O contamination.
- [ ] Persist run data:
  - [ ] Raw metrics rows.
  - [ ] Aggregate summaries.
  - [ ] Manifest metadata.
- [ ] Implement non-blocking telemetry emission architecture:
  - [ ] Timed code path emits metrics to lock-free queue only.
  - [ ] Background worker performs batch DB writes.
  - [ ] Timed code path must not call SQL driver APIs directly.
  - [ ] Add conformance test that fails if timed paths perform blocking DB/network I/O.
- [ ] Define telemetry hierarchy to avoid duplicate transport overhead:
  - [ ] Database remains system of record for full raw metric arrays.
  - [ ] ZMQ sidecar broadcasts only sampled aggregates (e.g., P99 snapshots) and heartbeat/status events.
  - [ ] Prohibit broadcasting full per-run raw arrays over ZMQ when DB persistence is enabled.
  - [ ] Add configurable ZMQ sampling interval and payload-size ceiling.

## 8) Timing Reviewability Requirements

- [x] In each benchmark source file, place timed-section declarations near top of file.
- [x] Keep `start_timer` and `stop_timer` calls visible and explicit.
- [x] Do not hide timing boundaries behind opaque abstractions.
- [x] Add comments above timed sections stating included and excluded work.
- [ ] Add review checklist item in PR template: timing boundaries match study definitions.
- [ ] Block merge unless timing-boundary reviewer signs off.

## 9) CMake and CTest

- [x] Add benchmark build targets in `CMakeLists.txt`:
  - [x] `bench_harness`
  - [ ] `bench_fastfhir`
  - [ ] `bench_json_fhir`
  - [ ] `bench_google_fhir`
  - [ ] `bench_hl7v2`
- [x] Add test targets:
  - [x] Smoke test binary.
  - [x] Timing conformance test binary.
  - [x] Schema output validation test binary.
- [x] Register tests with CTest:
  - [x] `ctest -L smoke`
  - [x] `ctest -L timing`
  - [x] `ctest -L schema`
- [ ] Add fixture generation target for deterministic test data.
- [ ] Add compiler warnings and sanitizer profile for non-release CI validation.

## 10) Notebooks and Analysis Workflow

- [ ] Keep notebook connectors environment-driven via `BENCH_DB_URL`.
- [ ] Validate expected tables before analysis cells execute.
- [ ] Add saved query templates for:
  - [ ] Stage latency summary.
  - [ ] Throughput comparison.
  - [ ] Section 7.2 time-vs-memory frontier.
- [ ] Add export cells to write CSV snapshots per run id.
- [ ] Add notebook runbook documenting required kernel packages.
- [ ] Define live-telemetry notebook behavior:
  - [ ] Subscribe to ZMQ heartbeat and sampled aggregate stream only.
  - [ ] Pull raw per-run arrays from DB queries, not ZMQ.
  - [ ] Display data-source attribution in notebook cells ("live sampled" vs "DB authoritative").

## 11) CI/CD and Quality Gates

- [ ] Build and test on every PR:
  - [ ] CMake configure/build.
  - [ ] CTest smoke + timing + schema labels.
- [ ] Run linting and static analysis for C++ and scripts.
- [ ] Validate SQL migrations apply cleanly on an empty DB.
- [ ] Validate Docker compose stack boots in CI for smoke path.
- [ ] Publish CI artifacts:
  - [ ] Logs.
  - [ ] Metrics snapshots.
  - [ ] Profiling outputs.
  - [ ] Manifest files.
- [ ] Add gate: fail if required manifest metadata fields are missing.

## 12) Reproducibility and Publication Readiness

- [ ] Record seed values and dataset generation parameters for every run.
- [ ] Lock deterministic datasets before benchmarking:
  - [ ] Pre-generate Synthea datasets for target scales (10/100/1000 and any large-cohort variants) before test execution.
  - [ ] Store generated artifacts as immutable versioned blobs with checksums.
  - [ ] Mount the exact same pre-generated artifacts on AWS and GCP runners.
  - [ ] Prohibit runtime Synthea generation during benchmark runs.
  - [ ] Add dataset manifest fields: generator version, seed, command args, checksum, artifact URI.
- [ ] Pin dependency versions (containers, system packages, Python libs).
- [ ] Store benchmark matrix configuration with each run artifact bundle.
- [ ] Keep a signed changelog of methodology updates impacting comparability.
- [ ] Produce final publication package:
  - [ ] Raw and aggregate tables.
  - [ ] Figure-generation notebooks.
  - [ ] Environment manifests.
  - [ ] Exact commit references.
