BEGIN;

CREATE TABLE IF NOT EXISTS manifest_table (
    run_id TEXT PRIMARY KEY,
    test_id TEXT,
    benchmark_commit_sha TEXT NOT NULL,
    benchmark_branch TEXT,
    environment_name TEXT NOT NULL,
    cloud_provider TEXT,
    region TEXT,
    zone TEXT,
    instance_sender TEXT,
    instance_receiver TEXT,
    os_image TEXT,
    kernel_version TEXT,
    cpu_pinning_policy TEXT,
    dataset_version TEXT,
    dataset_seed TEXT,
    dataset_generator_version TEXT,
    dataset_args TEXT,
    dataset_checksum TEXT,
    dataset_artifact_uri TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS raw_metrics_table (
    id BIGSERIAL PRIMARY KEY,
    run_id TEXT NOT NULL REFERENCES manifest_table(run_id) ON DELETE CASCADE,
    test_id TEXT,
    arm TEXT NOT NULL,
    stage TEXT NOT NULL,
    substage TEXT,
    scenario_name TEXT,
    start_ts TIMESTAMPTZ NOT NULL,
    end_ts TIMESTAMPTZ NOT NULL,
    duration_us BIGINT NOT NULL,
    payload_bytes BIGINT,
    throughput_rps DOUBLE PRECISION,
    peak_rss_mb DOUBLE PRECISION,
    rss_delta_mb DOUBLE PRECISION,
    queue_publish_ts TIMESTAMPTZ,
    queue_dequeue_ts TIMESTAMPTZ,
    enqueue_count BIGINT,
    dequeue_count BIGINT,
    batch_id TEXT,
    notes TEXT
);

CREATE TABLE IF NOT EXISTS aggregate_metrics_table (
    id BIGSERIAL PRIMARY KEY,
    run_id TEXT NOT NULL REFERENCES manifest_table(run_id) ON DELETE CASCADE,
    test_id TEXT,
    arm TEXT NOT NULL,
    stage TEXT NOT NULL,
    substage TEXT,
    scenario_name TEXT,
    n_samples BIGINT NOT NULL,
    p50_us DOUBLE PRECISION,
    p95_us DOUBLE PRECISION,
    p99_us DOUBLE PRECISION,
    median_us DOUBLE PRECISION,
    p50_ms DOUBLE PRECISION,
    p95_ms DOUBLE PRECISION,
    p99_ms DOUBLE PRECISION,
    median_ms DOUBLE PRECISION,
    throughput_rps DOUBLE PRECISION,
    peak_rss_mb DOUBLE PRECISION,
    rss_delta_mb DOUBLE PRECISION,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_raw_metrics_run_id ON raw_metrics_table(run_id);
CREATE INDEX IF NOT EXISTS idx_raw_metrics_stage ON raw_metrics_table(stage);
CREATE INDEX IF NOT EXISTS idx_raw_metrics_arm ON raw_metrics_table(arm);
CREATE INDEX IF NOT EXISTS idx_raw_metrics_start_ts ON raw_metrics_table(start_ts);
CREATE INDEX IF NOT EXISTS idx_raw_metrics_end_ts ON raw_metrics_table(end_ts);

CREATE INDEX IF NOT EXISTS idx_aggregate_metrics_run_id ON aggregate_metrics_table(run_id);
CREATE INDEX IF NOT EXISTS idx_aggregate_metrics_stage ON aggregate_metrics_table(stage);
CREATE INDEX IF NOT EXISTS idx_aggregate_metrics_arm ON aggregate_metrics_table(arm);
CREATE INDEX IF NOT EXISTS idx_aggregate_metrics_created_at ON aggregate_metrics_table(created_at);

CREATE VIEW v_stage_latency_summary AS
SELECT
    run_id,
    arm,
    stage,
    substage,
    COUNT(*) AS sample_count,
    percentile_cont(0.50) WITHIN GROUP (ORDER BY duration_us) AS p50_us,
    percentile_cont(0.95) WITHIN GROUP (ORDER BY duration_us) AS p95_us,
    percentile_cont(0.99) WITHIN GROUP (ORDER BY duration_us) AS p99_us,
    AVG(duration_us) AS avg_us,
    MIN(duration_us) AS min_us,
    MAX(duration_us) AS max_us
FROM raw_metrics_table
GROUP BY run_id, arm, stage, substage;

CREATE VIEW v_time_memory_frontier AS
SELECT
    run_id,
    arm,
    stage,
    percentile_cont(0.50) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS query_p50_ms,
    percentile_cont(0.95) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS query_p95_ms,
    percentile_cont(0.99) WITHIN GROUP (ORDER BY duration_us) / 1000.0 AS query_p99_ms,
    MAX(peak_rss_mb) AS peak_rss_mb,
    AVG(rss_delta_mb) AS avg_rss_delta_mb
FROM raw_metrics_table
GROUP BY run_id, arm, stage;

CREATE VIEW v_latest_run_status AS
SELECT
    m.run_id,
    m.environment_name,
    m.cloud_provider,
    m.region,
    m.zone,
    m.dataset_version,
    m.created_at,
    COALESCE(MAX(r.end_ts), m.created_at) AS last_metric_ts,
    COUNT(r.id) AS metric_rows,
    COUNT(a.id) AS aggregate_rows
FROM manifest_table m
LEFT JOIN raw_metrics_table r ON r.run_id = m.run_id
LEFT JOIN aggregate_metrics_table a ON a.run_id = m.run_id
GROUP BY m.run_id, m.environment_name, m.cloud_provider, m.region, m.zone, m.dataset_version, m.created_at;

COMMIT;
