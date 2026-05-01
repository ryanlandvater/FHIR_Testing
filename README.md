# FastFHIR vs. JSON-FHIR Benchmarking Study

This repository contains the toolchain for a publication-ready performance benchmarking study comparing binary FastFHIR, text-based JSON FHIR, Google's protobuf-backed FHIR implementation, and legacy HL7v2.

Current implementation phase: the harness now runs a shared single-patient fixture through both active arms. The canonical C++ in-memory source is `PatientData` (and nested `*Data` types) from FastFHIR-generated FHIR models because they natively support FHIR compatible in-memory data structures for parity in all arms. The JSON/FHIR arm serializes this in-memory model to FHIR JSON using nlohmann_json, and the FastFHIR arm serializes the same in-memory model to FFHR using assignment operators and generated field keys. This preserves representation parity across systems while keeping a single in-memory ground truth. Google FHIR and HL7v2 arms are deferred until the next phase.

The primary objective is to quantify the **Transformation Gap**: the compute and memory cost required to move from native EHR objects to wire-ready representations and then back into clinically useful access patterns.

The full methodology, including benchmark arms, timed stages, and fairness guardrails, is detailed in the [FastFHIR Benchmarking Study Design V1.1.md](FastFHIR%20Benchmarking%20Study%20Design%20V1.1.md).

## Repository Structure

- `bench/`: C++ benchmark harness source code.
- `services/`: Sender and receiver service implementations.
- `infra/`: Terraform scripts for provisioning cloud infrastructure (AWS/GCP).
- `docker/`: Dockerfiles for containerizing all services.
- `scripts/`: Orchestration scripts for local and cloud environments.
- `notebooks/`: Jupyter notebooks for data analysis and visualization.
- `sql/`: PostgreSQL schema, migrations, and helper views.
- `datasets/`: Location for pre-generated Synthea datasets.
- `artifacts/`: Output directory for logs, metrics, and profiling data.

## Local Development

The entire benchmark suite is containerized for reproducible local development.

### Prerequisites

- Docker and Docker Compose
- A `.env` file with database credentials (see `.env.example`)
- CMake 3.20+ and a C++20 toolchain for the local harness build
- FastFHIR repository cloned with submodules (required for the benchmark harness)

### Repository Setup

Use the provided setup scripts to initialize the repository. These scripts handle cloning FastFHIR with submodules and configuring the CMake build.

**On macOS/Linux:**

```bash
export FASTFHIR_REPO="https://github.com/<your-org>/FastFHIR.git"
./generate_repo.sh
```

**On Windows (PowerShell):**

```powershell
$env:FASTFHIR_REPO = "https://github.com/<your-org>/FastFHIR.git"
.\generate_repo.ps1
```

The scripts will:

1. Create a `.external/` directory (gitignored)
2. Clone FastFHIR with all submodules
3. Verify the checkout contains `include/FastFHIR.hpp`
4. Configure CMake with the correct `FASTFHIR_ROOT` path
5. Build the benchmark harness

This ensures the harness uses the real FastFHIR library types and APIs for both JSON ingestion and field key-based queries, not synthetic approximations.

### Ground Truth C++ Data Model

For benchmark fairness and reviewer clarity, this repository treats FastFHIR generated `PatientData` and nested `*Data` structs as the canonical C++ in-memory source of truth.

Key rules:

1. Build one in-memory `PatientData` fixture per test case and feed all arms from that same object.
2. Do not maintain a separate synthetic model for non-FastFHIR arms.
3. Keep representation parity across arms: each arm should perform its own serialization from the same in-memory object.
4. For FastFHIR, even though one-shot struct serialization is available, the benchmark path currently uses assignment operators for parity with other systems under test.

Rationale:

- Prevents unfair advantages from hand-tuned per-arm fixtures.
- Keeps Stage 1 timings comparable by requiring each arm to perform equivalent transformation work from the same source object.
- Makes code review easier by ensuring all serialization starts from one clearly defined in-memory model.

For upstream FastFHIR public API and ergonomics findings discovered during this integration, see [FFHRnotes.md](FFHRnotes.md).

### Environment Configuration

Database connectivity is environment-driven.

- Local shell and VS Code notebook sessions should read database settings from `.env`.
- Docker Compose publishes container-specific database settings to the `sender`, `receiver`, and `profiler` services.
- Cloud runners should publish their own environment variables for the target private database endpoint.

