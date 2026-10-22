#!/usr/bin/env python3
"""Reshape the raw driver rows of run.sh into the two plot CSVs (median over repeats).

usage: to_long.py fig14_raw.csv fig15_raw.csv OUT_DIR
writes OUT_DIR/conc_insert_throughput.csv  workload,tree,threads,mops
       OUT_DIR/conc_readwrite.csv          panel,tree,x,mops
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

WORKLOAD = {"K0_L0": "K=L=0", "K5_L5": "K=L=5", "K25_L25": "K=L=25", "mixed": "mixed"}


def median_rows(path, keyf, valf):
    acc = defaultdict(list)
    for r in csv.DictReader(open(path)):
        for key, val in valf(r):
            acc[keyf(r) + key].append(float(val))
    return {k: statistics.median(v) for k, v in acc.items()}


def main(raw14, raw15, out_dir):
    m14 = median_rows(raw14, lambda r: (WORKLOAD.get(r["workload"], r["workload"]), int(r["W"])),
                      lambda r: ((("ART-OLC",), r["nofp_Mips"]), (("QuART-OLC",), r["stail_Mips"])))
    with open(os.path.join(out_dir, "conc_insert_throughput.csv"), "w") as f:
        f.write("workload,tree,threads,mops\n")
        for (w, t, tree), v in sorted(m14.items(), key=lambda kv: (list(WORKLOAD.values()).index(kv[0][0]), kv[0][2], kv[0][1])):
            f.write(f"{w},{tree},{t},{v:.2f}\n")
    panel = {"a_threads": ("threads", "W"), "b_readfrac": ("read_pct", "read_pct")}
    m15 = median_rows(raw15, lambda r: (panel[r["subfig"]][0], int(r[panel[r["subfig"]][1]])),
                      lambda r: ((("ART-OLC",), r["art_mops"]), (("QuART-OLC",), r["stail_mops"])))
    with open(os.path.join(out_dir, "conc_readwrite.csv"), "w") as f:
        f.write("panel,tree,x,mops\n")
        for (p, x, tree), v in sorted(m15.items(), key=lambda kv: (kv[0][0], kv[0][2], kv[0][1])):
            f.write(f"{p},{tree},{x},{v:.2f}\n")
    print(f"wrote {out_dir}/conc_insert_throughput.csv and conc_readwrite.csv")


if __name__ == "__main__":
    main(*sys.argv[1:4])
