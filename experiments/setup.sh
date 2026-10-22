#!/usr/bin/env bash
# experiments/setup.sh — one-time bootstrap for the QuART artifact (idempotent).
#
#   1. Clone + build the BoDS workload generator (pinned commit)
#   2. Generate the BoDS workload .bin files the experiments read
#   3. Clone quick-insertion-tree (QuIT / B+-tree baselines, pinned commit)
#   4. Build QuART (build/run, build/test_conc_olc, build/test_conc_olc_readwrite)
#
# Every experiments/*/run.sh sources this script, so you never need to run it
# by hand; `bash experiments/setup.sh` just does the bootstrap ahead of time.
#
# Environment variables (all optional):
#   WORKLOAD_DIR   where the .bin workloads live (default: experiments/bods/workloads).
#                  Point it at pre-generated files to skip generation.
#   WORKLOAD_N     keys per BoDS workload (default: 500000000 = the paper; use a
#                  small value such as 1000000 for a smoke test — file names
#                  carry N, so sizes never collide)
#   GEN_WORKLOADS  0 = skip the K/L grid + TPC-H generation (§4.4 does this and
#                  generates its own small per-stream files)
#   BODS_DIR       where to clone BoDS (default: experiments/bods)
#   SKIP_BUILD     1 = do not (re)build QuART
#   JOBS           parallel make jobs (default: nproc)
#
# !!! DISK: at the paper's WORKLOAD_N=500M each file is ~1.9 GB; the grid is
# !!! 28 files ≈ 53 GB.  The TPC-H file (N=6M) is 23 MB.

set -euo pipefail

# Sourced twice in one shell (run.sh → setup.sh, sweep helpers…)? Do nothing.
if [[ -n "${QUART_SETUP_DONE:-}" ]]; then return 0 2>/dev/null || exit 0; fi

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# Third-party tools, pinned to the commits used for the paper: BoDS (the
# sortedness benchmark's workload generator) and the quick-insertion-tree (the
# QuIT and B+-tree baselines), the published implementations the paper cites.
BODS_REPO="https://github.com/BU-DiSC/bods.git"
BODS_COMMIT="4df9b758c5b73d89e9f56f647a9dc9e2f79e0025"
QUIT_REPO="https://github.com/BU-DiSC/quick-insertion-tree.git"
QUIT_COMMIT="f1c144ad13952bdf7a74b90a9a64b8aacf7b879b"

BODS_DIR="${BODS_DIR:-$EXPERIMENTS_DIR/bods}"
QUIT_DIR="$QUIT_SRC/quick-insertion-tree"
WORKLOAD_N="${WORKLOAD_N:-500000000}"
: "${WORKLOAD_DIR:=$BODS_DIR/workloads}"
mkdir -p "$WORKLOAD_DIR"

echo "=== QuART artifact setup ==="
echo "  REPO_ROOT    : $REPO_ROOT"
echo "  WORKLOAD_DIR : $WORKLOAD_DIR"
echo "  WORKLOAD_N   : $WORKLOAD_N"

# clone_pinned URL DIR COMMIT
clone_pinned() {
    local url="$1" dir="$2" commit="$3"
    if [[ ! -d "$dir/.git" ]]; then
        echo "[setup] cloning $url"
        git clone --quiet "$url" "$dir"
    fi
    if [[ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ]]; then
        git -C "$dir" fetch --quiet origin "$commit" 2>/dev/null || git -C "$dir" fetch --quiet origin
        git -C "$dir" checkout --quiet --detach "$commit"
    fi
}

# ---------------------------------------------------------------------------
# 1. BoDS
# ---------------------------------------------------------------------------
clone_pinned "$BODS_REPO" "$BODS_DIR" "$BODS_COMMIT"
BODS_BUILD="$BODS_DIR/build"
GENERATOR="$BODS_BUILD/sortedness_data_generator"
if [[ ! -x "$GENERATOR" ]]; then
    echo "[setup] building BoDS"
    cmake -S "$BODS_DIR" -B "$BODS_BUILD" -DCMAKE_BUILD_TYPE=Release -Wno-dev > /dev/null
    make -C "$BODS_BUILD" -j"$JOBS" > /dev/null
