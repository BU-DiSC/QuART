#!/usr/bin/env bash
# §3.3, Figure 6 — fast-path vs root inserts for QuART-tail / -lil / -stail
# as sortedness degrades (BoDS, N=500M).
#
# Uses a separate build with the QUART_FP_STATS counter (build-fpstats/), in
# which `run` prints a third column: the number of inserts that started at the
# fast pointer instead of the root.  Counts are deterministic, so each
# configuration runs once (REPEAT is ignored).
#
# Usage:  bash experiments/3.3-insert-types/run.sh
# Env:    WORKLOAD_DIR, WORKLOAD_N (default 500000000), TREES, K_VALUES
#         (default "0 1 5 10 25 100"), L (default 1; K=0 always uses L=0)
# Output: results/<ts>/bods_insert_types.csv
#         workload,N,K,L,tree_type,fp_inserts,root_inserts

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
GEN_WORKLOADS="${GEN_WORKLOADS:-1}" source "$EXPERIMENTS_DIR/setup.sh"
exp_init "3.3-insert-types (Fig 6)"

TREES="${TREES:-QuART_tail QuART_lil QuART_stail}"
K_VALUES="${K_VALUES:-0 1 5 10 25 100}"
L="${L:-1}"
N="$WORKLOAD_N"
FPBUILD="$REPO_ROOT/build-fpstats"
exp_meta trees "$TREES"; exp_meta k_values "$K_VALUES"; exp_meta L "$L"; exp_meta N "$N"

if [[ ! -x "$FPBUILD/run" ]]; then
    echo "[build] QuART with -DQUART_FP_STATS=ON → $FPBUILD"
    cmake -S "$REPO_ROOT" -B "$FPBUILD" -DCMAKE_BUILD_TYPE=Release -DQUART_FP_STATS=ON -Wno-dev > /dev/null
    make -C "$FPBUILD" -j"$JOBS" run > /dev/null
fi

OUT="$RESULTS_DIR/bods_insert_types.csv"
echo "workload,N,K,L,tree_type,fp_inserts,root_inserts" > "$OUT"
for K in $K_VALUES; do
    l="$L"; [[ "$K" == "0" ]] && l=0
    FILE="$WORKLOAD_DIR/workload_N${N}_K${K}_L${l}.bin"
    require_file "$FILE" || continue
    wname="$(basename "$FILE" .bin)"
    echo ">>> K=$K L=$l"
    for tree in $TREES; do
        if run_timed "$LOG_DIR/${wname}_${tree}.log" "$FPBUILD/run" -t "$tree" -f "$FILE" -N "$N"; then
            fp="$(printf '%s' "$RT_LAST" | cut -d, -f3)"
            echo "$wname,$N,$K,$l,$tree,$fp,$((N - fp))" >> "$OUT"
            printf '  %-12s fp_inserts=%s\n' "$tree" "$fp"
        else
            echo "$wname,$N,$K,$l,$tree,ERROR,ERROR" >> "$OUT"
        fi
    done
done
echo "wrote $OUT"
