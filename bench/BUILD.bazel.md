# `bench/BUILD.bazel` — Bazel Build Configuration

## Purpose

Bazel BUILD file defining the benchmark's **build targets** for the Bazel build system. Used by `generate_repo.sh` for the Google FHIR build pipeline and as an alternative to the CMake build on macOS/Linux.

## Target Graph

```
bench_core_common  (FFHR + JSON + HL7v2 headers)
       │
       ├── bench_core_hl7v2  (HL7v2 arm)
       │
       └── bench_core_harness  (Google FHIR arm)
                │
          ┌─────┴──────┐
          │             │
  bench_harness   bench_timing_conformance
  (all 4 arms)    (FFHR + JSON only)
          │
  bench_harness_win
  (no Google FHIR)
```

## Target Details

### `bench_core_common` — cc_library

The core shared library containing FFHR, JSON, and HL7v2 support:

```python
cc_library(
    name = "bench_core_common",
    srcs = [
        "arm_fastfhir.cpp",
        "arm_json_fhir.cpp",
        "synthea_fixture.cpp",
    ],
    hdrs = [
        "bench_test_1.hpp",
        "bench_test_2.hpp",
        "bench_test_3.hpp",
        "bench_test_4.hpp",
        "harness.hpp",
        "hl7v2_message.hpp",
    ],
    deps = [
        "//:fastfhir_runtime",    # FastFHIR library target
        "@nlohmann_json//:json",  # External dependency
    ],
)
```

**Note**: `arm_hl7v2.cpp` is intentionally **not** in this target. It's separated to allow Windows builds without HL7v2.

### `bench_core_hl7v2` — cc_library

```python
cc_library(
    name = "bench_core_hl7v2",
    srcs = ["arm_hl7v2.cpp"],
    deps = [":bench_core_common"],
)
```

Separate target because HL7v2 has zero non-standard dependencies and can build on any platform.

### `bench_core_harness` — cc_library

```python
cc_library(
    name = "bench_core_harness",
    srcs = ["arm_google_fhir.cpp"],
    deps = [
        ":bench_core_common",
        ":bench_core_hl7v2",
        "//:google_fhir_runtime",  # Google FHIR Bazel target
    ],
)
```

Pulls in all four arms + Google FHIR runtime. Only builds on platforms where Google FHIR compiles (macOS with Bazel).

### `bench_harness` — cc_binary

```python
cc_binary(
    name = "bench_harness",
    srcs = ["main.cpp"],
    deps = [":bench_core_harness"],
    defines = ["HAVE_GOOGLE_FHIR", "HAVE_HL7V2", "HAVE_LIBPQ"],
)
```

The main benchmark binary with all arms enabled. macOS-specific rpath and libpq link options.

### `bench_timing_conformance` — cc_binary / cc_test

```python
cc_binary(name = "bench_timing_conformance", ...)
cc_test(name = "timing_conformance_test", ...)
```

Two forms of the same conformance test — one binary, one `bazel test` target. Uses only `bench_core_common` (FFHR + JSON arms).

⛔ Both currently fail to build — [TASKS.md § PORT-1, PORT-3](../TASKS.md).

### `read_path_bench` — cc_binary

```python
cc_binary(
    name = "read_path_bench",
    srcs = ["read_path_bench.cpp"],
    copts = ["-std=c++20"],
    deps = ["//:fastfhir_runtime"],
)
```

Read-path traversal validation: reports whole-document traversal cost as an
average per Bundle entry and **fails (exit 1) above 50 µs/entry**. Depends only
on `//:fastfhir_runtime`, not on the shared bench headers — which is why it is
**the only target that currently builds**, and the reference for how the
ported arms should call the FastFHIR API. See
[`read_path_bench.cpp.md`](read_path_bench.cpp.md).

```bash
bazel run //bench:read_path_bench -- /path/to/bundle.ffhr
```

### `bench_harness_win` — cc_binary

```python
cc_binary(
    name = "bench_harness_win",
    srcs = ["main.cpp"],
    data = ["enrich.json"],
    deps = [
        ":bench_core_harness",
        "@libpq_win//:libpq",
    ],
    defines = ["HAVE_GOOGLE_FHIR", "HAVE_HL7V2", "HAVE_LIBPQ"],
)
```

Windows build linking a Windows libpq. **All four arms, Google FHIR included** —
it differs from `bench_harness` only in the libpq it links (`@libpq_win//:libpq`
instead of Homebrew's) and in dropping the macOS-specific rpath/link options.

*(An earlier revision of this doc claimed this target excluded Google FHIR.
That was wrong — the four-arm comparison is the point of the benchmark, and
dropping an arm on one platform would make the two platforms
non-comparable.)*

## Platform Preprocessor Defines

| Define | Set By | Effect |
|---|---|---|
| `HAVE_GOOGLE_FHIR` | `bench_harness` defines | Enables Google FHIR arm in `main.cpp` |
| `HAVE_HL7V2` | `bench_harness`, `bench_harness_win` | Enables HL7v2 arm |
| `HAVE_LIBPQ` | `bench_harness`, `bench_harness_win` | Enables PostgreSQL persistence |

## Key Design Decisions

| Decision | Rationale |
|---|---|
| `arm_hl7v2.cpp` in separate target | Enables Windows builds without any HL7v2 dependency issues |
| `bench_core_harness` wraps Google FHIR | Google FHIR is the heaviest dependency; isolating it prevents rebuild cascades |
| Two conformance test targets (binary + test) | Allows both `bazel run` and `bazel test` invocation patterns |
| `bench_harness_win` excludes Google FHIR | Google FHIR's Bazel build is macOS-only in this workspace |
