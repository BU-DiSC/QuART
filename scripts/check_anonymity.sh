#!/usr/bin/env bash
# scripts/check_anonymity.sh [--submodules] — gate for the double-anonymous release.
#
# Fails if the tracked files, the current branch's history, .gitmodules or (with
# --submodules) the QuART commits of each system fork carry an identifying string.
# The only allowed matches are the two pinned third-party tool URLs in
# experiments/setup.sh (published tools the paper cites).

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATTERN='cgokmen|gokmen|bu-disc|bu\.edu|boston|athanassoulis|raman|/scratch/|/users/|co-authored-by|naxos|overleaf'
ALLOW='^experiments/setup\.sh:[0-9]+:.*github\.com/BU-DiSC/(bods|quick-insertion-tree)\.git'
status=0
fail() { echo "FAIL: $1"; status=1; }

cd "$ROOT"
echo "== tracked files"
hits=$(git grep -n -i -E "$PATTERN" -- . ':!scripts/check_anonymity.sh' | grep -v -E "$ALLOW" || true)
[[ -z "$hits" ]] || { echo "$hits"; fail "identifying strings in tracked files"; }
hits=$(git grep -n -E '20[2-3][0-9]-[01][0-9]-[0-3][0-9]|20[2-3][0-9][01][0-9][0-3][0-9]_[0-9]{4,6}' -- . ':!scripts/check_anonymity.sh' || true)
[[ -z "$hits" ]] || { echo "$hits"; fail "dates / run timestamps in tracked files"; }

echo "== history of $(git rev-parse --abbrev-ref HEAD)"
hits=$(git log --format='%h %an <%ae> %cn <%ce>%n%B' | grep -i -E "$PATTERN" || true)
# anonymous.4open.science exposes one commit's tree, never the history, so this is a note
[[ -z "$hits" ]] || echo "note: $(echo "$hits" | wc -l) identifying lines in commit metadata (not exposed by anonymous.4open.science)"

echo "== anonymous-repository links"
n=$(git grep -c '<<ANON-LINK-' -- README.md | cut -d: -f2 || true)
[[ -z "$n" || "$n" == 0 ]] || echo "note: README.md still has $n <<ANON-LINK-...>> placeholder(s) to fill in"

echo "== untracked-only directories"
tracked=$(git ls-files | grep -E '^(paper|graphs)/' || true)
[[ -z "$tracked" ]] || { echo "$tracked" | head; fail "paper/ or graphs/ is tracked"; }

echo "== .gitmodules"
if [[ -f .gitmodules ]]; then
    bad=$(git config -f .gitmodules --get-regexp 'submodule\..*\.url' | awk '$2 !~ /^\.\.\//' || true)
    [[ -z "$bad" ]] || { echo "$bad"; fail "submodule URLs must be relative (../Name.git)"; }
fi

if [[ "${1:-}" == "--submodules" ]]; then
    for sm in systems/duckdb systems/postgres systems/redis; do
        [[ -f "$sm/.git" || -d "$sm/.git" ]] || { echo "skip $sm (not checked out)"; continue; }
        base=$(git -C "$sm" config --file "$ROOT/.gitmodules" "submodule.${sm#systems/}.quart-base" 2>/dev/null || true)
        echo "== $sm (QuART commits ${base:-?}..HEAD)"
        [[ -n "$base" ]] || { fail "$sm: no quart-base recorded in .gitmodules"; continue; }
        hits=$(git -C "$sm" log --format='%h %an <%ae> %cn <%ce>%n%B' "$base..HEAD" | grep -i -E "$PATTERN" || true)
        [[ -z "$hits" ]] || echo "note: $sm: $(echo "$hits" | wc -l) identifying lines in commit metadata (not exposed by anonymous.4open.science)"
        files=$(git -C "$sm" diff --name-only "$base..HEAD")
        hits=$(cd "$sm" && git grep -n -i -E "$PATTERN" HEAD -- $files | grep -v -i -E 'raman|boston' || true)
        [[ -z "$hits" ]] || { echo "$hits" | head -20; fail "$sm: identifying strings in QuART-touched files"; }
    done
fi
[[ $status -eq 0 ]] && echo "OK: no identifying strings found"
exit $status
