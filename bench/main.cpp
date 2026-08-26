// FastFHIR Benchmark Harness
// Supports macOS (all 4 arms) and Windows (FFHR + JSON arms).
// Platform differences gated by __APPLE__ and HAVE_GOOGLE_FHIR macros.

#include "harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef HAVE_LIBPQ
#include <libpq-fe.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Synthea data path: macOS uses a hardcoded primary path; other platforms
// use a relative datasets/ directory.
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
static fs::path find_synthea_dir()
{
  const fs::path primary = "/Users/RyanLandvater/Programming_Projects/FastFHIR-benchmarking/datasets/synthea";
  const fs::path fallback = "datasets/synthea";
  if (fs::exists(primary))
    return primary;
  if (fs::exists(fallback))
    return fallback;
  return {};
}
#else
static fs::path find_synthea_dir()
{
  const fs::path dir = "datasets/synthea";
  if (fs::exists(dir))
    return dir;
  return {};
}
#endif

// ---------------------------------------------------------------------------
// Warmup wrapper — runs only the arms available on this platform.
// ---------------------------------------------------------------------------
static void warmup_arms(bench::BundleBenchFixture &fixture, int warmup_iterations)
{
  for (int w = 0; w < warmup_iterations; ++w)
  {
    (void)bench::run_fastfhir_bundle(fixture);
    (void)bench::run_json_bundle(fixture);
#if defined(HAVE_HL7V2)
    (void)bench::run_hl7v2_bundle(fixture);
#endif
#if defined(HAVE_GOOGLE_FHIR)
    (void)bench::run_google_fhir_bundle(fixture);
#endif
  }
}

// ---------------------------------------------------------------------------
// Per-arm run helpers — each returns its metrics vector.
// ---------------------------------------------------------------------------
static std::vector<bench::MetricEvent> run_fastfhir(bench::BundleBenchFixture &bundle)
{
  return bench::run_fastfhir_bundle(bundle).metrics;
}
static std::vector<bench::MetricEvent> run_json(bench::BundleBenchFixture &bundle)
{
  return bench::run_json_bundle(bundle).metrics;
}
#if defined(HAVE_HL7V2)
static std::vector<bench::MetricEvent> run_hl7v2(bench::BundleBenchFixture &bundle)
{
  return bench::run_hl7v2_bundle(bundle).metrics;
}
#endif
#if defined(HAVE_GOOGLE_FHIR)
static std::vector<bench::MetricEvent> run_google_fhir(bench::BundleBenchFixture &bundle)
{
  return bench::run_google_fhir_bundle(bundle).metrics;
}
#endif

// ---------------------------------------------------------------------------
// Cross-arm validation (only when all arms are available)
// ---------------------------------------------------------------------------
#if defined(HAVE_GOOGLE_FHIR)
static bool validate_parity(const bench::ArmRunResult &ff,
                            const bench::ArmRunResult &jf,
                            const bench::ArmRunResult &h2,
                            bool &failed)
{
  if (!bench::validate_results(ff, jf))
  {
    failed = true;
    std::cerr << "  [validate] values: fastfhir=[" << ff.queried_value << "]"
              << " json=[" << jf.queried_value << "]\n";
  }
  if (!bench::validate_hl7_results(ff, jf, h2))
  {
    failed = true;
    std::cerr << "  [validate-hl7] values: baseline_json=[" << jf.queried_value << "]"
              << " hl7=[" << h2.queried_value << "]\n";
  }
  return failed;
}
#endif

// ===========================================================================

