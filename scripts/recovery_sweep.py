#!/usr/bin/env python3
"""Instrument G test 5 driver: corruption and recovery as INDEPENDENT processes.

For each format x k x trial: invoke `corruption_probe --mode corrupt` (writes a
corrupted artifact) and then `corruption_probe --mode recover` (reads ONLY that
artifact) as separate subprocesses -- neither can leak state to the other, so
recovery is genuinely a scanner's view of the corrupted bytes.

Emits results/recovery_curve.csv with columns:
    format,bits_corrupted,trial,recovered_pct

Artifacts come from the harness's --dump-artifacts mode (one representative
bundle's Test-1 wire payload per arm). Structural bits only (per-format syntax
regions -- the JSON analogue of FFHR's header/block headers). Qualifiers
preserved: malformed-not-hostile; integrity-not-authenticity.

Run from the repo root after:
    ./bazel-bin/bench/bench_harness --dump-artifacts artifacts
    bazel build -c opt //bench:corruption_probe
Then: .venv/bin/python scripts/recovery_sweep.py
"""

import pathlib
import subprocess
import sys

PROBE = pathlib.Path("bazel-bin/bench/corruption_probe")
ARTIFACTS = {
    "fastfhir": "artifacts/fastfhir.bin",
    "json": "artifacts/json.bin",
    "protobuf": "artifacts/google_fhir.bin",
    "hl7v2": "artifacts/hl7v2.bin",
}
KS = [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
TRIALS = 20


def run(args: list[str]) -> str:
    p = subprocess.run([str(PROBE)] + args, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"corruption_probe failed: {' '.join(args)}\n{p.stderr}")
    return p.stdout.strip()


def main() -> int:
    for artifact in ARTIFACTS.values():
        if not pathlib.Path(artifact).is_file():
            sys.exit(f"missing artifact {artifact} -- run the harness with "
                     "--dump-artifacts artifacts first")
    out = pathlib.Path("results")
    out.mkdir(exist_ok=True)
    csv = out / "recovery_curve.csv"
    with open(csv, "w") as f:
        f.write("format,bits_corrupted,trial,recovered_pct\n")
        for fmt, artifact in ARTIFACTS.items():
            count_out = run(["--mode", "count", "--format", fmt, "--in", artifact])
            clean_count = int(count_out.split("=")[-1])
            print(f"{fmt}: clean recoverable units = {clean_count}", file=sys.stderr)
            for k in KS:
                for t in range(TRIALS):
                    seed = 20260826 + t * 7 + k
                    corrupted = out / f"_corrupt_{fmt}_{k}_{t}.bin"
                    run(["--mode", "corrupt", "--format", fmt, "--bits", str(k),
                         "--seed", str(seed), "--in", artifact, "--out", str(corrupted)])
                    rec_out = run(["--mode", "recover", "--format", fmt, "--in", str(corrupted)])
                    recovered = int(rec_out.split("=")[-1])
                    pct = 100.0 * recovered / clean_count if clean_count else 0.0
                    f.write(f"{fmt},{k},{t},{pct:.1f}\n")
                    corrupted.unlink(missing_ok=True)
            print(f"  {fmt}: done", file=sys.stderr)
    print(f"wrote {csv}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
