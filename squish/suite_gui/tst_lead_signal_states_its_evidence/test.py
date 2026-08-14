# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# The combined indication (REQ-F-036) must state WHAT IT WAS BUILT FROM, on screen.
#
# What this checks that a unit test cannot: the label a person actually reads is
# consistent with the evidence behind it. The invariant of REQ-F-035 is that an
# unmeasurable read counts as nothing and is SAID to be unmeasurable — a "4 of 5"
# assembled from absent feeds is a lie. A unit test can prove leadSignal() honours
# that; only a GUI test can prove the window does not quietly render an ellipsis, a
# bare 0, or a strength it has no inputs for.
#
# Deliberately independent of whether any feed arrived. This suite runs with no
# credentials and may or may not reach Yahoo, so every assertion below holds in both
# cases: the check is that the number of inputs is stated and that the claim never
# exceeds it. A test that needed live data would be a test that fails on a train.
#
# @relation(REQ-F-036, scope=file)
# @relation(REQ-F-035, scope=file)
#
# The markers are for Test Center's repository mapping (`testcentercmd integration map
# --prefix='@relation('`), which scans test files for requirement ids. GUI tests
# deliberately carry NO TS id: docs/test_spec.md pairs a TS id with a C++ implementation
# in tests/tst_*.cpp, and listing one here without that implementation would register as
# a specified-but-unimplemented test in tools/trace_report.py.

import builtins
import re

import names

# `int` inside a Squish script is the Qt INT TYPE injected by the Python bindings, not
# Python's builtin, so `int("4")` fails with "No matching 'int(str)' overload found:
# int::int(), int::int(int), int::int(int *)". Reach for the builtin explicitly — this
# cost a run to discover and would cost the next reader the same.
to_int = builtins.int


def lead_labels():
    return (("NSDQ100", names.nasdaqLeadSignal), ("SPX500", names.spLeadSignal))


def main():
    startApplication("TradingApp")
    test.verify("SIMULATION" in str(waitForObject(names.modeLabel).text),
                "the run must be in simulation")

    clickButton(waitForObject(names.heavyButton))
    # Wait for the window by one of its children, as tst_heavyweights_early_read does:
    # the dialog class is reported differently by the wrapper across builds, while the
    # objectNames REQ-N-007 guarantees resolve reliably.
    test.verify(waitForObject(names.nasdaqHeavyTable) is not None,
                "the heavyweights window opened")

    for index, label in lead_labels():
        text = str(waitForObject(label).text)

        # 1. The indication is STATED, not left as the ellipsis the label is built with.
        #    This is the regression that matters: the labels used to be filled only when
        #    a feed arrived, so a run with every feed down looked identical to a run that
        #    had not started yet.
        test.verify(len(text) > 0 and text.strip() != "…",
                    "%s states an indication rather than an ellipsis: %s"
                    % (index, text[:120]))

        # 2. It says how many inputs it had. That count is the denominator of every
        #    claim the label makes, so a label without it cannot be judged at all.
        counted = re.search(r"(\d+)\s+of\s+(\d+)\s+inputs\s+meas", text)
        test.verify(counted is not None,
                    "%s reports how many of its inputs were measurable: %s"
                    % (index, text[:160]))
        if counted is None:
            continue
        measured, total = to_int(counted.group(1)), to_int(counted.group(2))

        # 3. Nine independent reads plus the constituent field. Pinned so that adding a
        #    read without surfacing it here fails visibly instead of silently widening
        #    the denominator behind the reader's back.
        test.compare(total, 10,
                     "%s weighs the nine reads plus the constituent field" % index)
        test.verify(0 <= measured <= total,
                    "%s cannot measure more inputs than it has (%d of %d)"
                    % (index, measured, total))

        # 4. THE HONESTY RULE. With nothing measurable there is no direction and no
        #    strength — the label must say so instead of printing a number. With
        #    something measurable, a strength is allowed.
        if measured == 0:
            test.verify("No usable read" in text,
                        "%s with no measurable input says so" % index)
            test.verify("strength" not in text,
                        "%s claims no strength when it measured nothing: %s"
                        % (index, text[:160]))
        else:
            test.verify("strength" in text or "No usable read" in text,
                        "%s either states a strength or declines to: %s"
                        % (index, text[:160]))

        # 5. Every input that could not be read is NAMED as unmeasurable, rather than
        #    being dropped from the list or shown as a flat 0. The count of such
        #    statements must account for every unmeasured input.
        unmeasurable = text.count("not measurable") + text.count("no prices read")
        test.verify(unmeasurable >= total - measured,
                    "%s names each unmeasured input (%d named, %d unmeasured)"
                    % (index, unmeasurable, total - measured))

        # 6. The leverage it names is an upper bound from the evidence, and it is only
        #    named when there is an indication to justify it.
        if measured > 0 and "strength" in text:
            test.verify(re.search(r"max\s+x\d+", text) is not None,
                        "%s names the leverage its evidence justifies: %s"
                        % (index, text[:160]))

    # 7. The stand-in caveat is still on screen beside the indication. The combined
    #    number is the part most likely to be read as authoritative, so the sentence
    #    saying these ten names are NOT real breadth has to sit next to it.
    caveat = str(waitForObject(names.heavyCaveat).text)
    test.verify("STAND-IN" in caveat.upper(),
                "the window still names itself a stand-in for breadth")

    names.closeAppGracefully(waitForObject(names.mainWindow))
