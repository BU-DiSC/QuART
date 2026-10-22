#!/usr/bin/env bash
# experiments/common.sh — helpers shared by every experiments/*/run.sh.
#
# Source it right after `set -euo pipefail`:
#   source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"   # adjust depth
#   exp_init "4.2-tpch"
#
# It defines the repo paths, the common knobs (REPEAT, JOBS, NUMACTL), the
# results-directory convention, and `run_timed` — the one place where a
# benchmark binary is executed and its exit status checked.
#
# Knobs (environment, all optional):
#   REPEAT       timed repetitions per configuration (each run.sh has its own default)
#   JOBS         parallel make jobs (default: nproc)
#   NUMACTL      prefix for every timed command, e.g. "numactl --cpunodebind=0 --membind=0"
#                or "taskset -c 8"  (default: none)
#   RUN_NAME     name of this run's results directory, <exp>/results/<RUN_NAME>
#                (default "latest"; an existing directory of that name is replaced)
#   RESULTS_DIR  overrides the results directory entirely

EXPERIMENTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$EXPERIMENTS_DIR/.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build}"
QUIT_SRC="$EXPERIMENTS_DIR/4.1-bods"
QUIT_BUILD="${QUIT_BUILD:-$QUIT_SRC/build}"
JOBS="${JOBS:-$(nproc)}"
NUMACTL="${NUMACTL:-}"

# ---------------------------------------------------------------------------
# exp_init NAME — set EXP_DIR (the calling run.sh's directory), RESULTS_DIR
# (<exp>/results/$RUN_NAME, default "latest"), LOG_DIR and start meta.txt.
# Call once, before any timing.  A previous run of the same name is replaced;
# "paper" is reserved for the tracked data behind the paper.
# ---------------------------------------------------------------------------
exp_init() {
    EXP_NAME="$1"
    EXP_DIR="$(cd "$(dirname "${BASH_SOURCE[1]}")" && pwd)"
    RUN_NAME="${RUN_NAME:-latest}"
    [[ "$RUN_NAME" != paper ]] || { echo "RUN_NAME=paper is reserved for the paper's data" >&2; return 1; }
    RESULTS_DIR="${RESULTS_DIR:-$EXP_DIR/results/$RUN_NAME}"
    [[ -d "$RESULTS_DIR" && -z "${RESULTS_DIR_KEEP:-}" ]] && rm -rf "$RESULTS_DIR"
    LOG_DIR="$RESULTS_DIR/logs"
    mkdir -p "$LOG_DIR"
    META="$RESULTS_DIR/meta.txt"
    {
        echo "experiment=$EXP_NAME"
        echo "run=$RUN_NAME"
        echo "git=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        echo "cpu=$(lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1)"
        echo "cores=$(nproc)"
        echo "repeat=${REPEAT:-}"
        echo "numactl=${NUMACTL:-}"
    } > "$META"
    echo "=== $EXP_NAME ==="
    echo "  results : $RESULTS_DIR"
    echo "  repeat  : ${REPEAT:-1}"
    [[ -n "$NUMACTL" ]] && echo "  numactl : $NUMACTL"
    return 0
}

# exp_meta KEY VALUE — record a knob in meta.txt
exp_meta() { echo "$1=$2" >> "$META"; }

# ---------------------------------------------------------------------------
# run_timed LOG CMD... — run one benchmark process under $NUMACTL, append its
# stdout/stderr to LOG, and set RT_STATUS / RT_LAST (last stdout line).
# Returns non-zero when the process failed or printed nothing.
# ---------------------------------------------------------------------------
run_timed() {
    local log="$1"; shift
    local out
    # shellcheck disable=SC2086
    if out=$($NUMACTL "$@" 2>>"$log"); then RT_STATUS=0; else RT_STATUS=$?; fi
    printf '%s\n' "$out" >> "$log"
    RT_LAST="$(printf '%s\n' "$out" | tail -n 1)"
    [[ $RT_STATUS -eq 0 && -n "$RT_LAST" ]]
}

# ---------------------------------------------------------------------------
# avg_cols LOG NCOLS CMD... — run CMD $REPEAT times; print the integer averages
# of the first NCOLS comma-separated columns of each run's last stdout line,
# joined by commas.  Returns 1 (and prints ERROR,...) if any run failed.
# ---------------------------------------------------------------------------
avg_cols() {
    local log="$1" ncols="$2"; shift 2
    local reps="${REPEAT:-1}" i c v
    local -a sums
    for ((c = 0; c < ncols; c++)); do sums[c]=0; done
    for ((i = 1; i <= reps; i++)); do
        echo "--- run $i/$reps: $*" >> "$log"
        if ! run_timed "$log" "$@"; then
            echo "  [WARN] run failed (status $RT_STATUS), see $log" >&2
            local err="ERROR"
            for ((c = 1; c < ncols; c++)); do err+=",ERROR"; done
            echo "$err"
            return 1
        fi
        for ((c = 0; c < ncols; c++)); do
            v="$(printf '%s' "$RT_LAST" | cut -d',' -f$((c + 1)) | tr -d '[:space:]')"
            sums[c]=$(( sums[c] + v ))
        done
    done
    local out=""
    for ((c = 0; c < ncols; c++)); do
        out+="$(( sums[c] / reps ))"
        (( c < ncols - 1 )) && out+=","
    done
    echo "$out"
}

# ---------------------------------------------------------------------------
# bin_for_tree TREE — print the command words that run TREE on one workload
# (append: -f FILE -N N).  QuIT and the B+-tree come from run_quit.cpp and
# take no -t flag; everything else goes through build/run -t.
# ---------------------------------------------------------------------------
bin_for_tree() {
    case "$1" in
        B+-tree) echo "$QUIT_BUILD/bptree" ;;
        QuIT)    echo "$QUIT_BUILD/quit_2k" ;;
        QuIT_4k) echo "$QUIT_BUILD/quit_4k" ;;
        *)       echo "$BUILD/run -t $1" ;;
    esac
}

# ensure_quart_built [BUILD_DIR] [EXTRA CMAKE FLAGS...]
ensure_quart_built() {
    local dir="${1:-$BUILD}"; shift || true
    if [[ ! -x "$dir/run" || ! -x "$dir/test_conc_olc" || ! -x "$dir/test_conc_olc_readwrite" ]]; then
        echo "[build] QuART → $dir $*"
        cmake -S "$REPO_ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Release -Wno-dev "$@" > /dev/null
        make -C "$dir" -j"$JOBS" > /dev/null
    fi
}

# ensure_quit_built — quit_2k / quit_4k / bptree from experiments/4.1-bods
ensure_quit_built() {
    if [[ ! -d "$QUIT_SRC/quick-insertion-tree" ]]; then
        echo "error: $QUIT_SRC/quick-insertion-tree missing; run experiments/setup.sh" >&2
        return 1
    fi
    if [[ ! -x "$QUIT_BUILD/quit_2k" || ! -x "$QUIT_BUILD/quit_4k" || ! -x "$QUIT_BUILD/bptree" ]]; then
        echo "[build] QuIT / B+-tree → $QUIT_BUILD"
        cmake -S "$QUIT_SRC" -B "$QUIT_BUILD" -DCMAKE_BUILD_TYPE=Release -Wno-dev > /dev/null
        make -C "$QUIT_BUILD" -j"$JOBS" > /dev/null
    fi
}

# require_file PATH — true if PATH exists, else print [SKIP] and return 1
require_file() {
    [[ -f "$1" ]] && return 0
    echo "  [SKIP] missing workload: $1" >&2
    return 1
}