For local development, the checked-in `.env.example` defines the expected variables:

```bash
POSTGRES_USER=bench
POSTGRES_PASSWORD=bench
POSTGRES_DB=benchmark
POSTGRES_PORT=5432
BENCH_DB_URL=postgresql+psycopg2://bench:bench@localhost:5432/benchmark
```

The notebooks are written to use `BENCH_DB_URL` first. That means:

- In a local editor session, they connect to the database exposed on `localhost`.
- In the `profiler` container, they connect using the container-published `BENCH_DB_URL` that targets the Compose `db` service.
- In cloud environments, they connect to whatever private database URL the runner publishes.

The intent is that notebook code does not need to be edited per environment. Only the environment variables change.

### Quick Start

1.  **Initialize the Repository:**
    Run the setup script to clone FastFHIR and configure the build (see [Repository Setup](#repository-setup) above).

2.  **Build and Start the Stack:**
    This command builds the service images, starts the Docker Compose stack (database, sender, receiver, profiler), and waits for all services to be healthy.

    ```bash
    ./scripts/local_up.sh
    ```

3.  **Run the Smoke Test:**
    This script executes a minimal benchmark run to validate that the entire pipeline is functional, from data generation to database writes.

    ```bash
    ./scripts/local_smoke.sh
    ```

4.  **Run a Full Local Benchmark:**
    Execute the local benchmark matrix for the active single-patient slice.

    ```bash
    ./scripts/local_benchmark.sh
    ```

    At the current stage, this produces real JSON/FHIR stage timings and the real FastFHIR timings from the same canonical FHIR JSON source.

    Optional flags:

    ```bash
    ./scripts/local_benchmark.sh --iterations 50
    ./scripts/local_benchmark.sh --run-id my_local_run_001
    ```

5.  **Analyze Results:**
    The `profiler` service runs a JupyterLab instance. Access it at `http://localhost:8888` to open the analysis notebooks in the `notebooks/` directory. These notebooks use environment-provided database settings, so the same notebook files work in a local VS Code session and inside the profiler container without manual hostname edits.

6.  **Tear Down the Stack:**
    Stop and remove the running containers.

    ```bash
    ./scripts/local_down.sh
    ```

    To also remove the Docker volumes (including database data), use the `--volumes` flag:

    ```bash
    ./scripts/local_down.sh --volumes
    ```

### Available Scripts

- `scripts/local_up.sh`: Builds and starts the Docker Compose stack.
- `scripts/local_down.sh`: Stops the Docker Compose stack.
- `scripts/local_reset.sh`: Resets the database by truncating all metric tables.
- `scripts/local_smoke.sh`: Runs a quick end-to-end test and verifies FFHR/JSON stage rows are persisted.
- `scripts/local_benchmark.sh`: Builds and runs the harness locally, then writes raw and aggregate metrics to Postgres for the active single-patient slice.
- `scripts/local_ingest_stress.sh`: Runs a stress test on the metrics ingestion pipeline.

## Cloud Deployment

The `infra/` directory contains Terraform modules to provision identical, performance-tuned infrastructure on AWS and GCP. The `scripts/` directory includes wrappers for planning, applying, and destroying cloud environments, as well as deploying and executing the benchmarks remotely.

Cloud execution should follow the same configuration model as local and Docker execution: publish `POSTGRES_*` variables and/or `BENCH_DB_URL` for the private database endpoint in the runner environment, and keep notebook and benchmark code environment-driven rather than hardcoding hostnames.

- `scripts/cloud_plan.sh`: Runs `terraform plan`.
- `scripts/cloud_apply.sh`: Runs `terraform apply`.
- `scripts/cloud_destroy.sh`: Runs `terraform destroy`.
- `scripts/cloud_bootstrap.sh`: Prepares remote hosts.
- `scripts/cloud_deploy_artifacts.sh`: Syncs binaries and configuration.
- `scripts/cloud_run_benchmark.sh`: Executes the benchmark suite on the cloud instances.
- `scripts/cloud_collect_results.sh`: Retrieves artifacts from the cloud environment.

## Contributing

Please see the [Implementation Checklist](implementation_checklist.md) for the project roadmap and contribution guidelines. All contributions should be tied to a specific checklist item.
