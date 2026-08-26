// bench/provenance_test.cpp — the gate that keeps a number from becoming
// evidence it has not earned (TASKS.md IN-0).
//
// Two things are tested, and they are the two that would fail silently:
//   1. The SHA-256 used for corpus identity, against the FIPS 180-4 vectors. A
//      wrong digest makes every corpus regeneration look identical, which is
//      the one thing corpus_sha256 exists to detect.
//   2. missing_fields(), which is the whole enforcement mechanism. If it ever
//      returns empty for an incomplete record, the harness will happily write
//      an artifact nobody can reproduce.

#include "provenance.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
  if (!ok) ++failures;
}

bool has(const std::vector<std::string>& v, std::string_view prefix) {
  return std::any_of(v.begin(), v.end(),
                     [&](const std::string& s) { return s.rfind(prefix, 0) == 0; });
}

// A record that would legitimately pass, so each test below can knock out
// exactly one field and see the gate react to that field alone.
bench::provenance::Provenance complete_record() {
  bench::provenance::Provenance p;
  p.fastfhir_sha = "a9fd4e9000000000000000000000000000000000";
  p.fastfhir_tag = "v2026.1.0";
  p.production_profile = "us-core,billing,medication-admin,supply";
  p.production_profile_source = "cmake-cache";
  p.codesystem_enums = 80;
  p.generated_cpp = 44;
  p.compilation_mode = "opt";
  p.compiler = "clang";
  p.compiler_version = "17.0.0";
  p.os = "macos";
  p.arch = "arm64";
  p.cpu_model = "Apple M3 Max";
  p.corpus_id = "/tmp/synthea";
  p.corpus_sha256 = std::string(64, 'a');
  p.corpus_doc_count = 342;
  p.benchmark_sha = "38705db0000000000000000000000000000000000";
  p.seed = 20260825u;
  return p;
}

}  // namespace

int main() {
  std::cout << "sha256\n";
  check(bench::provenance::sha256_self_test(), "FIPS 180-4 vectors (empty, abc, 448-bit, 1e6 'a')");

  std::cout << "provenance gate\n";
  {
    const auto p = complete_record();
    check(bench::provenance::missing_fields(p).empty(), "a complete record passes");
  }
  {
    auto p = complete_record();
    p.fastfhir_sha.clear();
    check(has(bench::provenance::missing_fields(p), "fastfhir_sha"),
          "missing fastfhir_sha is caught");
  }
  {
    // The Debug trap: same code, ~10x slower, nothing in the numbers says so.
    auto p = complete_record();
    p.compilation_mode = "unoptimized";
    check(has(bench::provenance::missing_fields(p), "compilation_mode"),
          "a non-opt build cannot produce an artifact");
  }
  {
    // Worse than missing: an ambiguous profile looks established. Three build
    // trees carrying two different values is the real state of this machine.
    auto p = complete_record();
    p.production_profile_ambiguous = true;
    check(has(bench::provenance::missing_fields(p), "production_profile (ambiguous"),
          "an ambiguous profile cannot produce an artifact");
  }
  {
    // seed 0 means "random", and a random workload is not reproducible.
    auto p = complete_record();
    p.seed = 0;
    check(has(bench::provenance::missing_fields(p), "seed"), "seed 0 (random) is rejected");
  }
  {
    auto p = complete_record();
    p.corpus_sha256 = "tooshort";
    check(has(bench::provenance::missing_fields(p), "corpus_sha256"),
          "a malformed corpus digest is caught");
  }
  {
    auto p = complete_record();
    p.codesystem_enums = -1;
    check(has(bench::provenance::missing_fields(p), "codesystem_enums"),
          "missing profile corroboration is caught");
  }

  std::cout << "json\n";
  {
    const std::string json = bench::provenance::to_json(complete_record());
    check(json.front() == '{' && json.find("\"compilation_mode\": \"opt\"") != std::string::npos,
          "to_json emits the record");
    check(json.find("\"seed\": 20260825") != std::string::npos, "seed is emitted as a number");
  }
  {
    // Anything reaching a JSON string field has to survive being quoted.
    auto p = complete_record();
    p.cpu_model = "weird \"quoted\" \\ model\n";
    const std::string json = bench::provenance::to_json(p);
    check(json.find("weird \\\"quoted\\\" \\\\ model\\n") != std::string::npos,
          "json_escape handles quotes, backslashes and newlines");
  }

  std::cout << (failures == 0 ? "\nOK\n" : "\nFAILED\n");
  return failures == 0 ? 0 : 1;
}
