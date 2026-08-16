# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/check_object_names.py — enforces that every member
widget assigned in src/ui/*.cpp gets a stable objectName (REQ-N-007), per the
module's own docstring rule: `m_foo = new QSomething(...)` must be followed
somewhere in the file by `m_foo->setObjectName(...)`; non-member locals and
non-widget types (QTimer et al.) are exempt.
"""

import check_object_names as con


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------------------
# missing_in()
# ---------------------------------------------------------------------------


def test_missing_in_flags_member_widget_without_object_name(tmp_path):
    path = tmp_path / "gap.cpp"
    write(
        path,
        "void MainWindow::setup() {\n"
        "    m_button = new QPushButton(this);\n"
        "}\n",
    )
    gaps = con.missing_in(path)
    assert gaps == [(2, "button", "QPushButton")]


def test_missing_in_no_gap_when_object_name_set(tmp_path):
    path = tmp_path / "ok.cpp"
    write(
        path,
        "void MainWindow::setup() {\n"
        "    m_label = new QLabel(this);\n"
        '    m_label->setObjectName(QStringLiteral("label"));\n'
        "}\n",
    )
    assert con.missing_in(path) == []


def test_missing_in_object_name_set_further_down_the_file(tmp_path):
    # The docstring says the call may sit a few lines later (multi-line ctor);
    # the check searches the WHOLE file text, not just nearby lines.
    path = tmp_path / "ok_later.cpp"
    write(
        path,
        "void MainWindow::setup() {\n"
        "    m_edit = new QLineEdit(\n"
        "        this);\n"
        "    doOtherSetupWork();\n"
        '    m_edit->setObjectName(QStringLiteral("edit"));\n'
        "}\n",
    )
    assert con.missing_in(path) == []


def test_missing_in_ignores_non_widget_types(tmp_path):
    path = tmp_path / "nonwidget.cpp"
    write(
        path,
        "void MainWindow::setup() {\n"
        "    m_timer = new QTimer(this);\n"
        "    m_settings = new QSettings(this);\n"
        "}\n",
    )
    assert con.missing_in(path) == []


def test_missing_in_ignores_non_member_locals(tmp_path):
    path = tmp_path / "local.cpp"
    write(
        path,
        "void MainWindow::setup() {\n"
        "    auto* button = new QPushButton(this);\n"
        "}\n",
    )
    assert con.missing_in(path) == []


def test_missing_in_multiple_gaps_in_one_file(tmp_path):
    path = tmp_path / "multi.cpp"
    write(
        path,
        "void MainWindow::setup() {\n"
        "    m_button = new QPushButton(this);\n"
        "    m_label = new QLabel(this);\n"
        '    m_label->setObjectName(QStringLiteral("label"));\n'
        "    m_combo = new QComboBox(this);\n"
        "}\n",
    )
    gaps = con.missing_in(path)
    assert gaps == [(2, "button", "QPushButton"), (5, "combo", "QComboBox")]


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def test_main_clean_tree_returns_0(tmp_path, monkeypatch, capsys):
    ui = tmp_path / "src" / "ui"
    ui.mkdir(parents=True)
    write(
        ui / "clean.cpp",
        "void MainWindow::setup() {\n"
        "    m_label = new QLabel(this);\n"
        '    m_label->setObjectName(QStringLiteral("label"));\n'
        "}\n",
    )
    monkeypatch.setattr(con, "ROOT", tmp_path)
    monkeypatch.setattr(con, "UI", ui)
    monkeypatch.setattr("sys.argv", ["check_object_names.py"])

    rc = con.main()
    out = capsys.readouterr().out
    assert rc == 0
    assert "every member widget in src/ui has a stable objectName" in out


def test_main_reports_gaps_and_returns_1(tmp_path, monkeypatch, capsys):
    ui = tmp_path / "src" / "ui"
    ui.mkdir(parents=True)
    write(
        ui / "gap.cpp",
        "void MainWindow::setup() {\n"
        "    m_button = new QPushButton(this);\n"
        "}\n",
    )
    monkeypatch.setattr(con, "ROOT", tmp_path)
    monkeypatch.setattr(con, "UI", ui)
    monkeypatch.setattr("sys.argv", ["check_object_names.py"])

    rc = con.main()
    out = capsys.readouterr().out
    assert rc == 1
    assert "gap.cpp:2: m_button (QPushButton) has no objectName" in out
    assert "1 widget(s) without a stable objectName" in out
    assert 'm_button->setObjectName(QStringLiteral("button"));' not in out


def test_main_fix_hint_prints_the_exact_line_to_add(tmp_path, monkeypatch, capsys):
    ui = tmp_path / "src" / "ui"
    ui.mkdir(parents=True)
    write(
        ui / "gap.cpp",
        "void MainWindow::setup() {\n"
        "    m_button = new QPushButton(this);\n"
        "}\n",
    )
    monkeypatch.setattr(con, "ROOT", tmp_path)
    monkeypatch.setattr(con, "UI", ui)
    monkeypatch.setattr("sys.argv", ["check_object_names.py", "--fix-hint"])

    rc = con.main()
    out = capsys.readouterr().out
    assert rc == 1
    assert 'm_button->setObjectName(QStringLiteral("button"));' in out
