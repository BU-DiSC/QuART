#!/usr/bin/env python3
"""Figure 12: insert (a) and query (b) throughput on the TPC-H-inspired workload.

Inserts: N keys / avg_insert_ns.  Queries: the drivers time N/100 random lookups,
so query throughput is (N/100) / avg_query_ns.
"""
import argparse
import os
import sys

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import add_args, load, locate, mops  # noqa: E402
from style import apply, label, save  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "tpch.csv", "tpch_throughput.pdf").parse_args()
df = load(locate(EXP_DIR, "tpch.csv", args.results))
ORDER = ["B+-tree", "QuIT", "ART", "QuART_tail", "QuART_lil", "QuART_stail"]
COLORS = {"B+-tree": "#4878CF", "QuIT": "#D65F5F", "ART": "#6ACC65", "QuART_tail": "#E07B00",
          "QuART_lil": "#1A6B2A", "QuART_stail": "#6A0DAD"}
SHORT = {"QuART_tail": "tail", "QuART_lil": "lil", "QuART_stail": "stail"}
present = set(df["tree_type"])
df = df.set_index("tree_type").loc[[t for t in ORDER if t in present]].reset_index()
df["insert_mops"] = mops(df["N"], df["avg_insert_ns"])
df["query_mops"] = mops(df["N"] // 100, df["avg_query_ns"])
labels = [label(SHORT.get(t, t)) for t in df["tree_type"]]
colors = [COLORS[t] for t in df["tree_type"]]

apply(font_size=20)
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
for ax, col, title in ((ax1, "insert_mops", "(a) TPC-H Inserts"), (ax2, "query_mops", "(b) TPC-H Queries")):
    ax.bar(range(len(df)), df[col], color=colors, edgecolor="black", linewidth=0.6)
    ax.set_ylabel("Throughput (M ops/s)")
    ax.set_title(title, fontweight="bold")
    ax.set_xticks(range(len(df)))
    ax.set_xticklabels(labels)
    ax.set_axisbelow(True)
fig.tight_layout()
print(df[["tree_type", "insert_mops", "query_mops"]].round(2).to_string(index=False))
save(fig, args.out)
