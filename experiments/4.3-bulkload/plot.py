#!/usr/bin/env python3
"""Figure 13: repeated ART insertion vs QuART bulk loading, insertion time vs number of keys."""
import argparse
import os
import sys

import numpy as np

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import add_args, load, locate  # noqa: E402
from style import apply, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "bulkload.csv", "bulkload_performance.pdf").parse_args()
df = load(locate(EXP_DIR, "bulkload.csv", args.results))
df["seconds"] = df["avg_insert_ns"] / 1e9
sizes = sorted(df["N"].unique())
reg = df[df["insertion_method"] == "regular"].set_index("N").reindex(sizes)
bulk = df[df["insertion_method"] == "bulkload"].set_index("N").reindex(sizes)

apply(font_size=15)
x = np.arange(len(sizes))
bw = 0.35
fig, ax = plt.subplots(figsize=(8, 4))
ax.bar(x - bw / 2, reg["seconds"], bw, label="Repeated Insertions")
ax.bar(x + bw / 2, bulk["seconds"], bw, label="Bulkload")
ax.set_xticks(x)
ax.set_xticklabels([f"{n / 1e6:.0f}M" for n in sizes])
ax.set_xlabel("Number of Elements")
ax.set_ylabel("Insertion Time (seconds)")
ax.legend(ncol=2, loc="upper center", labelspacing=0.2, handletextpad=0.4, columnspacing=0.8)
fig.tight_layout()
for n in sizes:
    print(f"N={n / 1e6:6.0f}M  regular {reg.loc[n, 'seconds']:.2f}s  bulk {bulk.loc[n, 'seconds']:.2f}s  "
          f"({reg.loc[n, 'seconds'] / bulk.loc[n, 'seconds']:.1f}x)")
save(fig, args.out)
