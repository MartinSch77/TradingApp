#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Publish a release: the binaries, the documentation, and the QUALIFICATION
# evidence — but only when there is proof that the pipeline actually ran green.
#
#   tools/publish_release.sh --dry-run          # say exactly what would be published
#   tools/publish_release.sh                    # publish for the version in CMakeLists
#   tools/publish_release.sh --tag v1.0.1       # …or for a named tag
#   tools/publish_release.sh --allow-dirty      # publish from a modified tree (see below)
#
# WHY THE CHECKS COME FIRST
#
# A release is a claim: "this build passed". The whole point of this repository is
# that such claims are backed by artefacts, so this script refuses to publish
# unless the artefacts are present, consistent and NEWER than the sources they
# describe. Publishing yesterday's evidence for today's code is the one failure
# mode a packaging script can have that nobody notices.
#
# Verified before anything is uploaded:
#   * a clean working tree (a release built from uncommitted code cannot be
#     reproduced by anyone, including you — --allow-dirty exists for a dry run)
#   * JUnit results exist, contain no failures, and are newer than the newest
#     tracked source file
#   * every analyzer output exists and totals ZERO findings
#   * the metrics ratchet and the requirements traceability still pass (both are
#     seconds, so they are re-run rather than trusted)
#   * the quality-report PDF exists and is newer than the test results
#
# WHAT IS PUBLISHED
#
#   binaries        downloads/*.AppImage, *.zip, *.apk (+ .sha256) whose file name
#                   carries THIS version. Artefacts named for another version are
#                   listed and skipped: .github/workflows/release.yml rebuilds all
#                   four platforms on the tag, and those are the ones to attach.
#   documentation   docs/*.md, the generated Doxygen HTML and the traceability
#                   matrix, as TradingApp-<version>-docs.zip
#   qualification   the quality PDF, requirements/design/test-spec documents, every
#                   JUnit XML and every analyzer output, as
#                   TradingApp-<version>-qualification.zip — this is the bundle CI
#                   cannot produce on its own, because the Axivion section needs a
#                   licensed Suite and this machine has one
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DRY_RUN=0
ALLOW_DIRTY=0
TAG=""
while [ $# -gt 0 ]; do
    case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    --tag)
        shift
        TAG="${1:-}"
        ;;
    -h | --help)
        sed -n '2,40p' "$0"
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done

VERSION="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
TAG="${TAG:-v$VERSION}"
OUT="$ROOT/downloads"
FAIL=0

say() { printf '%s\n' "$*"; }
ok() { printf '  ok    %s\n' "$*"; }
bad() {
    printf '  FAIL  %s\n' "$*"
    FAIL=1
}
note() { printf '  note  %s\n' "$*"; }

say "== publishing TradingApp $VERSION as $TAG =="
say ""
say "-- evidence that the pipeline ran --"

# 1. A reproducible starting point.
if [ -n "$(git status --porcelain)" ]; then
    if [ "$ALLOW_DIRTY" -eq 1 ]; then
        note "working tree is modified (--allow-dirty)"
    else
        bad "working tree is modified — commit first, or pass --allow-dirty for a dry run"
    fi
else
    ok "working tree clean at $(git rev-parse --short HEAD)"
fi

# 2. The newest tracked source file is the yardstick: every artefact must be newer.
NEWEST_SRC=0
NEWEST_SRC_FILE=""
while IFS= read -r f; do
    [ -f "$f" ] || continue
    t=$(stat -c %Y "$f" 2>/dev/null || echo 0)
    if [ "$t" -gt "$NEWEST_SRC" ]; then
        NEWEST_SRC=$t
        NEWEST_SRC_FILE="$f"
    fi
done < <(git ls-files 'src/*' 'tests/*' 'CMakeLists.txt' 'tests/CMakeLists.txt')

newer_than_sources() { # $1 = path
    local t
    t=$(stat -c %Y "$1" 2>/dev/null || echo 0)
    [ "$t" -ge "$NEWEST_SRC" ]
}

