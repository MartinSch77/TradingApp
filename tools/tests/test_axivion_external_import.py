# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for axivion/external_import.py.

Like architecture.py, this is Axivion Suite configuration: it imports the
real `axivion.config` and `bauhaus.teecap` modules that only exist inside an
installed Suite. These tests stub both with small recording fakes so the
module's own logic — the cat/type platform branch, the per-tool
GenericFormat/ImportExternalAnalysisOutput registration loop, and
_project_root()'s "walk up to CMakeLists.txt" search — can be verified in
isolation. The "matchlist" regexes are exactly what CLAUDE.md calls out as
the reason this layer exists in Python rather than JSON, so several tests
pin the exact provider -> regex correspondence."""

import pathlib
import sys
import types

import pytest


class _Match:
    """Stand-in for bauhaus.teecap.Match: just remembers the pattern."""

    def __init__(self, pattern):
        self.pattern = pattern


class _FakeAnalysis:
    def __init__(self):
        self.copy_calls = []
        self.activate_calls = []

    def copy(self, rule_type, name, **kwargs):
        self.copy_calls.append((rule_type, name, kwargs))

    def activate(self, *names):
        self.activate_calls.append(names)


@pytest.fixture
def stub_axivion_bauhaus(monkeypatch):
    analysis = _FakeAnalysis()

    mod_axivion = types.ModuleType("axivion")
    mod_axivion_config = types.ModuleType("axivion.config")
    mod_axivion_config.get_analysis = lambda: analysis
    mod_axivion.config = mod_axivion_config

    mod_bauhaus = types.ModuleType("bauhaus")
    mod_teecap = types.ModuleType("bauhaus.teecap")
    mod_teecap.Match = _Match
    mod_bauhaus.teecap = mod_teecap

    monkeypatch.setitem(sys.modules, "axivion", mod_axivion)
    monkeypatch.setitem(sys.modules, "axivion.config", mod_axivion_config)
    monkeypatch.setitem(sys.modules, "bauhaus", mod_bauhaus)
    monkeypatch.setitem(sys.modules, "bauhaus.teecap", mod_teecap)
    sys.modules.pop("external_import", None)

    yield analysis

    sys.modules.pop("external_import", None)


def _reload(monkeypatch, platform):
    monkeypatch.setattr(sys, "platform", platform)
    sys.modules.pop("external_import", None)
    import external_import
    return external_import


# --------------------------------------------------------------------------
# cat/type platform branch
# --------------------------------------------------------------------------

def test_cat_command_on_non_windows(stub_axivion_bauhaus, monkeypatch):
    mod = _reload(monkeypatch, "linux")
    assert mod._CAT_COMMAND == "cat"
    assert mod._CAT_ARGS == []


def test_cat_command_on_windows(stub_axivion_bauhaus, monkeypatch):
    mod = _reload(monkeypatch, "win32")
    assert mod._CAT_COMMAND == "cmd"
    assert mod._CAT_ARGS == ["/c", "type"]


# --------------------------------------------------------------------------
# the per-tool registration loop
# --------------------------------------------------------------------------

def test_every_tool_registers_a_format_and_an_import_rule(stub_axivion_bauhaus, monkeypatch):
    analysis = stub_axivion_bauhaus
    mod = _reload(monkeypatch, "linux")

    assert len(analysis.copy_calls) == 2 * len(mod._TOOLS)
    assert len(analysis.activate_calls) == len(mod._TOOLS)

    by_name = {name: kwargs for _rule_type, name, kwargs in analysis.copy_calls}
    for tool, (log, regex) in mod._TOOLS.items():
        format_rule = f"GenericFormat {tool}"
        import_rule = f"ImportExternalAnalysisOutput {tool}"

        assert (format_rule, import_rule) in analysis.activate_calls

        assert by_name[format_rule]["provider"] == tool
        assert by_name[format_rule]["matchlist"].pattern == regex

        expected_log_path = str(mod._ROOT / "analysis-results" / log)
        assert by_name[import_rule]["options"] == [expected_log_path]
        assert by_name[import_rule]["capture_stdout_provider"] == tool
        assert by_name[import_rule]["check_returncode"] is False
        assert by_name[import_rule]["strip_path_prefix"] == str(mod._ROOT)
        assert by_name[import_rule]["command"] == mod._CAT_COMMAND


def test_windows_tools_use_the_type_command(stub_axivion_bauhaus, monkeypatch):
    analysis = stub_axivion_bauhaus
    mod = _reload(monkeypatch, "win32")
    by_name = {name: kwargs for _rule_type, name, kwargs in analysis.copy_calls}
    import_rule = "ImportExternalAnalysisOutput cppcheck"
    assert by_name[import_rule]["command"] == "cmd"
    assert by_name[import_rule]["options"][0] == "/c"
    assert by_name[import_rule]["options"][1] == "type"


def test_gcc_style_and_pipe_regex_are_used_where_documented(stub_axivion_bauhaus, monkeypatch):
    mod = _reload(monkeypatch, "linux")
    # clang-tidy/clazy/gcc-analyzer/msvc-analyze are GCC-style lines.
    assert mod._TOOLS["clang-tidy"][1] == mod._GCC_STYLE
    assert mod._TOOLS["clazy"][1] == mod._GCC_STYLE
    # cppcheck and the sanitizer logs use the pipe format.
    assert mod._TOOLS["cppcheck"][1] == mod._PIPE
    assert mod._TOOLS["asan-ubsan"][1] == mod._PIPE


# --------------------------------------------------------------------------
# _project_root()
# --------------------------------------------------------------------------

def test_project_root_found_by_walking_up_to_cmakelists(
    stub_axivion_bauhaus, monkeypatch, tmp_path
):
    mod = _reload(monkeypatch, "linux")
    root = (tmp_path / "proj").resolve()
    nested = root / "axivion" / "windows"
    nested.mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("", encoding="utf-8")

    monkeypatch.setattr(mod, "__file__", str(nested / "external_import.py"))
    assert mod._project_root() == root


def test_project_root_falls_back_when_no_cmakelists_found(
    stub_axivion_bauhaus, monkeypatch, tmp_path
):
    mod = _reload(monkeypatch, "linux")
    lonely = (tmp_path / "a" / "b").resolve()
    lonely.mkdir(parents=True)
    fake_file = lonely / "external_import.py"

    monkeypatch.setattr(mod, "__file__", str(fake_file))
    expected = pathlib.Path(str(fake_file)).resolve().parent.parent
    assert mod._project_root() == expected


def test_root_used_at_import_time_is_the_real_project_root(stub_axivion_bauhaus, monkeypatch):
    """Sanity check on the un-monkeypatched happy path: importing the module
    for real inside this checkout must find the actual repo root."""
    mod = _reload(monkeypatch, "linux")
    assert (mod._ROOT / "CMakeLists.txt").is_file()
