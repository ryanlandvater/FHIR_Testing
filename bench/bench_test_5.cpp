// ===========================================================================
// Test 5 driver -- Instrument G corruption/recovery comparison
// (macro-parity restructure, handoff.md "SESSION CONTINUITY")
// ===========================================================================
// Links the four arm TUs, each of which compiled bench_test_5.hpp under its
// own ARM_* macro, and dispatches FIVE INDEPENDENT process modes so no mode
// can leak state to another:
//
//   --hash <format> --in WIRE [--out FP]
//       structural fingerprint of a CLEAN wire (the baseline producer).
//   --corrupt <format> --bits K --seed S --in WIRE --out DAMAGED
//       flip K random STRUCTURAL bits (per-format syntax regions).
//   --recover <format> --in DAMAGED [--out FP]
//       resync from the corrupted bytes ONLY; writes the recovered units.
//   --check --baseline FP --recovered FP
//       a THIRD process holding the baseline the recoverer never saw:
//       (a) report integrity -- the recovered file's digest must match its
//       own units; (b) content verification -- every recovered unit must
//       exist in the baseline with the same (parent, offset, tag); (c) the
//       recovery percentage. The recoverer never sees the clean artifact.
//   --positions <format> --in WIRE
//       the structural-position count of a clean wire -- the density
//       denominator for flaw B (k flips means different damage per format).
//
// The fingerprint files (FP) are binary: u64 LE unit count, then per unit
// (u64 LE parent, u64 LE offset, u16 LE tag) -- the exact canonical
// serialization StreamFingerprint::finalize() digests -- then the 32-byte
// digest itself. The digest lets --check re-derive the report's integrity
// stamp instead of trusting the recoverer's summary line.
//
// The check is content verification, not boundary survival. It says so here
// because for a long time it was not true: a unit counted as recovered when
// its (parent, offset, tag) identity matched, and NOTHING read the bytes the
// unit carried. Every arm was scored on whether its container was still
// findable -- v2 scanned for `XXX|`, protobuf asked only whether
// ParseFromArray() succeeded, JSON looked for a `"resource"` marker.
//
// Measured on the v2 artifact before the fix: destroying all 14,701 segment
// terminators scored 100.0; destroying 20,673 interior field separators
// scored 100.0; obliterating 95.4% of the file -- every byte of clinical
// content replaced with 'Z' -- scored 100.0 with digest_ok=1.
//
// Each unit now carries a hash of its own data, and the check asks two
// questions instead of one: identity says the entry is THERE, content says it
// is INTACT. An entry that is still findable but no longer carries what was
// written is `wrong`, not recovered -- 01223 != 01223Nsomething -- and pct is
// CORRECT over baseline. Flaw B (k flips means different damage per format)
// is still served by --positions and is NOT fixed by this.
// ===========================================================================

#include "bench_test_5.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace t5 = bench::test_5;

namespace {

// ---------------------------------------------------------------------------
// File IO + the binary fingerprint-file format
// ---------------------------------------------------------------------------

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("cannot open " + path);
  const auto n = in.tellg();
  if (n < 0)
    throw std::runtime_error("cannot size " + path);
  std::vector<uint8_t> bytes(static_cast<std::size_t>(n));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!in)
    throw std::runtime_error("short read on " + path);
  return bytes;
}

void write_file(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out)
    throw std::runtime_error("cannot write " + path);
}

void append_u64(std::string& s, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

void append_u32(std::string& s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

// The canonical unit serialization -- must match StreamFingerprint::finalize
// byte-for-byte, because the digest IS the integrity stamp over these bytes.
// One unit is 28 bytes: parent(8) offset(8) tag(4) content(8). `content` is
// the hash of the unit's DATA -- it rides in the digest so a payload change
// moves the integrity stamp, which a header-only fingerprint could not see.
std::string serialize_units(const std::vector<t5::UnitRef>& units) {
  std::string canon;
  canon.reserve(units.size() * 28);
  for (const auto& u : units) {
    append_u64(canon, static_cast<std::uint64_t>(u.parent));
    append_u64(canon, static_cast<std::uint64_t>(u.offset));
    append_u32(canon, u.tag);
    append_u64(canon, u.content);
  }
  return canon;
}

std::string hex_digest(const std::string& digest) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned char c : digest) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xF]);
  }
  return out;
}

