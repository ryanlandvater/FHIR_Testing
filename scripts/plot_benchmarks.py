#!/usr/bin/env python3
"""Graphical representations of the four-arm benchmark (TASKS.md IN-0).

Reads the harness CSV (or PostgreSQL) plus the `provenance.json` written by
`--results-dir`, and renders the figures. Two rules govern everything here:

1.  **A figure carries its provenance or it is not a figure.** The compiled
    profile and the compilation mode change these numbers by ~10x and are
    invisible in them. Every chart is stamped, and a run whose provenance gate
    failed is stamped PROVISIONAL — NOT AN ARTIFACT, in the corner, on purpose.
    A chart is the easiest thing in this repo to screenshot into a slide, so it
    is the place where the citation rules most need enforcing.

2.  **Known-unfair comparisons say so on the chart.** Test 1 is not at parity
    (TASKS.md PA-1), `value[x]` is excluded from every arm (D2), Test 3 charges
    the FastFHIR arm a `print_json` penalty (PA-7). Those captions are attached
    to the figures they apply to, not left to a reader who has not read TASKS.md.

Usage
-----
    # from a captured CSV
    ./bazel-bin/bench/bench_harness --runs 3 --results-dir results/dev > results/dev/metrics.csv
    python3 scripts/plot_benchmarks.py --csv results/dev/metrics.csv --results-dir results/dev

    # or straight from PostgreSQL (needs the //bench:bench_harness_pg build)
    python3 scripts/plot_benchmarks.py --db "host=localhost dbname=benchmark user=bench password=bench"
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.lines import Line2D

# ---------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------
# Categorical slots 1-4 of the reference palette, validated for this exact set:
# lightness band PASS, chroma floor PASS, worst adjacent CVD dE 9.1 (protan),
# worst adjacent normal-vision dE 22.9. Aqua and yellow fall below 3:1 contrast
# on the light surface, so the relief rule applies -- hence direct labels on
# every series AND the table view written alongside the figures.
#
# Colour follows the ARM, never its rank: a chart with three arms must not
# repaint the survivors, and fastfhir is blue in every figure ever produced.
ARM_COLOR = {
    "fastfhir": "#2a78d6",        # slot 1, blue
    "fastfhir_compact": "#8fb8ea",  # the same library, compact stream (IN-E gate)
    "json_fhir": "#eb6834",    # slot 2, orange
    "hl7v2": "#1baf7a",        # slot 3, aqua
    "google_fhir": "#eda100",  # slot 4, yellow
}
ARM_LABEL = {
    "fastfhir": "FastFHIR",
    "fastfhir_compact": "FastFHIR compact",
    "json_fhir": "nlohmann + simdjson",
    "hl7v2": "HL7v2",
    "google_fhir": "Google protobuf",
}
OTHER_COLOR = "#8a8a85"

SURFACE = "#fcfcfb"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
TEXT_MUTED = "#8a8a85"
GRID = "#e6e6e3"
WARN = "#c1462f"

STAGES = [
    ("test_1_serialize", "Test 1 — serialize"),
    ("test_2_random_access", "Test 2 — random access"),
    ("test_3_query", "Test 3 — query"),
    ("test_4_enrich", "Test 4 — enrich"),
]

# Caveats that belong ON the figure, keyed to the task that would retire them.
CAVEAT_PARITY = (
    "NOT AT PARITY (PA-1): the FastFHIR arm serializes every POCO field via append_obj; "
    "the other three write the ~25 fields the macro assignment layer covers."
)
CAVEAT_CHOICE = "value[x] is excluded from every arm (D2 / upstream CAPI-3): 95.1% of the corpus's choice values."
CAVEAT_QUERY = (
    "Test 3: the FastFHIR arm reads via node lenses (no whole-POCO materialization, 2026-08-26); "
    "the print_json penalty for packed date/time remains only on Patient.birthDate (PA-7 / CAPI-4)."
)
CAVEAT_RA_GRANULARITY = (
    "Test 2 (random access) is not the same ordinal space in every arm: HL7v2 addresses "
    "MESSAGES (5) where the other arms address resources (1,473). A format finding, not a probe defect."
)
CAVEAT_ENRICH = (
    "Not the same operation in every arm (PA-9): FastFHIR::Memory is a shared_ptr handle, so the "
    "FastFHIR arm appends in place while the others build a separate buffer."
)


def apply_style() -> None:
    plt.rcParams.update(
        {
            "figure.facecolor": SURFACE,
            "axes.facecolor": SURFACE,
            "savefig.facecolor": SURFACE,
            "axes.edgecolor": GRID,
            "axes.linewidth": 0.8,
            "axes.labelcolor": TEXT_SECONDARY,
            "axes.titlecolor": TEXT_PRIMARY,
            "axes.grid": True,
            "axes.axisbelow": True,
            "grid.color": GRID,
            "grid.linewidth": 0.8,
            "grid.linestyle": "-",  # solid hairline; dashed grids read as thresholds
            "xtick.color": TEXT_MUTED,
            "ytick.color": TEXT_MUTED,
            "xtick.labelcolor": TEXT_SECONDARY,
            "ytick.labelcolor": TEXT_SECONDARY,
            "text.color": TEXT_PRIMARY,
            "font.family": "sans-serif",
            "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
            "font.size": 9,
            "axes.titlesize": 10,
            "axes.titleweight": "bold",
            "legend.frameon": False,
            "legend.fontsize": 8.5,
            "figure.dpi": 140,
        }
    )


def arm_color(arm: str) -> str:
    return ARM_COLOR.get(arm, OTHER_COLOR)


def arm_label(arm: str) -> str:
    return ARM_LABEL.get(arm, arm)


# Compact-archive rows (FF arm only, IN-E losslessness gate) are the same
# operations as their standard counterparts, so the figures render them as a
# second series of the same library: arm = fastfhir_compact, test = the
# standard stage name. The source CSV keeps the raw test_*_compact names.
# test_1_compact is intentionally NOT mapped: its duration is the archive
# transform (a write), not a serialize variant -- it lives on fig2/fig7.
COMPACT_STAGE_MAP = {
    "test_2_compact": "test_2_random_access",
    "test_3_compact": "test_3_query",
}


def with_compact_arm(df: pd.DataFrame) -> pd.DataFrame:
    out = df.copy()
    mask = out["test"].isin(COMPACT_STAGE_MAP)
    out.loc[mask, "arm"] = "fastfhir_compact"
    out.loc[mask, "test"] = out.loc[mask, "test"].map(COMPACT_STAGE_MAP)
    return out


def ordered_arms(df: pd.DataFrame) -> list[str]:
    """Fixed order, never data-dependent, so colour never shifts between runs."""
    known = [a for a in ARM_COLOR if a in set(df["arm"])]
    unknown = sorted(set(df["arm"]) - set(ARM_COLOR))
    return known + unknown


# ---------------------------------------------------------------------------
# Provenance
# ---------------------------------------------------------------------------


@dataclass
class Provenance:
    data: dict
    path: Optional[Path]

    @property
    def is_artifact(self) -> bool:
        """Mirrors missing_fields() in bench/provenance.hpp.

        Deliberately re-implemented rather than trusted: a figure is generated
        long after the run, often from a CSV someone copied, and the gate has to
        hold at render time too.
        """
        if not self.data:
            return False
        d = self.data
        required = [
            "fastfhir_sha", "fastfhir_tag", "production_profile", "compilation_mode",
            "compiler", "compiler_version", "os", "arch", "cpu_model",
            "corpus_id", "corpus_sha256", "benchmark_sha",
        ]
        if any(not d.get(k) for k in required):
            return False
        if d.get("production_profile_ambiguous"):
            return False
        if d.get("compilation_mode") != "opt":
            return False
        if not d.get("seed"):
            return False
        # "if the tree had uncommitted changes, the artifact is not
        # reproducible" -- handoff.md. Development runs are provisional by
        # construction, which is correct rather than inconvenient.
        if d.get("fastfhir_dirty") or d.get("benchmark_dirty"):
            return False
        if len(str(d.get("corpus_sha256", ""))) != 64:
            return False
        if not (d.get("codesystem_enums", 0) > 0 and d.get("generated_cpp", 0) > 0):
            return False
        return True

    @property
    def why_not_artifact(self) -> list[str]:
        if not self.data:
            return ["no provenance.json found — run the harness with --results-dir"]
        d, reasons = self.data, []
        if d.get("compilation_mode") != "opt":
            reasons.append(f"compilation_mode is {d.get('compilation_mode')!r}, not 'opt'")
        if d.get("production_profile_ambiguous"):
            reasons.append("production_profile is ambiguous — pin it with --profile")
        if not d.get("seed"):
            reasons.append("seed 0 (random) is not reproducible")
        if d.get("fastfhir_dirty"):
            reasons.append("FastFHIR tree was dirty")
        if d.get("benchmark_dirty"):
            reasons.append("benchmark tree was dirty")
        return reasons or ["one or more required provenance fields is unestablished"]

    def stamp(self) -> str:
        d = self.data
        if not d:
            return "no provenance — these numbers cannot be cited"
        return (
            f"FastFHIR {d.get('fastfhir_tag', '?')}"
            f"{' (dirty)' if d.get('fastfhir_dirty') else ''}"
            f"  ·  profile {d.get('production_profile', '?')}"
            f"  ·  {d.get('compilation_mode', '?')} {d.get('compiler', '')}{d.get('compiler_version', '')}"
            f"  ·  {d.get('os', '?')}/{d.get('arch', '?')} {d.get('cpu_model', '')}"
            f"  ·  corpus {d.get('corpus_doc_count', '?')} docs sha {str(d.get('corpus_sha256', '?'))[:12]}"
            f"  ·  seed {d.get('seed', '?')}"
        )


def load_provenance(results_dir: Optional[Path], csv_path: Optional[Path]) -> Provenance:
    for candidate in [
        results_dir / "provenance.json" if results_dir else None,
        csv_path.parent / "provenance.json" if csv_path else None,
    ]:
        if candidate and candidate.is_file():
            return Provenance(json.loads(candidate.read_text()), candidate)
    return Provenance({}, None)


# ---------------------------------------------------------------------------
# Figure furniture
# ---------------------------------------------------------------------------


def finish(fig, prov: Provenance, caveats: list[str]) -> None:
    """Stamp every figure with provenance, caveats, and artifact status.

    This is the enforcement point. A chart leaves here able to say what built it
    and what is wrong with it, or it does not leave.
    """
    lines = [c for c in caveats if c]
    y = 0.055 + 0.016 * len(lines)
    fig.text(0.008, y, prov.stamp(), fontsize=6.4, color=TEXT_MUTED, ha="left", va="bottom")
    for i, caveat in enumerate(lines):
        fig.text(
            0.008,
            y - 0.017 * (i + 1),
            f"!  {caveat}",
            fontsize=6.4,
            color=WARN,
            ha="left",
            va="bottom",
        )

    if not prov.is_artifact:
        fig.text(
            0.992,
            0.988,
            "PROVISIONAL — NOT AN ARTIFACT",
            fontsize=7.5,
            color=WARN,
            weight="bold",
            ha="right",
            va="top",
        )
        fig.text(
            0.992,
            0.966,
            "; ".join(prov.why_not_artifact)[:150],
            fontsize=6,
            color=TEXT_MUTED,
            ha="right",
            va="top",
        )


def label_endpoints(ax, entries: list[tuple[float, float, str]]) -> None:
    """Direct labels at the line endpoints — never a number on every point.

    Also the relief for the two palette slots below 3:1 contrast on the light
    surface: identity never rests on colour alone. Labels are de-collided in
    display space, because where series converge (Test 3 does, hard) four labels
    land on one pixel row and the panel stops being readable.
    """
    if not entries:
        return
    ax.figure.canvas.draw()  # transforms must be current to measure anything

    rows = []
    for x, y, arm in entries:
        _, py = ax.transData.transform((x, y))
        rows.append([py, x, y, arm])
    rows.sort(key=lambda r: r[0])

    gap_px = 11.5 * ax.figure.dpi / 72.0  # ~7pt text plus leading
    for i in range(1, len(rows)):
        if rows[i][0] - rows[i - 1][0] < gap_px:
            rows[i][0] = rows[i - 1][0] + gap_px

    for target_py, x, y, arm in rows:
        _, anchor_py = ax.transData.transform((x, y))
        dy_points = (target_py - anchor_py) * 72.0 / ax.figure.dpi
        ax.annotate(
            arm_label(arm),
            xy=(x, y),
            xytext=(6, dy_points),
            textcoords="offset points",
            fontsize=7,
            color=TEXT_SECONDARY,
            va="center",
            ha="left",
            annotation_clip=False,
        )


def legend_for(fig, arms: list[str], ncol: int = 4) -> None:
    handles = [
        Line2D([], [], color=arm_color(a), lw=2, marker="o", ms=5, label=arm_label(a))
        for a in arms
    ]
    fig.legend(
        handles=handles,
        loc="lower center",
        ncol=ncol,
        bbox_to_anchor=(0.5, 0.0),
        labelcolor=TEXT_SECONDARY,
    )


def fmt_bytes(v: float) -> str:
    for unit, scale in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if v >= scale:
            return f"{v / scale:.1f} {unit}"
    return f"{v:.0f} B"


# ---------------------------------------------------------------------------
# Figures
# ---------------------------------------------------------------------------


def bundle_size_key(df: pd.DataFrame) -> pd.Series:
    """A common x for every arm: the JSON arm's wire size for that bundle.

    Each arm produces a different number of bytes for the same clinical content,
    so an arm's own size cannot be the shared axis -- the four lines would not be
    over the same bundles. The JSON arm is the neutral ruler here (it is the
    baseline everywhere else in this repo), keyed by target_mb, which does still
    identify *which* bundle was built even where it fails to control its size.
    """
    ruler = (
        df[(df["test"] == "test_1_serialize") & (df["arm"] == "json_fhir") & (df["bytes_out"] > 0)]
        .groupby("target_mb")["bytes_out"]
        .median()
    )
    if ruler.empty:  # pre-IN-0 data: fall back to the requested size
        return df["target_mb"] * (1 << 20)
    return df["target_mb"].map(ruler).fillna(df["target_mb"] * (1 << 20))


def fig_duration_by_stage(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """Small multiples: duration vs the bundle's ACTUAL wire size, per stage.

    The x-axis is measured bytes, not `target_mb`, and that is not a stylistic
    choice. `target_mb` is a request, and the fixture stops accumulating as soon
    as one patient exceeds it -- each Synthea patient is ~3 MB ingested, so every
    target below ~4 MB yields a bundle of exactly one randomly chosen patient. In
    this run the 1 MB target produced 273 KB of FastFHIR wire and the 2 MB target
    produced 130 KB: the smaller request produced the larger bundle. Plotting
    against target_mb below ~4 MB plots against noise (TASKS.md CO-4).

    Four stages on one linear axis would span four orders of magnitude; small
    multiples keep each stage readable and never invite a dual axis.
    """
    df = with_compact_arm(df)
    arms = ordered_arms(df)
    size_key = bundle_size_key(df)

    fig, axes = plt.subplots(2, 2, figsize=(10.5, 7.4), sharex=True)
    fig.suptitle("Stage duration vs actual bundle wire size", fontsize=12, y=0.985,
                 x=0.008, ha="left", color=TEXT_PRIMARY)

    for ax, (stage, title) in zip(axes.flat, STAGES):
        sub = df[df["test"] == stage]
        entries = []
        for arm in arms:
            a = sub[sub["arm"] == arm]
            if a.empty:
                continue
            g = a.groupby(size_key).agg(dur=("duration_ns", "median")).sort_index()
            xs = g.index.to_numpy() / 1024.0
            ys = g["dur"].to_numpy() / 1000.0
            ax.plot(xs, ys, color=arm_color(arm), lw=2, marker="o", ms=5,
                    mec=SURFACE, mew=1.2)
            entries.append((xs[-1], ys[-1], arm))
        ax.set_title(title, loc="left", pad=8)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_ylabel("median duration (µs)")
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
        label_endpoints(ax, entries)

    for ax in axes[1]:
        ax.set_xlabel("bundle wire size, JSON arm (KiB)")

    fig.tight_layout(rect=[0, 0.14, 1, 0.96])
    legend_for(fig, arms)
    finish(fig, prov, [CAVEAT_PARITY, CAVEAT_RA_GRANULARITY, CAVEAT_QUERY, CAVEAT_CHOICE])
    save(fig, out / "fig1_duration_by_stage", exts)


def fig_wire_size(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """Wire bytes produced at Test 1 per arm, plus the FastFHIR compact archive.

    The compact series (dashed) is FastFHIR's archive-mode size, emitted by the
    harness only when the compact stream re-parses to content identical to the
    standard stream (IN-E losslessness gate, handoff.md). The remaining IN-E
    rows (gzip(JSON), protobuf, gzip(protobuf)) are still on the table, not
    this figure.
    """
    arms = ordered_arms(df)
    t1 = df[(df["test"] == "test_1_serialize") & (df["bytes_out"] > 0)]
    if t1.empty:
        print("  skip fig2: no test_1_serialize rows with bytes_out")
        return
    compact = df[(df["test"] == "test_1_compact") & (df["bytes_out"] > 0)]
    if compact.empty:
        print("  note: no test_1_compact rows — compact archive not measured (IN-E gate)")

    fig, (ax_abs, ax_ratio) = plt.subplots(1, 2, figsize=(10.5, 4.6))
    fig.suptitle("Wire size produced (Test 1 — serialize)", fontsize=12, y=0.985,
                 x=0.008, ha="left", color=TEXT_PRIMARY)

    abs_entries: list[tuple[float, float, str]] = []
    for arm in arms:
        s = t1[t1["arm"] == arm].groupby("target_mb")["bytes_out"].median().sort_index()
        if s.empty:
            continue
        ax_abs.plot(s.index.to_numpy(), s.to_numpy() / (1 << 20), color=arm_color(arm),
                    lw=2, marker="o", ms=5, mec=SURFACE, mew=1.2)
        abs_entries.append((s.index.to_numpy()[-1], s.to_numpy()[-1] / (1 << 20), arm))
    if not compact.empty:
        c = compact.groupby("target_mb")["bytes_out"].median().sort_index()
        ax_abs.plot(c.index.to_numpy(), c.to_numpy() / (1 << 20), color=arm_color("fastfhir"),
                    lw=2, ls="--", marker="s", ms=5, mec=SURFACE, mew=1.2)
        abs_entries.append((c.index.to_numpy()[-1], c.to_numpy()[-1] / (1 << 20),
                            "fastfhir compact"))
    ax_abs.set_title("Absolute", loc="left", pad=8)
    ax_abs.set_xscale("log", base=2)
    ax_abs.set_xlabel("target bundle size (MB)")
    ax_abs.set_ylabel("wire bytes out (MiB)")
    label_endpoints(ax_abs, abs_entries)

    ratio_entries: list[tuple[float, float, str]] = []
    baseline = t1[t1["arm"] == "json_fhir"].groupby("target_mb")["bytes_out"].median()
    if not baseline.empty:
        for arm in arms:
            s = t1[t1["arm"] == arm].groupby("target_mb")["bytes_out"].median()
            common = s.index.intersection(baseline.index)
            if len(common) == 0:
                continue
            ratio = (s.loc[common] / baseline.loc[common]).sort_index()
            ax_ratio.plot(ratio.index.to_numpy(), ratio.to_numpy(), color=arm_color(arm),
                          lw=2, marker="o", ms=5, mec=SURFACE, mew=1.2)
            ratio_entries.append((ratio.index.to_numpy()[-1], ratio.to_numpy()[-1], arm))
        if not compact.empty:
            c = compact.groupby("target_mb")["bytes_out"].median()
            common = c.index.intersection(baseline.index)
            if len(common) > 0:
                ratio = (c.loc[common] / baseline.loc[common]).sort_index()
                ax_ratio.plot(ratio.index.to_numpy(), ratio.to_numpy(),
                              color=arm_color("fastfhir"), lw=2, ls="--", marker="s", ms=5,
                              mec=SURFACE, mew=1.2)
                ratio_entries.append((ratio.index.to_numpy()[-1], ratio.to_numpy()[-1],
                                      "fastfhir compact"))
        ax_ratio.axhline(1.0, color=TEXT_MUTED, lw=0.8)
        ax_ratio.annotate("JSON baseline = 1.0", xy=(0.02, 1.0), xycoords=("axes fraction", "data"),
                          xytext=(0, 4), textcoords="offset points", fontsize=6.8, color=TEXT_MUTED)
    ax_ratio.set_title("Relative to the JSON arm", loc="left", pad=8)
    ax_ratio.set_xscale("log", base=2)
    ax_ratio.set_xlabel("target bundle size (MB)")
    ax_ratio.set_ylabel("× JSON wire bytes")
    label_endpoints(ax_ratio, ratio_entries)

    for ax in (ax_abs, ax_ratio):
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)

    # Arms plus the compact proxy — the compact series is a FastFHIR-native
    # variant, not an arm, and a dashed line has to say what it is.
    handles = [Line2D([], [], color=arm_color(a), lw=2, marker="o", ms=5, label=arm_label(a))
               for a in arms]
    if not compact.empty:
        handles.append(Line2D([], [], color=arm_color("fastfhir"), lw=2, ls="--", marker="s",
                              ms=5, label="fastfhir (compact archive)"))
    fig.legend(handles=handles, loc="lower center", ncol=5, bbox_to_anchor=(0.5, 0.0),
               labelcolor=TEXT_SECONDARY)

    fig.tight_layout(rect=[0, 0.16, 1, 0.95])
    finish(fig, prov, [CAVEAT_PARITY, CAVEAT_CHOICE,
                       "FastFHIR compact (dashed) is emitted only when the harness verified the "
                       "compact stream re-parses to content identical to the standard stream "
                       "(IN-E losslessness gate)."])
    save(fig, out / "fig2_wire_size", exts)


def fig_enrich_delta(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """Bytes added by appending one Observation — the WF-4.1 claim, measured.

    A bar chart because the story is a comparison of four magnitudes at one
    corpus size, not a trend.
    """
    t4 = df[(df["test"] == "test_4_enrich") & (df["bytes_out"] > 0)].copy()
    if t4.empty:
        print("  skip fig4: no test_4_enrich rows with bytes")
        return
    t4["delta"] = t4["bytes_out"] - t4["bytes_in"]
    target = sorted(t4["target_mb"].unique())[-1]
    sub = t4[t4["target_mb"] == target]
    arms = [a for a in ordered_arms(df) if a in set(sub["arm"])]
    deltas = [sub[sub["arm"] == a]["delta"].median() for a in arms]

    fig, ax = plt.subplots(figsize=(8.4, 4.4))
    fig.suptitle(f"Bytes added by appending one Observation ({target} MB target bundle)",
                 fontsize=12, y=0.985, x=0.008, ha="left", color=TEXT_PRIMARY)
    ypos = np.arange(len(arms))
    ax.barh(ypos, deltas, height=0.45, color=[arm_color(a) for a in arms],
            edgecolor=SURFACE, linewidth=2)  # 2px surface gap, not a border
    ax.set_yticks(ypos, [arm_label(a) for a in arms])
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.set_xlabel("bytes added (log scale)")
    ax.grid(axis="y", visible=False)
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    for y, d in zip(ypos, deltas):
        # Outside the bar end: a label inside a short log-scaled bar would clip.
        ax.annotate(fmt_bytes(d), xy=(d, y), xytext=(6, 0), textcoords="offset points",
                    va="center", fontsize=8, color=TEXT_SECONDARY)
    ax.margins(x=0.18)

    fig.tight_layout(rect=[0, 0.15, 1, 0.95])
    finish(fig, prov, [
        CAVEAT_ENRICH,
        "PA-10: the FastFHIR arm appends a whole new root Bundle block, not just the observation — "
        "the opposite of the 'append without touching any other byte' claim (WF-4.1) it should demonstrate.",
    ])
    save(fig, out / "fig4_enrich_delta", exts)


def fig_distribution(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """Violins: the full run-to-run distribution per arm, grouped by bundle size.

    Restored from the pre-IN-0 notebook, which had this and was right to. A
    median hides exactly what a benchmark most needs to show -- whether the
    spread is tight enough for the difference between two arms to mean anything.
    Log y, because four arms span three orders of magnitude on some stages.
    """
    df = with_compact_arm(df)
    arms = ordered_arms(df)
    targets = sorted(df["target_mb"].unique())
    fig, axes = plt.subplots(2, 2, figsize=(11.5, 7.6))
    fig.suptitle("Duration distribution across runs", fontsize=12, y=0.985, x=0.008,
                 ha="left", color=TEXT_PRIMARY)

    width = 0.8 / max(len(arms), 1)
    for ax, (stage, title) in zip(axes.flat, STAGES):
        sub = df[df["test"] == stage]
        for ai, arm in enumerate(arms):
            offset = (ai - (len(arms) - 1) / 2) * width
            data, pos = [], []
            for ti, mb in enumerate(targets):
                vals = sub[(sub["arm"] == arm) & (sub["target_mb"] == mb)]["duration_ns"]
                vals = vals.dropna().to_numpy() / 1000.0
                if len(vals) >= 2:
                    data.append(vals)
                    pos.append(ti + 1 + offset)
            if not data:
                continue
            parts = ax.violinplot(data, positions=pos, widths=width * 0.85,
                                  showmedians=True, showextrema=False)
            for body in parts["bodies"]:
                body.set_facecolor(arm_color(arm))
                body.set_edgecolor(SURFACE)   # 2px surface gap, not a border
                body.set_linewidth(1.2)
                body.set_alpha(0.62)
            parts["cmedians"].set_color(arm_color(arm))
            parts["cmedians"].set_linewidth(2)

        ax.set_title(title, loc="left", pad=8)
        ax.set_yscale("log")
        ax.set_ylabel("duration (µs)")
        ax.set_xticks(range(1, len(targets) + 1), [f"{mb} MB" for mb in targets])
        ax.set_xlim(0.5, len(targets) + 0.5)
        ax.grid(axis="x", visible=False)
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)

    for ax in axes[1]:
        ax.set_xlabel("target bundle size — see PA-12: this controls nothing below ~4 MB")

    fig.tight_layout(rect=[0, 0.14, 1, 0.96])
    legend_for(fig, arms)
    finish(fig, prov, [CAVEAT_PARITY, CAVEAT_CHOICE,
                       "Violin width is a kernel density estimate over few samples — read the spread, not its shape."])
    save(fig, out / "fig5_distribution", exts)


def fig_speedup(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """How many times faster FastFHIR is than each alternative, per stage.

    Ratios of medians. Values below 1.0 mean FastFHIR is SLOWER, and those bars
    are drawn and labelled like any other -- a speedup chart that only shows
    wins is advertising. Test 2 (random access) is FastFHIR's strongest stage;
    the one-time inversion vs simdjson belonged to the retired materialize walk
    (measured cause on record, TASKS.md PA-11 / D4).
    """
    df = with_compact_arm(df)
    med = df.groupby(["test", "arm"])["duration_ns"].median().unstack()
    if "fastfhir" not in med.columns:
        print("  skip fig6: no fastfhir rows")
        return
    others = [a for a in ordered_arms(df) if a != "fastfhir" and a in med.columns]
    stages = [(k, t) for k, t in STAGES if k in med.index]

    fig, ax = plt.subplots(figsize=(10, 5.0))
    fig.suptitle("FastFHIR speedup vs each alternative (median duration ratio)",
                 fontsize=12, y=0.985, x=0.008, ha="left", color=TEXT_PRIMARY)

    # Lollipops anchored at 1.0, not bars from the axis floor: on a log scale a
    # bar's length is arbitrary (there is no zero), and the quantity that means
    # something here is distance from parity. It also frees the region below 1.0
    # so the three sub-parity results are visible instead of buried under bars
    # that happen to pass through them.
    width = 0.8 / max(len(others), 1)
    xs = np.arange(len(stages))
    for ai, arm in enumerate(others):
        offset = (ai - (len(others) - 1) / 2) * width
        vals = [med.loc[k, arm] / med.loc[k, "fastfhir"] for k, _ in stages]
        for x, v in zip(xs + offset, vals):
            ax.plot([x, x], [1.0, v], color=arm_color(arm), lw=2, solid_capstyle="round",
                    zorder=2)
            ax.plot([x], [v], marker="o", ms=7, color=arm_color(arm), mec=SURFACE, mew=1.6,
                    zorder=3)
            ax.annotate(f"{v:.2f}×", xy=(x, v), xytext=(0, 8 if v >= 1 else -13),
                        textcoords="offset points", ha="center", fontsize=7.5,
                        color=(TEXT_SECONDARY if v >= 1 else WARN), zorder=4)

    ax.axhline(1.0, color=TEXT_MUTED, lw=1.0, zorder=1)
    ax.annotate("1.0× parity — below this line FastFHIR is slower",
                xy=(0.012, 1.0), xycoords=("axes fraction", "data"),
                xytext=(0, -9), textcoords="offset points", ha="left", fontsize=7,
                color=TEXT_MUTED)
    ax.set_yscale("log")
    ax.set_xticks(xs, [t for _, t in stages])
    ax.set_ylabel("× slower than FastFHIR (log, 1.0 = parity)")
    ax.margins(y=0.22)
    ax.grid(axis="x", visible=False)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)

    fig.tight_layout(rect=[0, 0.17, 1, 0.96])
    legend_for(fig, others, ncol=3)
    finish(fig, prov, [
        CAVEAT_PARITY,
        "Test 1 understates the gap: the FastFHIR arm is also writing ~2.2x MORE wire bytes than the JSON arm (PA-1).",
        "Test 2 (random access) is FastFHIR's strongest stage — up to ~856x at 36 MiB (TASKS.md IN-B2). "
        "The former materialize walk, which simdjson won, was retired with its cause on record (PA-11 / D4).",
    ])
    save(fig, out / "fig6_speedup", exts)


def fig_random_access(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """Test 2: cost of ONE field read, navigating from the root, out of order.

    The retired materialize walk read in layout order -- a contiguous tape's best
    case -- and this reads in the order a query does; the two disagree by three
    orders of magnitude. Publishing either alone misrepresents the result.
    """
    df = with_compact_arm(df)
    arms = ordered_arms(df)
    t2 = df[(df["test"] == "test_2_random_access") & (df["ops"] > 0)].copy()
    if t2.empty:
        print("  skip fig3: no test_2_random_access rows (pre-D4 data)")
        return
    t2["ns_per_read"] = t2["duration_ns"] / t2["ops"]
    size_key = bundle_size_key(df)
    t2["bundle_bytes"] = t2["target_mb"].map(
        df.assign(k=size_key).groupby("target_mb")["k"].median())

    fig, ax = plt.subplots(figsize=(9.5, 5.4))
    fig.suptitle("Cost of one random field read (Test 2 — navigate from the root)",
                 fontsize=12, y=0.985, x=0.008, ha="left", color=TEXT_PRIMARY)
    entries = []
    for arm in arms:
        g = t2[t2["arm"] == arm].groupby("bundle_bytes")["ns_per_read"].median().sort_index()
        if g.empty:
            continue
        xs = g.index.to_numpy() / 1024.0
        ys = g.to_numpy()
        ax.plot(xs, ys, color=arm_color(arm), lw=2, marker="o", ms=5, mec=SURFACE, mew=1.2)
        entries.append((xs[-1], ys[-1], arm))
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("bundle wire size, JSON arm (KiB)")
    ax.set_ylabel("ns per field read (lower is better)")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    label_endpoints(ax, entries)

    fig.tight_layout(rect=[0, 0.19, 1, 0.95])
    legend_for(fig, arms)
    finish(fig, prov, [
        "Every arm read the SAME fields — the harness fails the run if the byte totals disagree.",
        "The three scan formats have no O(1) index into a serialized document: simdjson's at(i), "
        "protobuf's TLV walk and HL7v2's MSH scan are all O(i). That is the property under test.",
        "HL7v2 is NOT the same operation: a v2 batch has no resource-level index, so it addresses "
        "MESSAGES (5) where the others address resources (1,473). A format finding, not a probe defect.",
        "Counterpoint not yet measured: a consumer wanting many random reads would build an index "
        "once and amortize it. Estimated crossover ~50k reads — do not quote it until IN-B3 measures it.",
    ])
    save(fig, out / "fig3_random_access", exts)


def fig_recovery(prov: Provenance, out: Path, exts: list[str]) -> None:
    """Instrument G test 5: % recoverable vs structural bits corrupted, per format.

    Data source: results/recovery_curve.csv, emitted by scripts/recovery_sweep.py
    (corruption and recovery run as INDEPENDENT subprocesses per sample; the
    recoverer sees only the corrupted bytes). Recoverable units differ by
    format -- entries for FFHR/JSON/protobuf, SEGMENTS for HL7v2 (a v2 batch
    has no resource-level index): a granularity finding, stated on the figure.
    Structural flips only (syntax regions); payload bytes untouched.
    Qualifiers: malformed-not-hostile; integrity-not-authenticity.
    """
    csv_path = Path("results/recovery_curve.csv")
    if not csv_path.is_file():
        print("  skip fig8: no results/recovery_curve.csv — run "
              "scripts/recovery_sweep.py from the repo root")
        return
    rc = pd.read_csv(csv_path)
    labels = {
        "fastfhir": "FastFHIR (entries)",
        "json": "JSON (entries)",
        "protobuf": "protobuf TLV (entries)",
        "hl7v2": "HL7v2 (segments)",
    }

    fig, ax = plt.subplots(figsize=(9.5, 5.2))
    fig.suptitle("Recoverability under structural corruption (Instrument G, test 5)",
                 fontsize=12, y=0.985, x=0.008, ha="left", color=TEXT_PRIMARY)
    for i, fmt in enumerate(labels):
        sub = rc[rc["format"] == fmt]
        if sub.empty:
            continue
        med = sub.groupby("bits_corrupted")["recovered_pct"].median()
        lo = sub.groupby("bits_corrupted")["recovered_pct"].min()
        hi = sub.groupby("bits_corrupted")["recovered_pct"].max()
        color = arm_color(["fastfhir", "json_fhir", "google_fhir", "hl7v2"][i])
        ax.fill_between(med.index.to_numpy(), lo.to_numpy(), hi.to_numpy(),
                        color=color, alpha=0.14, lw=0)
        ax.plot(med.index.to_numpy(), med.to_numpy(), color=color, lw=2, marker="o",
                ms=4, mec=SURFACE, mew=1.2, label=labels[fmt])
    ax.axhline(100.0, color=TEXT_MUTED, lw=0.8)
    ax.annotate("100% = fully recoverable", xy=(0.02, 100.0), xycoords=("axes fraction", "data"),
                xytext=(0, 4), textcoords="offset points", fontsize=6.8, color=TEXT_MUTED)
    ax.set_xscale("log", base=2)
    ax.set_xlabel("structural bits corrupted (per-format syntax regions)")
    ax.set_ylabel("recoverable units (%)")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.legend(loc="lower left", ncol=1, frameon=False, labelcolor=TEXT_SECONDARY, fontsize=8)

    fig.tight_layout(rect=[0, 0.18, 1, 0.95])
    finish(fig, prov, [
        "METHODOLOGICALLY SUSPECT (2026-08-26): HL7v2 counts SEGMENTS, the others count "
        "ENTRIES; k is not normalized per format; the HL7v2 recover checks only segment names, "
        "not content, and its structural set excludes pipes/carrots. Do not cite — fix per "
        "handoff.md §Test 5 flaws.",
        "Corruption and recovery are INDEPENDENT processes (scripts/recovery_sweep.py): "
        "the recoverer reads only the corrupted bytes, a scanner's view.",
        "Structural flips only (FFHR header+block headers; JSON brace/bracket/quote/colon/"
        "comma; protobuf TLV record headers; HL7v2 segment terminators+names) -- payload "
        "bytes untouched.",
        "Granularity: recoverable units are entries for FFHR/JSON/protobuf and SEGMENTS "
        "for HL7v2 (a v2 batch has no resource-level index) -- not the same unit, a "
        "format finding.",
        "20 trials per point; band = min..max. The FFHR recovery pre-validates the header "
        "because Parser construction SEGVs on a corrupted one (CAPI-13).",
    ])
    save(fig, out / "fig8_recovery", exts)


def fig_compact_speed(df: pd.DataFrame, prov: Provenance, out: Path, exts: list[str]) -> None:
    """FastFHIR compact vs standard stream, per stage: duration ratio vs size.

    The claim under test: the reader is layout-agnostic, so reading a compact
    archive costs about the same as the standard stream. Rows exist only when
    the compact losslessness gate passed (IN-E). Enrich has NO compact row: the
    API refuses to open a Builder on a compact archive (write-once -- CAPI-10).
    """
    pairs = [
        ("test_2_random_access", "test_2_compact", "Test 2 — random access"),
        ("test_3_query", "test_3_compact", "Test 3 — query"),
    ]
    if df[(df["arm"] == "fastfhir") & df["test"].isin([p[1] for p in pairs])].empty:
        print("  skip fig7: no *_compact rows (losslessness gate or pre-CAPI-10 data)")
        return

    fig, ax = plt.subplots(figsize=(9.5, 5.0))
    fig.suptitle("FastFHIR: compact stream vs standard stream (duration ratio)",
                 fontsize=12, y=0.985, x=0.008, ha="left", color=TEXT_PRIMARY)
    colors = [arm_color("fastfhir"), "#b58900"]
    entries = []
    for (std_name, cmp_name, label), color in zip(pairs, colors):
        std = df[(df["arm"] == "fastfhir") & (df["test"] == std_name)].groupby("target_mb")["duration_ns"].median()
        cmp = df[(df["arm"] == "fastfhir") & (df["test"] == cmp_name)].groupby("target_mb")["duration_ns"].median()
        common = std.index.intersection(cmp.index)
        if len(common) == 0:
            continue
        ratio = (cmp.loc[common] / std.loc[common]).sort_index()
        ax.plot(ratio.index.to_numpy(), ratio.to_numpy(), color=color, lw=2, marker="o",
                ms=5, mec=SURFACE, mew=1.2, label=label)
        entries.append((ratio.index.to_numpy()[-1], ratio.to_numpy()[-1], label))
    ax.axhline(1.0, color=TEXT_MUTED, lw=0.8)
    ax.annotate("1.0 = same speed as standard", xy=(0.02, 1.0), xycoords=("axes fraction", "data"),
                xytext=(0, 4), textcoords="offset points", fontsize=6.8, color=TEXT_MUTED)
    ax.set_xscale("log", base=2)
    ax.set_xlabel("target bundle size (MB)")
    ax.set_ylabel("median duration ratio (compact / standard)")
    label_endpoints(ax, entries)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.legend(loc="upper left", ncol=1, frameon=False, labelcolor=TEXT_SECONDARY, fontsize=8)

    fig.tight_layout(rect=[0, 0.17, 1, 0.96])
    finish(fig, prov, [
        "FastFHIR arm only; rows exist only when the compact losslessness gate passed (IN-E).",
        "Test 4 (enrich) has NO compact row: the API refuses to open a Builder on a compact "
        "archive (write-once format) — CAPI-10. Enrichment is standard-stream-only.",
        "Small-bundle ratios include a fixed parser/root cost in the dense compact root; "
        "whether the ratio compresses at scale is what this curve shows.",
    ])
    save(fig, out / "fig7_compact_speed", exts)


def fig_provenance_card(prov: Provenance, out: Path, exts: list[str]) -> None:
    """The provenance record as an image, so an exported figure set is self-describing."""
    d = prov.data
    fig = plt.figure(figsize=(8.4, 5.2))
    fig.text(0.04, 0.94, "Run provenance", fontsize=13, weight="bold", color=TEXT_PRIMARY,
             va="top")
    if not d:
        fig.text(0.04, 0.84, "No provenance.json found.\nRe-run the harness with --results-dir DIR.",
                 fontsize=9, color=WARN, va="top")
        save(fig, out / "fig0_provenance", exts)
        return

    status = "ARTIFACT — citable" if prov.is_artifact else "PROVISIONAL — NOT AN ARTIFACT"
    fig.text(0.04, 0.875, status, fontsize=9.5, weight="bold",
             color=("#1b7f4d" if prov.is_artifact else WARN), va="top")
    if not prov.is_artifact:
        for i, r in enumerate(prov.why_not_artifact):
            fig.text(0.06, 0.845 - 0.028 * i, f"· {r}", fontsize=8, color=WARN, va="top")

    rows = [
        ("FastFHIR", f"{d.get('fastfhir_tag','?')} @ {str(d.get('fastfhir_sha',''))[:12]}"
                     f"{'  DIRTY' if d.get('fastfhir_dirty') else ''}"),
        ("Profile", f"{d.get('production_profile','?')}  ({d.get('production_profile_source','?')})"),
        ("Profile evidence", f"{d.get('codesystem_enums','?')} code-system enums · "
                             f"{d.get('generated_cpp','?')} generated .cpp · "
                             f"{d.get('generated_resource_count','?')} resource types"),
        ("Build", f"{d.get('compilation_mode','?')} · {d.get('compiler','')} {d.get('compiler_version','')} · "
                  f"{d.get('os','')}/{d.get('arch','')}"),
        ("CPU", d.get("cpu_model", "?")),
        ("Corpus", f"{d.get('corpus_doc_count','?')} docs · "
                   f"{(d.get('corpus_bytes') or 0) / (1 << 20):.0f} MiB"),
        ("Corpus sha256", str(d.get("corpus_sha256", "?"))),
        ("Benchmark", f"{str(d.get('benchmark_sha',''))[:12]}"
                      f"{'  DIRTY' if d.get('benchmark_dirty') else ''}"),
        ("Seed", str(d.get("seed", "?"))),
        ("Generated", d.get("generated_at", "?")),
    ]
    top = 0.845 - (0.028 * len(prov.why_not_artifact) if not prov.is_artifact else 0) - 0.04
    for i, (k, v) in enumerate(rows):
        y = top - 0.062 * i
        fig.text(0.04, y, k, fontsize=8, color=TEXT_MUTED, va="top")
        fig.text(0.28, y, v, fontsize=8.5, color=TEXT_PRIMARY, va="top", family="monospace")
    save(fig, out / "fig0_provenance", exts)


# ---------------------------------------------------------------------------
# Table view (the relief for the two low-contrast palette slots)
# ---------------------------------------------------------------------------


def write_tables(df: pd.DataFrame, prov: Provenance, out: Path) -> None:
    agg = (
        df.groupby(["test", "arm"])
        .agg(runs=("duration_ns", "size"),
             median_us=("duration_ns", lambda s: s.median() / 1000.0),
             p90_us=("duration_ns", lambda s: s.quantile(0.9) / 1000.0),
             median_ops=("ops", "median"),
             median_bytes_in=("bytes_in", "median"),
             median_bytes_out=("bytes_out", "median"))
        .round(2)
        .reset_index()
    )
    agg.to_csv(out / "summary.csv", index=False)

    lines = [
        "# Benchmark summary",
        "",
        f"**{'ARTIFACT — citable' if prov.is_artifact else 'PROVISIONAL — NOT AN ARTIFACT'}**",
        "",
    ]
    if not prov.is_artifact:
        lines += [f"- {r}" for r in prov.why_not_artifact] + [""]
    lines += [f"`{prov.stamp()}`", "", "## Caveats carried on every figure", ""]
    lines += [f"- {c}" for c in (CAVEAT_PARITY, CAVEAT_CHOICE, CAVEAT_RA_GRANULARITY, CAVEAT_QUERY, CAVEAT_ENRICH)]
    lines += ["", "## Medians by stage and arm", ""]
    for stage, title in STAGES:
        sub = agg[agg["test"] == stage]
        if sub.empty:
            continue
        lines += [f"### {title}", "",
                  "| Arm | runs | median µs | p90 µs | ops | ns/op | median bytes in | median bytes out |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for arm in ordered_arms(df):
            row = sub[sub["arm"] == arm]
            if row.empty:
                continue
            r = row.iloc[0]
            ops = int(r["median_ops"]) if r["median_ops"] > 0 else 0
            ns_per_op = f"{(r['median_us'] * 1000.0 / ops):,.2f}" if ops else "—"
            lines.append(
                f"| {arm_label(arm)} | {int(r['runs'])} | {r['median_us']:,.2f} | {r['p90_us']:,.2f} "
                f"| {ops:,} | {ns_per_op} | {int(r['median_bytes_in']):,} "
                f"| {int(r['median_bytes_out']):,} |"
            )
        lines.append("")
    (out / "summary.md").write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# IO
# ---------------------------------------------------------------------------


def save(fig, stem: Path, exts: list[str]) -> None:
    for ext in exts:
        path = stem.with_suffix(f".{ext}")
        fig.savefig(path, bbox_inches="tight")
        print(f"  wrote {path}")
    plt.close(fig)


REQUIRED_COLUMNS = {"arm", "test", "duration_ns", "target_mb", "patients_in_bundle"}


def load_csv(path: Path) -> pd.DataFrame:
    # The harness interleaves progress lines on stderr, but a redirected stdout
    # can still pick up a repeated header if runs were concatenated.
    df = pd.read_csv(path)
    df = df[df["arm"] != "arm"]
    return coerce(df, str(path))


def load_db(connstr: str) -> pd.DataFrame:
    try:
        import psycopg2  # noqa: F401
        from sqlalchemy import create_engine, text
    except ImportError:
        sys.exit("--db needs sqlalchemy + psycopg2 (pip install sqlalchemy psycopg2-binary)")
    from sqlalchemy import create_engine, text

    kv = dict(part.split("=", 1) for part in connstr.split() if "=" in part)
    url = (f"postgresql+psycopg2://{kv.get('user','')}:{kv.get('password','')}"
           f"@{kv.get('host','localhost')}:{kv.get('port','5432')}/{kv.get('dbname','benchmark')}")
    engine = create_engine(url, pool_pre_ping=True)
    # Latest run only: mixing runs mixes provenance, and a chart spanning two
    # profiles is a chart of two different libraries.
    with engine.connect() as conn:
        run_id = conn.execute(text("SELECT MAX(run_id) FROM benchmark_results")).scalar()
    df = pd.read_sql(
        text("SELECT arm, stage AS test, duration_ns, bytes_in, bytes_out, target_mb, "
             "patients_in_bundle FROM benchmark_results WHERE run_id = :rid"),
        engine, params={"rid": run_id},
    )
    engine.dispose()
    print(f"  loaded run_id {run_id} from PostgreSQL")
    return coerce(df, f"postgres run_id={run_id}")


def coerce(df: pd.DataFrame, source: str) -> pd.DataFrame:
    missing = REQUIRED_COLUMNS - set(df.columns)
    if missing:
        sys.exit(f"{source}: missing column(s) {sorted(missing)}")
    for col in ("bytes_in", "bytes_out", "ops"):
        if col not in df.columns:
            # Pre-IN-0 data. Charts that need bytes will skip themselves rather
            # than silently plot zeros.
            print(f"  note: {source} predates IN-0 — no {col}; size figures will be skipped")
            df[col] = 0
    for col in ("duration_ns", "ops", "bytes_in", "bytes_out", "target_mb", "patients_in_bundle"):
        df[col] = pd.to_numeric(df[col], errors="coerce")
    return df.dropna(subset=["duration_ns"]).reset_index(drop=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--csv", type=Path, help="harness stdout CSV")
    src.add_argument("--db", help="PostgreSQL connection string (bench_harness_pg builds)")
    ap.add_argument("--results-dir", type=Path,
                    help="directory holding provenance.json (defaults to the CSV's directory)")
    ap.add_argument("--out", type=Path, default=Path("results/figures"),
                    help="where figures are written (default: results/figures)")
    ap.add_argument("--format", default="png", choices=["png", "svg", "both"])
    args = ap.parse_args()

    apply_style()
    exts = ["png", "svg"] if args.format == "both" else [args.format]

    df = load_csv(args.csv) if args.csv else load_db(args.db)
    if df.empty:
        sys.exit("no rows to plot")
    prov = load_provenance(args.results_dir, args.csv)

    args.out.mkdir(parents=True, exist_ok=True)
    print(f"{len(df)} rows · {df['arm'].nunique()} arms · "
          f"{sorted(df['target_mb'].unique())} MB targets")
    print(f"provenance: {prov.path or 'NOT FOUND'} — "
          f"{'artifact' if prov.is_artifact else 'PROVISIONAL'}")

    fig_provenance_card(prov, args.out, exts)
    fig_duration_by_stage(df, prov, args.out, exts)
    fig_wire_size(df, prov, args.out, exts)
    fig_random_access(df, prov, args.out, exts)
    fig_recovery(prov, args.out, exts)
    fig_compact_speed(df, prov, args.out, exts)
    fig_enrich_delta(df, prov, args.out, exts)
    fig_distribution(df, prov, args.out, exts)
    fig_speedup(df, prov, args.out, exts)
    write_tables(df, prov, args.out)
    print(f"  wrote {args.out / 'summary.md'}\n  wrote {args.out / 'summary.csv'}")

    if not prov.is_artifact:
        print("\nNOTE: every figure is stamped PROVISIONAL — NOT AN ARTIFACT.")
        for r in prov.why_not_artifact:
            print(f"  - {r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
