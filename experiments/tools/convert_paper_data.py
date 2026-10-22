#!/usr/bin/env python3
"""One-off conversion of the paper's figure data into each experiment's results/paper/.

The paper's figures were drawn from ad-hoc CSVs with older tree names and column names.
This script rewrote them into the schemas written by the run.sh scripts, so that
`plot.py` reads a fresh run and the paper's data the same way.  It is kept for
provenance; the converted files are committed under experiments/*/results/paper/.

usage: convert_paper_data.py SRC_DIR    (SRC_DIR = the old graphs/data directory)
"""
import os
import re
import sys

import pandas as pd

SRC = sys.argv[1]
EXP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREE = {"QuART_stail_reset_2": "QuART_stail", "QuART_lil_can": "QuART_lil", "QuIT_2k": "QuIT", "BPTree": "B+-tree"}
DROP = {"QuART_stail_reset_bidir"}
S1 = ["workload", "N", "K", "L", "tree_type", "avg_insert_ns", "avg_query_ns"]


def out(sub, name, df, note):
    d = os.path.join(EXP, sub, "results", "paper")
    os.makedirs(d, exist_ok=True)
    df.to_csv(os.path.join(d, name), index=False)
    with open(os.path.join(d, "meta.txt"), "w") as f:
        f.write(note.strip() + "\n")
    print(f"{sub}/results/paper/{name}: {len(df)} rows")


def kl(w):
    m = re.search(r"_K(\d+)_L(\d+)", w)
    return int(m.group(1)), int(m.group(2))


# Fig 6 — fast-path insert counts (no L column in the source; the sweep used L=1, L=0 at K=0)
d = pd.read_csv(f"{SRC}/bods_insert_types.csv").rename(columns={"type_of_tree": "tree_type", "number_of_fp_inserts": "fp_inserts"})
d["N"] = 500000000
d["L"] = d["K"].apply(lambda k: 0 if k == 0 else 1)
d["workload"] = d.apply(lambda r: f"workload_N500000000_K{r.K}_L{r.L}", axis=1)
d["root_inserts"] = d["N"] - d["fp_inserts"]
d = d[["workload", "N", "K", "L", "tree_type", "fp_inserts", "root_inserts"]].sort_values(["tree_type", "K"])
out("3.3-insert-types", "bods_insert_types.csv", d, """
source: the paper's Figure 6 data (fast-path insert counts, BoDS N=500M).
The source recorded only K; L=1 is assumed (L=0 at K=0), the convention of the Fig 9 sweep.
fp_inserts = inserts that started at the fast pointer; root_inserts = N - fp_inserts.
Counts from run.sh (QUART_FP_STATS build) match this definition for stail exactly
(K=0: N - 500M/256 - 1 = 498,046,874).  For tail and lil the counter reports only the
inserts that appended directly at fp; the paper's tail/lil counts also include the
few inserts per 256-key group that match fp's prefix but re-descend from the root to
expand a leaf, so they are about 0.4% higher at K=0.
""")

# Fig 9 — raw run behind the paper's figure
d = pd.read_csv(f"{SRC}/../quart_vs_art_raw_run.csv")  # the unedited run.sh output behind Fig 9
d["N"] = 500000000
out("4.1-bods/quart-vs-art", "quart_vs_art.csv", d[S1], """
source: the single-repeat run behind the paper's Figure 9 (same measurements as the
earlier run.sh output, columns renamed).  The CSV the paper's figure
was drawn from had been hand-edited afterwards (QuART_stail +0.2..0.5 Mops at every K,
QuART_tail/QuART_lil +0.1..0.3 at K=100); this file is the unedited measurement, so the
regenerated figure differs from the paper's by those amounts.
""")

# Fig 10 — K×L grid, ART and the three variants
d = pd.read_csv(f"{SRC}/bods_times.csv").rename(columns={"type_of_tree": "tree_type", "avg_insert_time": "avg_insert_ns", "avg_query_time": "avg_query_ns"})
d = d[~d["tree_type"].isin(DROP)].copy()
d["tree_type"] = d["tree_type"].replace(TREE)
d["workload"] = d.apply(lambda r: f"workload_N{r.N}_K{r.K}_L{r.L}", axis=1)
out("4.1-bods/stail-vs-art", "stail_vs_art.csv", d[S1].sort_values(["K", "L", "tree_type"]), """
source: the paper's Figure 10 data (BoDS N=500M, K,L in {1,5,10,25,100} plus K=L=0;
ART, QuART_tail, QuART_lil, QuART_stail; an experimental bidirectional stail variant was dropped).
Tree names were mapped from the development names (QuART_stail_reset_2 -> QuART_stail,
QuART_lil_can -> QuART_lil).  Repeat count not recorded.
""")

# Fig 11 — stail vs QuIT grid
d = pd.read_csv(f"{SRC}/quit_experiments.csv").rename(columns={"insertion_time_ns": "avg_insert_ns", "query_time_ns": "avg_query_ns"})
d["tree_type"] = d["tree_type"].replace(TREE)
d[["K", "L"]] = d["workload"].apply(lambda w: pd.Series(kl(w)))
d["N"] = 500000000
out("4.1-bods/stail-vs-quit", "stail_vs_quit.csv", d[S1].sort_values(["K", "L", "tree_type"]), """
source: the paper's Figure 11 data (BoDS N=500M grid; QuART_stail vs QuIT with 2 KB nodes).
Tree names mapped from QuART_stail_reset_2 / QuIT_2k.  Repeat count not recorded.
""")

# Fig 12 — TPC-H
d = pd.read_csv(f"{SRC}/tpch_experiment.csv").rename(columns={"insertion_ns": "avg_insert_ns", "query_ns": "avg_query_ns"})
d["tree_type"] = d["tree_type"].replace(TREE)
d["workload"] = "workload_N6000000_K9667_L01"
out("4.2-tpch", "tpch.csv", d[S1], """
source: the paper's Figure 12 data (REPEAT=200 averages, N=6M, K=96.67, L=0.1, 60000 lookups).
Query throughput = (N/100) / avg_query_ns.
""")

# Fig 13 — bulk load
d = pd.read_csv(f"{SRC}/bulkload_times.csv").rename(columns={"insertion_time_ns": "avg_insert_ns", "query_time_ns": "avg_query_ns"})
out("4.3-bulkload", "bulkload.csv", d[["N", "insertion_method", "avg_insert_ns", "avg_query_ns"]], """
source: the paper's Figure 13 data (synthetic sorted keys 1..N, REPEAT=5 averages).
""")

# Fig 14/15 — only the long-form medians exist
d14 = pd.read_csv(f"{SRC}/conc_insert_throughput.csv")
d15 = pd.read_csv(f"{SRC}/conc_ycsb.csv")
out("4.4-concurrent", "conc_insert_throughput.csv", d14, """
source: the paper's Figure 14 and 15 data (median of 5 runs, one 24-core socket, N=20M per stream).
Only these long-form medians were kept; the raw per-run driver rows (fig14_raw.csv /
fig15_raw.csv that run.sh writes) were not archived.
""")
d15.to_csv(os.path.join(EXP, "4.4-concurrent", "results", "paper", "conc_readwrite.csv"), index=False)
print("4.4-concurrent/results/paper/conc_readwrite.csv:", len(d15), "rows")
