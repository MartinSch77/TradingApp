#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# cppcheck's free ADDONS — MISRA first, the other three after it. Informational by
# design: none of them gates a build, and the reason for each is measured below.
#
#   tools/misra_cppcheck.sh [build]                 # rule IDs only
#   tools/misra_cppcheck.sh build /path/rules.txt   # with your own rule texts
#
# The addon (cppcheck's addons/misra.py) is free and ships with cppcheck itself.
# Two facts decide how it is used here, and both are measured rather than assumed:
#
#  1. It implements MISRA **C** 2012 ("Partially reused for MISRA C++ 2008
#     checking", says its own header). This codebase is C++23, and the mismatch
#     dominates the output. Measured over the whole project with cppcheck 2.13:
#
#         404  misra-config        the addon could not configure/parse the file at
#                                  all (Q_OBJECT, Q_DECLARE_METATYPE and friends)
#         110  misra-c2012-12.3    "comma operator" reported on TEMPLATE ARGUMENT
#                                  LISTS — `QHash<QString, double>` is not a comma
#                                  operator, it is C++ the addon cannot parse
#          13  misra-c2012-17.2    no-recursion; mostly asynchronous continuations
#                                  the addon reads as self-calls because the call
#                                  happens from a network callback
#         ---
#         527  total, of which 514 are the language mismatch rather than the code
#
#     A checker whose 97% is a language mismatch cannot gate a build: it would
#     either be silenced wholesale (dishonest) or fail every run.
#  2. MISRA C++ 2023 IS checked in this project — by Axivion, whose configuration
#     here is MISRA-C++-2023-only (axivion/, and `./build_all.sh axivion`). That
#     is the check that applies to this language, and it is enforced.
#
# So this script exists to be run deliberately: to look at the 17.2 recursion
# reports, or to produce a MISRA-shaped report for someone who asks for one, with
# the false-positive rate stated next to it. The MISRA rule TEXTS are copyrighted
# and cannot ship with cppcheck or with this repository; pass a rule-texts file
# extracted from your own copy of the standard as the second argument to get
# readable messages instead of bare rule numbers.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build}"
RULE_TEXTS="${2:-}"
OUT="$ROOT/analysis-results"
mkdir -p "$OUT"

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "SKIPPED: cppcheck not installed (./setup.sh installs it)"
    exit 3
fi
if [ ! -f "$ROOT/$BUILD_DIR/compile_commands.json" ]; then
    echo "no compile database in $BUILD_DIR — configure that build tree first" >&2
    exit 1
fi

ADDON="misra"
if [ -n "$RULE_TEXTS" ]; then
    # cppcheck takes addon arguments as a JSON file; write one so the rule texts
    # reach misra.py.
    ADDON="$OUT/misra-addon.json"
    printf '{"script": "misra", "args": ["--rule-texts=%s"]}\n' "$RULE_TEXTS" > "$ADDON"
    echo "using rule texts from $RULE_TEXTS"
else
    echo "no rule-texts file given — messages will be bare rule IDs"
    echo "(the MISRA texts are copyrighted; extract them from your own copy of the standard)"
fi

echo "== cppcheck MISRA addon ($(cppcheck --version)) =="
# --std=c++20: the addon's front end does not parse C++23, and the point here is
# to get as far as it can rather than to fail on the first `if constexpr`.
cppcheck --project="$ROOT/$BUILD_DIR/compile_commands.json" \
    --addon="$ADDON" \
    --std=c++20 \
    --enable=style \
    --library=qt \
    -i "$ROOT/$BUILD_DIR" \
    --template='{file}|{line}|{severity}|{id}|{message}' \
    --output-file="$OUT/cppcheck-misra.txt" --quiet || true

TOTAL=$(grep -c "misra" "$OUT/cppcheck-misra.txt" 2>/dev/null || true)
COMMA=$(grep -c "misra-c2012-12.3" "$OUT/cppcheck-misra.txt" 2>/dev/null || true)
CONFIG=$(grep -c "misra-config" "$OUT/cppcheck-misra.txt" 2>/dev/null || true)
echo "MISRA-C-2012 findings: ${TOTAL:-0} (analysis-results/cppcheck-misra.txt)"
echo "  of which ${CONFIG:-0} are misra-config (the addon could not parse the file at all)"
echo "  and ${COMMA:-0} are rule 12.3 on C++ TEMPLATE ARGUMENT LISTS — false by construction"
echo "  MISRA C++ 2023 for this codebase is enforced by Axivion: ./build_all.sh axivion"

# The other three free addons, for the same reason: worth looking at, not worth
# gating. Their measured character is in the header of this file and in
# tools/static_analysis.sh, where they are deliberately absent.
echo
echo "== the other free cppcheck addons (informational) =="
cppcheck --project="$ROOT/$BUILD_DIR/compile_commands.json" \
    --addon=threadsafety --addon=findcasts --addon=misc \
    --enable=style \
    --library=qt \
    -i "$ROOT/$BUILD_DIR" \
    --template='{file}|{line}|{severity}|{id}|{message}' \
    --output-file="$OUT/cppcheck-addons.txt" --quiet || true
if [ -f "$OUT/cppcheck-addons.txt" ]; then
    echo "addon findings: $(grep -c . "$OUT/cppcheck-addons.txt") (analysis-results/cppcheck-addons.txt)"
    cut -d"|" -f4 "$OUT/cppcheck-addons.txt" | sort | uniq -c | sort -rn | sed "s/^/  /"
    echo "  misc-implicitlyVirtual wants 'virtual' repeated on an 'override' — the"
    echo "  opposite of modern C++; threadsafety flags getenv in Config::load, which"
    echo "  runs once before any thread exists; findcasts is a cast INVENTORY."
fi
# Informational by design: never fails the caller.
exit 0
