#!/usr/bin/env bash
# §4.1, Figure 10 — QuART-stail vs ART insert and query speedup over the full
# K×L sortedness grid (BoDS N=500M; K,L in {1,5,10,25,100} plus K=L=0).
#
# Usage:  bash experiments/4.1-bods/stail-vs-art/run.sh
# Env:    WORKLOAD_DIR, WORKLOAD_N, REPEAT (default 1), TREES (default
#         "ART QuART_stail"), KS / LS (default "1 5 10 25 100"), NUMACTL
# Output: results/<ts>/stail_vs_art.csv   (schema S1, see sweep.sh)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../common.sh"
source "$EXPERIMENTS_DIR/setup.sh"
source "$QUIT_SRC/sweep.sh"
REPEAT="${REPEAT:-1}"
exp_init "4.1 stail-vs-art (Fig 10)"
TREES="${TREES:-ART QuART_stail}"
N="$WORKLOAD_N"
exp_meta trees "$TREES"; exp_meta N "$N"

mapfile -t PAIRS < <(grid_pairs "$N" "${KS:-1 5 10 25 100}" "${LS:-1 5 10 25 100}")
bods_sweep "$RESULTS_DIR/stail_vs_art.csv" "$N" "$TREES" "${PAIRS[@]}"
echo "wrote $RESULTS_DIR/stail_vs_art.csv"
