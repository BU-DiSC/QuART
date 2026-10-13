#!/usr/bin/env bash
# Experiment 4.4 (§4.4): Concurrent QuART evaluation
#
# Reproduces the paper's concurrent figures, A/B'ing the two OLC tree types on
# identical multi-stream workloads:
#   QuART_conc_olc_art   = plain OLC ART  ("ART-OLC",  no fast path)
#   QuART_conc_olc_stail = OLC ART + per-thread stail fast-path tail ("QuART-OLC")
#
#   Figure 14 : concurrent insert throughput vs thread count (= # streams),
#               W in {1,2,4,8,16}, over four workloads —
#               K=L=0 (fully sorted), K=L=5 (near-sorted), K=L=25 (moderately
#               disordered), and `mixed` (streams drawn evenly from the three).
#               Driver: build/test_conc_olc  (--bods / --bods-mixed).
#   Figure 15 : mixed concurrent read/write on the near-sorted K=L=5 workload —
#               (a) throughput vs thread count at a 50/50 read/write mix;
#               (b) throughput vs read fraction at 16 threads.
#               Driver: build/test_conc_olc_readwrite.
#
# Each thread ingests its own BoDS N=20M near-sorted stream; streams occupy
# disjoint key ranges (start = i*40M+1).  These small per-stream files are
# generated here (NOT by setup.sh, which only makes the heavy N=500M §4.1/§4.2
# workloads); this script sources setup.sh with GEN_WORKLOADS=0 so it only
# clones/builds BoDS and builds the project.
#
# Usage:
#   bash experiments/4.4-concurrent/run.sh
#
# Environment variables (all optional):
#   THREADS       – space-separated thread counts to sweep (default: "1 2 4 8 16")
#   PER_STREAM    – keys per stream (default: 20000000 = full file; 0 also = full)
#   WORKLOAD_DIR  – where the per-stream .bin files live (default: bods/workloads)
#   JOBS          – parallel make jobs (default: nproc)
#
# Output:
#   experiments/4.4-concurrent/results/fig14_<TIMESTAMP>.csv   (insert throughput)
#   experiments/4.4-concurrent/results/fig15_<TIMESTAMP>.csv   (mixed read/write)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"
JOBS="${JOBS:-$(nproc)}"

THREADS="${THREADS:-1 2 4 8 16}"
PER_STREAM="${PER_STREAM:-20000000}"

# Bootstrap: clone+build BoDS and build the project, but SKIP the heavy N=500M
# workload generation (we make our own small per-stream files below).
GEN_WORKLOADS=0
source "$SCRIPT_DIR/../setup.sh"

# $GENERATOR and $WORKLOAD_DIR are exported by setup.sh.
GENERATOR="${GENERATOR:-$BODS_DIR/build/sortedness_data_generator}"
: "${WORKLOAD_DIR:=$BODS_DIR/workloads}"
mkdir -p "$WORKLOAD_DIR"

STREAM_N=20000000      # keys per stream file (matches test_conc_olc STREAM_N)
STREAM_STEP=40000000   # per-stream start-offset stride (matches STREAM_STEP)

# The three single-level sortedness workloads used in Fig 14 / Fig 15.
SERIES_LIST=(K0_L0 K5_L5 K25_L25)

# Largest thread count we will sweep ⇒ how many start offsets we must generate.
WMAX=1
for W in $THREADS; do (( W > WMAX )) && WMAX=$W; done

# ---------------------------------------------------------------------------
# Generate one BoDS N=20M file per (series, stream offset) if missing.
#   file: workload_N20000000_<series>_start<i*40M+1>.bin
#   keys: near-sorted run in value window [start, start+20M]  (start_index=start)
# ---------------------------------------------------------------------------
gen_stream() {
    local series="$1" start="$2"
    local K="${series%_*}"; K="${K#K}"     # "K5_L5" -> "5"
    local L="${series#*_L}"                 # "K5_L5" -> "5"
    local OUT="$WORKLOAD_DIR/workload_N${STREAM_N}_${series}_start${start}.bin"
    [[ -f "$OUT" ]] && return 0
    echo "[4.4] Generating $series stream @start=$start ..."
    local TOML; TOML="$(mktemp --suffix=.toml)"
    cat > "$TOML" <<EOF
title = "QuART concurrent per-stream workload"

[global]
domain = ${STREAM_N}

[[partition]]
start_index = ${start}
number_of_entries = ${STREAM_N}
K = ${K}
L = ${L}
seed = 1234
alpha = 1
beta = 1
payload = 0
window_size = 1
output_file = "${OUT}"
is_fixed = false
is_binary = true
reverse_order = true
EOF
    "$GENERATOR" --toml_file "$TOML"
    rm -f "$TOML"
}

echo "[4.4] Ensuring per-stream BoDS files for ${#SERIES_LIST[@]} series × $WMAX offsets..."
for series in "${SERIES_LIST[@]}"; do
    for (( i=0; i<WMAX; i++ )); do
        start=$(( i * STREAM_STEP + 1 ))
        gen_stream "$series" "$start"
    done
done

