#!/usr/bin/env bash
# §4.2, Figure 12 — insert and query throughput of B+-tree, QuIT, ART and the
# three QuART variants on the TPC-H-inspired lineitem order (BoDS N=6M,
# K=96.67, L=0.1; file workload_N6000000_K9667_L01.bin).
#
# Usage:  bash experiments/4.2-tpch/run.sh
# Env:    WORKLOAD_FILE (default $WORKLOAD_DIR/workload_N6000000_K9667_L01.bin),
#         N (default 6000000), REPEAT (default 10; the paper used 200),
#         TREES (default "B+-tree QuIT ART QuART_tail QuART_lil QuART_stail"), NUMACTL
# Output: results/<ts>/tpch.csv   (schema S1; query throughput in the figure
#         is (N/100) lookups / avg_query_ns)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
source "$EXPERIMENTS_DIR/setup.sh"
source "$QUIT_SRC/sweep.sh"
REPEAT="${REPEAT:-10}"
exp_init "4.2 tpch (Fig 12)"
N="${N:-6000000}"
WORKLOAD_FILE="${WORKLOAD_FILE:-$WORKLOAD_DIR/workload_N6000000_K9667_L01.bin}"
TREES="${TREES:-B+-tree QuIT ART QuART_tail QuART_lil QuART_stail}"
exp_meta trees "$TREES"; exp_meta N "$N"; exp_meta workload_file "$WORKLOAD_FILE"
ensure_quit_built

bods_sweep "$RESULTS_DIR/tpch.csv" "$N" "$TREES" "96.67,0.1,$WORKLOAD_FILE"
echo "wrote $RESULTS_DIR/tpch.csv"
