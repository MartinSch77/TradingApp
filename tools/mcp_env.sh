#!/usr/bin/env bash
# Resolve the three environment variables .mcp.json needs for the Axivion MCP
# servers (axdocumentation, axdashboard):
#
#   AXIVION_SUITE_DIR      the Suite root (holds bin/rfgscript + mcps/)
#   AXIVION_MCP_PYTHON     interpreter of the Suite's MCP venv
#   AXIVION_DATABASES_DIR  dashboard database directory
#
#   tools/mcp_env.sh             report the resolved values (read-only)
#   tools/mcp_env.sh --export    print `export VAR=…` lines, for `eval`
#   tools/mcp_env.sh --persist   idempotently add them to ~/.profile
#
# Why via the environment at all: .mcp.json must not carry machine-specific
# absolute paths, and Claude Code's ${VAR} / ${VAR:-default} interpolation
# cannot branch on the platform — the Suite's MCP venv is .venv/bin/python here
# and .venv\Scripts\python.exe on Windows. Resolving that difference HERE keeps
# .mcp.json byte-identical on both. Windows counterpart: tools/mcp_env.ps1.
#
# NOTE: `$(VAR)` is Axivion's OWN config syntax (see axivion/ci_config.json) and
# does NOT work in .mcp.json — Claude Code expands `${VAR}` only, and passes
# `$(VAR)` through verbatim without even a warning. The servers then die with a
# bare "cannot find the path specified" from the shell.
#
# Exit 3 = "skipped" (the repo-wide convention for an absent license-bound
# tool), so a machine without the Axivion Suite is not a failure.
set -uo pipefail

EXIT_SKIPPED=3
MODE="${1:-report}"
PROFILE="${HOME}/.profile"
MARK_BEGIN='# >>> TradingApp Axivion MCP env >>>'
MARK_END='# <<< TradingApp Axivion MCP env <<<'

# Validate before the discovery below, so a mistyped argument reports the usage
# instead of the "no Suite installed" skip.
case "$MODE" in
report | --export | --persist) ;;
*)
    echo "usage: $0 [report|--export|--persist]" >&2
    exit 2
    ;;
esac

# Same candidate list as find_suite() in axivion/start_analysis.sh — keep the
# two in sync. An explicit AXIVION_SUITE_DIR wins so an exotic install can
# override the search entirely.
find_suite() {
    local c
    for c in \
        ${AXIVION_SUITE_DIR:+"$AXIVION_SUITE_DIR"} \
        ${BAUHAUS_HOME:+"$BAUHAUS_HOME"} \
        ${AXIVIONBASE:+"$AXIVIONBASE/bauhaus-suite"} \
        "$HOME/bauhaus-suite" \
        /opt/bauhaus-suite \
        /usr/local/bauhaus-suite; do
        [ -x "$c/bin/axivion_ci" ] && { echo "$c"; return 0; }
    done
    if command -v axivion_ci >/dev/null 2>&1; then
        c="$(dirname "$(dirname "$(command -v axivion_ci)")")"
        [ -x "$c/bin/axivion_ci" ] && { echo "$c"; return 0; }
    fi
    return 1
}

SUITE="$(find_suite)" || {
    echo "SKIPPED: the Axivion Suite is not installed (license-bound)."
    echo "         The axdocumentation / axdashboard MCP servers stay unavailable;"
    echo "         Claude Code reports them as 'Missing environment variables'."
    exit $EXIT_SKIPPED
}

MCP_PYTHON="$SUITE/mcps/axivion-mcps/.venv/bin/python"
DATABASES_DIR="${AXIVION_DATABASES_DIR:-$HOME/AxivionDashboard/config}"

# The MCP servers are a Suite technology preview — an older Suite has no mcps/.
if [ ! -x "$MCP_PYTHON" ]; then
    echo "SKIPPED: $SUITE has no MCP venv ($MCP_PYTHON)." >&2
    echo "         The Axivion MCP servers ship with Suite 7.12 and newer." >&2
    exit $EXIT_SKIPPED
fi

emit_exports() {
    printf 'export AXIVION_SUITE_DIR=%q\n' "$SUITE"
    printf 'export AXIVION_MCP_PYTHON=%q\n' "$MCP_PYTHON"
    printf 'export AXIVION_DATABASES_DIR=%q\n' "$DATABASES_DIR"
}

case "$MODE" in
report)
    printf '%-22s %s\n' "AXIVION_SUITE_DIR" "$SUITE"
    printf '%-22s %s\n' "AXIVION_MCP_PYTHON" "$MCP_PYTHON"
    printf '%-22s %s%s\n' "AXIVION_DATABASES_DIR" "$DATABASES_DIR" \
        "$([ -d "$DATABASES_DIR" ] || echo '  (does not exist yet)')"
    # Report on the durable store (what --persist writes), not on this process:
    # a shell started before --persist ran shows nothing even though the setup
    # is complete. Windows counterpart checks the User environment scope.
    if ! grep -qF "$MARK_BEGIN" "$PROFILE" 2>/dev/null; then
        echo
        echo "not persisted yet — run: tools/mcp_env.sh --persist"
        echo "(then start a new login shell, or your IDE from one)"
    fi
    ;;
--export)
    emit_exports
    ;;
--persist)
    # Rewrite the guarded block rather than appending a second copy.
    if [ -f "$PROFILE" ] && grep -qF "$MARK_BEGIN" "$PROFILE"; then
        sed -i "/$MARK_BEGIN/,/$MARK_END/d" "$PROFILE"
    fi
    {
        echo "$MARK_BEGIN"
        echo "# written by tools/mcp_env.sh — consumed by .mcp.json (Axivion MCP servers)"
        emit_exports
        echo "$MARK_END"
    } >>"$PROFILE"
    echo "wrote the Axivion MCP variables to $PROFILE:"
    emit_exports | sed 's/^/  /'
    echo
    echo "Start a new login shell (or your IDE from one) so Claude Code sees them."
    ;;
esac
