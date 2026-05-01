# FastFHIR vs. JSON-FHIR Benchmarking Study

This repository contains the complete toolchain for a publication-ready performance benchmarking study comparing binary FastFHIR, text-based JSON FHIR, Google's protobuf-backed FHIR implementation, and legacy HL7v2.

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

### Quick Start

1.  **Build and Start the Stack:**
    This command builds the service images, starts the Docker Compose stack (database, sender, receiver, profiler), and waits for all services to be healthy.

    ```bash
    ./scripts/local_up.sh
    ```

2.  **Run the Smoke Test:**
    This script executes a minimal benchmark run to validate that the entire pipeline is functional, from data generation to database writes.

    ```bash
    ./scripts/local_smoke.sh
    ```

3.  **Run a Full Benchmark:**
    Execute the complete benchmark matrix across all defined arms and scenarios.

    ```bash
    ./scripts/local_benchmark.sh
    ```

4.  **Analyze Results:**
    The `profiler` service runs a JupyterLab instance. Access it at `http://localhost:8888` to open the analysis notebooks in the `notebooks/` directory. These notebooks connect directly to the benchmark database to query, analyze, and visualize results.

5.  **Tear Down the Stack:**
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
- `scripts/local_smoke.sh`: Runs a quick end-to-end test of the system.
- `scripts/local_benchmark.sh`: Executes the full benchmark suite.
- `scripts/local_ingest_stress.sh`: Runs a stress test on the metrics ingestion pipeline.

## Cloud Deployment

The `infra/` directory contains Terraform modules to provision identical, performance-tuned infrastructure on AWS and GCP. The `scripts/` directory includes wrappers for planning, applying, and destroying cloud environments, as well as deploying and executing the benchmarks remotely.

- `scripts/cloud_plan.sh`: Runs `terraform plan`.
- `scripts/cloud_apply.sh`: Runs `terraform apply`.
- `scripts/cloud_destroy.sh`: Runs `terraform destroy`.
- `scripts/cloud_bootstrap.sh`: Prepares remote hosts.
- `scripts/cloud_deploy_artifacts.sh`: Syncs binaries and configuration.
- `scripts/cloud_run_benchmark.sh`: Executes the benchmark suite on the cloud instances.
- `scripts/cloud_collect_results.sh`: Retrieves artifacts from the cloud environment.

## Contributing

Please see the [Implementation Checklist](implementation_checklist.md) for the project roadmap and contribution guidelines. All contributions should be tied to a specific checklist item.
