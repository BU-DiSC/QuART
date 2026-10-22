#!/usr/bin/env python3
"""Figure 17: standalone insert (a) and query (b) throughput of rax vs QuART-rax per K (rows of
each (K, tree) are averaged over repetitions), plus the in-server stream-append ratio when
redis_inserver.csv is present."""
import argparse
import os
import sys

import numpy as np
import pandas as pd

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import add_args, load, locate, mops  # noqa: E402
from style import apply, label, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "redis_standalone.csv", "redis_performance.pdf").parse_args()
path = locate(EXP_DIR, "redis_standalone.csv", args.results)
df = load(path)
df = df[df["insert_ns"].notna() & (df["insert_ns"] > 0)]
avg = df.groupby(["K", "tree_type"])[["insert_ns", "query_ns", "N"]].mean().reset_index()
avg["insert_mops"] = mops(avg["N"], avg["insert_ns"])
avg["query_mops"] = mops(avg["N"], avg["query_ns"])
ks = sorted(avg["K"].unique())
bad = df[pd.to_numeric(df["verify"], errors="coerce") != 1]
if len(bad):
    print(f"warning: {len(bad)} run(s) failed verification", file=sys.stderr)

apply(font_size=11)
COLORS = {"RAX": "#55a868", "QuART": "#6a0dad"}
NAMES = {"RAX": "RAX", "QuART": "QuART-rax"}
x = np.arange(len(ks))
w = 0.36
fig, axes = plt.subplots(1, 2, figsize=(6.95, 1.85))
for ax, col, title in ((axes[0], "insert_mops", "(a) Inserts"), (axes[1], "query_mops", "(b) Queries")):
    for off, tree in ((-w / 2, "RAX"), (w / 2, "QuART")):
        d = avg[avg["tree_type"] == tree].set_index("K").reindex(ks)
        ax.bar(x + off, d[col], w, color=COLORS[tree], label=label(NAMES[tree]), edgecolor="none")
    ax.set_xticks(x)
    ax.set_xticklabels([str(int(k)) for k in ks])
    ax.set_xlabel(f"$K$ ({label('pct')})")
    ax.set_title(title)
    ax.set_ylabel("Throughput (M ops/sec)")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.yaxis.grid(True, linestyle=":", color="lightgray")
    ax.set_axisbelow(True)
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False, bbox_to_anchor=(0.5, 1.02))
fig.tight_layout(rect=(0, 0, 1, 0.92))
for k in ks:
    r = avg[(avg["K"] == k) & (avg["tree_type"] == "RAX")].iloc[0]
    q = avg[(avg["K"] == k) & (avg["tree_type"] == "QuART")].iloc[0]
    print(f"K={int(k):3d}: insert rax {r.insert_mops:.2f} QuART-rax {q.insert_mops:.2f} M/s ({q.insert_mops / r.insert_mops:.2f}x); "
          f"query {r.query_mops:.2f} vs {q.query_mops:.2f} ({q.query_mops / r.query_mops:.2f}x)")
save(fig, args.out)

inserver = os.path.join(os.path.dirname(path), "redis_inserver.csv")
if os.path.exists(inserver):
    d = load(inserver)
    med = d.groupby("config")["ns_per_entry"].median()
    for cfg in med.index:
        if str(cfg) != "off":
            print(f"in-server stream appends: rax {med['off']:.1f} ns/entry, QuART-rax (depth {cfg}) {med[cfg]:.1f} ns/entry "
                  f"= {med['off'] / med[cfg]:.2f}x")
