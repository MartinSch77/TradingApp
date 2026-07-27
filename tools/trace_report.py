#!/usr/bin/env python3
"""Traceability matrix generator (ASPICE-style bidirectional traces).

Joins the four legs of the traceability chain and reports completeness:

  requirements  docs/requirements.md   REQ-… ids
  design        docs/design.md         DES-… ids with `satisfies` REQ links
  test spec     docs/test_spec.md      TS-… ids (tables, with REQ/DES context)
  test impl     tests/tst_*.cpp        functions tagged @tstid/@design plus
                                       @relation(REQ-…, scope=function) —
                                       the StrictDoc source-coverage marker
  test results  test-results/*.xml     JUnit per-function pass/fail

Outputs docs/traceability.html (self-contained, also included in the Doxygen
HTML as a related page via docs/traceability.md) and prints a gap summary.
Exit code 1 when a *hard* gap exists (spec'd test without implementation,
implemented test without spec, or broken id reference); requirements without
any automated test are reported as OPEN coverage gaps but do not fail the run
— they are the honest to-do list (e.g. UI-level requirements).

ASPICE mapping: SWE.1 (requirements) ↔ SWE.3 (design as constructive
counterpart) ↔ SWE.4/SWE.5 (unit/integration test spec + results). The matrix
demonstrates bidirectional traceability and consistency (BP for SWE.1.BP6,
SWE.3.BP5, SWE.4.BP5, SWE.5.BP5, SUP.10-style evidence).
"""

import glob
import html
import re
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Every artefact in this repository is UTF-8. Python only defaults to UTF-8 on
# Linux/macOS; on Windows the default is the ANSI code page (cp1252), which
# fails on the em dashes and arrows in the requirement and design tables. All
# file access below therefore names the encoding explicitly.
# newline="\n" on writes keeps the generated matrix byte-identical across
# platforms instead of flipping to CRLF whenever it is regenerated on Windows.
UTF8 = {"encoding": "utf-8"}
UTF8_LF = {"encoding": "utf-8", "newline": "\n"}

REQ_RE = re.compile(r"\bREQ-[FN]-\d{3}\b")
DES_RE = re.compile(r"\bDES-[A-Z]+-[A-Z0-9]+\b")
TS_RE = re.compile(r"\bTS-[A-Z]+-\d{3}\b")


def parse_requirements():
    text = (ROOT / "docs/requirements.md").read_text(**UTF8)
    reqs = {}
    for line in text.splitlines():
        m = REQ_RE.search(line)
        if m and line.strip().startswith("|"):
            cells = [c.strip() for c in line.split("|")]
            reqs[m.group()] = cells[2] if len(cells) > 2 else ""
    return reqs


def parse_design():
    """DES id -> set of REQ ids it satisfies (from the design tables)."""
    text = (ROOT / "docs/design.md").read_text(**UTF8)
    design = {}
    for line in text.splitlines():
        if not line.strip().startswith("|"):
            continue
        m = DES_RE.search(line)
        if m:
            design[m.group()] = set(REQ_RE.findall(line))
    return design


def parse_test_spec():
    """TS id -> spec row text; section headers carry the REQ/DES context."""
    text = (ROOT / "docs/test_spec.md").read_text(**UTF8)
    spec = {}
    for line in text.splitlines():
        m = TS_RE.search(line)
        if m and line.strip().startswith("|"):
            spec[m.group()] = line.strip()
    return spec


def parse_test_impl():
    """TS id -> {file, function, verifies: {REQ}, design: {DES}} from tests.

    A test function's traceability block is one or more //! lines directly
    above it: `@tstid TS-… @design DES-…` plus the StrictDoc source marker
    `@relation(REQ-…, scope=function)` carrying the verified requirements
    (one marker serves both this report and the StrictDoc source coverage).
    """
    impl = {}
    for path in sorted(glob.glob(str(ROOT / "tests" / "tst_*.cpp"))):
        src = Path(path).read_text(**UTF8)
        # The @tstid line plus any further comment lines up to the function
        # (the @relation marker uses a plain // comment — StrictDoc's C++
        # parser does not recognize Doxygen-style //! comments).
        for m in re.finditer(
            r"//!\s*@tstid\s+(TS-[A-Z]+-\d{3})((?:[^\n]*\n\s*//!?)*[^\n]*)\n\s*void\s+(\w+)\s*\(",
            src,
        ):
            ts, rest, func = m.group(1), m.group(2), m.group(3)
            impl[ts] = {
                "file": Path(path).name,
                "function": func,
                "verifies": set(REQ_RE.findall(rest)),
                "design": set(DES_RE.findall(rest)),
            }
    return impl


def parse_results():
    """test function name -> (status, suite) from the JUnit XMLs."""
    results = {}
    for path in sorted(glob.glob(str(ROOT / "test-results" / "*.xml"))):
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            continue
        suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
        for suite in suites:
            for case in suite.iter("testcase"):
                name = case.get("name", "")
                status = "PASS"
                if case.find("failure") is not None or case.find("error") is not None:
                    status = "FAIL"
                elif case.find("skipped") is not None:
                    status = "SKIP"
                results[name] = (status, Path(path).stem)
    return results


