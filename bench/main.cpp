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

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc);

  // Defaults
  int iterations = 1;
  int warmup_iterations = 1;
  int num_runs = 10;
  int64_t bundle_max_mb = 0;
  bool bundle_max_mb_explicit = false;
  int64_t fastfhir_vma_mb = 0;
  std::string db_connstr;  // PostgreSQL connection string
  std::vector<int64_t> target_sizes_bytes = {
      1LL  * 1024 * 1024,
      2LL  * 1024 * 1024,
      4LL  * 1024 * 1024,
      8LL  * 1024 * 1024,
      16LL * 1024 * 1024,
      32LL * 1024 * 1024,
      64LL * 1024 * 1024,
      256LL* 1024 * 1024,
  };

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--iterations" && i + 1 < args.size()) {
      iterations = std::max(1, std::atoi(args[i + 1].data()));
    } else if (args[i] == "--warmup-iterations" && i + 1 < args.size()) {
      warmup_iterations = std::max(0, std::atoi(args[i + 1].data()));
    } else if (args[i] == "--runs" && i + 1 < args.size()) {
      num_runs = std::max(1, std::atoi(args[i + 1].data()));
    } else if (args[i] == "--bundle-max-mb" && i + 1 < args.size()) {
      bundle_max_mb = std::max<int64_t>(1, std::atoll(args[i + 1].data()));
      bundle_max_mb_explicit = true;
    } else if (args[i] == "--ff-vma-mb" && i + 1 < args.size()) {
      fastfhir_vma_mb = std::max<int64_t>(0, std::atoll(args[i + 1].data()));
    } else if (args[i] == "--db" && i + 1 < args.size()) {
      db_connstr = std::string(args[i + 1]);
    } else if (args[i] == "--bundle-targets-mb" && i + 1 < args.size()) {
      target_sizes_bytes.clear();
      std::stringstream ss{std::string(args[i + 1])};
      std::string token;
      while (std::getline(ss, token, ',')) {
        const auto mb = std::atoll(token.c_str());
        if (mb > 0) target_sizes_bytes.push_back(mb * 1024 * 1024);
      }
    }
  }

  // Apply explicit --bundle-max-mb cap, then sort and deduplicate.
  if (bundle_max_mb_explicit) {
    target_sizes_bytes.erase(
        std::remove_if(target_sizes_bytes.begin(), target_sizes_bytes.end(),
                       [bundle_max_mb](int64_t b) { return b > bundle_max_mb * 1024 * 1024; }),
        target_sizes_bytes.end());
  }
  std::sort(target_sizes_bytes.begin(), target_sizes_bytes.end());
  target_sizes_bytes.erase(
      std::unique(target_sizes_bytes.begin(), target_sizes_bytes.end()),
      target_sizes_bytes.end());

  if (target_sizes_bytes.empty()) {
    if (bundle_max_mb_explicit) {
      std::cerr << "No target sizes remain after applying --bundle-max-mb " << bundle_max_mb << "\n";
    } else {
      std::cerr << "No target sizes were configured.\n";
    }
    return 1;
  }

  // Locate Synthea data
    const fs::path primary_dir = "/Users/RyanLandvater/Programming_Projects/FHIR_Testing/datasets/synthea"; //"datasets/synthea/fhir";
  const fs::path fallback_dir = "datasets/synthea";
  const fs::path synthea_dir = fs::exists(primary_dir) ? primary_dir : fallback_dir;
  if (!fs::exists(synthea_dir)) {
    std::cerr << "Synthea data not found at " << primary_dir << " or " << fallback_dir << "\n";
    return 1;
  }

  // Pre-load all patient JSON files and ingest to in-RAM bundle records.
  struct IngestedPatient {
    bench::BundlePatient patient;
  };
  std::vector<IngestedPatient> all_patients;

  std::cerr << "Ingesting Synthea JSON files from " << synthea_dir << " ...\n";
  for (const auto& entry : fs::directory_iterator(synthea_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
    try {
      auto patient = bench::make_bundle_patient_from_json(entry.path());
      all_patients.push_back({std::move(patient)});
    } catch (const std::exception& ex) {
      std::cerr << "  skip " << entry.path().filename() << ": " << ex.what() << "\n";
    }
  }

  if (all_patients.empty()) {
    std::cerr << "No patient JSON files found in " << synthea_dir << ".\n"
              << "Run ./generate_repo.sh to download Synthea input files.\n";
    return 1;
  }
  std::cerr << "Loaded " << all_patients.size() << " patients.\n\n";

  // Random number generator for patient selection
  std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<std::size_t> patient_dist(0, all_patients.size() - 1);

  // CSV header
  std::cout << "arm,test,duration_ns,target_mb,patients_in_bundle\n" << std::flush;

#ifdef HAVE_LIBPQ
  PGconn* db_conn = nullptr;
  int run_id = -1;
  if (!db_connstr.empty()) {
    db_conn = PQconnectdb(db_connstr.c_str());
    if (PQstatus(db_conn) != CONNECTION_OK) {
      std::cerr << "Database connection failed: " << PQerrorMessage(db_conn) << "\n";
      PQfinish(db_conn);
      db_conn = nullptr;
    } else {
      PGresult* alter_res = PQexec(
          db_conn,
          "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS duration_ns BIGINT");
      if (PQresultStatus(alter_res) != PGRES_COMMAND_OK) {
        std::cerr << "Failed to ensure duration_ns column: " << PQerrorMessage(db_conn);
      }
      PQclear(alter_res);

      alter_res = PQexec(
          db_conn,
          "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS duration_us BIGINT");
      if (PQresultStatus(alter_res) != PGRES_COMMAND_OK) {
        std::cerr << "Failed to ensure duration_us column: " << PQerrorMessage(db_conn);
      }
      PQclear(alter_res);

      // Insert benchmark run record
      const std::string run_query =
          "INSERT INTO benchmark_runs (iterations) VALUES (" + std::to_string(iterations) + ") RETURNING id";
      PGresult* res = PQexec(db_conn, run_query.c_str());
      if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        run_id = std::atoi(PQgetvalue(res, 0, 0));
        std::cerr << "Database run_id: " << run_id << "\n";
      } else {
        std::cerr << "Failed to insert run record: " << PQerrorMessage(db_conn);
      }
      PQclear(res);
    }
  }
