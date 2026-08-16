# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/check_process_docs.py — the process-framework
cross-reference checker (process files <-> work products <-> strategies
<-> templates).

Every path constant the module uses (ROOT, PROCESS_DIR, PROCESSES_DIR,
WORK_PRODUCTS_DIR, STRATEGIES_DIR, TEMPLATES_DIR) is monkeypatched to a
synthetic tmp_path tree per test, so nothing here depends on this
repository's real process/ directory.
"""

import check_process_docs as cpd

REQUIRED_SECTIONS = "\n".join(cpd.REQUIRED_PROCESS_SECTIONS)


def _wire_dirs(tmp_path, monkeypatch):
    root = tmp_path
    process = root / "process"
    processes = process / "processes"
    work_products = process / "work-products"
    strategies = process / "strategies"
    templates = process / "templates"
    for d in (processes, work_products, strategies, templates):
        d.mkdir(parents=True, exist_ok=True)

    monkeypatch.setattr(cpd, "ROOT", root)
    monkeypatch.setattr(cpd, "PROCESS_DIR", process)
    monkeypatch.setattr(cpd, "PROCESSES_DIR", processes)
    monkeypatch.setattr(cpd, "WORK_PRODUCTS_DIR", work_products)
    monkeypatch.setattr(cpd, "STRATEGIES_DIR", strategies)
    monkeypatch.setattr(cpd, "TEMPLATES_DIR", templates)
    return processes, work_products, strategies, templates


def valid_process_body(refs=""):
    return f"# A process\n\n{REQUIRED_SECTIONS}\n\n{refs}\n"


# ---------------------------------------------------------------------------
# find_refs
# ---------------------------------------------------------------------------


def test_find_refs_extracts_backticked_paths():
    text = "See `work-products/foo.md` and `strategies/bar.md` and prose."
    assert cpd.find_refs(text) == {("work-products", "foo.md"), ("strategies", "bar.md")}


def test_find_refs_ignores_non_matching_backticks():
    text = "Here's `some/code.py` and `templates/baz.md` and `not-a-kind/x.md`."
    assert cpd.find_refs(text) == {("templates", "baz.md")}


# ---------------------------------------------------------------------------
# check_sections
# ---------------------------------------------------------------------------


def test_check_sections_reports_each_missing_section(tmp_path, monkeypatch):
    monkeypatch.setattr(cpd, "ROOT", tmp_path)
    findings = []
    path = tmp_path / "proc.md"
    path.write_text("# A process\n\n## Purpose\nonly this one\n", encoding="utf-8")
    cpd.check_sections(path, findings)
    assert any("## Roles" in f for f in findings)
    assert any("## Verification / QA Hooks" in f for f in findings)
    assert not any("## Purpose" in f for f in findings)


def test_check_sections_clean_file_no_findings(tmp_path, monkeypatch):
    monkeypatch.setattr(cpd, "ROOT", tmp_path)
    findings = []
    path = tmp_path / "proc.md"
    path.write_text(valid_process_body(), encoding="utf-8")
    cpd.check_sections(path, findings)
    assert findings == []


# ---------------------------------------------------------------------------
# check_refs_resolve
# ---------------------------------------------------------------------------


def test_check_refs_resolve_flags_missing_target(tmp_path, monkeypatch):
    processes, work_products, strategies, templates = _wire_dirs(tmp_path, monkeypatch)
    path = processes / "SWE.1-x.md"
    path.write_text(valid_process_body("`work-products/missing.md`"), encoding="utf-8")
    findings = []
    refs = cpd.check_refs_resolve(path, findings)
    assert refs == {("work-products", "missing.md")}
    assert any("references work-products/missing.md, which does not exist" in f for f in findings)


def test_check_refs_resolve_existing_target_no_finding(tmp_path, monkeypatch):
    processes, work_products, strategies, templates = _wire_dirs(tmp_path, monkeypatch)
    (work_products / "foo.md").write_text("a work product", encoding="utf-8")
    path = processes / "SWE.1-x.md"
    path.write_text(valid_process_body("`work-products/foo.md`"), encoding="utf-8")
    findings = []
    refs = cpd.check_refs_resolve(path, findings)
    assert refs == {("work-products", "foo.md")}
    assert findings == []


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def test_main_processes_dir_missing_returns_1(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(cpd, "ROOT", tmp_path)
    monkeypatch.setattr(cpd, "PROCESSES_DIR", tmp_path / "process" / "processes")
    rc = cpd.main()
    assert rc == 1
    assert "not found" in capsys.readouterr().err


def test_main_fully_consistent_tree_returns_0(tmp_path, monkeypatch, capsys):
    processes, work_products, strategies, templates = _wire_dirs(tmp_path, monkeypatch)
    (work_products / "foo.md").write_text("a work product", encoding="utf-8")
    (strategies / "bar.md").write_text("a strategy", encoding="utf-8")
    (templates / "baz.md").write_text("a template", encoding="utf-8")
    (processes / "SWE.1-x.md").write_text(
        valid_process_body(
            "`work-products/foo.md` `strategies/bar.md` `templates/baz.md`"
        ),
        encoding="utf-8",
    )

    rc = cpd.main()
    out = capsys.readouterr().out
    assert rc == 0
    assert "OK — 1 processes, 1 work products, 1 strategies, 1 templates" in out


def test_main_ai_reviewer_instructions_exempt_from_template_reference_check(tmp_path, monkeypatch, capsys):
    processes, work_products, strategies, templates = _wire_dirs(tmp_path, monkeypatch)
    (templates / "ai-reviewer-instructions.md").write_text("read me first", encoding="utf-8")
    (processes / "SWE.1-x.md").write_text(valid_process_body(), encoding="utf-8")

    rc = cpd.main()
    assert rc == 0
    assert "not referenced" not in capsys.readouterr().err


def test_main_reports_unreferenced_work_product_strategy_and_template(tmp_path, monkeypatch, capsys):
    processes, work_products, strategies, templates = _wire_dirs(tmp_path, monkeypatch)
    (work_products / "orphan.md").write_text("unused", encoding="utf-8")
    (strategies / "orphan.md").write_text("unused", encoding="utf-8")
    (templates / "orphan.md").write_text("unused", encoding="utf-8")
    (processes / "SWE.1-x.md").write_text(valid_process_body(), encoding="utf-8")

    rc = cpd.main()
    err = capsys.readouterr().err
    assert rc == 1
    assert "work-products/orphan.md: not referenced from any processes/strategies/templates file" in err
    assert "strategies/orphan.md: not referenced from any process file" in err
    assert "templates/orphan.md: not referenced from any process file" in err


def test_main_strategies_and_templates_own_refs_checked_too(tmp_path, monkeypatch, capsys):
    processes, work_products, strategies, templates = _wire_dirs(tmp_path, monkeypatch)
    (processes / "SWE.1-x.md").write_text(
        valid_process_body("`strategies/bar.md`"), encoding="utf-8"
    )
    # strategy references a work product that does not exist -> its OWN refs are checked
    (strategies / "bar.md").write_text("uses `work-products/missing.md`", encoding="utf-8")

    rc = cpd.main()
    err = capsys.readouterr().err
    assert rc == 1
    assert "strategies/bar.md: references work-products/missing.md, which does not exist" in err