int main(int argc, char **argv)
{
  const std::vector<std::string_view> args(argv, argv + argc);

  // Defaults
  int iterations = 1;
  int warmup_iterations = 1;
  int num_runs = 10;
  int64_t bundle_max_mb = 0;
  bool bundle_max_mb_explicit = false;
  int64_t fastfhir_vma_mb = 0;
  std::string db_connstr;
  // Bundle composition was seeded from std::random_device, so two runs of the
  // same command measured different bundles and a corruption bug reproduced
  // only intermittently. Default to a fixed seed; --seed 0 restores random.
  unsigned int rng_seed = 20260825u;
  std::vector<int64_t> target_sizes_bytes = {
      1LL * 1024 * 1024,
      2LL * 1024 * 1024,
      4LL * 1024 * 1024,
      8LL * 1024 * 1024,
      16LL * 1024 * 1024,
      32LL * 1024 * 1024,
      64LL * 1024 * 1024,
      256LL * 1024 * 1024,
  };

  for (std::size_t i = 1; i < args.size(); ++i)
  {
    if (args[i] == "--iterations" && i + 1 < args.size())
    {
      iterations = std::max(1, std::atoi(args[i + 1].data()));
    }
    else if (args[i] == "--warmup-iterations" && i + 1 < args.size())
    {
      warmup_iterations = std::max(0, std::atoi(args[i + 1].data()));
    }
    else if (args[i] == "--runs" && i + 1 < args.size())
    {
      num_runs = std::max(1, std::atoi(args[i + 1].data()));
    }
    else if (args[i] == "--bundle-max-mb" && i + 1 < args.size())
    {
      bundle_max_mb = std::max<int64_t>(1, std::atoll(args[i + 1].data()));
      bundle_max_mb_explicit = true;
    }
    else if (args[i] == "--ff-vma-mb" && i + 1 < args.size())
    {
      fastfhir_vma_mb = std::max<int64_t>(0, std::atoll(args[i + 1].data()));
    }
    else if (args[i] == "--db" && i + 1 < args.size())
    {
      db_connstr = std::string(args[i + 1]);
    }
    else if (args[i] == "--seed" && i + 1 < args.size())
    {
      rng_seed = static_cast<unsigned int>(std::strtoul(args[i + 1].data(), nullptr, 10));
    }
    else if (args[i] == "--bundle-targets-mb" && i + 1 < args.size())
    {
      target_sizes_bytes.clear();
      std::stringstream ss{std::string(args[i + 1])};
      std::string token;
      while (std::getline(ss, token, ','))
      {
        const auto mb = std::atoll(token.c_str());
        if (mb > 0)
          target_sizes_bytes.push_back(mb * 1024 * 1024);
      }
    }
  }

  // Cap, sort, deduplicate targets
  if (bundle_max_mb_explicit)
  {
    target_sizes_bytes.erase(
        std::remove_if(target_sizes_bytes.begin(), target_sizes_bytes.end(),
                       [bundle_max_mb](int64_t b)
                       { return b > bundle_max_mb * 1024 * 1024; }),
        target_sizes_bytes.end());
  }
  std::sort(target_sizes_bytes.begin(), target_sizes_bytes.end());
  target_sizes_bytes.erase(
      std::unique(target_sizes_bytes.begin(), target_sizes_bytes.end()),
      target_sizes_bytes.end());

  if (target_sizes_bytes.empty())
  {
    std::cerr << "No target sizes configured.\n";
    return 1;
  }

  // Locate Synthea data
  const fs::path synthea_dir = find_synthea_dir();
  if (synthea_dir.empty())
  {
    std::cerr << "Synthea data not found. Place FHIR JSON files in datasets/synthea/\n";
    return 1;
  }

  // Pre-load all patient JSON files
  struct IngestedPatient
  {
    bench::BundlePatient patient;
  };
  std::vector<IngestedPatient> all_patients;

  std::cerr << "Ingesting Synthea JSON files from " << synthea_dir << " ...\n";
  for (const auto &entry : fs::directory_iterator(synthea_dir))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    try
    {
      auto patient = bench::make_bundle_patient_from_json(entry.path());
      all_patients.push_back({std::move(patient)});
    }
    catch (const std::exception &ex)
    {
      std::cerr << "  skip " << entry.path().filename() << ": " << ex.what() << "\n";
    }
  }

  if (all_patients.empty())
  {
    std::cerr << "No patient JSON files found in " << synthea_dir << ".\n";
    return 1;
  }
  std::cerr << "Loaded " << all_patients.size() << " patients.\n\n";

  std::mt19937 rng(rng_seed != 0 ? rng_seed : std::random_device{}());
  std::cerr << "Bundle composition seed: "
            << (rng_seed != 0 ? std::to_string(rng_seed) : std::string("random")) << "\n";
  std::uniform_int_distribution<std::size_t> patient_dist(0, all_patients.size() - 1);

  std::cout << "arm,test,duration_ns,target_mb,patients_in_bundle\n"
            << std::flush;

  // -----------------------------------------------------------------------
  // PostgreSQL connection
  // -----------------------------------------------------------------------