#endif


  // Main benchmark loop — one pass per target size, multiple random runs per size
  bool validation_failed = false;
  for (const int64_t target_bytes : target_sizes_bytes) {
    const int64_t target_mb = target_bytes / (1024 * 1024);
    std::cerr << "=== " << target_mb << " MB (" << num_runs << " runs, "
          << warmup_iterations << " warmups) ===\n";

    for (int sample_run = 0; sample_run < num_runs; ++sample_run) {
      for (int iter = 0; iter < iterations; ++iter) {
        // Build a fresh random bundle for each measured iteration.
        bench::BundleBenchFixture bundle{};
        bundle.target_size_bytes = target_bytes;
        bundle.fastfhir_vma_bytes = fastfhir_vma_mb > 0 ? fastfhir_vma_mb * 1024 * 1024 : 0;

        int64_t accumulated = 0;
        while (accumulated < target_bytes) {
          const auto& p = all_patients[patient_dist(rng)];
          bundle.bundle.push_back(bench::clone_bundle_patient(p.patient));
          accumulated += p.patient.memory.size();
        }
        bundle.actual_ingested_bytes = accumulated;

        const int64_t n_patients = static_cast<int64_t>(bundle.bundle.size());
        if (sample_run == 0 && iter == 0) {
          std::cerr << "  Typical bundle: " << n_patients << " patients, "
                    << (accumulated / (1024 * 1024)) << " MB ingested FFHR\n";
        }

#ifdef HAVE_LIBPQ
        auto maybe_insert_metric = [&](const bench::MetricEvent& m) {
          if (!(db_conn && run_id >= 0)) {
            return;
          }

          const std::string run_id_s = std::to_string(run_id);
            const std::string duration_ns_s = std::to_string(m.duration_ns);
            const std::string duration_us_s = std::to_string((m.duration_ns + 999) / 1000);
          const std::string target_mb_s = std::to_string(target_mb);
          const std::string n_patients_s = std::to_string(n_patients);
          const std::string stage_s = bench::to_string(m.stage);

            const char* params[7] = {
              run_id_s.c_str(),
              m.arm.c_str(),
              stage_s.c_str(),
              duration_ns_s.c_str(),
              duration_us_s.c_str(),
              target_mb_s.c_str(),
              n_patients_s.c_str(),
          };

          PGresult* res = PQexecParams(
              db_conn,
              "INSERT INTO benchmark_results "
              "(run_id, arm, stage, duration_ns, duration_us, target_mb, patients_in_bundle) "
              "VALUES ($1::int, $2::text, $3::text, $4::bigint, $5::bigint, $6::int, $7::int)",
              7,
              nullptr,
              params,
              nullptr,
              nullptr,
              0);
          if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "Failed metric insert: " << PQerrorMessage(db_conn);
          }
          PQclear(res);
        };
#endif

        // Warmup passes are intentionally not recorded.
        for (int warm = 0; warm < warmup_iterations; ++warm) {
          (void)bench::run_fastfhir_bundle(bundle);
          (void)bench::run_json_bundle(bundle);
          (void)bench::run_hl7v2_bundle(bundle);
        }

        const auto ff = bench::run_fastfhir_bundle(bundle);
        const auto jf = bench::run_json_bundle(bundle);
        const auto h2 = bench::run_hl7v2_bundle(bundle);

        if (!bench::validate_results(ff, jf)) {
          validation_failed = true;
          std::cerr << "  [validate] values: fastfhir=[" << ff.queried_value << "]"
                    << " json=["     << jf.queried_value << "]\n";
        }
        if (!bench::validate_hl7_results(ff, jf, h2)) {
          validation_failed = true;
          std::cerr << "  [validate-hl7] values: baseline_json=[" << jf.queried_value << "]"
                    << " hl7=[" << h2.queried_value << "]\n";
        }

        std::vector<bench::MetricEvent> run_metrics;
        run_metrics.reserve(ff.metrics.size() + jf.metrics.size() + h2.metrics.size());
        run_metrics.insert(run_metrics.end(), ff.metrics.begin(), ff.metrics.end());
        run_metrics.insert(run_metrics.end(), jf.metrics.begin(), jf.metrics.end());
        run_metrics.insert(run_metrics.end(), h2.metrics.begin(), h2.metrics.end());

        for (const auto& m : run_metrics) {
          std::cout << m.arm << "," << bench::to_string(m.stage) << "," << m.duration_ns
                    << "," << target_mb << "," << n_patients << "\n";
#ifdef HAVE_LIBPQ
          maybe_insert_metric(m);
#endif
        }
        std::cout << std::flush;
      }  // Close iterations loop
    }  // Close sample_run loop
  }  // Close target_sizes_bytes loop

#ifdef HAVE_LIBPQ
  if (db_conn) {
    PQfinish(db_conn);
  }
#endif

  if (validation_failed) {
    std::cerr << "Validation failed: one or more cross-arm parity checks did not match.\n";
    return 2;
  }

  return 0;
}
