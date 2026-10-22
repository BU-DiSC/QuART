#!/usr/bin/env bash
# §4.4, Figures 14 and 15 — concurrent QuART-OLC (QuART_conc_olc_stail) vs
# ART-OLC (QuART_conc_olc_art).
#   Fig 14: insert throughput vs thread count on K=L=0, K=L=5, K=L=25 and a
#           mix of the three; each thread ingests its own BoDS N=20M stream in
#           a disjoint key range (start = i*40M+1).      driver: test_conc_olc
#   Fig 15: mixed read/write on K=L=5 — (a) threads at a 50/50 mix,
#           (b) read fraction at the largest thread count.  driver: test_conc_olc_readwrite
# The per-stream files are generated here (setup.sh is sourced with
# GEN_WORKLOADS=0 so it only builds BoDS and QuART).
#
# Usage:  bash experiments/4.4-concurrent/run.sh
# Env:    THREADS (default "1 2 4 8 16"), STREAM_N (keys per stream file,
#         default 20000000), PER_STREAM (keys inserted per stream, default =
#         STREAM_N), READ_PCTS (default "5 25 50 75 95"), REPEAT (default 1,
#         median is kept), WORKLOAD_DIR, NUMACTL (e.g. one socket)
# Output: results/<ts>/fig14_raw.csv, fig15_raw.csv  (driver rows + repeat column)
#         results/<ts>/conc_insert_throughput.csv  workload,tree,threads,mops
#         results/<ts>/conc_readwrite.csv          panel,tree,x,mops

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
GEN_WORKLOADS=0 source "$EXPERIMENTS_DIR/setup.sh"
REPEAT="${REPEAT:-1}"
exp_init "4.4 concurrent (Fig 14, 15)"

THREADS="${THREADS:-1 2 4 8 16}"
STREAM_N="${STREAM_N:-20000000}"
PER_STREAM="${PER_STREAM:-$STREAM_N}"
READ_PCTS="${READ_PCTS:-5 25 50 75 95}"
STREAM_STEP=40000000
SERIES="K0_L0 K5_L5 K25_L25"
exp_meta threads "$THREADS"; exp_meta stream_n "$STREAM_N"; exp_meta per_stream "$PER_STREAM"
exp_meta read_pcts "$READ_PCTS"

WMAX=1; for W in $THREADS; do (( W > WMAX )) && WMAX=$W; done
for s in $SERIES; do
    K="${s%_*}"; K="${K#K}"; L="${s#*_L}"
    for (( i = 0; i < WMAX; i++ )); do generate_workload "$STREAM_N" "$K" "$L" $(( i * STREAM_STEP + 1 )); done
done
[[ "$STREAM_N" == 20000000 ]] || echo "note: STREAM_N=$STREAM_N (the drivers expect 20M-key stream files; smaller files are read in full)"

RAW14="$RESULTS_DIR/fig14_raw.csv"; RAW15="$RESULTS_DIR/fig15_raw.csv"
echo "repeat,workload,W,per_stream,total,serial_art_ns,olc_nofp_ns,olc_stail_ns,nofp_Mips,stail_Mips,nofp_xART,stail_xART,stail_vs_nofp" > "$RAW14"
echo "repeat,subfig,series,read_pct,W,warm,new,read_ops,write_ops,art_ns,stail_ns,art_mops,stail_mops,stail_vs_art,read_miss_art,read_miss_stail" > "$RAW15"

for (( rep = 1; rep <= REPEAT; rep++ )); do
    for W in $THREADS; do
        for label in $SERIES mixed; do
            flags=(--bods "$WORKLOAD_DIR" --series "$label")
            [[ "$label" == mixed ]] && flags=(--bods-mixed "$WORKLOAD_DIR")
            log="$LOG_DIR/fig14_${label}_W${W}_r${rep}.log"
            if QUART_STREAM_N="$STREAM_N" run_timed "$log" "$BUILD/test_conc_olc" "$W" "$PER_STREAM" --no-serial "${flags[@]}"; then
                row="$(printf '%s' "$RT_LAST" | sed "s/^[^,]*/$label/")"
                echo "$rep,$row" >> "$RAW14"; echo "  [fig14] $row"
            else
                echo "  [WARN] fig14 $label W=$W failed, see $log" >&2
            fi
        done
    done
    rw() {  # rw PART W READ_PCT
        local log="$LOG_DIR/fig15_$1_W$2_r$3_rep${rep}.log"
        if QUART_STREAM_N="$STREAM_N" run_timed "$log" "$BUILD/test_conc_olc_readwrite" "$2" "$3" --series K5_L5 --bods "$WORKLOAD_DIR"; then
            echo "$rep,$1,$RT_LAST" >> "$RAW15"; echo "  [fig15] $1,$RT_LAST"
        else
            echo "  [WARN] fig15 $1 W=$2 read=$3 failed, see $log" >&2
        fi
    }
    for W in $THREADS; do rw a_threads "$W" 50; done
    for R in $READ_PCTS; do rw b_readfrac "$WMAX" "$R"; done
done

python3 "$EXP_DIR/to_long.py" "$RAW14" "$RAW15" "$RESULTS_DIR"