std::string stamp_fingerprint(const t5::StreamFingerprint& fp) {
  // [unit count][18-byte units][32-byte digest] -- digest over the units ONLY,
  // so --check can recompute it from the units it reads back.
  std::string buf;
  append_u64(buf, fp.units.size());
  buf += serialize_units(fp.units);
  buf += fp.digest;
  return buf;
}

// Reads a fingerprint file back; returns false (with `err`) on any structural
// violation, including a digest that does not match the units in the file.
bool read_fingerprint(const std::string& path, t5::StreamFingerprint& fp,
                      std::string& err) {
  try {
    const auto bytes = read_file(path);
    if (bytes.size() < 40) {  // 8 count + nothing + 32 digest
      err = path + ": file too short to be a fingerprint";
      return false;
    }
    std::uint64_t count = 0;
    for (int i = 0; i < 8; ++i)
      count |= static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)]) << (8 * i);
    const std::size_t expect = 8 + static_cast<std::size_t>(count) * 28 + 32;
    if (bytes.size() != expect) {
      err = path + ": size mismatch (declares " + std::to_string(count) +
            " units, file is " + std::to_string(bytes.size()) + " bytes)";
      return false;
    }
    fp.units.clear();
    fp.units.reserve(static_cast<std::size_t>(count));
    std::size_t p = 8;
    for (std::uint64_t i = 0; i < count; ++i) {
      t5::UnitRef u;
      u.parent = 0;
      u.offset = 0;
      u.tag = 0;
      u.content = 0;
      for (int b = 0; b < 8; ++b) {
        u.parent |= static_cast<std::size_t>(bytes[p + static_cast<std::size_t>(b)]) << (8 * b);
        u.offset |= static_cast<std::size_t>(bytes[p + 8 + static_cast<std::size_t>(b)]) << (8 * b);
      }
      for (int b = 0; b < 4; ++b)
        u.tag |= static_cast<std::uint32_t>(bytes[p + 16 + static_cast<std::size_t>(b)]) << (8 * b);
      for (int b = 0; b < 8; ++b)
        u.content |= static_cast<std::uint64_t>(bytes[p + 20 + static_cast<std::size_t>(b)]) << (8 * b);
      fp.units.push_back(u);
      p += 28;
    }
    // Report-integrity stamp: re-derive the digest over the units we read.
    fp.digest.assign(bytes.begin() + static_cast<std::ptrdiff_t>(p), bytes.end());
    std::string canon = serialize_units(fp.units);
    bench::provenance::sha256::Ctx c;
    bench::provenance::sha256::update(c, canon.data(), canon.size());
    std::string recomputed(32, '\0');
    bench::provenance::sha256::finish(c,
        reinterpret_cast<unsigned char*>(recomputed.data()));
    if (recomputed != fp.digest) {
      err = path + ": digest does not match its units (corrupt or truncated file)";
      return false;
    }
    return true;
  } catch (const std::exception& ex) {
    err = ex.what();
    return false;
  }
}

// ---------------------------------------------------------------------------
// Arm dispatch -- linear table, no branching on format strings (style guide).
// ---------------------------------------------------------------------------

const t5::ArmOps* find_arm(const std::string& name) {
  // Bazel links all four. The CMake/Xcode debug build (CMakeLists.txt) can
  // link a subset -- google_fhir needs protobuf + the generated google-fhir
  // protos, which only exist inside the Bazel graph -- so each entry is
  // guarded by the macro that build defines. Bazel defines none of these and
  // gets the full table.
  static const t5::ArmOps* kArms[] = {
#if !defined(BENCH_NO_ARM_FASTFHIR)
      &t5::arm_ops_fastfhir(),
#endif
#if !defined(BENCH_NO_ARM_JSON)
      &t5::arm_ops_json(),
#endif
#if !defined(BENCH_NO_ARM_GOOGLE_FHIR)
      &t5::arm_ops_google_fhir(),
#endif
      &t5::arm_ops_hl7v2(),
  };
  for (const t5::ArmOps* arm : kArms)
    if (name == arm->name)
      return arm;
  return nullptr;
}