# 3. Tests: present, no failures, newer than the sources.
XML_COUNT=$(find test-results -name '*.xml' -type f 2>/dev/null | wc -l)
if [ "$XML_COUNT" -eq 0 ]; then
    bad "no JUnit results in test-results/ — run ./build_all.sh test"
else
    TEST_FAILS=$(grep -ho 'failures="[0-9]*"' test-results/*.xml 2>/dev/null |
        grep -oE '[0-9]+' | awk '{s+=$1} END {print s+0}')
    TEST_ERRS=$(grep -ho 'errors="[0-9]*"' test-results/*.xml 2>/dev/null |
        grep -oE '[0-9]+' | awk '{s+=$1} END {print s+0}')
    TEST_CASES=$(grep -ho '<testcase ' test-results/*.xml 2>/dev/null | wc -l)
    if [ "$TEST_FAILS" -ne 0 ] || [ "$TEST_ERRS" -ne 0 ]; then
        bad "$XML_COUNT suites: $TEST_FAILS failure(s), $TEST_ERRS error(s)"
    elif newer_than_sources "$(find test-results -name '*.xml' -type f -printf '%T@ %p\n' |
        sort -rn | head -1 | cut -d' ' -f2-)"; then
        ok "$XML_COUNT suites, $TEST_CASES cases, 0 failures"
    else
        bad "test results are older than $NEWEST_SRC_FILE — re-run ./build_all.sh test"
    fi
fi

# 4. Analyzers: every output present, and every one of them empty.
ANALYZERS=(cppcheck clang-tidy clazy gcc-analyzer clang-analyzer pmd-cpd qmllint codespell)
ANALYSIS_TOTAL=0
MISSING_ANALYZER=0
for a in "${ANALYZERS[@]}"; do
    f="analysis-results/$a.txt"
    if [ ! -f "$f" ]; then
        MISSING_ANALYZER=1
        continue
    fi
    n=$(grep -c . "$f" 2>/dev/null) || n=0
    ANALYSIS_TOTAL=$((ANALYSIS_TOTAL + ${n:-0}))
done
if [ "$MISSING_ANALYZER" -eq 1 ]; then
    bad "analyzer outputs missing from analysis-results/ — run ./build_all.sh analysis"
elif [ "$ANALYSIS_TOTAL" -ne 0 ]; then
    bad "$ANALYSIS_TOTAL analyzer finding(s) — a release claims zero"
else
    ok "${#ANALYZERS[@]} analyzers, 0 findings"
fi

# 5. The two gates that cost seconds are re-run rather than trusted.
if python3 tools/lizard_metrics.py . analysis-results >/dev/null 2>&1; then
    ok "code-metrics ratchet clean"
else
    bad "code-metrics ratchet — see python3 tools/lizard_metrics.py . analysis-results"
fi
if TRACE=$(python3 tools/trace_report.py 2>&1) && printf '%s' "$TRACE" | grep -q "0 hard gaps"; then
    ok "requirements traceability: $(printf '%s' "$TRACE" | head -1 |
        grep -oE '[0-9]+ requirements' | head -1), 0 hard gaps"
else
    bad "requirements traceability has hard gaps — python3 tools/trace_report.py"
fi

# 6. The report itself.
PDF="$OUT/TradingApp-quality-report.pdf"
if [ ! -f "$PDF" ]; then
    bad "no quality report — run python3 tools/make_report.py"
elif newer_than_sources "$PDF"; then
    ok "quality report $(date -r "$PDF" '+%Y-%m-%d %H:%M')"
else
    bad "quality report is older than $NEWEST_SRC_FILE — re-run python3 tools/make_report.py"
fi

say ""
if [ "$FAIL" -ne 0 ]; then
    say "REFUSING to publish: the evidence above does not support the claim a release makes."
    say "Run ./build_all.sh (or the named stages) and try again."
    exit 1
fi

# --- assemble ---------------------------------------------------------------
say "-- assembling --"
mkdir -p "$OUT"
DOCS_ZIP="$OUT/TradingApp-$VERSION-docs.zip"
QUAL_ZIP="$OUT/TradingApp-$VERSION-qualification.zip"

# The BINARIES are collected FIRST, before this script writes any archive of its
# own: the archives are named for the same version, so a scan run afterwards would
# match them too and attach each one twice.
BIN_ASSETS=()
SKIPPED=()
while IFS= read -r f; do
    base="$(basename "$f")"
    case "$base" in
    TradingApp-quality-report.pdf) continue ;;
    "$(basename "$DOCS_ZIP")" | "$(basename "$QUAL_ZIP")") continue ;;
    "$(basename "$DOCS_ZIP").sha256" | "$(basename "$QUAL_ZIP").sha256") continue ;;
    esac
    case "$base" in
    *"$VERSION"*) BIN_ASSETS+=("$f") ;;
    *) SKIPPED+=("$base") ;;
    esac
done < <(find "$OUT" -maxdepth 1 -type f \
    \( -name '*.AppImage' -o -name '*.zip' -o -name '*.apk' -o -name '*.sha256' \) |
    sort)

rm -f "$DOCS_ZIP" "$QUAL_ZIP" "$DOCS_ZIP.sha256" "$QUAL_ZIP.sha256"

# The CORRESPONDING SOURCE, which GPL-3.0-or-later obliges us to offer alongside
# every binary (see THIRD_PARTY_LICENSES.md). git archive is used deliberately: it
# exports exactly what the tag contains, so the archive cannot quietly include a
# local edit that never made it into the published commit. A dirty tree is already
# refused above, so HEAD is the published tree.
SOURCE_TGZ="$OUT/TradingApp-$VERSION-source.tar.gz"
rm -f "$SOURCE_TGZ" "$SOURCE_TGZ.sha256"
if git archive --format=tar.gz --prefix="TradingApp-$VERSION/" -o "$SOURCE_TGZ" HEAD; then
    ok "source         $(basename "$SOURCE_TGZ") ($(du -h "$SOURCE_TGZ" | cut -f1))"
else
    bad "could not produce the corresponding source archive — GPL-3.0-or-later requires it"
fi

# Documentation: what a reader needs, not the whole tree. The licence texts travel
# with it so a recipient of the docs zip alone still has the terms.
DOC_ITEMS=(docs/*.md README.md LICENSE THIRD_PARTY_LICENSES.md LICENSES)
[ -f docs/traceability.html ] && DOC_ITEMS+=(docs/traceability.html)
[ -d docs/html ] && DOC_ITEMS+=(docs/html)
[ -d docs/uml ] && DOC_ITEMS+=(docs/uml)
zip -qr "$DOCS_ZIP" "${DOC_ITEMS[@]}" >/dev/null 2>&1 || true
ok "documentation  $(basename "$DOCS_ZIP") ($(du -h "$DOCS_ZIP" | cut -f1))"

# Qualification: the claim AND its evidence, in one archive.
QUAL_ITEMS=("$PDF" docs/requirements.md docs/design.md docs/test_spec.md docs/verification.md
    docs/vmodel.md requirements/requirements.sdoc test-results analysis-results)
[ -f docs/traceability.html ] && QUAL_ITEMS+=(docs/traceability.html)
[ -d coverage ] && QUAL_ITEMS+=(coverage)
zip -qr "$QUAL_ZIP" "${QUAL_ITEMS[@]}" -x '*.gcda' -x '*.gcno' >/dev/null 2>&1 || true
ok "qualification  $(basename "$QUAL_ZIP") ($(du -h "$QUAL_ZIP" | cut -f1))"

# A checksum for each archive this script produced, like the packagers do.
for z in "$DOCS_ZIP" "$QUAL_ZIP" "$SOURCE_TGZ"; do
    [ -f "$z" ] || continue
    (cd "$OUT" && sha256sum "$(basename "$z")" > "$(basename "$z").sha256")
done

ASSETS=("$PDF" "$DOCS_ZIP" "$DOCS_ZIP.sha256" "$QUAL_ZIP" "$QUAL_ZIP.sha256")
# The source archive is not optional furniture: it is the thing that makes shipping
# the GPL binaries lawful, so it is attached to every release.
[ -f "$SOURCE_TGZ" ] && ASSETS+=("$SOURCE_TGZ" "$SOURCE_TGZ.sha256")
for f in "${BIN_ASSETS[@]}"; do
    ASSETS+=("$f")
    ok "binary         $(basename "$f")"
done
if [ "${#BIN_ASSETS[@]}" -eq 0 ]; then
    note "no binary for $VERSION in downloads/ — .github/workflows/release.yml builds"
    note "    all four platforms on the tag and attaches them there"
fi
if [ "${#SKIPPED[@]}" -gt 0 ]; then
    note "skipped (built for another version):"
    for f in "${SKIPPED[@]}"; do note "    $f"; done
fi

say ""
say "-- publishing --"
if [ "$DRY_RUN" -eq 1 ]; then
    say "(dry run) would attach ${#ASSETS[@]} asset(s) to $TAG:"
    for f in "${ASSETS[@]}"; do say "    $(basename "$f")"; done
    exit 0
fi
if ! command -v gh >/dev/null 2>&1; then
    say "gh CLI not installed — the assets are in downloads/, attach them by hand" >&2
    exit 1
fi

NOTES="$(mktemp)"
{
    echo "Built and verified on $(date -u '+%Y-%m-%d %H:%M UTC') from $(git rev-parse --short HEAD)."
    echo
    echo "| Evidence | Result |"
    echo "|---|---|"
    echo "| Test suite | $TEST_CASES cases, 0 failures |"
    echo "| Static analysis | ${#ANALYZERS[@]} analyzers, 0 findings |"
    echo "| Code metrics | ratchet clean |"
    echo "| Traceability | 0 hard gaps |"
    echo
    echo "\`TradingApp-$VERSION-qualification.zip\` holds the full evidence set: the quality"
    echo "report PDF (including the Axivion MISRA C++ 2023 result, which a public runner"
    echo "cannot produce), the requirements/design/test-spec documents, every JUnit XML and"
    echo "every analyzer output. \`TradingApp-$VERSION-docs.zip\` holds the documentation and"
    echo "the generated API reference."
    echo
    echo "### Licence"
    echo
    echo "TradingApp is free software under **GPL-3.0-or-later**."
    echo "\`TradingApp-$VERSION-source.tar.gz\` is the complete corresponding source for this"
    echo "tag, attached so that the binaries above can be redistributed lawfully. The licence"
    echo "texts are in \`LICENSE\` and \`LICENSES/\`, and every component this build links or"
    echo "ships is inventoried in \`THIRD_PARTY_LICENSES.md\`."
    echo
    echo "The Qt libraries bundled inside the artefacts are used under their open-source"
    echo "licences: LGPLv3 for Core/Gui/Widgets/Network/Concurrent, and **GPLv3 for Qt"
    echo "Charts**, which has no LGPL option — that module is why this project is GPL."
} > "$NOTES"

if ! gh release view "$TAG" >/dev/null 2>&1; then
    gh release create "$TAG" --title "eToro Trader $TAG" --notes-file "$NOTES" --latest ||
        {
            say "gh release create failed — does the tag exist on the remote?" >&2
            rm -f "$NOTES"
            exit 1
        }
    say "created release $TAG"
else
    gh release edit "$TAG" --notes-file "$NOTES" --prerelease=false --draft=false --latest >/dev/null
    say "updated release $TAG"
fi
rm -f "$NOTES"

gh release upload "$TAG" "${ASSETS[@]}" --clobber
say ""
say "attached ${#ASSETS[@]} asset(s) to $TAG"
gh release view "$TAG" --json assets -q '.assets[].name' | sed 's/^/    /'
