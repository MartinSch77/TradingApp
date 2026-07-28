#!/usr/bin/env python3
"""Coverity defects -> analysis-results/coverity.txt for the Axivion import.

Coverity runs here as the cloud service only -- scan.coverity.com project
"tradingapp", submitted by .github/workflows/coverity.yml -- and it analyses
server-side, so its defects do not fall out of a local build the way cppcheck's
or clazy's do. This tool is the bridge onto the one dashboard this project
insists on: it normalizes the exported defect list into the pipe format
axivion/external_import.py imports as provider "coverity" --

    src/domain/Foo.cpp|123|high|RESOURCE_LEAK|CID 1234: Variable "p" going out of scope leaks the storage it points to

Usage (the Python tools are shared verbatim between Linux and Windows):

    python3 tools/coverity_findings.py coverity-export.csv
    python3 tools/coverity_findings.py errors.json -o analysis-results/coverity.txt
    python3 tools/coverity_findings.py export.csv --strip-prefix /home/runner/work

Accepted inputs, auto-detected ("-" reads stdin):

  * the CSV the Coverity Scan web UI exports from "View Defects" -- the route
    that matches this project's cloud-only setup.
  * the JSON an issues view returns (rows of column values).
  * `cov-format-errors --json-output-v7` output, in case a licensed cov-analyze
    is ever at hand -- richest source (checker, impact, main event line).

Those three spell their fields differently, and the spellings have changed
between Coverity versions, so every lookup walks a candidate list instead of one
hard-coded key. A record that carries none of the known spellings for file or
checker is COUNTED AND REPORTED, never silently dropped -- an import layer that
quietly produces an empty log is worse than one that says it did not understand
the export.

Defects triaged away in Coverity (false positive / intentional / dismissed) are
skipped, so triage on the Coverity side does not have to be repeated on the
Axivion side. Pass --include-triaged to import them anyway.
"""

import argparse
import csv
import io
import json
import pathlib
import sys

# Field-name candidates, most specific first. json-output-v7 uses the long
# "mainEvent*" names; Connect views and CSV exports use display names.
_FILE_KEYS = (
    'strippedMainEventFilePathname',
    'mainEventFilePathname',
    'strippedFilePathname',
    'filePathname',
    'displayFile',
    'File',
    'file',
    'filename',
    'Path',
)
_LINE_KEYS = (
    'mainEventLineNumber',
    'displayLineNumber',
    'lineNumber',
    'Line Number',
    'Line',
    'line',
)
_CHECKER_KEYS = (
    'checkerName',
    'displayCheckerName',
    'checker',
    'Checker',
    'Type',
    'type',
)
_IMPACT_KEYS = ('displayImpact', 'impact', 'Impact', 'severity', 'Severity')
_CID_KEYS = ('cid', 'CID', 'mergeKey')
_MESSAGE_KEYS = (
    'subcategoryShortDescription',
    'subcategoryLongDescription',
    'longDescription',
    'displayType',
    'Type',
    'type',
    'Description',
    'description',
)
# Triage states that mean "already dealt with in Coverity".
_TRIAGED = {
    'false positive',
    'intentional',
    'dismissed',
    'ignore',
    'no test needed',
}
_TRIAGE_KEYS = (
    'classification',
    'Classification',
    'action',
    'Action',
    'localStatus',
    'status',
    'Status',
    'triage',
)


def _flatten(record: dict) -> dict:
    """One flat mapping per defect: top-level keys win over nested ones.

    json-output-v7 hides the human-readable text one level down, in
    "checkerProperties" ("subcategoryShortDescription") and "properties".
    """
    flat = {key: value for key, value in record.items() if not isinstance(value, dict)}
    for value in record.values():
        if isinstance(value, dict):
            for nested_key, nested_value in value.items():
                if not isinstance(nested_value, (dict, list)):
                    flat.setdefault(nested_key, nested_value)
    return flat


def _first(flat: dict, keys: tuple, default: str = '') -> str:
    for key in keys:
        value = flat.get(key)
        if value not in (None, ''):
            return str(value)
    return default


def _main_event_text(record: dict) -> str:
    """The description of the event Coverity marks as the defect's main one."""
    for event in record.get('events') or []:
        if isinstance(event, dict) and event.get('main'):
            return str(event.get('eventDescription') or '')
    return ''


def _clean(text: str) -> str:
    """One line, no pipes -- the import matcher splits on both."""
    return ' '.join(str(text).replace('|', '/').split())


def _relative(path: str, root: pathlib.Path, strip_prefixes: list) -> str:
    """Project-relative, forward slashes, whatever machine produced the export.

    A Coverity Scan export carries the CI runner's absolute paths
    (/home/runner/work/TradingApp/TradingApp/src/...), which no local dashboard
    can resolve. Explicit --strip-prefix values go first, then the project root,
    then the last occurrence of the root's own directory name -- that final rule
    is what folds the doubled GitHub checkout path down to "src/...".
    """
    text = str(path).replace('\\', '/').strip()
    if not text:
        return ''
    for prefix in strip_prefixes:
        prefix = prefix.replace('\\', '/').rstrip('/')
        if prefix and text.startswith(prefix + '/'):
            return text[len(prefix) + 1:]
    root_text = str(root).replace('\\', '/').rstrip('/')
    if root_text and text.startswith(root_text + '/'):
        return text[len(root_text) + 1:]
    marker = '/' + root.name + '/'
    if marker in text:
        return text.rsplit(marker, 1)[1]
    return text.lstrip('/')


