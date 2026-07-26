# TradingApp — external-analyzer import layer (Axivion Python configuration).
#
# Brings the output of the project's third-party analyzers AND the dynamic
# checkers onto the Axivion dashboard through the Suite's official import
# mechanism (reference manual 6.2.10 "ImportExternalAnalysisOutput" + 6.2.4.4
# "ExternalAnalysisFormats"):
#
#   static   tools/static_analysis.sh -> analysis-results/{cppcheck,clang-tidy,clazy}.txt
#   dynamic  tools/sanitize.sh        -> analysis-results/sanitize-{asan-ubsan,tsan,valgrind}.txt
#            (ASan+UBSan+LSan / ThreadSanitizer / valgrind memcheck, normalized
#            by tools/parse_sanitizer_log.py)
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

import axivion.config
from bauhaus import teecap

analysis = axivion.config.get_analysis()

_ROOT = pathlib.Path(__file__).resolve().parent.parent  # the project directory

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
    'codespell': ('codespell.txt', _PIPE),
    'sonarqube': ('sonarqube.txt', _PIPE),
    'asan-ubsan': ('sanitize-asan-ubsan.txt', _PIPE),
    'tsan': ('sanitize-tsan.txt', _PIPE),
    'valgrind': ('sanitize-valgrind.txt', _PIPE),
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
        command='cat',
        options=[str(_ROOT / 'analysis-results' / _log)],
        capture_stdout_provider=_tool,
        check_returncode=False,  # missing log -> nothing to import
        strip_path_prefix=str(_ROOT),  # some logs contain absolute paths
    )
    analysis.activate(_format_rule, _import_rule)