std::string usage() {
  return
      "usage:\n"
      "  bench_test_5 --hash <format> --in WIRE [--out FP]\n"
      "  bench_test_5 --corrupt <format> --bits K --seed S --in WIRE --out DAMAGED\n"
      "  bench_test_5 --recover <format> --in DAMAGED [--out FP]\n"
      "  bench_test_5 --check --baseline FP --recovered FP\n"
      "  bench_test_5 --positions <format> --in WIRE\n"
      "formats: fastfhir, json, google_fhir, hl7v2\n";
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

int mode_hash(const t5::ArmOps& arm, const std::string& in,
              const std::string& out_path) {
  const auto wire = read_file(in);
  const auto fp = arm.calc_hash(wire);
  std::printf("units=%zu digest=%s\n", fp.units.size(), hex_digest(fp.digest).c_str());
  if (!out_path.empty())
    write_file(out_path, stamp_fingerprint(fp));
  return 0;
}

int mode_positions(const t5::ArmOps& arm, const std::string& in) {
  const auto wire = read_file(in);
  std::printf("positions=%zu\n", arm.count_positions(wire));
  return 0;
}

int mode_corrupt(const t5::ArmOps& arm, std::size_t bits, unsigned seed,
                 const std::string& in, const std::string& out_path) {
  const auto clean = read_file(in);
  const auto damaged = arm.corrupt(clean, bits, seed);
  std::string bytes(damaged.begin(), damaged.end());
  write_file(out_path, bytes);
  std::printf("damaged=%zu\n", damaged.size());
  return 0;
}

int mode_recover(const t5::ArmOps& arm, const std::string& in,
                 const std::string& out_path) {
  const auto wire = read_file(in);
  const auto fp = arm.recover(wire);
  std::printf("units=%zu digest=%s\n", fp.units.size(), hex_digest(fp.digest).c_str());
  if (!out_path.empty())
    write_file(out_path, stamp_fingerprint(fp));
  return 0;
}

// Recovered units must be a SUBSET of the baseline on the anchored triple
// (parent, offset, tag). Count multiset intersection on two lexicographically
// sorted copies -- merge, not lookup: 44k units on the FFHR artifact.
struct Triple {
  std::uint64_t parent;
  std::uint64_t offset;
  std::uint32_t tag;
  bool operator<(const Triple& o) const {
    if (parent != o.parent) return parent < o.parent;
    if (offset != o.offset) return offset < o.offset;
    return tag < o.tag;
  }
  bool operator==(const Triple& o) const {
    return parent == o.parent && offset == o.offset && tag == o.tag;
  }
  // Deliberately outside `<` and `==`: identity orders the merge, content is
  // the SECOND question asked once two units are known to be the same unit.
  std::uint64_t content = 0;
};

std::vector<Triple> to_triples(const std::vector<t5::UnitRef>& units) {
  std::vector<Triple> out;
  out.reserve(units.size());
  for (const auto& u : units)
    out.push_back({static_cast<std::uint64_t>(u.parent),
                   static_cast<std::uint64_t>(u.offset), u.tag, u.content});
  std::sort(out.begin(), out.end());
  return out;
}

int mode_check(const std::string& base_path, const std::string& rec_path) {
  t5::StreamFingerprint baseline, recovered;
  std::string err;
  if (!read_fingerprint(base_path, baseline, err) ||
      !read_fingerprint(rec_path, recovered, err)) {
    std::fprintf(stderr, "check: %s\n", err.c_str());
    return 1;
  }
  // Digest verification already happened inside read_fingerprint (the digest
  // stored in each file is re-derived from its own units).

  const auto base = to_triples(baseline.units);
  const auto rec = to_triples(recovered.units);

  // FOUR OUTCOMES, because "found" and "intact" are different claims.
  //
  //   correct  -- the unit is there AND carries the data it carried before.
  //   wrong    -- the unit is there and the data CHANGED. This is the outcome
  //               that did not exist before, and it is the dangerous one: the
  //               record still parses, still looks well-formed, and reads as
  //               valid clinical data that is not what was written
  //               (01223 -> 01223Nsomething).
  //   missing  -- in the baseline, not in the recovery. Honest loss.
  //   spurious -- in the recovery, not in the baseline. Invention.
  //
  // pct is CORRECT over baseline. It used to be `matched`, which counted a
  // unit whose every data byte had been destroyed: obliterating 95.4% of the
  // v2 artifact scored 100.0.
  std::size_t correct = 0, wrong = 0;
  std::size_t i = 0, j = 0;
  while (i < base.size() && j < rec.size()) {
    if (rec[j] < base[i]) {
      ++j;  // recovered a unit the baseline never had -- spurious
      continue;
    }
    if (base[i] < rec[j]) {
      ++i;  // baseline unit the recovery missed -- lost
      continue;
    }
    if (base[i].content == rec[j].content)
      ++correct;
    else
      ++wrong;
    ++i;
    ++j;
  }
  const std::size_t matched = correct + wrong;
  const std::size_t spurious = rec.size() - matched;
  const std::size_t missing = base.size() - matched;
  const double pct = base.empty() ? 0.0 : 100.0 * static_cast<double>(correct) /
                                              static_cast<double>(base.size());
  std::printf("baseline=%zu recovered=%zu correct=%zu wrong=%zu missing=%zu "
              "spurious=%zu digest_ok=1 pct=%.1f\n",
              base.size(), rec.size(), correct, wrong, missing, spurious, pct);
  return 0;
}

}  // namespace

