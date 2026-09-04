// Diagnostic probe for the FFHR recovery phase transition found in bench_test_5
// (recovery-ON collapses from ~100% to ~3% between k=32 and k=40 flips on the
// real bundle, with the collapse CONSTANT across k once it triggers -- the
// signature of one poison verdict that the REC-19.7 reapply loop then trusts).
//
// Replays a corrupt+recover trial and exposes what recover() concluded and
// what apply() actually wrote:
//   - the FF_RecoveryReport verdict census (per repair class + holes)
//   - the FF_ApplyReport (applied / declined / failed)
//   - every byte apply() changed, tagged AT-FLIP vs NOT-FLIP with the distance
//     to the nearest actual corruption site. A repair that rewrites a byte the
//     corruption never touched is by construction a WRONG repair -- the flip
//     model is single-bit and a correct fix must land exactly on a damaged bit.
//
// Usage:
//   recovery_probe --clean artifacts/fastfhir.bin --damaged D.bin [--out REPAIRED.bin]
// The damaged file is produced by: bench_test_5 --corrupt fastfhir --bits K
// --seed S --in clean --out D.bin  (flip sites are then D.bin XOR clean.bin).

#define ARM_FASTFHIR
#include "bench_test_5.hpp"
#undef ARM_FASTFHIR

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

