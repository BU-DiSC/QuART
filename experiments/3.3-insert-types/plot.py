#!/usr/bin/env python3
"""Figure 6: fast-path vs root inserts for QuART-tail / -lil / -stail vs out-of-order entries (K)."""
import argparse
import os
import sys

import numpy as np

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import add_args, load, locate  # noqa: E402
from style import apply, label, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "bods_insert_types.csv",
                "bods_insert_types.pdf").parse_args()
df = load(locate(EXP_DIR, "bods_insert_types.csv", args.results))
N = int(df["N"].max())
# one L per K: K=0 is the fully sorted file (L=0); the sweep uses L=1 elsewhere
if (df["K"] > 0).any():
    df = df[(df["K"] == 0) | (df["L"] == df[df["K"] > 0]["L"].min())]

apply(font_size=55)
trees = [t for t in ("QuART_tail", "QuART_lil", "QuART_stail") if t in set(df["tree_type"])]
fig, axes = plt.subplots(1, len(trees), figsize=(12 * len(trees), 10), squeeze=False)
axes = axes[0]
fig.subplots_adjust(wspace=0)
for i, (ax, tree) in enumerate(zip(axes, trees)):
    d = df[df["tree_type"] == tree].sort_values("K")
    x = np.arange(len(d))
    ax.bar(x, d["fp_inserts"], width=0.4, label="Fast-Path Inserts", color="#032F4D", alpha=0.8)
    ax.bar(x, d["root_inserts"], bottom=d["fp_inserts"], width=0.4, label="Top-Inserts", color="#CC0000", alpha=0.8)
    if i == len(trees) // 2:
        ax.set_xlabel(f"Out-of-order entries ({label('pct')})")
    ax.set_title(f"({chr(97 + i)}) {label(tree.replace('QuART_', ''))} - Insert Types", fontweight="bold", pad=25)
    ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, p: f"{v / 1e6:.0f}M"))
    ax.set_facecolor("white")
    ax.set_ylim(0, N)
    ax.set_xticks(x)
    ax.set_xticklabels([int(k) for k in d["K"]])
    if i > 0:
        ax.set_yticklabels([])
    if i == len(trees) - 1:
        ax.set_ylabel(f"{label('hash')} of Entries", labelpad=12)
        ax.yaxis.set_label_position("right")
    print(tree + ": " + "  ".join(f"K{int(k)}={fp / 1e6:.1f}M" for k, fp in zip(d["K"], d["fp_inserts"])))
fig.patch.set_facecolor("white")
fig.tight_layout()
fig.subplots_adjust(wspace=0, bottom=0.36)
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="lower center", frameon=True, ncol=2, bbox_to_anchor=(0.5, 0))
save(fig, args.out)
