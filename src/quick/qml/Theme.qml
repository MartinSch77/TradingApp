// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

pragma Singleton

import QtQuick

// The cockpit's design tokens, in ONE place (REQ-F-038, DES-UI-COCKPIT).
//
// Every colour below was VALIDATED rather than chosen by eye, with the palette validator,
// against this surface in dark mode. The numbers are recorded because they are the argument
// for keeping the trading convention instead of abandoning it:
//
//   up #008300 vs down #e66767 : CVD dE 8.6 (protanopia, >= 8 target) · normal-vision 32.6
//   + reference #3987e5, all three pairs : CVD dE 8.6 · normal-vision 29.0 · all >= 3:1
//
// So green/red stays — traders expect it — but 8.6 clears the target without margin, which
// is why every polarity mark in this cockpit ALSO carries a sign, an arrow or a word. Colour
// is never the only channel.
//
// Deliberately NOT derived by inverting a light theme: dark steps were selected for the dark
// surface and validated against it. A light variant would be re-stepped and re-validated,
// not flipped.
QtObject {
    // id kept for readability; the singleton is referenced as `Theme` by consumers.
    id: theme

    // surfaces
    readonly property color surface: "#111318"
    readonly property color panel: "#181b22"
    readonly property color plane: "#0d0d0d"
    readonly property color gridline: "#2c2c2a"

    // ink
    readonly property color ink: "#ffffff"
    readonly property color inkSecondary: "#c3c2b7"
    readonly property color inkMuted: "#898781"

    // polarity — a DIVERGING pair with a neutral midpoint, not a categorical palette
    readonly property color up: "#008300"
    readonly property color down: "#e66767"
    readonly property color neutral: "#383835"
    readonly property color reference: "#3987e5"

    // status, used one at a time and always beside an icon or label
    readonly property color warn: "#fab219"

    readonly property int radius: 6
    readonly property int gap: 10
    // Transitions on state change only. A permanently animating dashboard costs frames and
    // attention for nothing.
    readonly property int anim: 180

    // No helper FUNCTION here on purpose: a JS function on a `pragma Singleton` is not
    // reliably callable from a binding (measured: "Property 'polarity' of object Theme is not
    // a function"). Consumers pick with a declarative ternary over the tokens above, which is
    // more idiomatic QML regardless.
}
