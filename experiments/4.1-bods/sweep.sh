#!/usr/bin/env bash
# experiments/4.1-bods/sweep.sh — the K/L sweep shared by §4.1 and §4.2.
#
# bods_sweep OUT_CSV N "TREE ..." "K,L,FILE" ...
#   For every (K,L,FILE) and every TREE: run the tree's binary $REPEAT times
#   on FILE (see bin_for_tree in common.sh) and append one row in the S1 schema
#     workload,N,K,L,tree_type,avg_insert_ns,avg_query_ns
#   Missing files are skipped with a [SKIP] line; failed runs get ERROR cells.
#   Requires common.sh to be sourced and exp_init to have been called.

S1_HEADER="workload,N,K,L,tree_type,avg_insert_ns,avg_query_ns"

bods_sweep() {
    local out="$1" n="$2" trees="$3"; shift 3
    [[ -s "$out" ]] || echo "$S1_HEADER" > "$out"
    local spec k l file wname tree bin avg
    for spec in "$@"; do
        IFS=, read -r k l file <<< "$spec"
        require_file "$file" || continue
        wname="$(basename "$file" .bin)"
        echo ">>> K=$k L=$l  ($wname)"
        for tree in $trees; do
            # shellcheck disable=SC2206
            bin=( $(bin_for_tree "$tree") )
            avg="$(avg_cols "$LOG_DIR/${wname}_${tree}.log" 2 "${bin[@]}" -f "$file" -N "$n")" || true
            echo "$wname,$n,$k,$l,$tree,$avg" >> "$out"
            printf '  %-12s %s\n' "$tree" "$avg"
        done
    done
}

# grid_pairs N "K ..." "L ..." — print "K,L,FILE" specs for the K×L grid
# (plus K=0,L=0), using $WORKLOAD_DIR/workload_N<N>_K<K>_L<L>.bin.
grid_pairs() {
    local n="$1" ks="$2" ls="$3" k l
    echo "0,0,$WORKLOAD_DIR/workload_N${n}_K0_L0.bin"
    for k in $ks; do for l in $ls; do
        echo "$k,$l,$WORKLOAD_DIR/workload_N${n}_K${k}_L${l}.bin"
    done; done
}
