// read_path_bench.cpp — read-path traversal validation for FastFHIR, ported
// from the validation instrument built 2026-08-19 during TASKS.md OPEN TOPIC
// investigation (see /tmp/ff_read_bench.cpp for the original).
//
// Reports every whole-document read-path metric as an average **per Bundle
// entry** (µs / entry_count) and gates the run: the run FAILS (exit 1) if any
// average exceeds 50 µs per entry.
//
// Build/run (Bazel, opt — see .bazelrc --compilation_mode=opt):
//   bazelisk run //bench:read_path_bench -- /path/to/bundle.ffhr
//
// The binary self-reports its optimization mode: Release defines NDEBUG; a
// Debug build of FastFHIR measures ~10x slower on these paths (see README,
// "FastFHIR build flags — the Debug trap") — the 50 µs/entry gate is a
// regression bound for optimized builds, not a Debug-vs-Release discriminator.
#include "FF_Parser.hpp"
#include "FF_Compactor.hpp"
#include "FF_FieldKeys.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

using namespace FastFHIR;
using Clock = std::chrono::steady_clock;

static constexpr double kMaxUsPerEntry = 50.0;  // validation gate

// ---- mmap RAII (same pattern as tools/exporter) ----
class MappedFile {
    const BYTE* m_data = nullptr;
    size_t m_size = 0;
    int fd = -1;
public:
    MappedFile(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd == -1) throw std::runtime_error("open failed: " + path);
        struct stat sb;
        if (fstat(fd, &sb) == -1) throw std::runtime_error("fstat failed");
        m_size = static_cast<size_t>(sb.st_size);
        m_data = static_cast<const BYTE*>(mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (m_data == MAP_FAILED) throw std::runtime_error("mmap failed");
    }
    ~MappedFile() { if (m_data && m_data != MAP_FAILED) munmap(const_cast<BYTE*>(m_data), m_size); if (fd != -1) close(fd); }
    const BYTE* data() const { return m_data; }
    size_t size() const { return m_size; }
};

// ---- null sink so print_json pays for traversal, not terminal I/O ----
struct NullBuf : std::streambuf {
    char buf[4096];
    NullBuf() { setp(buf, buf + sizeof(buf)); }
    int overflow(int c) override { return std::char_traits<char>::not_eof(c); }
};
struct NullStream : std::ostream {
    NullBuf buf;
    NullStream() : std::ostream(&buf) {}
};

// ---- reflective whole-doc walk through the PUBLIC API (allocations included) ----
// Mirrors production traversal: arrays via entries() (vector<Node>), objects via
// fields() (vector<FF_FieldInfo>) + owner-keyed lookup, exactly as is_empty()/print do.
static size_t walk_node(const Reflective::Node& n) {
    size_t count = 1;
    switch (n.kind()) {
    case FF_FIELD_ARRAY: {
        auto es = n.entries();
        for (const auto& e : es) count += walk_node(e);
        break;
    }
    case FF_FIELD_BLOCK: {
        auto fs = n.fields();
        for (const auto& f : fs) {
            FF_FieldKey key = FF_FieldKey::from_cstr(n.recovery(), f.kind, f.field_offset,
                                                     f.child_recovery, f.array_entries_are_offsets, f.name);
            Reflective::Entry ent = n[key];
            if (ent) {
                Reflective::Node child = ent.as_node();
                if (child) count += walk_node(child);
            }
        }
        break;
    }
    default: break; // strings/codes/scalars: count the node, do not descend
    }
    return count;
}

