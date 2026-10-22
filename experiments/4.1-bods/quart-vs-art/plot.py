#!/usr/bin/env python3
"""Figure 9: insert throughput of ART vs QuART-tail / -lil / -stail as K grows."""
import argparse
import os
import sys

import numpy as np

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "..", "plot"))
from results import add_args, load, locate, mops  # noqa: E402
from style import apply, label, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "quart_vs_art.csv", "quart_vs_art.pdf").parse_args()
df = load(locate(EXP_DIR, "quart_vs_art.csv", args.results))
df["insert_mops"] = mops(df["N"], df["avg_insert_ns"])

apply(font_size=15)
COLORS = {"ART": "#032F4D", "QuART_tail": "#CC0000", "QuART_lil": "#F0A500", "QuART_stail": "#2CA02C"}
trees = [t for t in COLORS if t in set(df["tree_type"])]
k_vals = sorted(df["K"].unique())
x = np.arange(len(k_vals))
bw = 0.2
offsets = np.linspace(-(len(trees) - 1) / 2, (len(trees) - 1) / 2, len(trees)) * bw
fig, ax = plt.subplots(figsize=(8, 3.4))
for i, tree in enumerate(trees):
    d = df[df["tree_type"] == tree].set_index("K")
    ax.bar(x + offsets[i], [d.loc[k, "insert_mops"] if k in d.index else 0 for k in k_vals], width=bw,
           label=label(tree), color=COLORS[tree], alpha=0.85)
ax.set_xlabel(f"K ({label('pct')})")
ax.set_ylabel("Insert Throughput (Mops/s)")
ax.set_xticks(x)
ax.set_xticklabels([int(k) for k in k_vals])
ax.set_ylim(0)
ax.grid(axis="y", linestyle="--", alpha=0.4)
ax.legend(frameon=True)
fig.tight_layout()
for k in k_vals:
    d = df[df["K"] == k].set_index("tree_type")["insert_mops"]
    print(f"K={int(k):3d}: " + "  ".join(f"{t}={d[t]:.1f}" for t in trees if t in d.index))
save(fig, args.out)
