#!/usr/bin/env bash
# §5.3, Figure 17 and the in-server stream result — rax vs QuART-rax
# (systems/redis).
#   sweep:     test-standalone/test_quart_redis.sh — inserts BoDS N=500M keys,
#              K=L in {0,1,5,10,25,100}, into rax and QuART-rax, verifies and
#              looks up every key.                                  → Fig 17
#   in-server: test-redis/ab_bulk_insert.sh — 500M sorted stream appends with
#              DEBUG STREAM-BULK-INSERT, stock vs QuART-rax (group depth 7),
#              median of 3.                                         → "1.08x" in §5.3
#
# Usage:  bash experiments/5.3-redis/run.sh
# Env:    WORKLOAD_DIR, WORKLOAD_N (default 500000000), N (keys inserted,
#         default WORKLOAD_N), KS (default "0 1 5 10 25 100"), REPS (default 1;
#         the paper averaged 2), INSERVER (default 1; 0 skips the server A/B),
#         INSERVER_REPS (3), PORT (7410), SKIP_BUILD=1
# Output: results/<ts>/redis_standalone.csv, redis_inserver.csv

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
source "$EXPERIMENTS_DIR/setup.sh"
exp_init "5.3 redis (Fig 17)"
REDIS="$REPO_ROOT/systems/redis"
N="${N:-$WORKLOAD_N}"; KS="${KS:-0 1 5 10 25 100}"; REPS="${REPS:-1}"
exp_meta N "$N"; exp_meta ks "$KS"; exp_meta reps "$REPS"; exp_meta inserver "${INSERVER:-1}"
[[ -f "$REDIS/src/rax_quart.c" ]] || { echo "error: $REDIS missing — run 'git submodule update --init systems/redis', or unpack the Redis fork from the link in README.md into systems/redis" >&2; exit 1; }

if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
    echo "[build] redis + standalone harness"
    make -s -j"$JOBS" -C "$REDIS" > "$LOG_DIR/redis_build.log" 2>&1
    make -s -C "$REDIS/src" test-rax-quart-workloads >> "$LOG_DIR/redis_build.log" 2>&1
fi

echo ">>> standalone sweep (K=L in $KS, N=$N)"
WORKLOAD_DIR="$WORKLOAD_DIR" WORKLOAD_N="$WORKLOAD_N" N="$N" KS="$KS" REPS="$REPS" \
    OUT="$RESULTS_DIR/redis_standalone.csv" bash "$REDIS/test-standalone/test_quart_redis.sh" 2>&1 | tee "$LOG_DIR/standalone.log" | grep -E '^rep|^[0-9]' || true

if [[ "${INSERVER:-1}" == 1 ]]; then
    echo ">>> in-server stream appends (N=$N, ${INSERVER_REPS:-3} reps)"
    WORKLOAD="$WORKLOAD_DIR/workload_N${WORKLOAD_N}_K0_L0.bin" WORK="$RESULTS_DIR/redis_work" PORT="${PORT:-7410}" \
        bash "$REDIS/test-redis/ab_bulk_insert.sh" "$N" "${INSERVER_REPS:-3}" "$RESULTS_DIR/redis_inserver.csv" off 7 2>&1 | tee "$LOG_DIR/inserver.log" || true
    rm -rf "$RESULTS_DIR/redis_work"
fi
python3 "$EXP_DIR/plot.py" --results "$RESULTS_DIR"