bool write_file(const char* path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  const char* clean_path = nullptr;
  const char* damaged_path = nullptr;
  const char* out_path = nullptr;
  const char* variant_dir = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--clean") && i + 1 < argc) clean_path = argv[++i];
    else if (!std::strcmp(argv[i], "--damaged") && i + 1 < argc) damaged_path = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
    else if (!std::strcmp(argv[i], "--variant-dir") && i + 1 < argc) variant_dir = argv[++i];
  }
  if (!clean_path || !damaged_path) {
    std::fprintf(stderr, "usage: recovery_probe --clean FILE --damaged FILE [--out FILE] [--variant-dir DIR]\n");
    return 2;
  }
  const auto clean = read_file(clean_path);
  const auto damaged = read_file(damaged_path);
  if (clean.empty() || damaged.size() != clean.size()) {
    std::fprintf(stderr, "size mismatch clean=%zu damaged=%zu\n", clean.size(),
                 damaged.size());
    return 2;
  }

  // Actual corruption sites: the driver flips exactly one bit per distinct
  // structural position, so XOR clean vs damaged recovers the flip set.
  std::vector<std::size_t> flips;
  for (std::size_t i = 0; i < clean.size(); ++i)
    if (clean[i] != damaged[i]) flips.push_back(i);
  std::fprintf(stderr, "flip sites: %zu\n", flips.size());

  // The recovery, exactly as the bench FFHR arm runs it (bench_test_5.hpp
  // ARM_FASTFHIR recover_stream): diagnose, then apply into a copy.
  FastFHIR::Memory mem = bench::test_5::arm_fastfhir::wrap_wire_bytes(damaged);
  FastFHIR::Recovery rec(mem);
  const auto rep = rec.recover();
  std::printf("report blocks_total=%zu intact=%zu corroborated=%zu tag_repaired=%zu "
              "position_repaired=%zu extent_derived=%zu ambiguous=%zu unrecovered=%zu "
              "holes=%zu gaps=%zu failures=%zu\n",
              rep.blocks_total, rep.intact, rep.corroborated, rep.tag_repaired,
              rep.position_repaired, rep.extent_derived, rep.ambiguous,
              rep.unrecovered, rep.holes, rep.gaps.size(), rep.failures.size());

  std::vector<BYTE> repaired;
  const FastFHIR::FF_ApplyReport ar = rec.apply(rep, repaired);
  std::printf("apply applied=%zu declined=%zu failed=%zu\n", ar.applied,
              ar.declined, ar.failed);
  if (repaired.empty()) {
    std::fprintf(stderr, "apply produced nothing\n");
    return 1;
  }
  // Every ExtentDerived verdict: which array, which extent. This is the class
  // that writes whole ENTRY_COUNT fields -- the write that collapsed the
  // census (1473 -> 28 over intact bytes).
  for (const auto& v : rep.blocks) {
    if (v.class_ != FastFHIR::RepairClass::ExtentDerived) continue;
    std::printf("  ExtentDerived: parent=%llu field=%llu child=%llu kind=%d declared=%d "
                "derived=%u cost=%u\n",
                static_cast<unsigned long long>(v.block.parent),
                static_cast<unsigned long long>(v.block.field),
                static_cast<unsigned long long>(v.block.child),
                static_cast<int>(v.block.kind),
                static_cast<int>(v.block.declared), v.derived_extent, v.bit_cost);
  }
  if (out_path) write_file(out_path, repaired);

  // Every byte apply() changed, as contiguous runs, tagged against the actual
  // flip sites. A write that lands on a byte the corruption never touched is a
  // wrong repair by construction (single-bit flip model).
  auto nearest_flip = [&](std::size_t off) -> std::size_t {
    std::size_t best = static_cast<std::size_t>(-1);
    for (const std::size_t f : flips)
      best = std::min(best, f > off ? f - off : off - f);
    return best;
  };
  struct Run { std::size_t start, end; };
  std::vector<Run> runs;
  for (std::size_t i = 0; i < damaged.size(); ++i) {
    if (repaired[i] == damaged[i]) continue;
    if (runs.empty() || runs.back().end != i) runs.push_back({i, i + 1});
    else runs.back().end = i + 1;
  }
  std::size_t total_changed = 0;
  for (const auto& r : runs) total_changed += r.end - r.start;
  std::printf("changed bytes: %zu in %zu runs\n", total_changed, runs.size());

  std::size_t not_at_flip = 0, shown = 0;
  std::size_t wrong_writes = 0;  // wrote neither the damaged byte nor the clean one
  for (const auto& r : runs) {
    bool touches_flip = false;
    for (const std::size_t f : flips)
      if (f >= r.start && f < r.end) { touches_flip = true; break; }
    if (!touches_flip) ++not_at_flip;
    std::size_t dist = static_cast<std::size_t>(-1);
    for (std::size_t i = r.start; i < r.end; ++i) dist = std::min(dist, nearest_flip(i));
    // Restored == repaired byte matches the CLEAN byte. Anything else writes a
    // value that was never on the wire -- the wrong-repair signature.
    bool restored = true;
    for (std::size_t i = r.start; i < r.end; ++i)
      if (repaired[i] != clean[i]) { restored = false; break; }
    if (!restored) ++wrong_writes;
    // One variant per write run: damaged bytes with ONLY this run applied, for
    // single-write bisection of the leaf-loss collapse.
    if (variant_dir) {
      auto variant = damaged;
      for (std::size_t i = r.start; i < r.end; ++i) variant[i] = repaired[i];
      char path[512];
      std::snprintf(path, sizeof path, "%s/write_%06zu_%zu.bin", variant_dir, r.start,
                    r.end - r.start);
      write_file(path, variant);
    }
    std::printf("  write %zu..%zu (%zu B) %s dist=%zu %s old=", r.start, r.end,
                r.end - r.start, touches_flip ? "AT-FLIP" : "NOT-FLIP", dist,
                restored ? "RESTORE" : "WRONG");
    for (std::size_t i = r.start; i < r.end && i < r.start + 10; ++i)
      std::printf("%02x", damaged[i]);
    std::printf(" -> ");
    for (std::size_t i = r.start; i < r.end && i < r.start + 10; ++i)
      std::printf("%02x", repaired[i]);
    std::printf(" (clean=");
    for (std::size_t i = r.start; i < r.end && i < r.start + 10; ++i)
      std::printf("%02x", clean[i]);
    std::printf(")\n");
    if (++shown >= 100) { std::printf("  ... (%zu more runs)\n", runs.size() - shown); break; }
  }
  std::printf("runs NOT touching any flip site: %zu / %zu; WRONG (neither damaged nor clean) writes: %zu / %zu\n",
              not_at_flip, runs.size(), wrong_writes, runs.size());
  return 0;
}
