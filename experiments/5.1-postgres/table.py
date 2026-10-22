#!/usr/bin/env python3
"""Table 2: PostgreSQL VACUUM replay — stail-over-ART and bulk-load-over-ART speedups per
workload (rows W1..W9) and table scale (columns).  Prints Markdown; --latex also writes the
tabular body used in the paper.  Exits 1 if any stream failed verification."""
import argparse
import os
import sys

EXP_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(EXP_DIR, "..", "plot"))
from results import add_args, load, locate  # noqa: E402

WORKLOADS = [("W1", "update"), ("W2", "range25"), ("W3", "range75"), ("W4", "striped7"), ("W5", "banded"),
             ("W6", "sparse"), ("W7", "wideband"), ("W8", "mixed"), ("W9", "abort")]


def scale_value(s):
    return float(s.rstrip("MB")) * {"M": 1e6, "B": 1e9}.get(s[-1], 1)


p = add_args(argparse.ArgumentParser(description=__doc__), EXP_DIR, "replay.csv", "postgres_vacuum_table.tex")
p.add_argument("--latex", action="store_true", help="also write the LaTeX tabular body to --out")
args = p.parse_args()
df = load(locate(EXP_DIR, "replay.csv", args.results))
cells = {(r["workload"], r["scale"]): r for _, r in df.iterrows()}
scales = sorted(df["scale"].unique(), key=scale_value)
failed = df[df["verify"] != "ok"]

head = ["Workload", "Density"] + [f"stail {s}" for s in scales] + [f"bulk {s}" for s in scales]
md = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
tex = []
for wid, wl in WORKLOADS:
    row = [cells.get((wl, s)) for s in scales]
    if not any(r is not None for r in row):
        continue
    density = next(float(r["density"]) for r in row if r is not None)
    stail = [f"{float(r['stail_speedup']):.2f}" if r is not None else "" for r in row]
    bulk = [(f"{float(r['bulk_speedup']):.2f}" if r["sorted"] == "yes" else "--") if r is not None else "" for r in row]
    md.append(f"| {wid} {wl} | {density:.2f} | " + " | ".join(stail + bulk) + " |")
    tex.append(f"  ${wid[0]}_{wid[1:]}$ & {density:.2f} & " + " & ".join(stail + bulk) + r" \\")
print("\n".join(md))
if args.latex:
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("% columns: workload, density, stail/ART at " + ", ".join(scales) + ", bulk/ART at " + ", ".join(scales) + "\n")
        f.write("\n".join(tex) + "\n")
    print(f"wrote {args.out}")
if len(failed):
    print(f"VERIFY FAILED for {len(failed)} stream(s): " + ", ".join(f"{r['scale']} {r['workload']}" for _, r in failed.iterrows()), file=sys.stderr)
    sys.exit(1)
