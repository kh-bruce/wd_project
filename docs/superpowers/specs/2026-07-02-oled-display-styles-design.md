# OLED Display Styles Overhaul — Design

**Date:** 2026-07-02
**Target:** `pump_side_ai2` — 1.3" SH1106 128×64 I2C OLED renderer (`display.cpp` / `display.h` / `config.h`)
**Branch context:** `tune-up/pump-manual-run`

## Goal

Replace the current OLED style set. Remove the **Dashboard** and **Animated** styles, keep **Retro**, and add four new single-frame styles: **Big Number**, **Status Cards**, **Radial Ring**, **Split Panel**. All five coexist and are cycled by the on-board button (short press), with the selection persisted to NVS exactly as today.

Rationale: the animated tank/impeller style is being retired; the new styles are simpler to render (pure snapshot draws, no animation state) and surface the water level and the five level setpoints more legibly. Design directions were validated visually via the brainstorming visual companion (mockups drawn at true 128×64).

## Non-goals

- No new sensors, MQTT topics, or data sources — every style reads only the existing `Frame` snapshot.
- No historical/trend buffer (the "Trend Graph" candidate was explicitly dropped — it was the only style needing stored history).
- No change to the display task, I2C ownership, bus-probe, splash, connecting-screen, or pump-running full-screen-flash logic. New styles inherit all of it unchanged.

## Final style list (button-cycle order)

`gDisplayStyle` is a 1-based index; short-press advances `1→2→3→4→5→1` and persists to NVS.

| # | Name         | Source                          |
|---|--------------|---------------------------------|
| 1 | Retro        | existing `drawRetro` (unchanged)|
| 2 | Big Number   | new `drawBigNumber`             |
| 3 | Status Cards | new `drawStatusCards`           |
| 4 | Radial Ring  | new `drawRadialRing`            |
| 5 | Split Panel  | new `drawSplitPanel`            |

**Factory default** (`DISPLAY_STYLE`, used on first flash / empty NVS): **Big Number** (style 2).

## Architecture

Each style is a `static void drawXxx(const Frame &f)` added to the `switch (gDisplayStyle)` in `renderOnce()` ([display.cpp:562](../../../pump_side_ai2/display.cpp)). This matches the existing pattern exactly. Key invariants preserved:

- **Single-frame draws.** No style holds state between frames. (The Big Number 15s toggle is time-derived from `f.nowMs`, not stored state — same technique the Retro style already uses.)
- **Fill math is shared.** Any level-vs-range rendering uses the existing `effMin(f)` / `effMax(f)` / `fillFraction(f)` helpers so displayed numbers and fill bars always agree, and so the tower's measured session extremes are preferred with pump-side thresholds as fallback ([display.cpp:103-116](../../../pump_side_ai2/display.cpp)).
- **Global overlays untouched.** The WiFi/MQTT connecting screen ([display.cpp:556](../../../pump_side_ai2/display.cpp)) and the pump-running full-screen XOR flash ([display.cpp:572](../../../pump_side_ai2/display.cpp)) wrap all styles; the four new draws need no code for either.

### `Frame` fields used

All already populated by `snapshot()` ([display.cpp:68](../../../pump_side_ai2/display.cpp)):
`water/waterValid`, `pump`, `pumpOnSinceMs`, `pumpStatusChangedMs`, `minLevel/maxLevel/deficient`, `towerMin/towerMax` (+valid flags), `wifiUp`, `rssi`, `mqttUp`, `hhmm/hhmmss`, `uptimeS`, `nowMs`. Existing helpers `pumpRunMin(f)` and `pumpStateAge(f,…)` are reused for pump-run duration.

### Units

**No "cm" or any unit label** anywhere. The water level is treated as a unitless number throughout (removed from all new styles; also confirmed absent in the visual mockups).

## Style specifications

Coordinates below are the 128×64 panel space; they are targets from the validated mockups, to be pixel-tuned during implementation against the real panel.

### 2 · Big Number
- **Top row (y≈0–6):** five setpoint values, `towerMin` far-left (x≈1), `MIN`/`DEFICIENT`/`MAX` evenly spaced in the middle, `towerMax` far-right (x≈116). 6px font.
- **15s toggle:** every 15s the top row swaps between the numeric values and 2-letter labels `Lo` (recmin) / `PL` (pump low = MIN) / `Pf` (prefill = DEFICIENT) / `PH` (pump max = MAX) / `Hi` (recmax). Driven by `(f.nowMs / cfg::DISPLAY_THR_TOGGLE_MS) % 2` — reuse the existing 15000ms constant, no new state.
- **Hero number:** `water` (`%.1f`), large (~36px `logisoso`-class font), **left-aligned at x≈4** (explicitly NOT centered). `"--.-"` when `!waterValid`.
- **Fill bar:** thin horizontal bar (y≈52) filled by `fillFraction(f)`.
- **Bottom (y≈59, 5px):** compact `PUMP <state> <run-min>   <hhmm>`.

