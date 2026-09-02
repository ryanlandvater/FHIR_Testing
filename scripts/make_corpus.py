#!/usr/bin/env python3
"""Build the test-5 corpus from REAL FHIR, and prove each encoding holds it.

Why this exists
---------------
The arms used to serialize from FastFHIR's own generated `PatientData` /
`ObservationData` structs (`generated_src/FF_DataTypes.hpp`). That is not a
neutral corpus: every competitor inherited FastFHIR's representation choices,
and the JSON arm could not emit valid FHIR at all -- extension URLs are a
uint32 intern index in that model, so it wrote

    {"urlIndex": 16, "valueDecimal": 42.56}

where FHIR requires

    {"url": "http://hl7.org/fhir/StructureDefinition/geolocation", ...}

The four artifacts therefore held four different documents. Measured on the old
corpus, for the same 1,473 resources: json 27,127 leaves, protobuf 19,186,
hl7v2 14,701, fastfhir 10,503 -- and json vs fastfhir shared only 9,020 paths.
A "parity" benchmark over those numbers compares nothing.

The rule here
-------------
ONE canonical document, taken verbatim from a real FHIR source. Every arm
encodes THAT. An encoder that cannot hold it round-trips lossily, and the loss
is a finding to report -- never a smaller denominator to score against.

Usage
-----
    scripts/make_corpus.py <synthea-bundle.json> [--out artifacts]

Synthea R4 sample bundles are what FastFHIR's own suite uses; CMake downloads
them to <fastfhir>/build/synthea_fhir_r4/.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys


def leaves(node, path: str = "", out: dict | None = None) -> dict:
    """Every scalar in the document, keyed by its FHIR element path."""
    if out is None:
        out = {}
    if isinstance(node, dict):
        for k, v in node.items():
            leaves(v, f"{path}.{k}" if path else k, out)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            leaves(v, f"{path}[{i}]", out)
    else:
        out[path] = node
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="a real FHIR Bundle (Synthea R4 sample)")
    ap.add_argument("--out", default="artifacts")
    ap.add_argument("--fastfhir", default="../FastFHIR",
                    help="FastFHIR checkout holding build/ff_ingest and build/ff_export")
    args = ap.parse_args()

    src = pathlib.Path(args.source)
    out = pathlib.Path(args.out)
    out.mkdir(exist_ok=True)
    ff = pathlib.Path(args.fastfhir)
    ingest, export = ff / "build" / "ff_ingest", ff / "build" / "ff_export"
    for tool in (ingest, export):
        if not tool.is_file():
            sys.exit(f"missing {tool} -- build FastFHIR first (cmake --build --preset ninja)")

    canonical = json.loads(src.read_bytes())
    if canonical.get("resourceType") != "Bundle":
        sys.exit(f"{src} is not a FHIR Bundle")
    want = leaves(canonical)
    print(f"canonical FHIR: {src.name}")
    print(f"  {len(canonical['entry'])} entries, {len(want)} leaf values, {src.stat().st_size} bytes")

    # The JSON arm's artifact IS the canonical document. No re-serialization,
    # so there is nothing for an encoder to lose before the comparison starts.
    shutil.copyfile(src, out / "json.bin")

    env = {"DYLD_LIBRARY_PATH": str(ff / "build")}
    ffhr = subprocess.run([str(ingest)], input=src.read_bytes(),
                          capture_output=True, env={**dict(**env)})
    if ffhr.returncode != 0 or not ffhr.stdout:
        sys.exit(f"ff_ingest failed:\n{ffhr.stderr.decode(errors='replace')}")
    (out / "fastfhir.bin").write_bytes(ffhr.stdout)

    back = subprocess.run([str(export)], input=ffhr.stdout,
                          capture_output=True, env={**dict(**env)})
    if back.returncode != 0 or not back.stdout:
        sys.exit(f"ff_export failed:\n{back.stderr.decode(errors='replace')}")
    got = leaves(json.loads(back.stdout))

    shared = want.keys() & got.keys()
    lost = sorted(want.keys() - got.keys())
    added = sorted(got.keys() - want.keys())
    changed = [k for k in shared if want[k] != got[k]]

    print(f"\nfastfhir.bin: {len(ffhr.stdout)} bytes")
    print(f"  round-trip: {len(shared)}/{len(want)} leaves held, "
          f"{len(lost)} lost, {len(added)} added, {len(changed)} changed")
    for k in lost[:5]:
        print(f"    LOST    {k}")
    for k in changed[:5]:
        print(f"    CHANGED {k}: {want[k]!r} -> {got[k]!r}")

    # A CHANGED value is a silent corruption and is never acceptable. A LOST
    # one is a coverage gap: report it, let the caller judge, do not hide it.
    if changed:
        sys.exit("FAIL: the encoder altered values -- the corpus is not sound")
    print("\nwrote artifacts/json.bin (canonical FHIR) and artifacts/fastfhir.bin")
    if lost:
        print(f"NOTE: {len(lost)} leaves do not survive the FastFHIR round-trip "
              f"({100*len(lost)/len(want):.3f}%). They are a real encoder gap, not "
              f"a licence to shrink the denominator: the comparison still scores "
              f"against all {len(want)} canonical leaves.")
    print("\nSTILL TO DO: hl7v2.bin and google_fhir.bin are produced by the old "
          "harness path, which serializes from FastFHIR's generated structs. "
          "They must be re-pointed at this canonical document before any "
          "cross-arm number means anything.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
