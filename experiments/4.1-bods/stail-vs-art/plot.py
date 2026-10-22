#!/usr/bin/env python3
"""Figure 10: QuART-stail over ART insert (a) and query (b) speedup across the K×L grid."""
import argparse
import os
import sys

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "..", "plot"))
from results import add_args, load, locate  # noqa: E402
from style import apply, label, save  # noqa: E402
from heatmap import heatmap_pair  # noqa: E402

args = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "stail_vs_art.csv",
                "stail_heatmaps_only.pdf").parse_args()
df = load(locate(EXP_DIR, "stail_vs_art.csv", args.results))
apply(font_size=30)
t = label("times")
fig = heatmap_pair(df, "ART", "QuART_stail",
                   f"(a) {label('QuART_stail')} vs. ART Insert Speedup ({t})",
                   f"(b) {label('QuART_stail')} vs. ART Query Speedup ({t})")
save(fig, args.out)
