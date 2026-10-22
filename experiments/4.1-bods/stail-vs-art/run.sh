#!/usr/bin/env bash
# Experiment: ART vs QuART_stail over the full K-L sortedness grid (N=500M).
#
# Usage:
#   bash experiments/4.1-bods/stail-vs-art/run.sh
#
# Environment variables (all optional):
#   WORKLOAD_DIR  – directory containing BoDS workload .bin files
#   REPEAT        – number of timed repetitions per configuration (default: 1)
#
# Output:
#   experiments/4.1-bods/stail-vs-art/results/results_<TIMESTAMP>.csv
#
# CSV columns:
#   workload, K, L, tree_type, avg_insert_ns, avg_query_ns

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PARENT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"
JOBS="${JOBS:-$(nproc)}"

source "$REPO_ROOT/experiments/setup.sh"

N=500000000
REPEAT="${REPEAT:-1}"

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTS_DIR="$SCRIPT_DIR/results"
RESULTS_FILE="$RESULTS_DIR/results_${SUFFIX}.csv"
LOG_DIR="$RESULTS_DIR/logs_${SUFFIX}"

mkdir -p "$RESULTS_DIR" "$LOG_DIR"

echo "Experiment: ART vs QuART_stail (full K-L grid)"
echo "  WORKLOAD_DIR : $WORKLOAD_DIR"
echo "  REPEAT       : $REPEAT"
echo "  Results      : $RESULTS_FILE"
echo "  Logs         : $LOG_DIR"
echo ""

echo "workload,K,L,tree_type,avg_insert_ns,avg_query_ns" > "$RESULTS_FILE"

TREES=(ART QuART_stail)

run_config() {
    local FILE="$1" N_VAL="$2" K_VAL="$3" L_VAL="$4" TREE="$5"
    local WNAME LOG INSERT_SUM QUERY_SUM FAILED
    WNAME="$(basename "$FILE" .bin)"
    LOG="$LOG_DIR/${WNAME}_${TREE}.log"
    INSERT_SUM=0; QUERY_SUM=0; FAILED=0

    echo "=== workload=$WNAME  tree=$TREE ===" > "$LOG"

    for ((i=1; i<=REPEAT; i++)); do
        echo "--- Run $i/$REPEAT ---" >> "$LOG"
        OUTPUT=$("$BUILD/run" -f "$FILE" -N "$N_VAL" -t "$TREE" 2>>"$LOG") || true
        STATUS=$?
        echo "$OUTPUT" >> "$LOG"
        if [[ $STATUS -ne 0 || -z "$OUTPUT" ]]; then
            echo "  [WARN] run failed: K=$K_VAL L=$L_VAL tree=$TREE run=$i" >&2
            FAILED=1; continue
        fi
        CSV_LINE=$(echo "$OUTPUT" | tail -1)
        INSERT_SUM=$((INSERT_SUM + $(echo "$CSV_LINE" | cut -d',' -f1 | xargs)))
        QUERY_SUM=$((QUERY_SUM  + $(echo "$CSV_LINE" | cut -d',' -f2 | xargs)))
    done

    if [[ $FAILED -eq 0 ]]; then
        AVG_INS=$((INSERT_SUM / REPEAT))
        AVG_QRY=$((QUERY_SUM  / REPEAT))
        echo "$WNAME,$K_VAL,$L_VAL,$TREE,$AVG_INS,$AVG_QRY" >> "$RESULTS_FILE"
        printf "  %-15s  avg_insert=%dns  avg_query=%dns\n" "$TREE" "$AVG_INS" "$AVG_QRY"
    else
        echo "$WNAME,$K_VAL,$L_VAL,$TREE,ERROR,ERROR" >> "$RESULTS_FILE"
        printf "  %-15s  [ERROR]\n" "$TREE" >&2
    fi
}

# K=0, L=0
FILE="$WORKLOAD_DIR/workload_N${N}_K0_L0.bin"
if [[ -f "$FILE" ]]; then
    echo ">>> K=0  L=0  (fully sorted)"
    for TREE in "${TREES[@]}"; do run_config "$FILE" "$N" 0 0 "$TREE"; done
else
    echo "[SKIP] Missing: $FILE" >&2
fi

# K ∈ {1,5,10,25,100} × L ∈ {1,5,10,25,100}
for K in 1 5 10 25 100; do
    for L in 1 5 10 25 100; do
        FILE="$WORKLOAD_DIR/workload_N${N}_K${K}_L${L}.bin"
        if [[ -f "$FILE" ]]; then
            echo ">>> K=$K  L=$L"
            for TREE in "${TREES[@]}"; do run_config "$FILE" "$N" "$K" "$L" "$TREE"; done
        else
            echo "[SKIP] Missing: $FILE" >&2
        fi
    done
done

echo ""
echo "Done. Results saved to $RESULTS_FILE"