def _records(payload) -> list:
    """The defect list, wherever this particular export shape keeps it."""
    if isinstance(payload, list):
        return [item for item in payload if isinstance(item, dict)]
    if not isinstance(payload, dict):
        return []
    # json-output-v7: {"issues": [...]}; Connect view: {"viewContentsV1": {"rows": [...]}}
    for key in ('issues', 'rows', 'defects', 'mergedDefects'):
        value = payload.get(key)
        if isinstance(value, list):
            return [item for item in value if isinstance(item, dict)]
    for value in payload.values():
        if isinstance(value, dict):
            nested = _records(value)
            if nested:
                return nested
    return []


def _parse(text: str) -> list:
    """JSON if it parses as JSON, CSV otherwise (both exports are offered)."""
    stripped = text.lstrip()
    if stripped.startswith(('{', '[')):
        return _records(json.loads(text))
    if not stripped:
        return []
    sample = text[:4096]
    try:
        dialect = csv.Sniffer().sniff(sample, delimiters=',;\t')
    except csv.Error:
        dialect = csv.excel
    return [row for row in csv.DictReader(io.StringIO(text), dialect=dialect) if row]


def _convert(records: list, root: pathlib.Path, strip_prefixes: list,
             include_triaged: bool):
    """(lines, skipped_triaged, unrecognized) for the given defect records."""
    lines = []
    skipped = 0
    unrecognized = 0
    for record in records:
        flat = _flatten(record)
        if not include_triaged:
            triage = _first(flat, _TRIAGE_KEYS).strip().lower()
            if triage in _TRIAGED:
                skipped += 1
                continue
        path = _relative(_first(flat, _FILE_KEYS), root, strip_prefixes)
        checker = _clean(_first(flat, _CHECKER_KEYS))
        if not path or not checker:
            unrecognized += 1
            continue
        line = _first(flat, _LINE_KEYS, '1')
        try:
            line = str(max(1, int(float(line))))
        except ValueError:
            line = '1'
        severity = _clean(_first(flat, _IMPACT_KEYS, 'warning')).lower() or 'warning'
        message = _clean(_first(flat, _MESSAGE_KEYS) or _main_event_text(record)
                         or checker)
        cid = _clean(_first(flat, _CID_KEYS))
        if cid:
            message = f'CID {cid}: {message}'
        lines.append(f'{path}|{line}|{severity}|{checker}|{message}')
    return lines, skipped, unrecognized


def main() -> int:
    root_default = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description='Normalize a Coverity export into the pipe format the '
                    'Axivion external-findings import reads.')
    parser.add_argument('export', help='Coverity JSON or CSV export ("-" = stdin)')
    parser.add_argument('-o', '--output', default=None,
                        help='output file (default: analysis-results/coverity.txt)')
    parser.add_argument('--root', default=str(root_default),
                        help='project root used to relativize paths')
    parser.add_argument('--strip-prefix', action='append', default=[],
                        metavar='PATH',
                        help='additional absolute path prefix to strip '
                             '(repeatable; e.g. the CI checkout directory)')
    parser.add_argument('--include-triaged', action='store_true',
                        help='also import defects Coverity has triaged away')
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    output = pathlib.Path(args.output) if args.output else root / 'analysis-results' / 'coverity.txt'

    if args.export == '-':
        text = sys.stdin.read()
    else:
        source = pathlib.Path(args.export)
        if not source.is_file():
            print(f'coverity: no such export: {source}', file=sys.stderr)
            return 1
        text = source.read_text(encoding='utf-8', errors='replace')

    try:
        records = _parse(text)
    except (json.JSONDecodeError, csv.Error) as error:
        print(f'coverity: cannot parse the export ({error})', file=sys.stderr)
        return 1

    lines, skipped, unrecognized = _convert(records, root, args.strip_prefix,
                                            args.include_triaged)

    output.parent.mkdir(parents=True, exist_ok=True)
    # encoding/newline pinned: both platforms must produce the same bytes.
    with open(output, 'w', encoding='utf-8', newline='\n') as handle:
        for line in lines:
            handle.write(line + '\n')

    print(f'coverity: {len(lines)} defects -> {output}'
          + (f' ({skipped} triaged, not imported)' if skipped else ''))
    if unrecognized:
        # Loud on purpose: an unfamiliar export shape must not look like "clean".
        print(f'coverity: WARNING {unrecognized} of {len(records)} records had no '
              'recognizable file/checker field — is this a Coverity export? '
              'Add the field names to _FILE_KEYS / _CHECKER_KEYS in '
              f'{pathlib.Path(__file__).name}.', file=sys.stderr)
    if not records:
        print('coverity: the export contained no defect records '
              '(empty log = no open findings for the provider).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
