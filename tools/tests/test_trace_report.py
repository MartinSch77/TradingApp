# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/trace_report.py — the requirements<->design<->test-spec
<->test-impl<->test-result traceability matrix generator that gates the
release on "hard gaps".

All tests monkeypatch trace_report.ROOT to a tmp_path fixture tree so the
suite never depends on (or drifts with) this repository's own requirements.
"""

import xml.etree.ElementTree as ET

import pytest

import trace_report


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------------------
# parse_requirements
# ---------------------------------------------------------------------------


def test_parse_requirements_reads_statement_cell(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "docs/requirements.md",
        "not a table line mentioning REQ-F-999\n"
        "| REQ-F-001 | Do the thing | T |\n",
    )
    reqs = trace_report.parse_requirements()
    assert reqs == {"REQ-F-001": "Do the thing"}


def test_parse_requirements_short_row_has_empty_statement(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    # Only two cells after split("|") counting -> len(cells) <= 2 branch.
    write(tmp_path / "docs/requirements.md", "|REQ-F-002\n")
    reqs = trace_report.parse_requirements()
    assert reqs == {"REQ-F-002": ""}


# ---------------------------------------------------------------------------
# parse_requirement_verification
# ---------------------------------------------------------------------------


def test_parse_requirement_verification_splits_methods(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "docs/requirements.md",
        "| REQ-F-001 | Do the thing | T |\n"
        "| REQ-F-002 | Another | A/I |\n",
    )
    verification = trace_report.parse_requirement_verification()
    assert verification["REQ-F-001"] == {"T"}
    assert verification["REQ-F-002"] == {"A", "I"}


def test_parse_requirement_verification_missing_cell_is_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(tmp_path / "docs/requirements.md", "|REQ-F-003\n")
    verification = trace_report.parse_requirement_verification()
    assert verification["REQ-F-003"] == set()


# ---------------------------------------------------------------------------
# parse_design
# ---------------------------------------------------------------------------


def test_parse_design_collects_satisfied_reqs(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "docs/design.md",
        "not a table row DES-FOO-A1\n"
        "| DES-FOO-A1 | REQ-F-001 REQ-F-002 |\n"
        "| plain row with no design id |\n",
    )
    design = trace_report.parse_design()
    assert design == {"DES-FOO-A1": {"REQ-F-001", "REQ-F-002"}}


# ---------------------------------------------------------------------------
# parse_test_spec
# ---------------------------------------------------------------------------


def test_parse_test_spec_reads_rows_only(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "docs/test_spec.md",
        "TS-FOO-001 mentioned in prose, not a row\n"
        "| TS-FOO-001 | some spec text |\n",
    )
    spec = trace_report.parse_test_spec()
    assert list(spec) == ["TS-FOO-001"]
    assert spec["TS-FOO-001"].startswith("| TS-FOO-001")


# ---------------------------------------------------------------------------
# parse_test_impl
# ---------------------------------------------------------------------------


def test_parse_test_impl_extracts_tags(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "tests/tst_sample.cpp",
        "//! @tstid TS-FOO-001 @design DES-FOO-A1\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_001_something()\n"
        "{\n"
        "}\n",
    )
    impl = trace_report.parse_test_impl()
    assert set(impl) == {"TS-FOO-001"}
    info = impl["TS-FOO-001"]
    assert info["file"] == "tst_sample.cpp"
    assert info["function"] == "TS_FOO_001_something"
    assert info["verifies"] == {"REQ-F-001"}
    assert info["design"] == {"DES-FOO-A1"}


def test_parse_test_impl_no_match_when_no_tstid(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "tests/tst_other.cpp",
        "// just a helper, not a tagged test\n"
        "void helperFunction()\n"
        "{\n"
        "}\n",
    )
    impl = trace_report.parse_test_impl()
    assert impl == {}


# ---------------------------------------------------------------------------
# parse_results
# ---------------------------------------------------------------------------


def test_parse_results_bare_testsuite_root(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "test-results/one.xml",
        '<testsuite name="tst_sample" tests="1">'
        '<testcase name="TS_FOO_001_something"/>'
        "</testsuite>",
    )
    results = trace_report.parse_results()
    assert results == {"TS_FOO_001_something": ("PASS", "one")}


def test_parse_results_testsuites_wrapper_and_statuses(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(
        tmp_path / "test-results/two.xml",
        "<testsuites>"
        '<testsuite name="a"><testcase name="pass_case"/></testsuite>'
        '<testsuite name="b">'
        '<testcase name="fail_case"><failure message="x"/></testcase>'
        '<testcase name="error_case"><error message="y"/></testcase>'
        '<testcase name="skip_case"><skipped/></testcase>'
        "</testsuite>"
        "</testsuites>",
    )
    results = trace_report.parse_results()
    assert results["pass_case"] == ("PASS", "two")
    assert results["fail_case"] == ("FAIL", "two")
    assert results["error_case"] == ("FAIL", "two")
    assert results["skip_case"] == ("SKIP", "two")


def test_parse_results_skips_malformed_xml(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    write(tmp_path / "test-results/broken.xml", "<not-xml-at-all")
    results = trace_report.parse_results()
    assert results == {}


# ---------------------------------------------------------------------------
# main() — the actual gate
# ---------------------------------------------------------------------------


def _base_tree(tmp_path):
    """Minimal directory scaffolding main() needs (docs/ + it writes into it)."""
    (tmp_path / "docs").mkdir(parents=True, exist_ok=True)
    (tmp_path / "tests").mkdir(parents=True, exist_ok=True)
    (tmp_path / "test-results").mkdir(parents=True, exist_ok=True)


def test_main_fully_traced_is_clean(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    _base_tree(tmp_path)
    write(
        tmp_path / "docs/requirements.md",
        "| REQ-F-001 | Do the thing | T |\n",
    )
    write(tmp_path / "docs/design.md", "| DES-FOO-A1 | REQ-F-001 |\n")
    write(tmp_path / "docs/test_spec.md", "| TS-FOO-001 | spec text |\n")
    write(
        tmp_path / "tests/tst_sample.cpp",
        "//! @tstid TS-FOO-001 @design DES-FOO-A1\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_001_something()\n"
        "{\n"
        "}\n",
    )
    write(
        tmp_path / "test-results/results.xml",
        '<testsuite name="tst_sample">'
        '<testcase name="TS_FOO_001_something"/>'
        "</testsuite>",
    )

    rc = trace_report.main()

    assert rc == 0
    html = (tmp_path / "docs/traceability.html").read_text(encoding="utf-8")
    assert "1/1 requirements fully traced" in html
    assert "0 hard gaps" in html
    assert "0 open coverage gaps" in html
    assert "COVERED · PASS" in html


def test_main_reports_every_hard_and_open_gap_kind(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    _base_tree(tmp_path)

    write(
        tmp_path / "docs/requirements.md",
        "| REQ-F-001 | Verified by test | T |\n"
        "| REQ-F-002 | Verified by analysis only | A/I |\n"
        "| REQ-F-003 | Needs a test but has none | T |\n",
    )
    # DES-BAR-B2 satisfies an unknown REQ; only REQ-F-001 gets a real design.
    write(
        tmp_path / "docs/design.md",
        "| DES-FOO-A1 | REQ-F-001 |\n"
        "| DES-BAR-B2 | REQ-F-404 |\n",
    )
    # TS-FOO-002 is specified but never implemented (hard gap).
    # TS-FOO-003/004 are specified and implemented (drift + unknown-ref cases).
    write(
        tmp_path / "docs/test_spec.md",
        "| TS-FOO-001 | spec text |\n"
        "| TS-FOO-002 | spec text, never implemented |\n"
        "| TS-FOO-003 | spec text, name drifted |\n"
        "| TS-FOO-004 | spec text, bad design ref |\n",
    )
    write(
        tmp_path / "tests/tst_sample.cpp",
        "//! @tstid TS-FOO-001 @design DES-FOO-A1\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_001_something()\n"
        "{\n"
        "}\n"
        "\n"
        # implemented but not in test_spec.md at all -> hard gap
        "//! @tstid TS-BAR-001\n"
        "// @relation(REQ-F-999, scope=function)\n"
        "void TS_BAR_001_case()\n"
        "{\n"
        "}\n"
        "\n"
        # function name does not start with TS_FOO_003 -> hard gap
        "//! @tstid TS-FOO-003\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void something_else_entirely()\n"
        "{\n"
        "}\n"
        "\n"
        # unknown @design reference -> hard gap
        "//! @tstid TS-FOO-004 @design DES-UNKNOWN-X1\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_004_case()\n"
        "{\n"
        "}\n",
    )
    write(
        tmp_path / "test-results/results.xml",
        '<testsuite name="tst_sample">'
        '<testcase name="TS_FOO_001_something"/>'
        "</testsuite>",
    )

    rc = trace_report.main()
    out = capsys.readouterr().out

    assert rc == 1  # hard gaps present

    assert "HARD GAP: TS-FOO-002: specified in test_spec.md but not implemented" in out
    assert "TS-BAR-001: implemented (tst_sample.cpp) but missing from test_spec.md" in out
    assert "does not start with TS_FOO_003" in out
    assert "TS-BAR-001: @relation references unknown REQ-F-999" in out
    assert "TS-FOO-004: @design references unknown DES-UNKNOWN-X1" in out
    assert "DES-BAR-B2: satisfies unknown REQ-F-404" in out
    assert "REQ-F-002: no design element claims to satisfy it" in out
    assert "REQ-F-003: no design element claims to satisfy it" in out

    # open gaps: REQ-F-002 has no "T" in its verification method
    assert "REQ-F-002: verified by A/I only, no automated test needed" in out
    # REQ-F-003 IS meant to have a test (method T) and has none -> real coverage gap
    assert "REQ-F-003: no automated test verifies it (coverage gap)" in out
    # TS-FOO-003 / TS-FOO-004's functions never ran -> "no recorded result"
    assert "TS-FOO-003: no recorded result" in out
    assert "TS-FOO-004: no recorded result" in out


def test_main_partial_result_when_mixed_pass_fail(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    _base_tree(tmp_path)
    write(tmp_path / "docs/requirements.md", "| REQ-F-001 | Do the thing | T |\n")
    write(tmp_path / "docs/design.md", "| DES-FOO-A1 | REQ-F-001 |\n")
    write(
        tmp_path / "docs/test_spec.md",
        "| TS-FOO-001 | spec text |\n| TS-FOO-002 | spec text |\n",
    )
    write(
        tmp_path / "tests/tst_sample.cpp",
        "//! @tstid TS-FOO-001\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_001_pass()\n"
        "{\n"
        "}\n"
        "\n"
        "//! @tstid TS-FOO-002\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_002_fail()\n"
        "{\n"
        "}\n",
    )
    write(
        tmp_path / "test-results/results.xml",
        '<testsuite name="tst_sample">'
        '<testcase name="TS_FOO_001_pass"/>'
        '<testcase name="TS_FOO_002_fail"><failure message="x"/></testcase>'
        "</testsuite>",
    )

    rc = trace_report.main()
    assert rc == 0  # no hard gaps, only a failing recorded result

    html = (tmp_path / "docs/traceability.html").read_text(encoding="utf-8")
    assert "COVERED · FAIL" in html


def test_main_partial_result_when_some_have_no_result(tmp_path, monkeypatch):
    monkeypatch.setattr(trace_report, "ROOT", tmp_path)
    _base_tree(tmp_path)
    write(tmp_path / "docs/requirements.md", "| REQ-F-001 | Do the thing | T |\n")
    write(tmp_path / "docs/design.md", "| DES-FOO-A1 | REQ-F-001 |\n")
    write(
        tmp_path / "docs/test_spec.md",
        "| TS-FOO-001 | spec text |\n| TS-FOO-002 | spec text |\n",
    )
    write(
        tmp_path / "tests/tst_sample.cpp",
        "//! @tstid TS-FOO-001\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_001_pass()\n"
        "{\n"
        "}\n"
        "\n"
        "//! @tstid TS-FOO-002\n"
        "// @relation(REQ-F-001, scope=function)\n"
        "void TS_FOO_002_never_run()\n"
        "{\n"
        "}\n",
    )
    write(
        tmp_path / "test-results/results.xml",
        '<testsuite name="tst_sample">'
        '<testcase name="TS_FOO_001_pass"/>'
        "</testsuite>",
    )

    trace_report.main()
    html = (tmp_path / "docs/traceability.html").read_text(encoding="utf-8")
    assert "PARTIAL RESULT" in html
