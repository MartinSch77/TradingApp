#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Send EVERY test result this project produces to Squish Test Center — the
# unit/integration suite and the Squish GUI suite alike.
#
#   tools/testcenter_upload.sh                       # upload test-results/**.xml
#   tools/testcenter_upload.sh --dry-run             # list exactly what would be sent
#   tools/testcenter_upload.sh --testcenter-dir DIR  # a specific installation
#   tools/testcenter_upload.sh --url URL --project P --batch B
#
# Every path and every setting is a PARAMETER with an environment fallback, because
# these tools live wherever their owner installed them:
#   --testcenter-dir | TESTCENTER_DIR       installation directory (auto-discovered)
#   --url            | TESTCENTER_URL       default http://localhost:8800
#   --project        | TESTCENTER_PROJECT   default TradingApp
#   --batch          | TESTCENTER_BATCH     default: the short git sha
#   --token          | TESTCENTER_TOKEN     an access token from Test Center
#   --user/--password| TESTCENTER_USER / TESTCENTER_PASSWORD
#   --label k=v      | TESTCENTER_LABELS    extra labels, repeatable (space-separated
#                                           in the environment variable)
#   --suite NAME     | TESTCENTER_SUITE     which suite these results are, default
#                                           unit+integration — the GUI run passes `gui`
#                                           so one batch never mixes the two
#
# UPLOADS GO THROUGH `testcentercmd`, which ships with Test Center (bin/testcentercmd)
# and with Squish. An earlier version of this script POSTed JUnit XML to an invented
# REST path — the product has a supported command-line client, and using it makes the
# protocol the vendor's business rather than this repository's guess.
#
# THREE ways to authenticate, in order of preference:
#   1. testcentercmd's OWN credential store — `testcentercmd config token <value>`,
#      which writes ~/.squish/ver1/testcentercmd.ini. Nothing secret then appears in a
#      command line, an environment variable or this repository, so this is the route
#      docs/qt-tools.md recommends. The script simply does not pass credentials and
#      lets the client find its own.
#   2. TESTCENTER_TOKEN — for CI, where a secret arrives as an environment variable.
#   3. TESTCENTER_USER + TESTCENTER_PASSWORD — a plain login, which also works.
#
# Labels are how the batch stays readable months later. Three names are the PRODUCT'S
# and are not free choices: `.git.revision` drives the Commit Summary section of the
# printable report, `.git.branch` selects the branch for repository lookups, and
# `.reference.url` becomes a clickable link in the References column.
#
# Licence-bound like Squish itself, so it follows the same rule: not installed, not
# reachable or not configured means the stage prints why and exits 3 ("skipped"). It
# is never a build gate — the quality PDF lists the missing licence instead.
# How to obtain and install it: docs/qt-tools.md.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="$ROOT/test-results"

DRY_RUN=0
TC_DIR="${TESTCENTER_DIR:-}"
URL="${TESTCENTER_URL:-http://localhost:8800}"
PROJECT="${TESTCENTER_PROJECT:-TradingApp}"
BATCH="${TESTCENTER_BATCH:-}"
TOKEN="${TESTCENTER_TOKEN:-}"
USER_EMAIL="${TESTCENTER_USER:-}"
PASSWORD="${TESTCENTER_PASSWORD:-}"
SUITE="${TESTCENTER_SUITE:-unit+integration}"
# shellcheck disable=SC2206  # deliberate word splitting: the variable carries a list
LABELS=(${TESTCENTER_LABELS:-})
ATTACH=()
WITH_COVERAGE=0

while [ $# -gt 0 ]; do
    case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --testcenter-dir) TC_DIR="${2:-}"; shift ;;
    --url) URL="${2:-}"; shift ;;
    --project) PROJECT="${2:-}"; shift ;;
    --batch) BATCH="${2:-}"; shift ;;
    --token) TOKEN="${2:-}"; shift ;;
    --user) USER_EMAIL="${2:-}"; shift ;;
    --password) PASSWORD="${2:-}"; shift ;;
    --suite) SUITE="${2:-}"; shift ;;
    --label) LABELS+=("${2:-}"); shift ;;
    --attachment) ATTACH+=("${2:-}"); shift ;;
    --with-coverage) WITH_COVERAGE=1 ;;
    -h | --help)
        sed -n '2,45p' "$0"
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done

[ -n "$BATCH" ] || BATCH="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null ||
    date -u +%Y%m%d-%H%M)"

