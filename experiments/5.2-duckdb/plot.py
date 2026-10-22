#!/usr/bin/env python3
"""Figure 16: per-thread, per-key speedup of QuART bulk loading over DuckDB's loader inside
CREATE INDEX, by sorted key-set size (rows) and thread count (columns).  Each cell is the
profiler's CREATE_INDEX operator time per key with DuckDB's loader divided by that with
QuART bulk loading (profile.csv from bench_paper.sh)."""
import argparse
import os
import re
import sys

import numpy as np

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import add_args, load, locate  # noqa: E402
from style import apply, label, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "profile.csv", "duckdb_speedup_heatmap.pdf").parse_args()
df = load(locate(EXP_DIR, "profile.csv", args.results))
df["grouped"] = df["grouped"].astype(str).str.lower()


def size_of(w):
    m = re.search(r"_(\d+)M$", w)
    return int(m.group(1)) if m else 0


sizes = sorted({w for w in df["workload"] if size_of(w)}, key=size_of)
threads = sorted(df["threads"].unique())
prof = {(r["workload"], r["grouped"], int(r["threads"])): r for _, r in df.iterrows()}
grid = np.full((len(sizes), len(threads)), np.nan)
for i, w in enumerate(sizes):
    for j, t in enumerate(threads):
        po, pg = prof.get((w, "false", t)), prof.get((w, "true", t))
        if po is not None and pg is not None:
            grid[i, j] = (po["create_index_s"] / po["keys"]) / (pg["create_index_s"] / pg["keys"])

apply(font_size=15)
plt.rcParams["axes.labelsize"] = 16
fig, ax = plt.subplots(figsize=(6.6, 2.5))
vmin, vmax = 1.0, max(np.nanmax(grid), 1.5)  # anchored at parity
im = ax.imshow(grid, cmap="viridis", aspect="auto", origin="lower", vmin=vmin, vmax=vmax)
for i in range(len(sizes)):
    for j in range(len(threads)):
        v = grid[i, j]
        if not np.isnan(v):
            ax.text(j, i, f"{v:.2f}{label('times')}", ha="center", va="center", fontsize=15,
                    color="black" if (v - vmin) / max(vmax - vmin, 1e-9) > 0.6 else "white")
ax.set_xticks(range(len(threads)))
ax.set_xticklabels([str(int(t)) for t in threads])
ax.set_yticks(range(len(sizes)))
ax.set_yticklabels([f"{size_of(w)}M" for w in sizes])
ax.set_xticks(np.arange(-0.5, len(threads), 1), minor=True)
ax.set_yticks(np.arange(-0.5, len(sizes), 1), minor=True)
ax.grid(which="minor", color="white", lw=2)
ax.tick_params(which="both", length=0)
for sp in ax.spines.values():
    sp.set_visible(False)
ax.set_xlabel("Threads")
ax.set_ylabel("Sorted keys")
cb = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.03)
cb.set_ticks(np.arange(1, np.floor(vmax) + 1))
cb.ax.tick_params(labelsize=14)
fig.tight_layout(pad=0.3)
print(f"per-thread per-key build speedup: min {np.nanmin(grid):.2f}x max {np.nanmax(grid):.2f}x")
for i, w in enumerate(sizes):
    print(f"  {size_of(w):>4}M: " + "  ".join("  n/a" if np.isnan(v) else f"{v:.2f}x" for v in grid[i]))
save(fig, args.out)
