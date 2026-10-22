#!/usr/bin/env bash
# §5.1, Table 2 — PostgreSQL VACUUM capture-and-replay (systems/postgres/quart_bench).
#
# 1. builds the QuART-enabled PostgreSQL in systems/postgres (prefix PG_PREFIX),
# 2. initdb + starts a private server on PGPORT,
# 3. captures the VACUUM key stream of workloads W1..W9 at each scale in
#    PG_ROWS with quart_bench/capture.sh (the paper: 250M, 1B, 5B rows — the 5B
#    tables need ~300 GB of disk and hours; shrink PG_ROWS to try it out),
# 4. builds quart_bench/replay and replays every stream with run_replay.sh.
#
# Usage:  bash experiments/5.1-postgres/run.sh
# Env:    PG_ROWS (default "250000000 1000000000 5000000000"), KEYBIN_DIR
#         (default $RESULTS_DIR/keybins; if it already holds vackeys_*.bin files
#         the capture step is skipped), PGPORT (54329), PGDATA (default
#         $RESULTS_DIR/pgdata, removed afterwards unless KEEP_PGDATA=1),
#         PG_PREFIX (default systems/postgres/install), PG_CONFIGURE_FLAGS
#         (default "--without-readline"; add e.g. --without-icu if configure
#         asks for it), REPS (10), TRIALS (3), SKIP_BUILD=1, WORKLOADS (subset
#         of W1..W9 names for capture.sh)
# Output: results/<ts>/replay.csv   (run_replay.sh schema; table.py prints Table 2)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
exp_init "5.1 postgres VACUUM (Table 2)"
PG_SRC="$REPO_ROOT/systems/postgres"
PG_PREFIX="${PG_PREFIX:-$PG_SRC/install}"
PG_ROWS="${PG_ROWS:-250000000 1000000000 5000000000}"
KEYBIN_DIR="${KEYBIN_DIR:-$RESULTS_DIR/keybins}"
export PGPORT="${PGPORT:-54329}" PGHOST=localhost PGDATABASE=postgres
PGDATA="${PGDATA:-$RESULTS_DIR/pgdata}"
exp_meta pg_rows "$PG_ROWS"; exp_meta keybin_dir "$KEYBIN_DIR"; exp_meta reps "${REPS:-10}"; exp_meta trials "${TRIALS:-3}"
[[ -f "$PG_SRC/configure" ]] || { echo "error: $PG_SRC is empty — run 'git submodule update --init systems/postgres', or unpack the PostgreSQL fork from the link in README.md there" >&2; exit 1; }

if [[ "${SKIP_BUILD:-0}" != 1 && ! -x "$PG_PREFIX/bin/postgres" ]]; then
    echo "[build] PostgreSQL → $PG_PREFIX"
    # shellcheck disable=SC2086
    ( cd "$PG_SRC" && ./configure --prefix="$PG_PREFIX" --quiet ${PG_CONFIGURE_FLAGS:---without-readline} \
        && make -s -j"$JOBS" && make -s install ) > "$LOG_DIR/pg_build.log" 2>&1 \
        || { echo "error: PostgreSQL build failed, see $LOG_DIR/pg_build.log" >&2; exit 1; }
fi
PATH="$PG_PREFIX/bin:$PATH"

if compgen -G "$KEYBIN_DIR/vackeys_*rows*.bin" > /dev/null; then
    echo "[capture] $KEYBIN_DIR already holds key streams; skipping capture"
else
    [[ -d "$PGDATA" ]] || initdb -D "$PGDATA" > "$LOG_DIR/initdb.log" 2>&1
    pg_ctl -D "$PGDATA" -l "$LOG_DIR/postgres.log" -o "-p $PGPORT -c shared_buffers=4GB" -w start > /dev/null
    cleanup() { pg_ctl -D "$PGDATA" -m fast stop > /dev/null 2>&1 || true; [[ "${KEEP_PGDATA:-0}" == 1 ]] || rm -rf "$PGDATA"; }
    trap cleanup EXIT
    for rows in $PG_ROWS; do
        echo "[capture] $rows rows"
        # shellcheck disable=SC2086
        bash "$PG_SRC/quart_bench/capture.sh" -n "$rows" -o "$KEYBIN_DIR" ${WORKLOADS:-} 2>&1 | tee -a "$LOG_DIR/capture_${rows}.log" | grep -E 'vackeys|error' || true
    done
fi

make -s -C "$PG_SRC/quart_bench" > "$LOG_DIR/replay_build.log" 2>&1
REPS="${REPS:-10}" TRIALS="${TRIALS:-3}" bash "$PG_SRC/quart_bench/run_replay.sh" "$KEYBIN_DIR" "$RESULTS_DIR/replay.csv" 2>&1 | tee "$LOG_DIR/replay.log" | grep -v '^\[' || true
echo "wrote $RESULTS_DIR/replay.csv"
python3 "$EXP_DIR/table.py" --results "$RESULTS_DIR"
