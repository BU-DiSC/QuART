#!/usr/bin/env bash
# §4.1, Figure 11 — QuART-stail vs QuIT insert and query speedup over the full
# K×L sortedness grid (BoDS N=500M).  QuIT is the quick-insertion-tree with
# 2 KB nodes (quit_2k); set TREES="QuART_stail QuIT QuIT_4k" to add 4 KB nodes.
#
# Usage:  bash experiments/4.1-bods/stail-vs-quit/run.sh
# Env:    WORKLOAD_DIR, WORKLOAD_N, REPEAT (default 1), TREES (default
#         "QuART_stail QuIT"), KS / LS, NUMACTL
# Output: results/<ts>/stail_vs_quit.csv   (schema S1, see sweep.sh)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../common.sh"
source "$EXPERIMENTS_DIR/setup.sh"
source "$QUIT_SRC/sweep.sh"
REPEAT="${REPEAT:-1}"
exp_init "4.1 stail-vs-quit (Fig 11)"
TREES="${TREES:-QuART_stail QuIT}"
N="$WORKLOAD_N"
exp_meta trees "$TREES"; exp_meta N "$N"
ensure_quit_built

mapfile -t PAIRS < <(grid_pairs "$N" "${KS:-1 5 10 25 100}" "${LS:-1 5 10 25 100}")
bods_sweep "$RESULTS_DIR/stail_vs_quit.csv" "$N" "$TREES" "${PAIRS[@]}"
echo "wrote $RESULTS_DIR/stail_vs_quit.csv"
