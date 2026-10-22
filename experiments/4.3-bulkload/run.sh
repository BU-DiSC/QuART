#!/usr/bin/env bash
# §4.3, Figure 13 — repeated ART insertion vs QuART grouped bulk loading on
# fully sorted keys 1..N, for N from 100M to 2B (synthetic, no workload file).
#
# Usage:  bash experiments/4.3-bulkload/run.sh
# Env:    SIZES (default "100000000 250000000 500000000 750000000 1000000000
#         1500000000 2000000000"), REPEAT (default 5), NUMACTL
#         The 2B point needs roughly 40 GB of RAM.
# Output: results/<ts>/bulkload.csv
#         N,insertion_method,avg_insert_ns,avg_query_ns   (regular | bulkload)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
GEN_WORKLOADS=0 source "$EXPERIMENTS_DIR/setup.sh"
REPEAT="${REPEAT:-5}"
exp_init "4.3 bulkload (Fig 13)"
SIZES="${SIZES:-100000000 250000000 500000000 750000000 1000000000 1500000000 2000000000}"
exp_meta sizes "$SIZES"

OUT="$RESULTS_DIR/bulkload.csv"
echo "N,insertion_method,avg_insert_ns,avg_query_ns" > "$OUT"
for n in $SIZES; do
    echo ">>> N=$n"
    for method in regular bulkload; do
        extra=(--synthetic); [[ "$method" == bulkload ]] && extra+=(--bulkload)
        avg="$(avg_cols "$LOG_DIR/N${n}_${method}.log" 2 "$BUILD/run" -N "$n" -t ART "${extra[@]}")" || true
        echo "$n,$method,$avg" >> "$OUT"
        printf '  %-9s %s\n' "$method" "$avg"
    done
done
echo "wrote $OUT"