fi
export GENERATOR BODS_DIR

# ---------------------------------------------------------------------------
# 2. Workloads.  generate_workload N K L [START] writes
#    $WORKLOAD_DIR/workload_N<N>_K<K>_L<L>[_start<START>].bin  (skipped if present)
# ---------------------------------------------------------------------------
generate_workload() {
    local n="$1" k="$2" l="$3" start="${4:-0}"
    local out="$WORKLOAD_DIR/workload_N${n}_K${k}_L${l}.bin"
    [[ "$start" != "0" ]] && out="$WORKLOAD_DIR/workload_N${n}_K${k}_L${l}_start${start}.bin"
    [[ -f "$out" ]] && return 0
    echo "[setup] generating N=$n K=$k L=$l start=$start"
    local toml; toml="$(mktemp --suffix=.toml)"
    cat > "$toml" <<TOML
title = "QuART workload"

[global]
domain = ${n}

[[partition]]
start_index = ${start}
number_of_entries = ${n}
K = ${k}
L = ${l}
seed = 1234
alpha = 1
beta = 1
payload = 0
window_size = 1
output_file = "${out}"
is_fixed = false
is_binary = true
reverse_order = true
TOML
    "$GENERATOR" --toml_file "$toml" > /dev/null
    rm -f "$toml"
}

if [[ "${GEN_WORKLOADS:-1}" == "1" ]]; then
    # §4.1 / §3.3 / §5.3: K=L=0, the K×L grid, and the two extra K points of Fig 9
    generate_workload "$WORKLOAD_N" 0 0
    for k_ in 1 5 10 25 100; do for l_ in 1 5 10 25 100; do
        generate_workload "$WORKLOAD_N" "$k_" "$l_"
    done; done
    unset k_ l_
    generate_workload "$WORKLOAD_N" 3 1
    generate_workload "$WORKLOAD_N" 50 1
    # §4.2: TPC-H-inspired lineitem order, N=6M, K≈96.67 L≈0.1 (file name K9667_L01)
    TPCH_FILE="$WORKLOAD_DIR/workload_N6000000_K9667_L01.bin"
    if [[ ! -f "$TPCH_FILE" ]]; then
        echo "[setup] generating TPC-H workload (N=6M, K=96.67, L=0.1)"
        toml="$(mktemp --suffix=.toml)"
        sed "s|^K = .*|K = 96.67|; s|^L = .*|L = 0.1|" <<TOML > "$toml"
title = "QuART TPC-H workload"

[global]
domain = 6000000

[[partition]]
start_index = 0
number_of_entries = 6000000
K = 0
L = 0
seed = 1234
alpha = 1
beta = 1
payload = 0
window_size = 1
output_file = "${TPCH_FILE}"
is_fixed = false
is_binary = true
reverse_order = true
TOML
        "$GENERATOR" --toml_file "$toml" > /dev/null
        rm -f "$toml"
    fi
else
    echo "[setup] GEN_WORKLOADS=0: skipping K/L grid + TPC-H generation"
fi
export WORKLOAD_DIR WORKLOAD_N

# ---------------------------------------------------------------------------
# 3. quick-insertion-tree (QuIT + B+-tree sources; built by ensure_quit_built)
# ---------------------------------------------------------------------------
clone_pinned "$QUIT_REPO" "$QUIT_DIR" "$QUIT_COMMIT"

# ---------------------------------------------------------------------------
# 4. QuART
# ---------------------------------------------------------------------------
[[ "${SKIP_BUILD:-0}" == "1" ]] || ensure_quart_built "$BUILD"

export QUART_SETUP_DONE=1
echo "[setup] done"