# The git labels. `.git.revision` is the product's own name for the commit a batch
# describes, so it is filled in automatically rather than left to whoever runs this.
#
# A DIRTY WORKING TREE STILL GETS THE REVISION, plus a `worktree=dirty` label beside
# it: the report needs a commit to anchor on, but a batch whose sources differ from
# that commit must say so, or a green run gets read as evidence for code that was
# never tested.
add_git_labels() {
    local sha branch remote url
    sha="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)" || return 0
    LABELS+=(".git.revision=$sha")
    branch="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    [ -n "$branch" ] && [ "$branch" != "HEAD" ] && LABELS+=(".git.branch=$branch")
    if ! git -C "$ROOT" diff --quiet HEAD 2>/dev/null ||
        [ -n "$(git -C "$ROOT" ls-files --others --exclude-standard 2>/dev/null)" ]; then
        LABELS+=("worktree=dirty")
    fi
    # A browsable link for the References column, derived from the remote so no URL is
    # hardcoded. Both remote spellings appear in practice; normalise ssh to https.
    remote="$(git -C "$ROOT" remote get-url origin 2>/dev/null)" || return 0
    case "$remote" in
    git@*) url="https://${remote#git@}"; url="${url/://}" ;;
    http*) url="$remote" ;;
    *) return 0 ;;
    esac
    LABELS+=(".reference.url=${url%.git}/commit/$sha")
}
add_git_labels
LABELS+=("suite=$SUITE" "platform=$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)")

LABEL_ARGS=()
for label in "${LABELS[@]}"; do
    [ -n "$label" ] && LABEL_ARGS+=("--label=$label")
done

# ATTACH_ARGS is built further down, AFTER the reports have been added to ATTACH —
# building it here would silently drop every auto-discovered PDF.

# Where testcentercmd is. In order: an explicit directory, PATH, the Test Center
# installer's own layout (~/testcenter-<version>-linux-x64), then a Squish install,
# which bundles the same client. Newest version wins.
find_testcentercmd() {
    if [ -n "$TC_DIR" ]; then
        local candidate
        for candidate in "$TC_DIR/bin/testcentercmd" "$TC_DIR/testcentercmd"; do
            [ -x "$candidate" ] && echo "$candidate" && return
        done
        return
    fi
    if command -v testcentercmd >/dev/null 2>&1; then
        command -v testcentercmd
        return
    fi
    local dir
    for dir in $(ls -d "$HOME"/testcenter-* /opt/testcenter* "$HOME"/squish-for-qt-* \
        /opt/squish* 2>/dev/null | sort -Vr); do
        [ -x "$dir/bin/testcentercmd" ] && echo "$dir/bin/testcentercmd" && return
    done
}

CMD="$(find_testcentercmd)"

mapfile -t XML < <(find "$RESULTS" -name '*.xml' -type f 2>/dev/null | sort)

if [ "${#XML[@]}" -eq 0 ]; then
    echo "no test results in $RESULTS — run tools/run_tests.sh first"
    exit 1
fi

# The Coco database is a RESULT format Test Center reads natively (CSMES/CSEXE), not an
# attachment: uploaded as a result it gives per-test coverage inside Test Center rather
# than a single summary number. Opt-in because the databases are large and the upload is
# then measured in minutes rather than seconds.
if [ "$WITH_COVERAGE" -eq 1 ]; then
    while IFS= read -r csmes; do
        [ -n "$csmes" ] && XML+=("$csmes")
    done < <(find "$ROOT/coverage" -name '*.csmes' -type f 2>/dev/null | sort)
fi

# The reports go up as ATTACHMENTS, because they are evidence rather than results.
# Static-analysis findings have no result format here — the GATE verdicts travel as
# test cases (tools/gates_to_junit.py) and the full documents ride along as files, which
# keeps "the build was acceptable" and "here is every finding" as separate claims.
for pdf in "$ROOT/downloads/TradingApp-quality-report.pdf" \
    "$ROOT/downloads/TradingApp-axivion-report.pdf" \
    "$ROOT/downloads/TradingApp-testcenter-report.pdf"; do
    [ -f "$pdf" ] && ATTACH+=("$pdf")
done

echo "== Squish Test Center upload =="
echo "results:  ${#XML[@]} file(s)"
for f in "${XML[@]}"; do
    printf '  %s\n' "${f#"$ROOT"/}"
