#!/usr/bin/env bash
# experiments/setup.sh
#
# Bootstrap script for the Qu-ART artifact.
# Run once (or call from any experiment run.sh) to:
#   1. Clone and build BoDS workload generator (https://github.com/BU-DiSC/bods)
#   2. Generate all workload .bin files needed by the experiments
#   3. Clone the quick-insertion-tree dependency into experiments/4.1-bods/
#   4. Build the QuART project (CMake)
#
# Already-done steps are skipped automatically (idempotent).
#
# Usage:
#   bash experiments/setup.sh          # run standalone
#   source experiments/setup.sh        # source from a run.sh (inherits WORKLOAD_DIR etc.)
#
# Environment variables (all optional):
#   BODS_DIR   – where to clone/find BoDS  (default: experiments/bods)
#   JOBS       – parallel make jobs        (default: nproc)

set -euo pipefail

EXPERIMENTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$EXPERIMENTS_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"
BODS_DIR="${BODS_DIR:-$EXPERIMENTS_DIR/bods}"
QUIT_DIR="$EXPERIMENTS_DIR/4.1-bods/quick-insertion-tree"
BODS_EXP_QUIT_DIR="$EXPERIMENTS_DIR/4.1-bods/quick-insertion-tree"
JOBS="${JOBS:-$(nproc)}"

echo "=== Qu-ART artifact setup ==="
echo "  REPO_ROOT : $REPO_ROOT"
echo "  BODS_DIR  : $BODS_DIR"
echo "  BUILD     : $BUILD"
echo ""

# ---------------------------------------------------------------------------
# 1. Set workload directory default
# ---------------------------------------------------------------------------
: "${WORKLOAD_DIR:=$BODS_DIR/workloads}"
mkdir -p "$WORKLOAD_DIR"

# ---------------------------------------------------------------------------
# 2. Clone and build BoDS (only if the generator binary is not yet present)
# ---------------------------------------------------------------------------
BODS_BUILD="$BODS_DIR/build"
GENERATOR="$BODS_BUILD/sortedness_data_generator"

if [[ ! -x "$GENERATOR" ]]; then
    if [[ ! -d "$BODS_DIR" ]]; then
        echo "[setup] Cloning BoDS..."
        git clone https://github.com/BU-DiSC/bods.git "$BODS_DIR"
    else
        echo "[setup] BoDS already present at $BODS_DIR"
    fi
    echo "[setup] Building BoDS..."
    mkdir -p "$BODS_BUILD"
    cmake -S "$BODS_DIR" -B "$BODS_BUILD" -DCMAKE_BUILD_TYPE=Release -Wno-dev
    make -C "$BODS_BUILD" -j"$JOBS"
else
    echo "[setup] BoDS already built"
fi

# ---------------------------------------------------------------------------
# 3. Generate workloads — each file is checked individually; existing files
#    are silently skipped.
# ---------------------------------------------------------------------------
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
# WARNING: LARGE DISK USAGE
#   This step generates binary workload files at N=500,000,000 keys each
#   (~1.9 GB per file) plus one TPC-H workload at N=6,000,000 keys (~23 MB).
#   Total disk space required: ~49 GB.
#   Workloads are written to: $WORKLOAD_DIR
#   To skip generation, set WORKLOAD_DIR to a directory containing
#   pre-generated .bin files before running this script.
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

# Helper: write a TOML and generate the .bin file if missing
generate_workload() {
    local N="$1" K="$2" L="$3"
    local OUTFILE="$WORKLOAD_DIR/workload_N${N}_K${K}_L${L}.bin"
    [[ -f "$OUTFILE" ]] && return 0

    echo "[setup] Generating workload N=$N K=$K L=$L ..."
    local TOML
    TOML="$(mktemp --suffix=.toml)"
    cat > "$TOML" <<EOF
title = "Qu-ART experiment workload"

[global]
domain = ${N}

[[partition]]
start_index = 0
number_of_entries = ${N}
K = ${K}
L = ${L}
seed = 1234
alpha = 1
beta = 1
payload = 0
window_size = 1
output_file = "${OUTFILE}"
is_fixed = false
is_binary = true
reverse_order = true
EOF
    "$GENERATOR" --toml_file "$TOML"
    rm -f "$TOML"
}

# The heavy N=500M (§4.1/§4.2) + TPC-H generation below can be skipped by
# setting GEN_WORKLOADS=0 — e.g. the §4.4 concurrent experiment sources setup
# only for the BoDS clone/build and the project build, then generates its own
# small per-stream N=20M files.
if [[ "${GEN_WORKLOADS:-1}" == "1" ]]; then

# --- §4.1 BoDS + §4.2 TPC-H: K-L grid, N=500M ---
N=500000000
echo "[setup] Checking N=${N} workloads..."
generate_workload "$N" 0 0
for K in 1 5 10 25 100; do
    for L in 1 5 10 25 100; do
        generate_workload "$N" "$K" "$L"
    done
done
# K=3 and K=50 at L=1 (extra points for the §4.1 throughput-vs-K curve)
generate_workload "$N" 3  1
generate_workload "$N" 50 1

# --- §4.2 TPC-H-inspired workload ---
# K≈96.67 L≈0.1 over N=6M keys (per SWARE methodology).
# BoDS encodes these as K=9667, L=01 in the filename (×100 and ×10 resp.).
# The generator accepts floats for K and L.
TPCH_FILE="$WORKLOAD_DIR/workload_N6000000_K9667_L01.bin"
if [[ ! -f "$TPCH_FILE" ]]; then
    echo "[setup] Generating TPC-H workload (N=6M, K≈96.67, L≈0.1)..."
    TOML="$(mktemp --suffix=.toml)"
    cat > "$TOML" <<EOF
title = "Qu-ART TPC-H workload"

[global]
domain = 6000000

[[partition]]
start_index = 0
number_of_entries = 6000000
K = 96.67
L = 0.1
seed = 1234
alpha = 1
beta = 1
payload = 0
window_size = 1
output_file = "${TPCH_FILE}"
is_fixed = false
is_binary = true
reverse_order = true
EOF
    "$GENERATOR" --toml_file "$TOML"
    rm -f "$TOML"
else
    echo "[setup] TPC-H workload already present"
fi

else
    echo "[setup] GEN_WORKLOADS=0 — skipping N=500M / TPC-H workload generation"
fi

# Export so run.sh scripts pick it up without extra flags
export WORKLOAD_DIR

# ---------------------------------------------------------------------------
# 3. Clone quick-insertion-tree (into each experiment folder that needs it)
# ---------------------------------------------------------------------------
for _QDIR in "$QUIT_DIR" "$BODS_EXP_QUIT_DIR"; do
    if [[ ! -d "$_QDIR" ]]; then
        echo "[setup] Cloning quick-insertion-tree into $_QDIR..."
        git clone https://github.com/BU-DiSC/quick-insertion-tree.git "$_QDIR"
    else
        echo "[setup] quick-insertion-tree already present at $_QDIR"
    fi
done

# ---------------------------------------------------------------------------
# 4. Build QuART (run + concurrent benchmarks)
# ---------------------------------------------------------------------------
mkdir -p "$BUILD"
if [[ ! -x "$BUILD/run" || ! -x "$BUILD/test_conc_olc" || ! -x "$BUILD/test_conc_olc_readwrite" ]]; then
    echo "[setup] Building QuART..."
    cmake -S "$REPO_ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    make -C "$BUILD" -j"$JOBS"
else
    echo "[setup] QuART binaries already built"
fi

echo ""
echo "[setup] Done. WORKLOAD_DIR=$WORKLOAD_DIR"
