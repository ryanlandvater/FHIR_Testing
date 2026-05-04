-- FastFHIR Benchmark Results Schema
-- Migration: 001_init_benchmark_schema.sql

CREATE TABLE IF NOT EXISTS benchmark_runs (
  id SERIAL PRIMARY KEY,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  hostname VARCHAR(255),
  iterations INT DEFAULT 1,
  notes TEXT
);

CREATE TABLE IF NOT EXISTS benchmark_results (
  id SERIAL PRIMARY KEY,
  run_id INT NOT NULL REFERENCES benchmark_runs(id) ON DELETE CASCADE,
  arm VARCHAR(50) NOT NULL,
  stage VARCHAR(50) NOT NULL,
  duration_ns BIGINT NOT NULL,
  target_mb INT NOT NULL,
  patients_in_bundle INT NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_results_run ON benchmark_results(run_id);
CREATE INDEX IF NOT EXISTS idx_results_arm_stage ON benchmark_results(arm, stage);
CREATE INDEX IF NOT EXISTS idx_results_target ON benchmark_results(target_mb);
