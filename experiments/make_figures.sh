#!/usr/bin/env bash
# experiments/make_figures.sh — regenerate every data-driven figure and table of
# the paper into experiments/figures/.
#
# Each plot script reads results/latest/ of its experiment (what run.sh writes
# by default), or results/paper/ (the data behind the paper) when no run exists.
#
# Usage:  bash experiments/make_figures.sh            # results/latest, else paper data
#         QUART_RESULTS=paper bash experiments/make_figures.sh   # always the paper data
#         QUART_RESULTS=smoke bash experiments/make_figures.sh   # a run made with RUN_NAME=smoke
# Env:    QUART_USETEX=0 to render text without LaTeX; QUART_PDFCROP=0 to skip pdfcrop

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/figures"
mkdir -p "$OUT"
export QUART_RESULTS="${QUART_RESULTS:-latest}"

declare -a NAMES=(
    "Fig 6   bods_insert_types.pdf         3.3-insert-types/plot.py"
    "Fig 9   quart_vs_art.pdf              4.1-bods/quart-vs-art/plot.py"
    "Fig 10  stail_heatmaps_only.pdf       4.1-bods/stail-vs-art/plot.py"
    "Fig 11  quit_speedup_heatmap.pdf      4.1-bods/stail-vs-quit/plot.py"
    "Fig 12  tpch_throughput.pdf           4.2-tpch/plot.py"
    "Fig 13  bulkload_performance.pdf      4.3-bulkload/plot.py"
    "Fig 14  conc_insert_throughput.pdf    4.4-concurrent/plot.py --which fig14"
    "Fig 15  conc_ycsb.pdf                 4.4-concurrent/plot.py --which fig15"
    "Tab 2   postgres_vacuum_table.tex     5.1-postgres/table.py --latex"
    "Fig 16  duckdb_speedup_heatmap.pdf    5.2-duckdb/plot.py"
    "Fig 17  redis_performance.pdf         5.3-redis/plot.py"
)
failed=0
printf '%-8s %-30s %s\n' "item" "output" "status"
for entry in "${NAMES[@]}"; do
    read -r fig num file script args <<< "$entry"
    log="$OUT/${file%.*}.log"
    if python3 "$HERE/$script" $args > "$log" 2>&1; then
        printf '%-8s %-30s ok\n' "$fig $num" "$file"
    else
        printf '%-8s %-30s FAILED (see %s)\n' "$fig $num" "$file" "$log"
        failed=1
    fi
done
echo "figures in $OUT   (data source: QUART_RESULTS=$QUART_RESULTS)"
exit $failed
