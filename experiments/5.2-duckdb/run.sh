#!/usr/bin/env bash
# §5.2, Figure 16 — CREATE INDEX in DuckDB with QuART grouped bulk loading vs
# DuckDB's loader: per-thread, per-key index-build speedup over key-set size
# (5M..500M sorted 32-bit integers) × thread count (1..24).
# Drives systems/duckdb/benchmark/quart_grouped_bulk_load/bench_paper.sh.
#
# Usage:  bash experiments/5.2-duckdb/run.sh
# Env:    DUCKDB_BIN (default systems/duckdb/build/release/duckdb; built with
#         `GEN=ninja make release` if missing — about an hour), SIZES (default
#         "5000000 20000000 100000000 200000000 500000000"; 500M needs ~20 GB),
#         THREADS (default "1 2 4 8 16 24"), REPEAT (3), PIN (numactl prefix
#         for duckdb; default: NUMACTL from common.sh, else socket 0 if numactl
#         exists; PIN="" disables), SKIP_BUILD=1
# Output: results/<ts>/profile.csv, timing.csv  (bench_paper.sh schemas)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
REPEAT="${REPEAT:-3}"
exp_init "5.2 duckdb CREATE INDEX (Fig 16)"
DUCK="$REPO_ROOT/systems/duckdb"
BENCH="$DUCK/benchmark/quart_grouped_bulk_load"
DUCKDB_BIN="${DUCKDB_BIN:-$DUCK/build/release/duckdb}"
SIZES="${SIZES:-5000000 20000000 100000000 200000000 500000000}"
THREADS="${THREADS:-1 2 4 8 16 24}"
if [[ -z "${PIN+x}" ]]; then
    PIN="$NUMACTL"
    [[ -z "$PIN" ]] && command -v numactl > /dev/null && PIN="numactl --cpunodebind=0 --membind=0"
fi
exp_meta sizes "$SIZES"; exp_meta threads "$THREADS"; exp_meta pin "$PIN"; exp_meta duckdb_bin "$DUCKDB_BIN"
[[ -f "$BENCH/bench_paper.sh" ]] || { echo "error: $BENCH missing — run 'git submodule update --init systems/duckdb', or unpack the DuckDB fork from the link in README.md into systems/duckdb" >&2; exit 1; }

if [[ ! -x "$DUCKDB_BIN" && "${SKIP_BUILD:-0}" != 1 ]]; then
    echo "[build] DuckDB release → $DUCK/build/release (this takes a while)"
    ( cd "$DUCK" && GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL="$JOBS" make release ) > "$LOG_DIR/duckdb_build.log" 2>&1
fi
[[ -x "$DUCKDB_BIN" ]] || { echo "error: no duckdb binary at $DUCKDB_BIN" >&2; exit 1; }

WORKLOADS=""
for n in $SIZES; do
    WORKLOADS+="sorted_int32_$((n / 1000000))M|CREATE TABLE t AS SELECT range::INTEGER AS k FROM range($n);"$'\n'
done
echo "$WORKLOADS" > "$RESULTS_DIR/workloads.txt"
# shellcheck disable=SC2086
WORKLOADS="$WORKLOADS" PIN="$PIN" bash "$BENCH/bench_paper.sh" "$DUCKDB_BIN" "$RESULTS_DIR" "$REPEAT" $THREADS
echo "wrote $RESULTS_DIR/profile.csv"
python3 "$EXP_DIR/plot.py" --results "$RESULTS_DIR"
