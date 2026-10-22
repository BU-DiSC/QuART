#!/usr/bin/env python3
"""Figures 14 and 15: concurrent QuART-OLC vs ART-OLC.

Fig 14 reads conc_insert_throughput.csv (workload,tree,threads,mops);
Fig 15 reads conc_readwrite.csv (panel,tree,x,mops).  --which fig14|fig15|both.
"""
import argparse
import os
import sys
from collections import defaultdict

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import FIGURES_DIR, load, locate  # noqa: E402
from style import apply, label, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--results", default=None, help="results directory (default: results/latest, else results/paper)")
p.add_argument("--out-dir", default=FIGURES_DIR)
p.add_argument("--which", default="both", choices=("fig14", "fig15", "both"))
args = p.parse_args()
apply(font_size=14)
plt.rcParams.update({"axes.titlesize": 15, "axes.labelsize": 15})

PURPLE, GREEN, ORANGE, BLUE = "#6a0dad", "#55a868", "#e07b00", "#4878cf"
WORKLOAD_COLORS = {"K=L=0": BLUE, "K=L=5": GREEN, "K=L=25": ORANGE, "mixed": PURPLE}
WORKLOAD_LABELS = {"K=L=0": r"$K{=}L{=}0$", "K=L=5": r"$K{=}L{=}5$", "K=L=25": r"$K{=}L{=}25$", "mixed": "mixed"}


def style_axes(ax, xticks, log=True):
    if log:
        ax.set_xscale("log", base=2)
        ax.minorticks_off()
    ax.set_xticks(xticks)
    ax.set_xticklabels([str(t) for t in xticks])
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.grid(axis="y", ls=":", lw=0.6, color="#b0b0b0")
    ax.set_axisbelow(True)
    ax.set_ylim(bottom=0)


def series(df, keycols):
    rows = defaultdict(list)
    for _, r in df.iterrows():
        rows[tuple(r[c] for c in keycols[:-1])].append((float(r[keycols[-1]]), float(r["mops"])))
    return {k: sorted(v) for k, v in rows.items()}


def fig14(out):
    df = load(locate(EXP_DIR, "conc_insert_throughput.csv", args.results))
    rows = series(df, ["workload", "tree", "threads"])
    threads = [int(t) for t in sorted(df["threads"].unique())]
    fig, ax = plt.subplots(figsize=(6.75, 3.0))
    for w, color in WORKLOAD_COLORS.items():
        for tree, ls, marker, lw in (("QuART-OLC", "-", "o", 2.2), ("ART-OLC", "--", "x", 1.8)):
            pts = rows.get((w, tree), [])
            ax.plot([p[0] for p in pts], [p[1] for p in pts], ls=ls, marker=marker, ms=7 if marker == "o" else 8,
                    mew=2 if marker == "x" else 1, lw=lw, color=color)
    style_axes(ax, threads)
    ax.set_xlabel(f"Threads (= {label('hash')} streams)")
    ax.set_ylabel("Throughput (M ops/s)")
    wl = [Line2D([], [], color=c, lw=2.2, label=WORKLOAD_LABELS[w]) for w, c in WORKLOAD_COLORS.items()]
    leg1 = ax.legend(handles=wl, title="Workload", loc="upper left", ncol=2, frameon=False, handlelength=1.6, columnspacing=1.2)
    ax.add_artist(leg1)
    tl = [Line2D([], [], color="black", ls="-", marker="o", ms=7, lw=2.2, label="QuART-OLC"),
          Line2D([], [], color="black", ls="--", marker="x", ms=8, mew=2, lw=1.8, label="ART-OLC")]
    ax.legend(handles=tl, loc="upper left", bbox_to_anchor=(0.47, 1.0), frameon=False, handlelength=2.2)
    fig.tight_layout(pad=0.3)
    for w in WORKLOAD_COLORS:
        a, q = dict(rows.get((w, "ART-OLC"), [])), dict(rows.get((w, "QuART-OLC"), []))
        print(f"{w:8s} " + "  ".join(f"W{t}: {q.get(t, 0):.0f}/{a.get(t, 0):.0f} ({q.get(t, 0) / a[t]:.2f}x)" for t in threads if t in a))
    save(fig, out)


def fig15(out):
    df = load(locate(EXP_DIR, "conc_readwrite.csv", args.results))
    rows = series(df, ["panel", "tree", "x"])
    fig, axes = plt.subplots(1, 2, figsize=(6.95, 3.2))
    styles = (("ART-OLC", GREEN, "--", "s", 2.0), ("QuART-OLC", PURPLE, "-", "o", 2.4))
    xt = [int(v) for v in sorted(df[df["panel"] == "threads"]["x"].unique())]
    xr = [int(v) for v in sorted(df[df["panel"] == "read_pct"]["x"].unique())]
    wmax = max(xt) if xt else 16
    for ax, panel, title, xlabel, xticks, log in (
            (axes[0], "threads", "(a) 50/50 read/write", f"Threads (= {label('hash')} streams)", xt, True),
            (axes[1], "read_pct", f"(b) read mix, {wmax} threads", f"Reads ({label('pct')})", xr, False)):
        for tree, color, ls, marker, lw in styles:
            pts = rows.get((panel, tree), [])
            ax.plot([p[0] for p in pts], [p[1] for p in pts], ls=ls, marker=marker, ms=7, lw=lw, color=color, label=tree)
        style_axes(ax, xticks, log)
        ax.set_title(title)
        ax.set_xlabel(xlabel)
        ax.set_ylabel("Throughput (M ops/s)")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False, bbox_to_anchor=(0.5, 1.02), handlelength=2.2)
    fig.tight_layout(rect=(0, 0, 1, 0.9), w_pad=1.5)
    save(fig, out)


if args.which in ("fig14", "both"):
    fig14(os.path.join(args.out_dir, "conc_insert_throughput.pdf"))
if args.which in ("fig15", "both"):
    fig15(os.path.join(args.out_dir, "conc_ycsb.pdf"))