#ifdef HAVE_LIBPQ
  PGconn *db_conn = nullptr;
  int run_id = -1;
  if (!db_connstr.empty())
  {
    db_conn = PQconnectdb(db_connstr.c_str());
    if (PQstatus(db_conn) != CONNECTION_OK)
    {
      std::cerr << "Database connection failed: " << PQerrorMessage(db_conn) << "\n";
      PQfinish(db_conn);
      db_conn = nullptr;
    }
    else
    {
      PGresult *r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS duration_ns BIGINT");
      PQclear(r);
      r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS duration_us BIGINT");
      PQclear(r);

#if defined(__APPLE__)
      const char *host_label = "macOS";
#else
      const char *host_label = "Windows";
#endif
      const std::string sql = "INSERT INTO benchmark_runs (hostname, iterations, notes)"
                              " VALUES ('" +
                              std::string(host_label) + "', " + std::to_string(iterations) + ", 'benchmark harness') RETURNING id";
      r = PQexec(db_conn, sql.c_str());
      if (PQresultStatus(r) == PGRES_TUPLES_OK)
      {
        run_id = std::atoi(PQgetvalue(r, 0, 0));
        std::cerr << "Database run_id: " << run_id << "\n";
      }
      else
      {
        std::cerr << "Failed to insert run record: " << PQerrorMessage(db_conn) << "\n";
      }
      PQclear(r);
    }
  }

  // Parameterized DB insert helper
  auto insert_metric = [&](const bench::MetricEvent &m, int64_t target_mb, int64_t n_patients)
  {
    if (!(db_conn && run_id >= 0))
      return;
    const std::string run_id_s = std::to_string(run_id);
    const std::string dur_ns_s = std::to_string(m.duration_ns);
    const std::string dur_us_s = std::to_string((m.duration_ns + 999) / 1000);
    const std::string target_s = std::to_string(target_mb);
    const std::string patients_s = std::to_string(n_patients);
    const std::string stage_s = bench::to_string(m.stage);

    const char *params[7] = {
        run_id_s.c_str(),
        m.arm.c_str(),
        stage_s.c_str(),
        dur_ns_s.c_str(),
        dur_us_s.c_str(),
        target_s.c_str(),
        patients_s.c_str(),
    };
    PGresult *r = PQexecParams(db_conn,
                               "INSERT INTO benchmark_results "
                               "(run_id, arm, stage, duration_ns, duration_us, target_mb, patients_in_bundle) "
                               "VALUES ($1::int, $2::text, $3::text, $4::bigint, $5::bigint, $6::int, $7::int)",
                               7, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      std::cerr << "DB insert failed: " << PQerrorMessage(db_conn) << "\n";
    PQclear(r);
  };
#else
  // Stub — built without libpq (the default //bench:bench_harness target).
  // PGconn is a libpq type, so it cannot even be *named* here; the previous
  // `PGconn *db_conn = nullptr;` only ever compiled because HAVE_LIBPQ was
  // always defined. Build //bench:bench_harness_pg for --db support.
  if (!db_connstr.empty())
  {
    std::cerr << "Warning: --db was given but this binary was built without libpq; "
                 "metrics will be written to stdout only. "
                 "Use //bench:bench_harness_pg for PostgreSQL persistence.\n";
  }
  auto insert_metric = [](const bench::MetricEvent &, int64_t, int64_t) {};
#endif

  // Emit CSV regardless of DB availability
  auto emit_metric = [&](const bench::MetricEvent &m, int64_t target_mb, int64_t n_patients)
  {
    std::cout << m.arm << "," << bench::to_string(m.stage) << "," << m.duration_ns
              << "," << target_mb << "," << n_patients << "\n"
              << std::flush;
  };

  // -----------------------------------------------------------------------
  // Main benchmark loop
  // -----------------------------------------------------------------------
  bool validation_failed = false;

  for (const int64_t target_bytes : target_sizes_bytes)
  {
    const int64_t target_mb = target_bytes / (1024 * 1024);
    std::cerr << "=== " << target_mb << " MB (" << num_runs << " runs, "
              << warmup_iterations << " warmups) ===\n";

    for (int sample_run = 0; sample_run < num_runs; ++sample_run)
    {
      for (int iter = 0; iter < iterations; ++iter)
      {
        // Build a fresh random bundle
        bench::BundleBenchFixture bundle{};
        bundle.target_size_bytes = target_bytes;
        bundle.fastfhir_vma_bytes = fastfhir_vma_mb > 0 ? fastfhir_vma_mb * 1024 * 1024 : 0;

        int64_t accumulated = 0;
        while (accumulated < target_bytes)
        {
          const auto &p = all_patients[patient_dist(rng)];
          bundle.bundle.push_back(bench::clone_bundle_patient(p.patient));
          accumulated += p.patient.memory.size();
        }
        bundle.actual_ingested_bytes = accumulated;
        const int64_t n_patients = static_cast<int64_t>(bundle.bundle.size());

        if (sample_run == 0 && iter == 0)
        {
          std::cerr << "  Typical bundle: " << n_patients << " patients, "
                    << (accumulated / (1024 * 1024)) << " MB ingested FFHR\n";
        }

        // Warmup (unmeasured)
        warmup_arms(bundle, warmup_iterations);

        // Timed runs — all available arms
        const auto ff = bench::run_fastfhir_bundle(bundle);
        const auto jf = bench::run_json_bundle(bundle);
#if defined(HAVE_HL7V2)
        const auto h2 = bench::run_hl7v2_bundle(bundle);
#endif
#if defined(HAVE_GOOGLE_FHIR)
        const auto gf = bench::run_google_fhir_bundle(bundle);
#endif

        // Cross-arm validation when all arms present
#if defined(HAVE_GOOGLE_FHIR)
        validate_parity(ff, jf, h2, validation_failed);
#endif

        // Collect and emit all metrics
        auto emit = [&](const std::vector<bench::MetricEvent> &metrics)
        {
          for (const auto &m : metrics)
          {
            emit_metric(m, target_mb, n_patients);
            insert_metric(m, target_mb, n_patients);
          }
        };
        emit(ff.metrics);
        emit(jf.metrics);
#if defined(HAVE_HL7V2)
        emit(h2.metrics);
#endif
#if defined(HAVE_GOOGLE_FHIR)
        emit(gf.metrics);
#endif
      }
    }
  }

#ifdef HAVE_LIBPQ
  if (db_conn)
    PQfinish(db_conn);
#endif

  if (validation_failed)
  {
    std::cerr << "Validation failed: one or more cross-arm parity checks did not match.\n";
    return 2;
  }
  return 0;
}