#if defined(BENCH_CMAKE_DEBUG_BUILD)
// This build exists to be STEPPED THROUGH, not to be quoted. Say so on every
// run rather than trusting whoever reads the output to remember which build
// produced it -- the numbers are correctness counts here, but the binary is
// -O0 and any timing taken from it is meaningless.
static void announce_debug_build() {
  std::fprintf(stderr,
               "*** CMake DEBUG build (-O0) -- for the debugger, NOT for results. ***\n"
               "*** Publishable numbers come from Bazel (--compilation_mode=opt). ***\n");
}
#else
static void announce_debug_build() {}
#endif

int main(int argc, char** argv) {
  announce_debug_build();
  enum class Mode { None, Hash, Corrupt, Recover, Check, Positions };
  Mode mode = Mode::None;
  std::string format, in_path, out_path, base_path, rec_path;
  std::size_t bits = 0;
  unsigned seed = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string tok(argv[i]);
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s: missing %s\n", tok.c_str(), what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (tok == "--hash") {
      mode = Mode::Hash;
      format = need("format");
    } else if (tok == "--corrupt") {
      mode = Mode::Corrupt;
      format = need("format");
    } else if (tok == "--recover") {
      mode = Mode::Recover;
      format = need("format");
    } else if (tok == "--check") {
      mode = Mode::Check;
    } else if (tok == "--positions") {
      mode = Mode::Positions;
      format = need("format");
    } else if (tok == "--in") {
      in_path = need("--in value");
    } else if (tok == "--out") {
      out_path = need("--out value");
    } else if (tok == "--baseline") {
      base_path = need("--baseline value");
    } else if (tok == "--recovered") {
      rec_path = need("--recovered value");
    } else if (tok == "--bits") {
      bits = std::strtoull(need("--bits value"), nullptr, 10);
    } else if (tok == "--seed") {
      seed = static_cast<unsigned>(std::strtoul(need("--seed value"), nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", tok.c_str());
      std::fputs(usage().c_str(), stderr);
      return 2;
    }
  }

  try {
    switch (mode) {
      case Mode::Hash: {
        const t5::ArmOps* arm = find_arm(format);
        if (!arm) { std::fprintf(stderr, "unknown format '%s'\n", format.c_str()); return 2; }
        return mode_hash(*arm, in_path, out_path);
      }
      case Mode::Positions: {
        const t5::ArmOps* arm = find_arm(format);
        if (!arm) { std::fprintf(stderr, "unknown format '%s'\n", format.c_str()); return 2; }
        return mode_positions(*arm, in_path);
      }
      case Mode::Corrupt: {
        const t5::ArmOps* arm = find_arm(format);
        if (!arm) { std::fprintf(stderr, "unknown format '%s'\n", format.c_str()); return 2; }
        if (out_path.empty()) { std::fputs("--corrupt requires --out\n", stderr); return 2; }
        return mode_corrupt(*arm, bits, seed, in_path, out_path);
      }
      case Mode::Recover: {
        const t5::ArmOps* arm = find_arm(format);
        if (!arm) { std::fprintf(stderr, "unknown format '%s'\n", format.c_str()); return 2; }
        return mode_recover(*arm, in_path, out_path);
      }
      case Mode::Check: {
        if (base_path.empty() || rec_path.empty()) {
          std::fputs("--check requires --baseline and --recovered\n", stderr);
          return 2;
        }
        return mode_check(base_path, rec_path);
      }
      default:
        std::fputs(usage().c_str(), stderr);
        return 2;
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "error: %s\n", ex.what());
    return 1;
  }
}
