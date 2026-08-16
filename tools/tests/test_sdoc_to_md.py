# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/sdoc_to_md.py — the StrictDoc -> docs/requirements.md
generator.

The script is flat top-level code (no main()/functions besides `cell`), and it
computes `ROOT = Path(__file__).resolve().parent.parent` and reads/writes
`requirements/requirements.sdoc` / `docs/requirements.md` AT IMPORT TIME. To
exercise it against synthetic fixtures — without ever touching this
repository's real requirements.sdoc/requirements.md, and while still letting
coverage.py attribute the executed lines to the real tools/sdoc_to_md.py file
(coverage identifies a traced frame by its `__file__` global, not merely by
the compiled code's filename, so a fully faked `__file__` silently traces
into a path outside the reported package) — each test execs the script's own
source with the REAL `__file__`/ROOT, but with `pathlib.Path.read_text` /
`write_text` patched to intercept ONLY the two calls this script itself makes
(against SDOC and OUT), substituting synthetic fixture content and capturing
the generated markdown in memory instead of touching disk.
"""

from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parent.parent / "sdoc_to_md.py"
SOURCE = SCRIPT.read_text(encoding="utf-8")
ROOT = SCRIPT.resolve().parent.parent
REAL_SDOC = ROOT / "requirements" / "requirements.sdoc"
REAL_OUT = ROOT / "docs" / "requirements.md"


def run_sdoc_to_md(monkeypatch, sdoc_text):
    """Exec the script with SDOC/OUT intercepted; returns (namespace, written).

    `written` is the text the script tried to write to docs/requirements.md
    (None if it never got there, e.g. the "nothing parsed" error path, which
    raises SystemExit and propagates to the caller).
    """
    orig_read_text = Path.read_text
    orig_write_text = Path.write_text
    written = {}

    def fake_read_text(self, *args, **kwargs):
        if self == REAL_SDOC:
            return sdoc_text
        return orig_read_text(self, *args, **kwargs)

    def fake_write_text(self, data, *args, **kwargs):
        if self == REAL_OUT:
            written["text"] = data
            return len(data)
        return orig_write_text(self, data, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", fake_read_text)
    monkeypatch.setattr(Path, "write_text", fake_write_text)

    namespace = {"__file__": str(SCRIPT), "__name__": "sdoc_to_md_under_test"}
    exec(compile(SOURCE, str(SCRIPT), "exec"), namespace)
    return namespace, written.get("text")


def test_generates_markdown_with_sections_and_rows(monkeypatch):
    sdoc = (
        "[[SECTION]]\n"
        "TITLE: Functional Requirements\n"
        "\n"
        "[REQUIREMENT]\n"
        "UID: REQ-F-001\n"
        "TITLE: Do the thing\n"
        "STATEMENT: >>>\n"
        "This is a multi paragraph\n"
        "statement continued.\n"
        "\n"
        "Second paragraph here.\n"
        "<<<\n"
        "VERIFICATION: T\n"
        "\n"
        "[REQUIREMENT]\n"
        "UID: REQ-F-002\n"
        "TITLE: Simple one\n"
        "STATEMENT: 'Quoted | statement'\n"
        "VERIFICATION: A/I\n"
    )
    ns, out_text = run_sdoc_to_md(monkeypatch, sdoc)

    assert out_text is not None
    assert "## Functional Requirements" in out_text
    assert "| REQ-F-001 |" in out_text
    assert "This is a multi paragraph statement continued.<br><br>Second paragraph here." in out_text
    assert "| REQ-F-002 | Quoted \\| statement | A/I |" in out_text
    # newline="\n" contract: no CRLF in the generated content
    assert "\r\n" not in out_text
    assert ns["sections"][0][0] == "Functional Requirements"


def test_no_requirement_blocks_at_all_exits_with_error(monkeypatch):
    with pytest.raises(SystemExit):
        run_sdoc_to_md(monkeypatch, "[[SECTION]]\nTITLE: Empty Section\n\n")


def test_section_present_but_empty_still_errors(monkeypatch):
    # sections list has an entry but its requirement list is empty ->
    # any(not reqs for _, reqs in sections) branch.
    sdoc = "[[SECTION]]\nTITLE: Empty Section\n\n[[SECTION]]\nTITLE: Another\n\n"
    with pytest.raises(SystemExit):
        run_sdoc_to_md(monkeypatch, sdoc)


def test_no_sections_at_all_exits(monkeypatch):
    with pytest.raises(SystemExit):
        run_sdoc_to_md(monkeypatch, "just some prose, not sdoc at all\n")
