#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Release notes for the release CI creates, built from the evidence it has.

.github/workflows/release.yml creates the GitHub release and attaches the binaries.
Until this tool existed it used `gh release create --generate-notes`, so a release
published by CI alone carried a bare list of merged PRs — none of the evidence the
release is a claim about (v1.1.2, 2026-09-29). tools/publish_release.sh later
REPLACES these notes with its own, which add the Axivion-backed qualification bundle;
this is what the release says until then, and it must not claim more than CI measured:

  * the test count and failures, counted from the JUnit XML the quality-report job
    uploaded (every suite, including the nested test-results/squish/ ones);
  * static analysis and the metrics ratchet as "passed the release gate" — the publish
    job only runs once that job succeeded, so it is a fact, not a re-measurement;
  * the traceability summary line exactly as tools/trace_report.py printed it;
  * and, stated plainly, that the Axivion result is NOT part of these notes yet.

Usage: release_notes.py --version 1.1.2 --commit abc1234 --test-results DIR
                        [--trace-summary LINE] [--out FILE]
"""

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def junit_counts(results_dir):
    """(cases, failures, errors, files) over every *.xml below `results_dir`.

    A malformed file is counted as one error rather than skipped: a report that
    cannot be read is not evidence of a pass.
    """
    cases = failures = errors = files = 0
    for path in sorted(Path(results_dir).rglob("*.xml")):
        files += 1
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            errors += 1
            continue
        for case in root.iter("testcase"):
            cases += 1
            if case.find("failure") is not None:
                failures += 1
            if case.find("error") is not None:
                errors += 1
    return cases, failures, errors, files


def render(version, commit, counts, trace_summary):
    """The notes as Markdown. Refuses (ValueError) to describe a red or empty suite."""
    cases, failures, errors, files = counts
    if files == 0 or cases == 0:
        raise ValueError("no JUnit results found — refusing to describe an unmeasured release")
    if failures or errors:
        raise ValueError(f"{failures} failure(s), {errors} error(s) — not a releasable suite")
    trace = trace_summary.strip() if trace_summary else "see docs/traceability.html"
    lines = [
        f"Built by CI from `{commit}`.",
        "",
        "| Evidence | Result |",
        "|---|---|",
        f"| Test suite | {cases} cases in {files} JUnit files, 0 failures |",
        "| Static analysis | 0 findings — passed the release gate |",
        "| Code metrics | ratchet clean — passed the release gate |",
        f"| Traceability | {trace} |",
        "",
        "The Axivion MISRA C++ 2023 result is **not** in these notes yet: a public runner",
        "cannot produce it. `tools/publish_release.sh`, run on a licensed machine, replaces",
        f"these notes and attaches `TradingApp-{version}-qualification.zip` (the quality",
        "report including Axivion, every JUnit XML and analyzer output) and",
        f"`TradingApp-{version}-docs.zip`.",
        "",
        "### Licence",
        "",
        "TradingApp is free software under **GPL-3.0-or-later**.",
        f"`TradingApp-{version}-source.tar.gz` is the complete corresponding source for this",
        "tag. The licence texts are in `LICENSE` and `LICENSES/`, and every component this",
        "build links or ships is inventoried in `THIRD_PARTY_LICENSES.md`.",
        "",
    ]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--test-results", required=True)
    parser.add_argument("--trace-summary", default="")
    parser.add_argument("--out", default="-")
    args = parser.parse_args(argv)
    try:
        notes = render(args.version, args.commit, junit_counts(args.test_results),
                       args.trace_summary)
    except ValueError as exc:
        print(f"release_notes: {exc}", file=sys.stderr)
        return 1
    if args.out == "-":
        sys.stdout.write(notes)
    else:
        Path(args.out).write_text(notes, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
