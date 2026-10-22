#!/usr/bin/env bash
# Experiment 4.3 (§4.3): Bulk Loading Performance
#
# Compares standard repeated ART insertion against QuART grouped bulk loading
# for increasing numbers of sorted keys (100M to 2B).  Output corresponds to
# Figure 13 in the paper: insertion time vs. number of elements.
#
# Both regular and bulk-load paths generate sequential keys 1..N synthetically
# (no workload file required), guaranteeing a fully sorted input.
#
# Usage:
#   bash experiments/4.3-bulkload/run.sh
#
# Environment variables (all optional):
#   REPEAT         – number of timed repetitions per configuration (default: 5)
#
# Output:
#   experiments/4.3-bulkload/results/results_<TIMESTAMP>.csv
#
# CSV columns:
#   N, insertion_method, avg_insert_ns, avg_query_ns
#
#   insertion_method: "regular" or "bulkload"
#   Speedup (Figure 13) = regular_time / bulkload_time at each N.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

# Bootstrap: clone deps, generate workloads, build binaries (idempotent)
source "$SCRIPT_DIR/../setup.sh"

REPEAT="${REPEAT:-5}"

# Test sizes matching Figure 13 (100M to 2B)
TEST_SIZES=(100000000 250000000 500000000 750000000 1000000000 1500000000 2000000000)

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTS_DIR="$SCRIPT_DIR/results"
RESULTS_FILE="$RESULTS_DIR/results_${SUFFIX}.csv"
LOG_DIR="$RESULTS_DIR/logs_${SUFFIX}"

mkdir -p "$RESULTS_DIR" "$LOG_DIR"

echo "Experiment 4.3 — Bulk Loading Performance"
echo "  REPEAT        : $REPEAT"
echo "  Results       : $RESULTS_FILE"
echo "  Logs          : $LOG_DIR"
echo ""

# ---------------------------------------------------------------------------
# CSV header
# ---------------------------------------------------------------------------
echo "N,insertion_method,avg_insert_ns,avg_query_ns" > "$RESULTS_FILE"

# ---------------------------------------------------------------------------
# Helper: run a configuration REPEAT times and append average to CSV
# ---------------------------------------------------------------------------
run_config() {
    local N_VAL="$1" METHOD="$2"
    local EXTRA_ARGS="--synthetic"
    [[ "$METHOD" == "bulkload" ]] && EXTRA_ARGS="--synthetic --bulkload"

    local LOG="$LOG_DIR/N${N_VAL}_${METHOD}.log"
    local INSERT_SUM=0 QUERY_SUM=0 FAILED=0

    echo "=== N=$N_VAL  method=$METHOD ===" > "$LOG"

    for ((i=1; i<=REPEAT; i++)); do
        echo "--- Run $i/$REPEAT ---" >> "$LOG"
        # shellcheck disable=SC2086
        OUTPUT=$("$BUILD/run" -N "$N_VAL" -t ART $EXTRA_ARGS 2>>"$LOG") || true
        STATUS=$?
        echo "$OUTPUT" >> "$LOG"

        if [[ $STATUS -ne 0 || -z "$OUTPUT" ]]; then
            echo "  [WARN] run failed: N=$N_VAL method=$METHOD run=$i" >&2
            FAILED=1
            break
        fi

        CSV_LINE=$(echo "$OUTPUT" | tail -1)
        INSERT_SUM=$((INSERT_SUM + $(echo "$CSV_LINE" | cut -d',' -f1 | xargs)))
        QUERY_SUM=$((QUERY_SUM  + $(echo "$CSV_LINE" | cut -d',' -f2 | xargs)))
    done

    if [[ $FAILED -eq 0 ]]; then
        AVG_INS=$((INSERT_SUM / REPEAT))
        AVG_QRY=$((QUERY_SUM  / REPEAT))
        echo "$N_VAL,$METHOD,$AVG_INS,$AVG_QRY" >> "$RESULTS_FILE"
        printf "  avg_insert=%dns  avg_query=%dns\n" "$AVG_INS" "$AVG_QRY"
    else
        echo "$N_VAL,$METHOD,ERROR,ERROR" >> "$RESULTS_FILE"
    fi
}

# ---------------------------------------------------------------------------
# Run for each test size
# ---------------------------------------------------------------------------
for N in "${TEST_SIZES[@]}"; do
    echo ">>> N=$N"

    echo "  [regular]"
    run_config "$N" "regular"

    # Bulk load uses grouped insertion on the same synthetic key sequence
    echo "  [bulkload]"
    run_config "$N" "bulkload"
done

echo ""
echo "Done. Results saved to $RESULTS_FILE"