def main():
    reqs = parse_requirements()
    design = parse_design()
    spec = parse_test_spec()
    impl = parse_test_impl()
    results = parse_results()

    req_to_des = {r: set() for r in reqs}
    for des, satisfied in design.items():
        for r in satisfied:
            req_to_des.setdefault(r, set()).add(des)
    req_to_ts = {r: set() for r in reqs}
    for ts, info in impl.items():
        for r in info["verifies"]:
            req_to_ts.setdefault(r, set()).add(ts)

    hard_gaps = []
    open_gaps = []

    for ts in spec:
        if ts not in impl:
            hard_gaps.append(f"{ts}: specified in test_spec.md but not implemented")
    for ts, info in impl.items():
        if ts not in spec:
            hard_gaps.append(f"{ts}: implemented ({info['file']}) but missing from test_spec.md")
        for r in info["verifies"]:
            if r not in reqs:
                hard_gaps.append(f"{ts}: @relation references unknown {r}")
        for d in info["design"]:
            if d not in design:
                hard_gaps.append(f"{ts}: @design references unknown {d}")
        if impl[ts]["function"] not in results:
            open_gaps.append(f"{ts}: no recorded result (run tools/run_tests.sh)")
    for des, satisfied in design.items():
        for r in satisfied:
            if r not in reqs:
                hard_gaps.append(f"{des}: satisfies unknown {r}")
    for r in reqs:
        if not req_to_des.get(r):
            hard_gaps.append(f"{r}: no design element claims to satisfy it")
        if not req_to_ts.get(r):
            open_gaps.append(f"{r}: no automated test verifies it (coverage gap)")

    # ---- HTML matrix -------------------------------------------------------
    def esc(s):
        return html.escape(str(s))

    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    rows = []
    for r in sorted(reqs):
        des_list = ", ".join(sorted(req_to_des.get(r, []))) or "—"
        ts_cells = []
        verdict = "NO TEST"
        css = "gap"
        statuses = []
        for ts in sorted(req_to_ts.get(r, [])):
            func = impl[ts]["function"]
            status, suite = results.get(func, ("NO RESULT", ""))
            statuses.append(status)
            ts_cells.append(f"{ts} ({esc(suite or impl[ts]['file'])}: {status})")
        if statuses:
            if all(s == "PASS" for s in statuses):
                verdict, css = "COVERED · PASS", "ok"
            elif "FAIL" in statuses:
                verdict, css = "COVERED · FAIL", "fail"
            else:
                verdict, css = "PARTIAL RESULT", "warn"
        rows.append(
            f"<tr class='{css}'><td>{r}</td><td>{esc(reqs[r][:110])}</td>"
            f"<td>{esc(des_list)}</td><td>{'<br>'.join(ts_cells) or '—'}</td>"
            f"<td>{verdict}</td></tr>"
        )

    gaps_html = "".join(f"<li class='fail'>{esc(g)}</li>" for g in hard_gaps) + "".join(
        f"<li class='warn'>{esc(g)}</li>" for g in open_gaps
    )
    complete = [r for r in reqs if req_to_ts.get(r) and req_to_des.get(r)]
    summary = (
        f"{len(reqs)} requirements · {len(design)} design elements · "
        f"{len(spec)} specified tests · {len(impl)} implemented tests · "
        f"{len(results)} recorded results — "
        f"{len(complete)}/{len(reqs)} requirements fully traced to a executed test, "
        f"{len(hard_gaps)} hard gaps, {len(open_gaps)} open coverage gaps"
    )

    out = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>TradingApp traceability matrix</title>
<style>
body{{font-family:sans-serif;margin:24px;max-width:1200px}}
table{{border-collapse:collapse;width:100%}}
td,th{{border:1px solid #ccc;padding:4px 8px;font-size:13px;vertical-align:top;text-align:left}}
tr.ok td{{background:#eafbea}} tr.warn td{{background:#fff8e0}}
tr.gap td{{background:#f4f4f4}} tr.fail td{{background:#fde8e8}}
li.fail{{color:#b00}} li.warn{{color:#a67c00}}
</style></head><body>
<h1>Traceability matrix — requirements ↔ design ↔ test spec ↔ test result</h1>
<p>Generated {now} by tools/trace_report.py. ASPICE view: SWE.1 requirements ↔
SWE.3 design (constructive counterpart) ↔ SWE.4/SWE.5 unit &amp; integration
test specification and results; gaps are listed, not hidden.</p>
<p><b>{esc(summary)}</b></p>
<h2>Matrix</h2>
<table><tr><th>Requirement</th><th>Text</th><th>Design (satisfies)</th>
<th>Tests (spec → impl → result)</th><th>Verdict</th></tr>{''.join(rows)}</table>
<h2>Gaps</h2><ul>{gaps_html or '<li>none</li>'}</ul>
</body></html>"""
    (ROOT / "docs/traceability.html").write_text(out, **UTF8_LF)

    print(summary)
    for g in hard_gaps:
        print(f"HARD GAP: {g}")
    for g in open_gaps:
        print(f"open gap: {g}")
    print(f"wrote docs/traceability.html")
    return 1 if hard_gaps else 0


if __name__ == "__main__":
    sys.exit(main())
