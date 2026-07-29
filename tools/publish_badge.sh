#!/usr/bin/env bash
# Publish one shields.io endpoint badge onto the `badges` branch.
#
#   tools/publish_badge.sh <file.json> <label> <message> <colour>
#
# The README reads these through https://img.shields.io/endpoint?url=…raw…/badges/<file>.
# Used by .github/workflows/tests.yml (coverage) and ci.yml (per-platform build
# status); GH_TOKEN and GITHUB_REPOSITORY must be set, i.e. it runs in CI.
#
# Why a script and not a few lines of YAML in each workflow: the branch holds
# SEVERAL badge files written by DIFFERENT workflows that can run at the same
# time. The obvious implementation — force-push an orphan branch with one file —
# deletes every other badge, and two concurrent jobs would then flip-flop the
# branch between their own single files. So this is additive:
#   * fetch the branch (or start it empty on the very first run),
#   * write only this one file,
#   * commit and push, retrying on the "fetch first" rejection that a
#     concurrent publisher causes.
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <file.json> <label> <message> <colour>" >&2
    exit 2
fi
FILE="$1" LABEL="$2" MESSAGE="$3" COLOUR="$4"
: "${GH_TOKEN:?GH_TOKEN must be set}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"

BRANCH="badges"
REMOTE="https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
git init -q -b "$BRANCH"
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git remote add origin "$REMOTE"

for attempt in 1 2 3 4 5; do
    # Start from whatever is on the branch right now; an empty branch (first run)
    # simply leaves the tree empty.
    if git fetch -q --depth 1 origin "$BRANCH" 2>/dev/null; then
        git reset -q --hard FETCH_HEAD
    fi
    printf '{"schemaVersion":1,"label":"%s","message":"%s","color":"%s"}\n' \
        "$LABEL" "$MESSAGE" "$COLOUR" > "$FILE"
    git add "$FILE"
    if git diff --cached --quiet; then
        echo "badge $FILE unchanged ($LABEL: $MESSAGE) — nothing to push"
        exit 0
    fi
    git commit -q -m "$LABEL: $MESSAGE"
    if git push -q origin "HEAD:$BRANCH" 2>/dev/null; then
        echo "badge published: $FILE ($LABEL: $MESSAGE, $COLOUR)"
        exit 0
    fi
    echo "push rejected (concurrent publisher?) — retry $attempt" >&2
    # Drop the local commit and start over from the remote state.
    git reset -q --hard HEAD~1 || git update-ref -d HEAD
    sleep $((attempt * 3))
done

echo "ERROR: could not publish $FILE after 5 attempts" >&2
exit 1