### 3 · Status Cards
Layout per user's sketch — one tall card on the left, a stacked column on the right:
- **SET card (left, full height, x≈2–48, y≈2–62):** title `SET` + rule, then the five setpoints one-per-row: `recL <towerMin>`, `low <MIN>`, `pref <DEFICIENT>`, `max <MAX>`, `recH <towerMax>`. 6px font.
- **WATER card (right-top, x≈52–126, y≈2–34):** label `WATER` + `water` big number (~17px).
- **PUMP card (right-bottom-left, x≈52–88, y≈38–62):** framed/emphasized; `PUMP` + state (`RUN`/`STOP`/`HEAT`) + run-min. The frame is the "active" emphasis (blinks with the global flash while RUNNING).
- **TIME card (right-bottom-right, x≈90–126, y≈38–62):** `TIME` + `hhmm`.
- LINK/MQTT/RSSI are **not shown** in this style — acceptable because a dropped link forces the connecting screen, so if the main UI is visible the link is up.

### 4 · Radial Ring
- **Ring:** ~13-dot circular gauge; lit dots fill clockwise by `fillFraction(f)`, remaining dots dim. (Dot-plotted circle, cheap; no trig-heavy per-frame cost beyond a fixed dot table or a small loop.)
- **Center:** `water` (~12px) with `of <effMax>` beneath it.
- **Corners:** top-left pump state (`●RUN`, blinks), top-right `hhmm`, bottom-left `MQTT`, bottom-right `rssi`.

### 5 · Split Panel
- **Divider** at x≈74 (vertical).
- **Left:** `LEVEL` label, big `water` (~26px), rule, then `▶RUN <min>` (~12px) and `<hhmm> up<Nh>` (~10px). MQTT line intentionally removed for breathing room.
- **Right:** wider segmented vertical bar (E-style segments, x≈80–104) filled from the bottom by `fillFraction(f)`; outer-edge ticks (short mark + value) for `towerMax` (top), `DEFICIENT`/prefill (at its true height within `[effMin,effMax]`), `towerMin` (bottom).

### 1 · Retro
Unchanged. Existing `drawRetro` stays exactly as-is.

## Changes by file

- **`display.cpp`**
  - Delete `drawDashboard` and its helpers if unused elsewhere; delete `drawAnimated`, `drawTankPage`, `drawSystemPage`, `drawImpeller` (the Animated style and its private helpers). Verify no other caller before removing.
  - Add `drawBigNumber`, `drawStatusCards`, `drawRadialRing`, `drawSplitPanel`.
  - Rewrite the `switch (gDisplayStyle)` to the new 1–5 mapping; update the fallback.
  - `displayInit()`: change the range clamp from `1..3` to `1..5` ([display.cpp:446](../../../pump_side_ai2/display.cpp)).
  - `displayCycleStyle()`: change the wrap from `>3 → 1` to `>5 → 1` ([display.cpp:481](../../../pump_side_ai2/display.cpp)).
  - Update the file header comment (currently "Three styles … Style B animated is default").
- **`display.h`**
  - Update the `displayCycleStyle()` doc comment (`1->..->1` range).
- **`config.h`**
  - `DISPLAY_STYLE`: set default to Big Number's index and update the comment listing the styles ([config.h:10-12](../../../pump_side_ai2/config.h)).
  - `DISPLAY_PAGE_ROTATE_MS` was only used by the Animated style — remove it (and its comment) if nothing else references it. Keep `DISPLAY_THR_TOGGLE_MS` (reused by Retro and Big Number).

## Testing / verification

This is firmware for an ESP32 with a physical panel, so verification is primarily on-device:

1. **Builds clean** — compiles with no warnings after the removals (watch for now-unused helpers/constants).
2. **Cycle order** — short-press walks `Retro → Big Number → Status Cards → Radial Ring → Split Panel → Retro`; selection survives a reboot (NVS).
3. **Default** — wipe NVS (or first flash) → boots into Big Number.
4. **Data correctness per style** — with live tower data: level number matches across styles; fill bar/ring track `fillFraction`; the five setpoints in Big Number and Status Cards match config/NVS; Big Number top row toggles every 15s.
5. **Overlays still work** — pull WiFi/MQTT → connecting screen appears over every style; while pumping → full-screen flash on every style.
6. **Edge cases** — `!waterValid` shows `--.-`; missing tower stats (`!towerMinValid/!towerMaxValid`) fall back to pump-side thresholds and show a sensible placeholder.

## Risks / open items

- **Font availability** — the large-number styles need suitable U8g2 fonts (e.g. `logisoso24_tn` already used by Dashboard, or a larger `logisoso` for Big Number's ~36px). Confirm the chosen fonts fit flash and render at the target sizes; adjust sizes to available fonts during implementation.
- **Pixel tuning** — mockup coordinates are approximate; exact glyph widths on U8g2 differ from the browser mockups, so positions will be nudged on real hardware.
- **Radial ring cost** — keep the ring as a fixed dot table or a bounded loop; avoid per-frame `sinf/cosf` over many points if it shows up in frame timing (the panel task is not on the WDT, but keep flushes snappy).