// bench: min of `runs` timings (TASKS.md OPEN TOPIC §E), returns best time in ms.
template <typename F>
static double bench_ms(int runs, F&& f, size_t* out_count = nullptr) {
    double best = 1e18;
    size_t c = 0;
    for (int i = 0; i < runs; ++i) {
        auto t0 = Clock::now();
        c = f();
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) best = ms;
    }
    if (out_count) *out_count = c;
    return best;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: read_path_bench <file.ffhr>\n"); return 2; }
    const int RUNS = 7;  // §E: minimum of >= 5 runs

    MappedFile file(argv[1]);
    const BYTE* base = file.data();
    const size_t size = file.size();

    Parser parser(base, size);   // construction is not the metric under test
    Reflective::Node root = parser.root();
    Reflective::Entry entry_slot = root[Fields::BUNDLE::ENTRY];
    const uint32_t entry_count = (uint32_t)entry_slot.size();
    if (entry_count == 0) {
        std::fprintf(stderr, "error: root is not a Bundle with entries (got %u)\n", entry_count);
        return 2;
    }

    printf("read_path_bench — FastFHIR read-path validation\n");
    printf("fixture: %s (%zu bytes = %.1f MiB)\n", argv[1], size, size / 1048576.0);
    printf("build:   %s\n", NDEBUG ? "Release (NDEBUG, optimized)" : "DEBUG (NOT optimized — numbers will be ~10x slow)");
    printf("bundle entries: %u\n", entry_count);
    printf("stream layout: %d, root type: %u\n\n", (int)parser.stream_layout(), (unsigned)parser.root_type());

    struct Metric { const char* name; double us_per_entry; bool absolute; };
    std::vector<Metric> metrics;

    // Parser ctor — absolute, not per-entry.
    metrics.push_back({"Parser construction (absolute)", bench_ms(RUNS, [&]() -> size_t {
        Parser p(base, size); return p.root_type(); }) * 1000.0, true});

    // validate_FFHR_stream() — full graph walk.
    metrics.push_back({"validate_FFHR_stream()", bench_ms(RUNS, [&]() -> size_t {
        return (size_t)parser.validate_FFHR_stream(); }) * 1000.0 / entry_count, false});

    // Reflective walk via the public API (the README "0 heap allocations" claim).
    {
        size_t visits = 0;
        double us = bench_ms(RUNS, [&]() -> size_t { return walk_node(parser.root()); }, &visits) * 1000.0;
        metrics.push_back({"reflective walk (public API)", us / entry_count, false});
        printf("  (walk visits %zu nodes)\n", visits);
    }

    // print_json to a null sink — the JSON export walk.
    metrics.push_back({"print_json() -> null sink", bench_ms(RUNS, [&]() -> size_t {
        NullStream sink; parser.print_json(sink); return 1; }) * 1000.0 / entry_count, false});

    // Bundle.entry.entries() — materializing the entry array (vector<Node>).
    metrics.push_back({"Bundle.entry.entries() (materialize)", bench_ms(RUNS, [&]() -> size_t {
        return entry_slot.entries().size(); }) * 1000.0 / entry_count, false});

    // Compactor::archive — the compaction walk.
    metrics.push_back({"Compactor::archive()", bench_ms(RUNS, [&]() -> size_t {
        Memory dest = Memory::create(512ULL * 1024 * 1024);
        return Compactor::archive(parser, dest, FF_CHECKSUM_NONE).size(); }) * 1000.0 / entry_count, false});

    // Report + gate.
    printf("%-40s %12s %14s\n", "metric", "avg us/entry", "gate <= 50us");
    double max_us = 0.0;
    bool pass = true;
    for (const auto& m : metrics) {
        bool ok = m.absolute || m.us_per_entry <= kMaxUsPerEntry;
        pass = pass && ok;
        if (!m.absolute && m.us_per_entry > max_us) max_us = m.us_per_entry;
        printf("%-40s %12.3f %14s\n", m.name, m.us_per_entry,
               m.absolute ? "n/a" : (ok ? "PASS" : "FAIL"));
    }
    printf("\nRESULT: %s (max avg %.3f us/entry, bound %g us/entry)\n",
           pass ? "PASS" : "FAIL", max_us, kMaxUsPerEntry);
    return pass ? 0 : 1;
}
