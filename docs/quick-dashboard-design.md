# Qt Quick dashboard — design specification

@page quickdesign Qt Quick dashboard — design specification

The hybrid dashboard: a QML surface hosted in a `QQuickWidget` inside the existing
Widgets window. Read-only by construction. This page is the design decided before any
QML was written, so the reasoning is reviewable rather than reverse-engineered.

Brief: *modern, good usability, colourful, attractive for traders, fast response,
clear, on-time results.*

## 1. The split, and why it is a safety boundary

| Area | Technology |
|---|---|
| Market cards, price chart, confluence meter, model read, risk tiles, positions **view**, replay transport | **QML** |
| Amount, leverage, SL/TP entry, the double-press gate, order submission | **Qt Widgets, untouched** |

Everything that can move money stays on the existing Widgets path. That path is
GUI-tested, object-name-gated (REQ-N-007) and governed by REQ-N-005; re-implementing it
in QML would mean re-earning all of that for no user benefit. The QML surface cannot
place an order — it has no route to one, the same property the bot simulation has.

## 2. Layout

Three columns on desktop, reflowing by width — `RowLayout` of `ColumnLayout`s with
`Layout.fillWidth`, not fixed geometry.

```
┌ nav rail ┬ market cards (4) ─────────────┬ confluence meter ┐
│ Dashboard│ price chart + volume pane     │ model read       │
│ Replay   │                               │ risk tiles       │
│ Risk     ├───────────────────────────────┴──────────────────┤
│ Bot      │ open positions      │ replay timeline            │
└──────────┴─────────────────────┴────────────────────────────┘
```

- **≥ 1600 px** three columns as drawn.
- **1000–1600 px** the right column drops beneath the chart.
- **< 1000 px (tablet/phone)** one column, nav rail collapses to icons.

## 3. Colour — measured, not chosen by eye

Surface `#111318`, page plane `#0d0d0d`, primary ink `#ffffff`, secondary `#c3c2b7`,
muted `#898781`, gridline `#2c2c2a`.

**Up/down is a polarity job, so it is a diverging pair with a neutral midpoint** — not
a categorical palette. The traditional trading green/red is kept, because it was
*validated* rather than assumed:

| Role | Hex | Result on `#111318` |
|---|---|---|
| up / long / take-profit | `#008300` | CVD ΔE **8.6** vs red (protan; ≥8 target), normal-vision **32.6**, contrast ≥3:1 |
| down / short / stop-loss | `#e66767` | as above |
| flat / neutral midpoint | `#383835` | — |
| entry / reference | `#3987e5` | trio all-pairs: CVD **8.6**, normal **29.0**, all ≥3:1 |

So Entry-blue, SL-red and TP-green may appear on the chart together. Verified with the
palette validator, both `--pairs all` and dark surface — the numbers above are its
output, not an estimate.

**Green/red still never carries meaning alone.** ΔE 8.6 clears the target but not
comfortably, so every polarity mark ships a second channel: a sign (`+`/`−`), an arrow
glyph, or the word Long/Short. That is also why the design does not depend on a trader
having normal colour vision to read their own P&L.

## 4. The confluence meter — the one place the mockup had to change

A "6/8" arc is effectively a two-slice donut, and the guidance is blunt about those:
the number is the chart. Worse, a single fraction cannot express the fact this
application is built around — **how many reads could be measured at all**.

Measured obstacle: the reserved status palette's *good* (`#0ca30c`) and *critical*
(`#d03b3b`) sit at **ΔE 4.1 under deuteranopia** — indistinguishable. A row of
green/amber/grey dots, as drawn in the mockup, is therefore unreadable for a
red-green-colourblind trader. It cannot ship that way.

**Decided:** nine discrete ticks around an arc, one per read, each encoding state by
**fill shape first and colour second**, with a hero number in the middle:

```
        ●●●●●○○✕✕            ●  agrees        (filled)
      6 of 9 agree           ○  disagrees     (outline)
      3 unmeasurable         ✕  unmeasurable  (hatched, muted)
```

Nine ticks, not a continuous sweep, because there are nine discrete reads. The label
states agreement **and** unmeasurability, since "6 of 9" and "6 of 9 with 3
unmeasurable" are different facts and REQ-F-035 forbids collapsing them. Each read is
named in a list beneath with its glyph, its direction and its number — identity never
rests on colour.

## 5. Price chart

- **Two panes, never two y-axes.** Price above, volume below, sharing the x-axis. A
  dual-axis plot invents a correlation that is not in the data.
- Candlesticks via `QCandlestickSeries`, behind an `IPriceChartBackend` seam so a Qt
  Graphs backend can be added later. Bodies use the polarity pair; **direction is also
  carried by the body being filled or hollow**, so an up and a down candle differ in
  shape as well as hue.
- Overlays: Entry (blue, dashed), SL (red, dashed), TP (green, dashed), each with a
  right-edge label carrying its price — a line without its number is decoration.
- Crosshair + tooltip by default: O/H/L/C, volume, and the timestamp.

## 6. "On-time results" is a display requirement, not a performance one

The feed lags. Measured on this project: the eToro rates row for `.24-7` indices runs
6–12 minutes late while the 1-minute candle close matches the bid exactly. A dashboard
that renders a stale price in the same style as a live one is lying quietly.

Every number therefore carries a freshness state, reusing what the app already tracks
(`allPnlLive`, quote `ageMs`):

| State | Rendering |
|---|---|
| live | full-strength ink, no marker |
| lagging (age > one bar) | secondary ink + age badge, e.g. `6m old` |
| stale / unknown | muted ink, value replaced by `—`, never a last-known number styled as current |

The header keeps the existing seconds clock, and the connection strip states latency
and message count as in the mockup.

## 7. Response budget

- The positions view binds to the existing `PositionsModel` — already a
  `QAbstractTableModel` with roles, so QML consumes it directly. This is the layered
  architecture paying off visibly: no adapter, no duplicated state.
- No JavaScript allocation per tick. Sparklines and the meter are `Qt Quick Shapes`
  (`PathPolyline`, `PathArc`) driven by property bindings; the C++ side pushes changed
  values, QML does not poll.
- Animations are transitions on state change (150–200 ms), never continuous
  animation — a permanently animating dashboard costs frames and attention for nothing.
- Everything heavy stays off the GUI thread, as REQ-N-006 already requires.

## 8. What the dashboard must never claim

The mockup, taken literally, would have the UI assert four things the requirements
forbid. These are design constraints, not preferences:

| Must not show | Must show instead | Why |
|---|---|---|
| `Confidence 74%` | `Evidence 74/100`, and a measured P(up) or **UNCALIBRATED** with its sample count | REQ-F-037: a probability is a claim about frequency; a weighted sum is not |
| `Model: GPT-4o` | `Local model · qwen2.5:1.5b`, marked advisory | It is a local model, and it never trades |
| A `Live Trading` dropdown | SIMULATION as an unmissable banner | Switching to real money stays deliberate; a combo box undercuts the double-press gate |
| `US10Y` as an open position | US10Y in the market cards only | `^TNX` is a reference read, not a tradable instrument here |

Display currency is **EUR**, not `$`.

## 9. Theme

Dark is the default and is *selected*, not an inverted light theme — the steps above
were validated against the dark surface. A light variant uses the same hues re-stepped
for a light surface and re-validated; a theme singleton holds both, so no component
carries a raw hex.