SUFFIX=$(date +"%Y%m%d_%H%M%S")
RESULTS_DIR="$SCRIPT_DIR/results"
LOG_DIR="$RESULTS_DIR/logs_${SUFFIX}"
FIG14="$RESULTS_DIR/fig14_${SUFFIX}.csv"
FIG15="$RESULTS_DIR/fig15_${SUFFIX}.csv"
mkdir -p "$RESULTS_DIR" "$LOG_DIR"

echo "Experiment 4.4 — Concurrent QuART evaluation"
echo "  THREADS     : $THREADS"
echo "  PER_STREAM  : $PER_STREAM"
echo "  WORKLOAD_DIR: $WORKLOAD_DIR"
echo "  Results     : $FIG14 , $FIG15"
echo ""

# ===========================================================================
# Figure 14 — concurrent insert throughput vs thread count.
#   test_conc_olc CSV row:
#     mode,W,per_stream,total,serial_art_ns,olc_nofp_ns,olc_stail_ns,
#     nofp_Mips,stail_Mips,nofp_xART,stail_xART,stail_vs_nofp
#   (serial baseline skipped with --no-serial ⇒ serial_art_ns = -1).
#   Plot ART-OLC = nofp_Mips, QuART-OLC = stail_Mips vs W, per workload.
# ===========================================================================
echo "workload,W,per_stream,total,serial_art_ns,olc_nofp_ns,olc_stail_ns,nofp_Mips,stail_Mips,nofp_xART,stail_xART,stail_vs_nofp" > "$FIG14"

run_fig14() {
    local label="$1"; shift   # workload label for the log/stdout
    local W="$1"; shift       # thread count
    local LOG="$LOG_DIR/fig14_${label}_W${W}.log"
    echo ">>> [fig14] $label  W=$W"
    # "$@" carries the workload-selection flags, including the BoDS dir:
    #   --bods <dir> --series X   |   --bods-mixed <dir>
    local ROW
    ROW=$("$BUILD/test_conc_olc" "$W" "$PER_STREAM" --no-serial "$@" \
            2>>"$LOG") || { echo "  [WARN] failed (see $LOG)"; return; }
    # test_conc_olc emits its CSV row (mode,...) on stdout; relabel col 1.
    ROW=$(echo "$ROW" | tail -1 | sed "s/^[^,]*/$label/")
    echo "$ROW" >> "$FIG14"
    echo "  $ROW"
}

for W in $THREADS; do
    run_fig14 "K0_L0"   "$W" --bods "$WORKLOAD_DIR" --series K0_L0
    run_fig14 "K5_L5"   "$W" --bods "$WORKLOAD_DIR" --series K5_L5
    run_fig14 "K25_L25" "$W" --bods "$WORKLOAD_DIR" --series K25_L25
    run_fig14 "mixed"   "$W" --bods-mixed "$WORKLOAD_DIR"
done

# ===========================================================================
# Figure 15 — mixed concurrent read/write on the near-sorted K=L=5 workload.
#   test_conc_olc_readwrite CSV row (see header of test_conc_olc_readwrite.cpp):
#     series,read_pct,W,warm,new,read_ops,write_ops,art_ns,stail_ns,...
#   (a) throughput vs thread count at 50/50 read/write
#   (b) throughput vs read fraction at 16 threads
# ===========================================================================
echo ">>> [fig15] mixed read/write (series=K5_L5)"
# test_conc_olc_readwrite prints one data row per run (no header); columns are:
#   series,read_pct,W,warm,new,read_ops,write_ops,art_ns,stail_ns,
#   art_mops,stail_mops,stail_vs_art,read_miss_art,read_miss_stail
# Plot ART-OLC = art_mops, QuART-OLC = stail_mops.
echo "subfig,series,read_pct,W,warm,new,read_ops,write_ops,art_ns,stail_ns,art_mops,stail_mops,stail_vs_art,read_miss_art,read_miss_stail" > "$FIG15"
readwrite_run() {
    local part="$1" W="$2" readpct="$3"
    local LOG="$LOG_DIR/fig15_${part}_W${W}_r${readpct}.log"
    echo ">>> [fig15-$part] W=$W read=${readpct}%"
    local ROW
    ROW=$("$BUILD/test_conc_olc_readwrite" "$W" "$readpct" --series K5_L5 \
            --bods "$WORKLOAD_DIR" 2>>"$LOG") || { echo "  [WARN] failed (see $LOG)"; return; }
    ROW=$(echo "$ROW" | grep -E '^K5_L5,' | tail -1)
    # prefix a sub-figure tag so the plot can split (a)/(b)
    echo "${part},${ROW}" >> "$FIG15"
    echo "  ${part},${ROW}"
}

# (a) 50/50 read/write, sweep thread count
for W in $THREADS; do readwrite_run "a_threads" "$W" 50; done
# (b) read-fraction sweep at the highest swept thread count (paper uses 16; set
#     THREADS to include 16 for the paper config — WMAX bounds generated files).
for R in 5 25 50 75 95; do readwrite_run "b_readfrac" "$WMAX" "$R"; done

echo ""
echo "Done. Results:"
echo "  Fig 14 (insert throughput)   : $FIG14"
echo "  Fig 15 (mixed read/write)    : $FIG15"
