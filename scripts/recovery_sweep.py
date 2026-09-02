#!/usr/bin/env python3
"""Instrument G test 5 driver: corruption and recovery as INDEPENDENT processes.

For each format x k x trial, `bench_test_5` is invoked as separate subprocesses --
neither the recoverer nor the checker can leak state to the other:

    --hash     clean wire  -> baseline fingerprint file  (the baseline producer)
    --corrupt  clean wire  -> corrupted artifact         (k structural flips)
    --recover  corrupted   -> recovered fingerprint      (corrupted bytes ONLY)
    --check    baseline + recovered fingerprints
               content verification per unit: identity (parent, offset, tag)
               AND a content hash of the unit's data bytes must match; the
               digest each recoverer stamped must match its own units.

Emits results/recovery_curve.csv with columns:
    format,bits_corrupted,positions_total,trial,recovered_pct,
    baseline_count,recovered_count,correct,wrong,missing,spurious

`recovered_pct` is CORRECT/baseline -- the KNOWN DENOMINATOR: the baseline is
the clean stream's own census (captured before any corruption). The check has
FOUR outcomes because "found" and "intact" are different claims:
  correct   unit is there AND carries the data it carried before
  wrong     unit is there and the data CHANGED (the dangerous one -- it still
            parses and reads as valid data that is not what was written)
  missing   in the baseline, not in the recovery -- honest loss
  spurious  in the recovery, not in the baseline -- invention
Each arm hashes its unit's own data bytes (FFHR the referenced block, JSON the
entry span, protobuf the record payload, HL7v2 the segment body AFTER the
header), so a blasted interior pipe makes that segment WRONG, not recovered.

Artifacts come from the harness's --dump-artifacts mode (one representative
bundle's Test-1 wire payload per arm). Structural bits only (per-format syntax
regions). Qualifiers preserved: malformed-not-hostile; integrity-not-authenticity.

Run from the repo root after:
    bazel build -c opt //bench:bench_harness //bench:bench_test_5
    ./bazel-bin/bench/bench_harness --dump-artifacts artifacts
Then: .venv/bin/python scripts/recovery_sweep.py
"""

import pathlib
import re
import subprocess
import sys

DRIVER = pathlib.Path("bazel-bin/bench/bench_test_5")
ARTIFACTS = {
    "fastfhir": "artifacts/fastfhir.bin",
    "json": "artifacts/json.bin",
    "google_fhir": "artifacts/google_fhir.bin",
    "hl7v2": "artifacts/hl7v2.bin",
}
KS = [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
TRIALS = 20


def run(args: list[str]) -> str:
    p = subprocess.run([str(DRIVER)] + args, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"bench_test_5 failed: {' '.join(args)}\n{p.stderr}{p.stdout}")
    return p.stdout.strip()


def field(line: str, key: str) -> str:
    """Pull key=value out of a driver summary line (e.g. 'pct=99.8')."""
    m = re.search(rf"\b{key}=([0-9.]+)", line)
    if not m:
        sys.exit(f"cannot parse '{key}' from driver output: {line!r}")
    return m.group(1)


def main() -> int:
    for artifact in ARTIFACTS.values():
        if not pathlib.Path(artifact).is_file():
            sys.exit(f"missing artifact {artifact} -- run the harness with "
                     "--dump-artifacts artifacts first")
    out = pathlib.Path("results")
    out.mkdir(exist_ok=True)
    csv = out / "recovery_curve.csv"
    with open(csv, "w") as f:
        f.write("format,bits_corrupted,positions_total,trial,recovered_pct,"
                "baseline_count,recovered_count,correct,wrong,missing,spurious\n")
        for fmt, artifact in ARTIFACTS.items():
            baseline = out / f"_baseline_{fmt}.fp"
            run(["--hash", fmt, "--in", artifact, "--out", str(baseline)])
            positions = int(field(run(["--positions", fmt, "--in", artifact]), "positions"))
            print(f"{fmt}: positions={positions}", file=sys.stderr)
            for k in KS:
                for t in range(TRIALS):
                    seed = 20260826 + t * 7 + k
                    damaged = out / f"_corrupt_{fmt}_{k}_{t}.bin"
                    recovered = out / f"_recovered_{fmt}_{k}_{t}.fp"
                    run(["--corrupt", fmt, "--bits", str(k), "--seed", str(seed),
                         "--in", artifact, "--out", str(damaged)])
                    run(["--recover", fmt, "--in", str(damaged), "--out", str(recovered)])
                    line = run(["--check", "--baseline", str(baseline),
                                "--recovered", str(recovered)])
                    pct = field(line, "pct")
                    base_n = field(line, "baseline")
                    rec_n = field(line, "recovered")
                    correct = field(line, "correct")
                    wrong = field(line, "wrong")
                    missing = field(line, "missing")
                    spurious = field(line, "spurious")
                    f.write(f"{fmt},{k},{positions},{t},{pct},{base_n},{rec_n},"
                            f"{correct},{wrong},{missing},{spurious}\n")
                    damaged.unlink(missing_ok=True)
                    recovered.unlink(missing_ok=True)
            baseline.unlink(missing_ok=True)
            print(f"  {fmt}: done", file=sys.stderr)
    print(f"wrote {csv}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
