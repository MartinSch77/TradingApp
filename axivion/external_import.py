# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# TradingApp — external-analyzer import layer (Axivion Python configuration).
#
# Brings the output of the project's third-party analyzers AND the dynamic
# checkers onto the Axivion dashboard through the Suite's official import
# mechanism (reference manual 6.2.10 "ImportExternalAnalysisOutput" + 6.2.4.4
# "ExternalAnalysisFormats"):
#
#   static   tools/static_analysis.{sh,ps1} -> analysis-results/
#            {cppcheck,clang-tidy,clazy,gcc-analyzer,msvc-analyze,codespell}.txt
#            (+ sonarqube.txt from tools/sonar_scan.{sh,ps1}, + coverity.txt from
#            tools/coverity_findings.py — Coverity Scan analyses in the cloud
#            (.github/workflows/coverity.yml), so its defects arrive as an
#            export that is converted, not as a local tool run)
#   dynamic  tools/sanitize.{sh,ps1}  -> analysis-results/
#            sanitize-{asan-ubsan,tsan,valgrind,asan,ubsan}.txt
#            (ASan+UBSan+LSan / ThreadSanitizer / valgrind memcheck, normalized
#            by tools/parse_sanitizer_log.py; asan/ubsan are the Windows split
#            of the combined Linux asan-ubsan build)
#
# One ImportExternalAnalysisOutput copy per tool cats its log during
# axivion_ci and re-emits every finding line as a style violation — provider
# is the tool name, errno the tool's own rule id — so dashboard filtering,
# suppression and delta views work exactly as for native rules.
#
# The import is tolerant when a log is missing (check_returncode = False): an
# analysis run without a prior static_analysis.sh / sanitize.sh imports
# nothing for that provider.
#
# Why a Python layer and not rule_config.json: the GenericFormat "matchlist"
# option is typed bauhaus.teecap.Match — the Suite's JSON validator rejects
# any JSON representation ("is not of expected type bauhaus.teecap.Match",
# verified against 7.12.3), so matchers can only be constructed in a Python
# configuration layer. This file IS part of the Axivion configuration: it is
# registered in axivion_config.json under "_Layers".

import pathlib
import sys

import axivion.config
from bauhaus import teecap

analysis = axivion.config.get_analysis()


def _project_root() -> pathlib.Path:
    """The project directory, found by walking up to the CMakeLists.txt.

    Not simply parent.parent: this layer is also loaded from the nested
    Windows configuration directory (axivion/windows/), where the project root
    is two levels further up.
    """
    for candidate in pathlib.Path(__file__).resolve().parents:
        if (candidate / 'CMakeLists.txt').is_file():
            return candidate
    return pathlib.Path(__file__).resolve().parent.parent


_ROOT = _project_root()

# The import rule shells out to a "print this file" command. There is no cat on
# Windows, and `type` is a cmd.exe builtin rather than an executable, so it has
# to be invoked through the interpreter.
if sys.platform == 'win32':
    _CAT_COMMAND, _CAT_ARGS = 'cmd', ['/c', 'type']
else:
    _CAT_COMMAND, _CAT_ARGS = 'cat', []

# clang-tidy.txt and clazy.txt hold GCC-style lines: file:line:col: warning: msg [id]
_GCC_STYLE = (
    r'(?P<filename>.+?):(?P<line>\d+):(?P<column>\d+): '
    r'(?P<severity>warning|error): (?P<message>.*) \[(?P<errno>[^\]]+)\]$'
)
# cppcheck and the sanitizer logs use the pipe format: file|line|severity|id|message
_PIPE = (
    r'(?P<filename>[^|]+)\|(?P<line>\d+)\|(?P<severity>[^|]*)\|'
    r'(?P<errno>[^|]*)\|(?P<message>.*)$'
)

# provider -> (log file under analysis-results/, matcher regex)
_TOOLS = {
    'cppcheck': ('cppcheck.txt', _PIPE),
    'clang-tidy': ('clang-tidy.txt', _GCC_STYLE),
    'clazy': ('clazy.txt', _GCC_STYLE),
    'gcc-analyzer': ('gcc-analyzer.txt', _GCC_STYLE),
    # The Clang Static Analyzer run standalone (tools/clang_analyzer.py): the
    # off-by-default checkers plus a deeper search than clang-tidy's inline
    # clang-analyzer-* checks can be configured for.
    'clang-analyzer': ('clang-analyzer.txt', _GCC_STYLE),
    # Code metrics (tools/lizard_metrics.py): per-function complexity, length
    # and parameter count over threshold. Ratcheted against
    # tools/lizard_baseline.json, so these arrive as visible-but-known debt.
    'lizard': ('lizard.txt', _PIPE),
    # Copy-paste detection (tools/cpd_scan.py) at >= 100 tokens — the clone GATE.
    # NOT because Axivion has no clone check: C++CloneDetection is _active in
    # rule_config.json and reports its own clones as issue type CL (2 at the time of
    # writing). The two coexist deliberately — PMD CPD's threshold is the one this
    # project gates on, Axivion's is informational. An earlier version of this comment
    # claimed the Axivion configuration was "MISRA-only", which was wrong: it also runs
    # 133 CWE rules and the clone check.
    'pmd-cpd': ('pmd-cpd.txt', _PIPE),
    # Windows counterpart of gcc-analyzer: MSVC /analyze, normalized to the
    # same GCC-style lines by tools/msvc_analyze.py. Absent on Linux runs.
    'msvc-analyze': ('msvc-analyze.txt', _GCC_STYLE),
    'codespell': ('codespell.txt', _PIPE),
    'sonarqube': ('sonarqube.txt', _PIPE),
    # Coverity Scan defects, exported from the cloud service and normalized by
    # tools/coverity_findings.py. Absent until someone runs that converter.
    'coverity': ('coverity.txt', _PIPE),
    'asan-ubsan': ('sanitize-asan-ubsan.txt', _PIPE),
    'tsan': ('sanitize-tsan.txt', _PIPE),
    'valgrind': ('sanitize-valgrind.txt', _PIPE),
    # Windows: ASan (MSVC /fsanitize=address) and UBSan (clang-cl) are separate
    # build trees rather than the single combined GCC build, so they report as
    # separate providers. Absent on Linux runs.
    'asan': ('sanitize-asan.txt', _PIPE),
    'ubsan': ('sanitize-ubsan.txt', _PIPE),
}

for _tool, (_log, _regex) in _TOOLS.items():
    _format_rule = f'GenericFormat {_tool}'
    _import_rule = f'ImportExternalAnalysisOutput {_tool}'
    analysis.copy(
        'GenericFormat',
        _format_rule,
        provider=_tool,
        matchlist=teecap.Match(_regex),
    )
    analysis.copy(
        'ImportExternalAnalysisOutput',
        _import_rule,
        command=_CAT_COMMAND,
        options=_CAT_ARGS + [str(_ROOT / 'analysis-results' / _log)],
        capture_stdout_provider=_tool,
        check_returncode=False,  # missing log -> nothing to import
        strip_path_prefix=str(_ROOT),  # some logs contain absolute paths
    )
    analysis.activate(_format_rule, _import_rule)
