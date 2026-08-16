# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for tools/parse_sanitizer_log.py.

Unlike the other seven tools, this module has no `main()` / `if __name__ ==
"__main__"` guard at all — it is a flat script whose top level runs
immediately on import, reading its four positional arguments straight out of
sys.argv (KIND, RAW, OUT, ROOT). To exercise it as a unit under pytest we set
sys.argv to the desired invocation, then import it fresh each time (the
module is evicted from sys.modules first, since Python only executes a
module's top level on its FIRST import per process).
"""

import importlib
import sys

import pytest


def run_module(kind, raw_path, out_path, root_path):
    """Import (or re-import) parse_sanitizer_log with the given argv, in-process."""
    old_argv = sys.argv
    sys.argv = ["parse_sanitizer_log.py", kind, str(raw_path), str(out_path), str(root_path)]
    sys.modules.pop("parse_sanitizer_log", None)
    try:
        return importlib.import_module("parse_sanitizer_log")
    finally:
        sys.argv = old_argv
        sys.modules.pop("parse_sanitizer_log", None)


@pytest.fixture
def project(tmp_path):
    root = tmp_path / "proj"
    (root / "src").mkdir(parents=True)
    (root / "tests").mkdir(parents=True)
    return root


# --------------------------------------------------------------------------
# asan/ubsan/tsan (shared grammar)
# --------------------------------------------------------------------------

def test_ubsan_runtime_error_line(project, tmp_path):
    src = project / "src" / "Config.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(f"{src}:47:5: runtime error: signed integer overflow\n", encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    text = out.read_text()
    assert "src/Config.cpp|47|error|ubsan|signed integer overflow" in text


def test_summary_line_asan(project, tmp_path):
    src = project / "src" / "Foo.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        f"SUMMARY: AddressSanitizer: heap-use-after-free {src}:10\n", encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    text = out.read_text()
    assert "src/Foo.cpp|10|error|addresssan-heap-use-after-free|" in text
    assert "AddressSanitizer: heap-use-after-free" in text


def test_summary_line_tsan(project, tmp_path):
    src = project / "src" / "Bar.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        f"SUMMARY: ThreadSanitizer: data race {src}:20\n", encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("tsan", raw, out, project)
    text = out.read_text()
    assert "src/Bar.cpp|20|error|threadsan-data-race|" in text


def test_leak_block_finds_first_project_frame(project, tmp_path):
    src = project / "src" / "Leaky.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "Direct leak of 128 byte(s) in 1 object(s) allocated from:\n"
        "    #0 0xdeadbeef in operator new /usr/include/new\n"
        f"    #1 0xdeadbeef in myFunc() {src}:33\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    text = out.read_text()
    assert "src/Leaky.cpp|33|warning|lsan-direct-leak|direct leak of 128 bytes" in text


def test_indirect_leak_kind(project, tmp_path):
    src = project / "src" / "Leaky2.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "Indirect leak of 64 byte(s) in 2 object(s) allocated from:\n"
        f"    #0 0xdeadbeef in myFunc() {src}:1\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    assert "lsan-indirect-leak|indirect leak of 64 bytes" in out.read_text()


def test_leak_block_with_no_project_frame_yields_nothing(project, tmp_path):
    raw = tmp_path / "raw.log"
    raw.write_text(
        "Direct leak of 999 byte(s) in 1 object(s) allocated from:\n"
        "    #0 0xdeadbeef in operator new /usr/include/new\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    assert out.read_text() == ""


def test_non_matching_lines_are_ignored(project, tmp_path):
    raw = tmp_path / "raw.log"
    raw.write_text("this is just noise\nand more noise\n", encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    assert out.read_text() == ""


def test_duplicate_findings_are_deduplicated(project, tmp_path):
    src = project / "src" / "Dup.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    line = f"{src}:5: runtime error: same thing twice\n"
    raw.write_text(line + line, encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    text = out.read_text()
    assert text.count("same thing twice") == 1


def test_raw_log_missing_yields_empty_output(project, tmp_path):
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", tmp_path / "does-not-exist.log", out, project)
    assert out.read_text() == ""


# --------------------------------------------------------------------------
# project_path: BASENAMES fallback for an absolute path outside ROOT
# --------------------------------------------------------------------------

def test_absolute_path_outside_root_falls_back_to_basename_map(project, tmp_path):
    # A file that DOES exist under project/src, but the sanitizer report will
    # reference it via a path OUTSIDE root (e.g. a build-tree symlink or a
    # different absolute prefix) — project_path() falls back to matching by
    # basename via BASENAMES.
    real = project / "src" / "Known.cpp"
    real.write_text("", encoding="utf-8")
    outside_dir = tmp_path / "elsewhere"
    outside_dir.mkdir()
    raw = tmp_path / "raw.log"
    raw.write_text(f"{outside_dir / 'Known.cpp'}:9: runtime error: via basename\n",
                   encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    text = out.read_text()
    assert "src/Known.cpp|9|error|ubsan|via basename" in text


def test_unknown_basename_outside_root_yields_nothing(project, tmp_path):
    raw = tmp_path / "raw.log"
    raw.write_text("/nowhere/Unknown.cpp:1: runtime error: ghost\n", encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("asan-ubsan", raw, out, project)
    assert out.read_text() == ""


# --------------------------------------------------------------------------
# valgrind
# --------------------------------------------------------------------------

def test_valgrind_invalid_read_with_frame(project, tmp_path):
    src = project / "src" / "Vg.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "==123== Invalid read of size 4\n"
        "==123==    at 0x1234: myFunc (Vg.cpp:15)\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("valgrind", raw, out, project)
    text = out.read_text()
    assert "src/Vg.cpp|15|error|valgrind-invalid-read|Invalid read of size 4" in text


def test_valgrind_definitely_lost_by_frame(project, tmp_path):
    src = project / "src" / "Leak.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "==1== 40 bytes in 1 blocks are definitely lost in loss record 1 of 2\n"
        "==1==    by 0x5678: myFunc (Leak.cpp:22)\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("valgrind", raw, out, project)
    text = out.read_text()
    assert "src/Leak.cpp|22|warning|valgrind-definitely-lost" in text


def test_valgrind_still_reachable_is_skipped(project, tmp_path):
    src = project / "src" / "Reach.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "==1== 16 bytes in 1 blocks are still reachable in loss record 1 of 1\n"
        "==1==    by 0x9999: myFunc (Reach.cpp:1)\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("valgrind", raw, out, project)
    assert out.read_text() == ""


def test_valgrind_blank_line_resets_pending(project, tmp_path):
    src = project / "src" / "Reset.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "==1== Invalid write of size 8\n"
        "==1== \n"
        "==1==    by 0x1: myFunc (Reset.cpp:3)\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("valgrind", raw, out, project)
    # blank line cleared `pending`, so the later frame line yields nothing
    assert out.read_text() == ""


def test_valgrind_frame_without_pending_head_is_ignored(project, tmp_path):
    src = project / "src" / "Alone.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text("==1==    by 0x1: myFunc (Alone.cpp:5)\n", encoding="utf-8")
    out = tmp_path / "out.txt"
    run_module("valgrind", raw, out, project)
    assert out.read_text() == ""


def test_valgrind_uninitialised_and_invalid_free_heads(project, tmp_path):
    src = project / "src" / "Multi.cpp"
    src.write_text("", encoding="utf-8")
    raw = tmp_path / "raw.log"
    raw.write_text(
        "==1== Use of uninitialised value of size 8\n"
        "==1==    at 0x1: f1 (Multi.cpp:1)\n"
        "==1== Invalid free() / delete / delete[]\n"
        "==1==    at 0x2: f2 (Multi.cpp:2)\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.txt"
    run_module("valgrind", raw, out, project)
    text = out.read_text()
    assert "valgrind-uninitialised" in text
    assert "valgrind-invalid-free" in text


def test_valgrind_missing_raw_log_yields_empty(project, tmp_path):
    out = tmp_path / "out.txt"
    run_module("valgrind", tmp_path / "missing.log", out, project)
    assert out.read_text() == ""
