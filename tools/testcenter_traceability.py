#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Export the requirements as Squish Test Center generic-traceability data.

    python3 tools/testcenter_traceability.py            # -> test-results/testcenter-traceability.csv
    python3 tools/testcenter_traceability.py --stdout   # print it instead
    python3 tools/testcenter_traceability.py --check    # verify only, write nothing

WHY THIS EXISTS. Test Center can show which requirements a test run covers, but it
only knows requirements it has been told about, and it learns them from one of the
3rd-party integrations (Jira, Xray, Polarion, Azure DevOps, ...). This project keeps
its requirements in StrictDoc, so none of those apply. The GENERIC traceability
integration is the documented answer: it takes a CSV of `id,name,uri,project` and
treats those rows as the external system's requirements.

That makes `requirements/requirements.sdoc` the source for Test Center too, which is
the whole point — the non-negotiable rule of this repository is that requirements live
in exactly one place. A hand-maintained requirement list inside Test Center would be a
second source of truth, and the first time it disagreed with the sdoc file the
traceability view would be reporting coverage of requirements that no longer exist.

The `uri` column points at the GENERATED requirements document on the code host, so a
requirement in Test Center is one click from its full statement. The host comes from
`git remote get-url origin`; nothing here hardcodes a URL.

AFTER RUNNING THIS, two manual steps remain in the Test Center UI (neither has a
command-line equivalent in 4.3.0):
  1. Global Settings -> turn on Generic Integration -> upload this CSV.
  2. Map the external project (the `project` column below) to the Test Center project.
Then the mapping from tests to requirements can be automated, because the test sources
already carry the ids:

    testcentercmd integration map --integration=generic --project=TradingApp \\
        --repository=<id> --branch=main --prefix='@relation('

The prefix is what keeps the match honest: every requirement reference in a test file
is written `// @relation(REQ-…, scope=function)`, so requiring the prefix means a bare
mention of an id in a comment or a string literal is not mistaken for a verification
link. Details and the surrounding workflow: docs/qt-tools.md.
"""

from __future__ import annotations

import argparse
import csv
import io
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDOC = ROOT / "requirements" / "requirements.sdoc"
DEFAULT_OUT = ROOT / "test-results" / "testcenter-traceability.csv"

# The external "project" these requirements belong to, as Test Center will list it.
# The sdoc document's own UID, so the name in Test Center matches the name in the file
# rather than being a third invention.
EXTERNAL_PROJECT = "SRS-TRADINGAPP"

UID_RE = re.compile(r"^UID:\s*(REQ-[FN]-\d{3})\s*$")
TITLE_RE = re.compile(r"^TITLE:\s*(.+?)\s*$")
VERIFICATION_RE = re.compile(r"^VERIFICATION:\s*(\S+)\s*$")


class Requirement:
    """One requirement, reduced to what Test Center's CSV columns need."""

    def __init__(self, uid: str, title: str, verification: str) -> None:
        self.uid = uid
        self.title = title
        self.verification = verification

    @property
    def name(self) -> str:
        # Test Center shows this string in its traceability views. It leads with the id
        # so the list sorts the way the requirements document reads, and it carries the
        # VERIFICATION method because "covered by a test" means something different for
        # a T requirement than for one verified by analysis or inspection.
        return f"{self.uid} [{self.verification}] {self.title}"


def parse_requirements(path: pathlib.Path) -> list[Requirement]:
    """Read the [REQUIREMENT] blocks out of the StrictDoc file.

    Deliberately a small line parser rather than a StrictDoc dependency: this repository
    already parses the same file this way in tools/trace_report.py, and the fields used
    here (UID, TITLE, VERIFICATION) are the ones the grammar at the top of the file
    declares REQUIRED, so they cannot silently go missing.
    """
    out: list[Requirement] = []
    uid: str | None = None
    title: str | None = None
    verification: str | None = None
    in_block = False

    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\n")
        if line.strip() == "[REQUIREMENT]":
            in_block, uid, title, verification = True, None, None, None
            continue
        if not in_block:
            continue
        # A new bracketed element ends the block. STATEMENT bodies are >>> ... <<<
        # blocks and are not read here, so no bracket inside one can be reached.
        if line.startswith("[") and line.strip() != "[REQUIREMENT]":
            in_block = False
            continue
        if match := UID_RE.match(line):
            uid = match.group(1)
        elif match := VERIFICATION_RE.match(line):
            verification = match.group(1)
        elif match := TITLE_RE.match(line):
            # TITLE appears after UID inside a requirement block; the document and
            # section titles are outside one and never reach here.
            title = match.group(1)
        if uid and title and verification:
            out.append(Requirement(uid, title, verification))
            in_block, uid, title, verification = False, None, None, None
    return out


