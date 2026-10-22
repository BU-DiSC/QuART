#!/usr/bin/env bash
# experiments/clean.sh
#
# Reverses everything done by experiments/setup.sh:
#   1. Remove the Qu-ART build directory
#   2. Remove the experiment 5.2 QuIT build directory
#   3. Remove the cloned quick-insertion-tree
#   4. Remove generated workload .bin files (experiments/bods/workloads/)
#   5. Remove the cloned BoDS directory
#
# The repo source tree is left untouched.
#
# Usage:
#   bash experiments/clean.sh           # dry-run: shows what would be removed
#   bash experiments/clean.sh --yes     # actually removes everything
#
# Environment variables (all optional, must match values used with setup.sh):
#   BODS_DIR   – location of the BoDS clone (default: experiments/bods)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DRY_RUN=true
if [[ "${1:-}" == "--yes" ]]; then
    DRY_RUN=false
fi

BODS_DIR="${BODS_DIR:-$SCRIPT_DIR/bods}"
QUIT_DIR="$SCRIPT_DIR/4.1-bods/quick-insertion-tree"
QUIT_BUILD="$SCRIPT_DIR/4.1-bods/build"
BUILD="$REPO_ROOT/build"

remove() {
    local TARGET="$1"
    if [[ -e "$TARGET" || -L "$TARGET" ]]; then
        if $DRY_RUN; then
            echo "  [dry-run] would remove: $TARGET"
        else
            echo "  removing: $TARGET"
            rm -rf "$TARGET"
        fi
    else
        echo "  already absent: $TARGET"
    fi
}

echo "=== Qu-ART clean ==="
if $DRY_RUN; then
    echo "  (dry-run — pass --yes to actually delete)"
fi
echo ""

echo "1. Qu-ART build directory"
remove "$BUILD"

echo "2. Experiment 5.2 QuIT build directory"
remove "$QUIT_BUILD"

echo "3. quick-insertion-tree clone"
remove "$QUIT_DIR"

echo "4. Generated workloads"
remove "$BODS_DIR/workloads"

echo "5. BoDS clone"
remove "$BODS_DIR"

echo ""
if $DRY_RUN; then
    echo "Dry-run complete. Run with --yes to delete."
else
    echo "Clean complete."
fi
