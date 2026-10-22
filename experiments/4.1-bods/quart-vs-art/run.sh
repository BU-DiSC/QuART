#!/usr/bin/env bash
# §4.1, Figure 9 — insert throughput of ART vs QuART-tail / -lil / -stail as K
# grows (BoDS N=500M; K=0,L=0 plus L=1 with K in {1,3,5,10,25,50,100}).
#
# Usage:  bash experiments/4.1-bods/quart-vs-art/run.sh
# Env:    WORKLOAD_DIR, WORKLOAD_N (default 500000000), REPEAT (default 1),
#         TREES (default "ART QuART_tail QuART_lil QuART_stail"), NUMACTL
# Output: results/<ts>/quart_vs_art.csv   (schema S1, see sweep.sh)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../common.sh"
source "$EXPERIMENTS_DIR/setup.sh"
source "$QUIT_SRC/sweep.sh"
REPEAT="${REPEAT:-1}"
exp_init "4.1 quart-vs-art (Fig 9)"
TREES="${TREES:-ART QuART_tail QuART_lil QuART_stail}"
N="$WORKLOAD_N"
exp_meta trees "$TREES"; exp_meta N "$N"
[[ "$TREES" == *QuIT* || "$TREES" == *B+-tree* ]] && ensure_quit_built

PAIRS=("0,0,$WORKLOAD_DIR/workload_N${N}_K0_L0.bin")
for K in 1 3 5 10 25 50 100; do PAIRS+=("$K,1,$WORKLOAD_DIR/workload_N${N}_K${K}_L1.bin"); done
bods_sweep "$RESULTS_DIR/quart_vs_art.csv" "$N" "$TREES" "${PAIRS[@]}"
echo "wrote $RESULTS_DIR/quart_vs_art.csv"