def repo_web_url() -> str | None:
    """The browsable base URL of the origin remote, or None when there is no remote."""
    try:
        remote = subprocess.run(
            ["git", "-C", str(ROOT), "remote", "get-url", "origin"],
            capture_output=True, text=True, check=True, timeout=10).stdout.strip()
    except (subprocess.SubprocessError, OSError):
        return None
    if not remote:
        return None
    if remote.startswith("git@"):
        remote = "https://" + remote[len("git@"):].replace(":", "/", 1)
    elif not remote.startswith("http"):
        return None
    return remote.removesuffix(".git")


def default_branch() -> str:
    """The branch to link into. The current one, falling back to main."""
    try:
        branch = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, check=True, timeout=10).stdout.strip()
    except (subprocess.SubprocessError, OSError):
        return "main"
    return branch if branch and branch != "HEAD" else "main"


def uri_for(req: Requirement, base: str | None, branch: str) -> str:
    """A link to the requirement's own heading in the generated document.

    docs/requirements.md is generated from the sdoc file, and its headings render as
    GitHub anchors, so `#req-f-001` lands on the statement itself. Without a remote the
    column carries the repository-relative path instead: the CSV column must not be
    empty, and a path is still information.
    """
    anchor = req.uid.lower()
    if not base:
        return f"docs/requirements.md#{anchor}"
    return f"{base}/blob/{branch}/docs/requirements.md#{anchor}"


def write_csv(reqs: list[Requirement], stream: io.TextIOBase, base: str | None,
              branch: str) -> None:
    # The header names are the product's: id, name, uri, project.
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(["id", "name", "uri", "project"])
    for req in reqs:
        writer.writerow([req.uid, req.name, uri_for(req, base, branch), EXTERNAL_PROJECT])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT,
                        help=f"output file (default {DEFAULT_OUT.relative_to(ROOT)})")
    parser.add_argument("--stdout", action="store_true", help="print instead of writing")
    parser.add_argument("--check", action="store_true",
                        help="parse and report only; write nothing")
    args = parser.parse_args()

    if not SDOC.is_file():
        print(f"requirements file not found: {SDOC}", file=sys.stderr)
        return 1

    reqs = parse_requirements(SDOC)
    if not reqs:
        # An empty export would upload cleanly and quietly remove every requirement
        # from Test Center's view if the "remove data not in this upload" option is on.
        print(f"no [REQUIREMENT] blocks parsed from {SDOC} — refusing to write an "
              f"empty traceability file", file=sys.stderr)
        return 1

    duplicates = {r.uid for r in reqs if [x.uid for x in reqs].count(r.uid) > 1}
    if duplicates:
        print(f"duplicate requirement ids: {sorted(duplicates)}", file=sys.stderr)
        return 1

    base, branch = repo_web_url(), default_branch()
    functional = sum(1 for r in reqs if r.uid.startswith("REQ-F-"))
    # Progress goes to stderr, never stdout: with --stdout the CSV IS stdout, and a
    # status line landing in it produces a file that uploads as a broken requirement.
    note = lambda text: print(text, file=sys.stderr)  # noqa: E731 — one local alias
    note(f"requirements: {len(reqs)} ({functional} functional, "
         f"{len(reqs) - functional} non-functional)")
    note(f"external project: {EXTERNAL_PROJECT}")
    note(f"link base: {base or '(no origin remote — relative paths)'} @ {branch}")

    if args.check:
        note("check only — nothing written")
        return 0
    if args.stdout:
        write_csv(reqs, sys.stdout, base, branch)
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as handle:
        write_csv(reqs, handle, base, branch)
    note(f"wrote {args.out.relative_to(ROOT) if args.out.is_relative_to(ROOT) else args.out}")
    note("next: Test Center -> Global Settings -> Generic Integration -> upload it,")
    note("      then map the external project to the Test Center project.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