done
ATTACH_ARGS=()
for file in ${ATTACH[@]+"${ATTACH[@]}"}; do
    [ -f "$file" ] && ATTACH_ARGS+=("--attachment=$file")
done
if [ "${#ATTACH_ARGS[@]}" -gt 0 ]; then
    echo "attach:   ${#ATTACH_ARGS[@]} file(s)"
    for f in ${ATTACH[@]+"${ATTACH[@]}"}; do
        [ -f "$f" ] && printf '  %s\n' "${f#"$ROOT"/}"
    done
fi
echo "client:   ${CMD:-<not found>}"
echo "server:   $URL"
echo "project:  $PROJECT"
echo "batch:    $BATCH"
echo "labels:   ${LABELS[*]}"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "(dry run — nothing sent)"
    exit 0
fi

if [ -z "$CMD" ]; then
    echo "SKIPPED: testcentercmd not found."
    echo "         Squish Test Center is licence-bound (qt.io); its installer unpacks to"
    echo "         ~/testcenter-<version>-linux-x64. Pass --testcenter-dir or set"
    echo "         TESTCENTER_DIR. See docs/qt-tools.md. Never a build gate."
    exit 3
fi

# Is a server actually there? Without this check testcentercmd waits for interactive
# credentials, which in a pipeline is a HUNG stage rather than a reported one.
if ! curl --max-time 10 -fsS -o /dev/null "$URL" 2>/dev/null; then
    echo "SKIPPED: no Test Center answering at $URL"
    echo "         Start it with:  <install-dir>/bin/testcenter start"
    echo "         then open $URL once to create the first user. Never a build gate."
    exit 3
fi

AUTH=()
if [ -n "$TOKEN" ]; then
    AUTH+=("--token=$TOKEN")
    echo "auth:     token from the environment"
elif [ -n "$USER_EMAIL" ] && [ -n "$PASSWORD" ]; then
    AUTH+=("--user=$USER_EMAIL" "--password=$PASSWORD")
    echo "auth:     user + password from the environment"
else
    # No credential was handed to us, which is NOT the same as having none: the client
    # keeps its own store (`testcentercmd config token`) and finds it without help. So
    # attempt the upload and let the server rule on it — measured behaviour is that a
    # client with an empty store fails immediately with "No authentication provided"
    # rather than prompting, which the rejected-login branch below turns into a skip.
    # Skipping here up front instead would make the RECOMMENDED credential route
    # the one that silently never runs.
    echo "auth:     none passed — using testcentercmd's own credential store"
fi

# --interactive=no is what keeps a pipeline from stopping at a credential prompt.
# One call with every file: Test Center groups them into the named batch itself.
#
# The output is captured as well as shown, because a REJECTED LOGIN has to be told apart
# from a broken upload: "no access to this server yet" is a skip like any other
# licence-bound obstacle, while a server that accepted the credentials and then failed is a
# real error worth stopping for.
OUTPUT="$("$CMD" --url="$URL" "${AUTH[@]}" --interactive=no \
    upload "$PROJECT" --batch="$BATCH" "${LABEL_ARGS[@]}" \
    ${ATTACH_ARGS[@]+"${ATTACH_ARGS[@]}"} "${XML[@]}" 2>&1)"
RC=$?
printf '%s\n' "$OUTPUT"

if [ "$RC" -ne 0 ]; then
    if printf '%s' "$OUTPUT" | grep -qiE "unauthor|forbidden|not authenticated|invalid (user|password|token|credential)|authentication|401|403|login"; then
        echo ""
        echo "SKIPPED: Test Center did not accept the login, so nothing was uploaded."
        echo "         Store a token once — no secret in a command line or a variable:"
        echo "             $CMD config token <value>"
        echo "         The value comes from the Test Center UI under ADMIN -> USER"
        echo "         MANAGEMENT, where the product calls it an UPLOAD TOKEN (route"
        echo "         /admin/accesstokens). It is NOT in the user menu, and it is NOT the"
        echo "         public RSA key shown for the Jira application link."
        echo "         TESTCENTER_TOKEN or TESTCENTER_USER + TESTCENTER_PASSWORD also work."
        echo "         Not a build gate: the results stay in $RESULTS and can be uploaded"
        echo "         later with exactly this command."
        exit 3
    fi
    echo "Test Center upload failed (rc=$RC) — the results are still in $RESULTS" >&2
    exit "$RC"
fi
echo "batch $BATCH is at $URL (project $PROJECT)"
