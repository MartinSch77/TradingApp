# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/release_notes.py — the notes .github/workflows/release.yml
writes on the release it creates, from the evidence CI actually has."""

import pytest

import release_notes


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def test_junit_counts_recurse_and_count_failures_and_errors(tmp_path):
    write(tmp_path / "a.xml",
          '<testsuite><testcase name="x"/><testcase name="y"><failure/></testcase></testsuite>')
    # Nested like test-results/squish/, wrapped in <testsuites>.
    write(tmp_path / "squish" / "b.xml",
          '<testsuites><testsuite><testcase name="z"><error/></testcase>'
          '</testsuite></testsuites>')
    write(tmp_path / "notes.txt", "not a report")
    assert release_notes.junit_counts(tmp_path) == (3, 1, 1, 2)


def test_an_unreadable_report_counts_as_an_error(tmp_path):
    write(tmp_path / "broken.xml", "<testsuite><testcase")
    assert release_notes.junit_counts(tmp_path) == (0, 0, 1, 1)


def test_render_states_the_evidence_and_what_is_still_missing():
    notes = release_notes.render("1.1.2", "a58940e", (482, 0, 0, 57),
                                 "84 requirements · … 0 hard gaps, 2 open coverage gaps\n")
    assert "Built by CI from `a58940e`." in notes
    assert "| Test suite | 482 cases in 57 JUnit files, 0 failures |" in notes
    assert "| Traceability | 84 requirements · … 0 hard gaps, 2 open coverage gaps |" in notes
    assert "passed the release gate" in notes
    # It says out loud that the Axivion evidence is not in yet, and where it comes from.
    assert "**not** in these notes yet" in notes
    assert "TradingApp-1.1.2-qualification.zip" in notes
    assert "TradingApp-1.1.2-source.tar.gz" in notes


def test_render_without_a_trace_summary_points_at_the_matrix():
    notes = release_notes.render("1.0.0", "c0ffee1", (1, 0, 0, 1), "")
    assert "| Traceability | see docs/traceability.html |" in notes


@pytest.mark.parametrize(
    "counts, message",
    [
        ((0, 0, 0, 0), "no JUnit results"),
        ((5, 0, 0, 0), "no JUnit results"),
        ((0, 0, 0, 3), "no JUnit results"),
        ((10, 1, 0, 2), "1 failure(s), 0 error(s)"),
        ((10, 0, 2, 2), "0 failure(s), 2 error(s)"),
    ],
)
def test_render_refuses_an_empty_or_red_suite(counts, message):
    with pytest.raises(ValueError, match=message.replace("(", r"\(").replace(")", r"\)")):
        release_notes.render("1.1.2", "a58940e", counts, "")


def test_main_writes_the_file_and_fails_on_a_red_suite(tmp_path, capsys):
    results = tmp_path / "results"
    write(results / "ok.xml", '<testsuite><testcase name="x"/></testsuite>')
    out = tmp_path / "notes.md"
    assert release_notes.main(["--version", "1.1.2", "--commit", "abc1234",
                               "--test-results", str(results), "--out", str(out)]) == 0
    assert "| Test suite | 1 cases in 1 JUnit files, 0 failures |" in out.read_text(
        encoding="utf-8")

    assert release_notes.main(["--version", "1.1.2", "--commit", "abc1234",
                               "--test-results", str(results)]) == 0
    assert "Built by CI from `abc1234`." in capsys.readouterr().out

    write(results / "red.xml", '<testsuite><testcase name="y"><failure/></testcase></testsuite>')
    assert release_notes.main(["--version", "1.1.2", "--commit", "abc1234",
                               "--test-results", str(results)]) == 1
    assert "not a releasable suite" in capsys.readouterr().err
