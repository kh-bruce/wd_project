# OLED Display Styles Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `pump_side_ai2` OLED style set — remove Dashboard + Animated, keep Retro, add four new single-frame styles (Big Number, Status Cards, Radial Ring, Split Panel), cycled 1–5 by the button with Big Number as the default.

**Architecture:** Each style is a `static void drawXxx(const Frame &f)` in `display.cpp`, selected by a `switch (gDisplayStyle)` in `renderOnce()`. All styles read only the existing per-frame `Frame` snapshot — no new tasks, no history buffer, no new data sources. The connecting-screen and pump-running full-screen-flash overlays already wrap every style and need no per-style code. Selection persists to NVS exactly as today.

**Tech Stack:** Arduino / ESP32, U8g2 (SH1106 128×64, full-buffer, HW I2C), Preferences (NVS). Firmware compiled with `arduino-cli`; safety logic has a host-side `clang++` test harness (`test/run.sh`) but **display.cpp is not part of it** (pure rendering, no U8g2 fake) — display verification is compile-check + on-device.

**Spec:** [docs/superpowers/specs/2026-07-02-oled-display-styles-design.md](../specs/2026-07-02-oled-display-styles-design.md)

---

## Reference: current code facts (read before starting)

- Style dispatch: `switch (gDisplayStyle)` at [display.cpp:562](../../../pump_side_ai2/display.cpp) — currently `1=drawDashboard`, `2=drawAnimated`, `3=drawRetro`.
- Range clamp on boot: [display.cpp:446](../../../pump_side_ai2/display.cpp) `if (gDisplayStyle < 1 || gDisplayStyle > 3) gDisplayStyle = DISPLAY_STYLE;`
- Cycle wrap: [display.cpp:481](../../../pump_side_ai2/display.cpp) `int next = gDisplayStyle + 1; if (next > 3) next = 1;`
- **Shared helpers to KEEP:** `effMin`/`effMax`/`fillFraction` ([display.cpp:103-116](../../../pump_side_ai2/display.cpp)), `pumpRunMin` ([display.cpp:119](../../../pump_side_ai2/display.cpp)), `pumpStateAge` ([display.cpp:127](../../../pump_side_ai2/display.cpp)). Retro uses `pumpStateAge`; new styles use `pumpRunMin`.
- **Style-private code to DELETE:** `drawDashboard` (Dashboard); `drawImpeller`, `drawTankPage`, `drawSystemPage`, `drawAnimated` (Animated). Confirmed no other callers (grep in Task 1).
- Fonts confirmed present: `u8g2_font_logisoso24_tn` (big digits, used by Dashboard), `u8g2_font_7x13B_tf`, `u8g2_font_6x10_tf`, `u8g2_font_5x8_tf`.
- `Frame` fields (all populated by `snapshot()` [display.cpp:68](../../../pump_side_ai2/display.cpp)): `water/waterValid`, `pump`, `pumpOnSinceMs`, `pumpStatusChangedMs`, `minLevel/maxLevel/deficient`, `towerMin/towerMax/towerMinValid/towerMaxValid`, `wifiUp`, `rssi`, `mqttUp`, `hhmm[6]`, `hhmmss[9]`, `uptimeS`, `nowMs`.
- `PumpStatus` enum: `RUNNING`, `STOPPED`, `OVERHEAT_PROTECTION` ([config.h:84](../../../pump_side_ai2/config.h)).

## Build / verify commands

- **Compile check** (fast, no upload) — run from repo root:
  ```bash
  arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2
  ```
  If the FQBN differs on this machine, use whatever `arduino-cli board list` / the existing flashing workflow uses; the point is a clean compile. Expected: `Sketch uses … bytes`, no errors.
- **Safety-logic tests unaffected** (sanity that removals didn't touch logic): `bash pump_side_ai2/test/run.sh` → expected last line `═══ N tests, 0 passed ═══` (i.e. all pass; the harness prints the passing count).
- **On-device** verification checklist is in the final task.

---

## Task 1: Remove Dashboard + Animated styles (leave a working 2-style set)

Delete the two retired styles and their private helpers, and shrink the dispatch to just the survivors so the file compiles at every step. After this task the panel cycles Dashboard-gone → only `drawRetro` remains reachable; we temporarily map style 1 = Retro so the build is valid.

**Files:**
- Modify: `pump_side_ai2/display.cpp`

- [ ] **Step 1: Confirm the helpers are style-private (no surprise callers)**

Run:
```bash
cd pump_side_ai2 && grep -n "drawDashboard\|drawImpeller\|drawTankPage\|drawSystemPage\|drawAnimated" display.cpp
```
Expected: matches only at the definitions (~138, 199, 246, 289, 309, 349, 357-358) and the two `switch` cases (563, 564). No references outside `display.cpp` (grep the whole dir):
```bash
grep -rn "drawDashboard\|drawAnimated\|drawImpeller\|drawTankPage\|drawSystemPage" . | grep -v display.cpp
```
Expected: no output.

- [ ] **Step 2: Delete `drawDashboard`**

Remove the entire function block `static void drawDashboard(const Frame &f) { … }` (from the `// Style A — Info Dashboard` banner comment through its closing brace, ~[display.cpp:135-191](../../../pump_side_ai2/display.cpp)).

- [ ] **Step 3: Delete the Animated style + its private helpers**

Remove the whole block from the `// Style B — Animated …` banner comment through the end of `drawAnimated` — i.e. `drawImpeller`, `drawTankPage`, `drawSystemPage`, and `drawAnimated` (~[display.cpp:193-359](../../../pump_side_ai2/display.cpp)). Leave `pumpRunMin` and `pumpStateAge` (they sit above, ~lines 118-133) intact — they are shared.

- [ ] **Step 4: Shrink the dispatch switch to the survivors**

In `renderOnce()`, replace the switch ([display.cpp:562-567](../../../pump_side_ai2/display.cpp)) with a temporary 1-style mapping so it compiles:

```cpp
  switch (gDisplayStyle) {
    case 1:  drawRetro(f); break;
    default: drawRetro(f); break;   // unknown value -> safe fallback
  }
```

- [ ] **Step 5: Relax the boot clamp temporarily**

At [display.cpp:446](../../../pump_side_ai2/display.cpp), keep it valid for the shrunken set for now:

```cpp
  if (gDisplayStyle < 1 || gDisplayStyle > 1) gDisplayStyle = 1;
```

(This is rewritten to `> 5` in Task 6; keeping it correct at each step avoids an out-of-range style selecting the fallback silently.)

- [ ] **Step 6: Compile check**

Run:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2
```
Expected: clean compile, no "defined but not used" warnings for `drawDashboard`/`drawAnimated`/helpers (they're gone). If a warning fires for `pumpRunMin` being unused now, that's expected and temporary — it gets used again in Task 2; leave it.

- [ ] **Step 7: Commit**

```bash
git add pump_side_ai2/display.cpp
git commit -m "pump_side_ai2: remove Dashboard + Animated OLED styles

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Add Big Number style (new default)

**Files:**
- Modify: `pump_side_ai2/display.cpp`

- [ ] **Step 1: Add the `drawBigNumber` function**

Insert after `drawRetro` (before the `// Public API` banner, ~[display.cpp:437](../../../pump_side_ai2/display.cpp)):

```cpp
// =====================================================================
// Style — Big Number: hero water level, unitless, left-aligned.
// Top row shows the five setpoints; every DISPLAY_THR_TOGGLE_MS it swaps
// between numeric values and 2-letter labels. Time-derived, no state.
// =====================================================================
static void drawBigNumber(const Frame &f) {
  // --- top row: five setpoints, values <-> labels every 15s ---
  bool showLabels = (f.nowMs / cfg::DISPLAY_THR_TOGGLE_MS) % 2 == 1;
  u8g2.setFont(u8g2_font_5x8_tf);
  char s[8];
  // rec min (far-left)
  if (showLabels)          u8g2.drawStr(0, 7, "Lo");
  else if (f.towerMinValid){ snprintf(s, sizeof(s), "%.0f", f.towerMin); u8g2.drawStr(0, 7, s); }
  else                     u8g2.drawStr(0, 7, "--");
  // pump low / prefill / pump max (evenly spaced middle)
  if (showLabels) { u8g2.drawStr(34, 7, "PL"); u8g2.drawStr(58, 7, "Pf"); u8g2.drawStr(82, 7, "PH"); }
  else {
    snprintf(s, sizeof(s), "%.0f", f.minLevel);   u8g2.drawStr(34, 7, s);
    snprintf(s, sizeof(s), "%.0f", f.deficient);  u8g2.drawStr(58, 7, s);
    snprintf(s, sizeof(s), "%.0f", f.maxLevel);   u8g2.drawStr(82, 7, s);
  }
  // rec max (far-right, right-aligned to edge)
  if (showLabels)           u8g2.drawStr(116, 7, "Hi");
  else if (f.towerMaxValid){ snprintf(s, sizeof(s), "%.0f", f.towerMax); u8g2.drawStr(128 - (int)strlen(s) * 5, 7, s); }
  else                      u8g2.drawStr(118, 7, "--");
  u8g2.drawHLine(0, 10, 128);

  // --- hero number: left-aligned, unitless ---
  u8g2.setFont(u8g2_font_logisoso24_tn);   // biggest confirmed-present digit font
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%.1f", f.water);
  else              strncpy(num, "--.-", sizeof(num));
  u8g2.drawStr(4, 42, num);                // x=4 => flush-left, NOT centered

  // --- fill bar ---
  const int bx = 6, by = 52, bw = 116, bh = 5;
  u8g2.drawFrame(bx, by, bw, bh);
  int fillW = (int)((bw - 2) * fillFraction(f));
  if (fillW > 0) u8g2.drawBox(bx + 1, by + 1, fillW, bh - 2);

  // --- bottom: pump state + run-min + clock ---
  u8g2.setFont(u8g2_font_5x8_tf);
  char bot[26];
  const char *st;
  switch (f.pump) {
    case RUNNING:             st = "RUN";  break;
    case OVERHEAT_PROTECTION: st = "HEAT"; break;
    default:                  st = "STOP"; break;
  }
  snprintf(bot, sizeof(bot), "PUMP %s %lum   %s", st, pumpRunMin(f), f.hhmm);
  u8g2.drawStr(0, 63, bot);
}
```

- [ ] **Step 2: Wire it into the dispatch as style 2**

Update the switch in `renderOnce()`:

```cpp
  switch (gDisplayStyle) {
    case 1:  drawRetro(f);     break;
    case 2:  drawBigNumber(f); break;
    default: drawBigNumber(f); break;   // default -> Big Number
  }
```

- [ ] **Step 3: Update the boot clamp for the growing set**

At the clamp line: `if (gDisplayStyle < 1 || gDisplayStyle > 2) gDisplayStyle = 2;`

- [ ] **Step 4: Compile check**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2`
Expected: clean compile. `pumpRunMin` warning from Task 1 is now resolved (it's used here).

- [ ] **Step 5: Commit**

```bash
git add pump_side_ai2/display.cpp
git commit -m "pump_side_ai2: add Big Number OLED style

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Add Status Cards style

Layout: one full-height SET card on the left (five setpoints, one per row); right column = WATER (top) with PUMP + TIME cards below.

**Files:**
- Modify: `pump_side_ai2/display.cpp`

- [ ] **Step 1: Add the `drawStatusCards` function**

Insert after `drawBigNumber`:

```cpp
// =====================================================================
// Style — Status Cards: tall SET card (left) + WATER/PUMP/TIME (right).
// =====================================================================
static void drawStatusCards(const Frame &f) {
  char v[8];
  // --- LEFT: full-height SET card with the five setpoints ---
  u8g2.drawFrame(2, 2, 46, 60);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(6, 10, "SET");
  u8g2.drawHLine(5, 13, 40);
  // helper: format a setpoint value or "--"
  #define BN_SET(row_y, label, valid, val) do { \
      if (valid) snprintf(v, sizeof(v), "%.0f", (double)(val)); else strncpy(v, "--", sizeof(v)); \
      char line[16]; snprintf(line, sizeof(line), "%-4s%s", label, v); \
      u8g2.drawStr(6, row_y, line); } while (0)
  BN_SET(24, "recL", f.towerMinValid, f.towerMin);
  BN_SET(33, "low",  true,            f.minLevel);
  BN_SET(42, "pref", true,            f.deficient);
  BN_SET(51, "max",  true,            f.maxLevel);
  BN_SET(60, "recH", f.towerMaxValid, f.towerMax);
  #undef BN_SET

  // --- RIGHT-TOP: WATER card ---
  u8g2.drawFrame(52, 2, 74, 32);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(56, 11, "WATER");
  u8g2.setFont(u8g2_font_logisoso24_tn);
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%.1f", f.water);
  else              strncpy(num, "--.-", sizeof(num));
  u8g2.drawStr(58, 31, num);

  // --- RIGHT-BOTTOM-LEFT: PUMP card (framed = emphasis; flashes when RUNNING) ---
  u8g2.drawFrame(52, 38, 36, 24);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(56, 46, "PUMP");
  const char *st;
  switch (f.pump) {
    case RUNNING:             st = "RUN";  break;
    case OVERHEAT_PROTECTION: st = "HEAT"; break;
    default:                  st = "STOP"; break;
  }
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(56, 60, st);
  u8g2.setFont(u8g2_font_5x8_tf);
  snprintf(v, sizeof(v), "%lum", pumpRunMin(f));
  u8g2.drawStr(76, 60, v);

  // --- RIGHT-BOTTOM-RIGHT: TIME card ---
  u8g2.drawFrame(90, 38, 36, 24);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(94, 46, "TIME");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(93, 60, f.hhmm);
}
```

- [ ] **Step 2: Wire it into the dispatch as style 3**

```cpp
  switch (gDisplayStyle) {
    case 1:  drawRetro(f);       break;
    case 2:  drawBigNumber(f);   break;
    case 3:  drawStatusCards(f); break;
    default: drawBigNumber(f);   break;
  }
```

- [ ] **Step 3: Update the boot clamp**

`if (gDisplayStyle < 1 || gDisplayStyle > 3) gDisplayStyle = 2;`

- [ ] **Step 4: Compile check**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2`
Expected: clean compile.

- [ ] **Step 5: Commit**

```bash
git add pump_side_ai2/display.cpp
git commit -m "pump_side_ai2: add Status Cards OLED style

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Add Radial Ring style

Ring of dots filling clockwise by fill fraction; center = level + `of <max>`; corners carry pump/clock/MQTT/RSSI.

**Files:**
- Modify: `pump_side_ai2/display.cpp`

- [ ] **Step 1: Add the `drawRadialRing` function**

Insert after `drawStatusCards`. Uses a fixed 12-dot ring computed once with a bounded loop (cheap; no per-dot state):

```cpp
// =====================================================================
// Style — Radial Ring: 12-dot circular gauge, level in the center.
// Dots fill clockwise starting from 12 o'clock by fillFraction.
// =====================================================================
static void drawRadialRing(const Frame &f) {
  const int cx = 63, cy = 33, r = 26;
  const int N = 12;
  int lit = (int)(fillFraction(f) * N + 0.5f);   // how many dots are "filled"
  for (int i = 0; i < N; i++) {
    // start at top (12 o'clock), go clockwise
    float ang = -PI / 2.0f + i * (2.0f * PI / N);
    int x = cx + (int)(r * cosf(ang));
    int y = cy + (int)(r * sinf(ang));
    if (i < lit) u8g2.drawDisc(x, y, 1);
    else         u8g2.drawPixel(x, y);
  }

  // center: big-ish value + "of <max>"
  u8g2.setFont(u8g2_font_6x10_tf);
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%.1f", f.water);
  else              strncpy(num, "--.-", sizeof(num));
  // rough centering: 6px glyphs
  int nx = cx - (int)strlen(num) * 3;
  u8g2.drawStr(nx, cy, num);
  u8g2.setFont(u8g2_font_5x8_tf);
  char ofs[10];
  float hi = effMax(f);
  snprintf(ofs, sizeof(ofs), "of %.0f", hi);
  u8g2.drawStr(cx - (int)strlen(ofs) * 2, cy + 9, ofs);

  // corners
  u8g2.setFont(u8g2_font_5x8_tf);
  const char *st;
  switch (f.pump) {
    case RUNNING:             st = "RUN";  break;
    case OVERHEAT_PROTECTION: st = "HEAT"; break;
    default:                  st = "STOP"; break;
  }
  u8g2.drawStr(0, 7, st);                                   // top-left: pump
  u8g2.drawStr(128 - (int)strlen(f.hhmm) * 5, 7, f.hhmm);  // top-right: clock
  u8g2.drawStr(0, 63, f.mqttUp ? "MQTT" : "mqtt?");        // bottom-left: link
  char rs[8];                                               // bottom-right: rssi
  snprintf(rs, sizeof(rs), "%ld", f.rssi);
  u8g2.drawStr(128 - (int)strlen(rs) * 5, 63, rs);
}
```

- [ ] **Step 2: Wire it into the dispatch as style 4**

```cpp
  switch (gDisplayStyle) {
    case 1:  drawRetro(f);       break;
    case 2:  drawBigNumber(f);   break;
    case 3:  drawStatusCards(f); break;
    case 4:  drawRadialRing(f);  break;
    default: drawBigNumber(f);   break;
  }
```

- [ ] **Step 3: Update the boot clamp**

`if (gDisplayStyle < 1 || gDisplayStyle > 4) gDisplayStyle = 2;`

- [ ] **Step 4: Compile check**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2`
Expected: clean compile. (`cosf/sinf/PI` are already used by the deleted impeller code's math via `<math.h>`/Arduino.h — confirm no missing-symbol error; PI is defined by Arduino.h.)

- [ ] **Step 5: Commit**

```bash
git add pump_side_ai2/display.cpp
git commit -m "pump_side_ai2: add Radial Ring OLED style

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Add Split Panel style

Left: LEVEL + big number + pump/clock. Right: wider segmented vertical bar with rec/prefill ticks on the outer edge.

**Files:**
- Modify: `pump_side_ai2/display.cpp`

- [ ] **Step 1: Add the `drawSplitPanel` function**

Insert after `drawRadialRing`:

```cpp
// =====================================================================
// Style — Split Panel: big level + pump/clock (left) | segmented bar (right).
// =====================================================================
static void drawSplitPanel(const Frame &f) {
  u8g2.drawVLine(74, 2, 60);

  // --- left: LEVEL + hero number + pump/clock ---
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(2, 7, "LEVEL");
  u8g2.setFont(u8g2_font_logisoso24_tn);
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%.1f", f.water);
  else              strncpy(num, "--.-", sizeof(num));
  u8g2.drawStr(1, 34, num);
  u8g2.drawHLine(2, 38, 70);
  u8g2.setFont(u8g2_font_6x10_tf);
  const char *st;
  switch (f.pump) {
    case RUNNING:             st = "RUN";  break;
    case OVERHEAT_PROTECTION: st = "HEAT"; break;
    default:                  st = "STOP"; break;
  }
  char l1[16];
  snprintf(l1, sizeof(l1), "%s %lum", st, pumpRunMin(f));
  u8g2.drawStr(2, 50, l1);
  u8g2.setFont(u8g2_font_5x8_tf);
  char l2[16];
  snprintf(l2, sizeof(l2), "%s up%luh", f.hhmm, f.uptimeS / 3600UL);
  u8g2.drawStr(2, 62, l2);

  // --- right: segmented vertical bar (fills bottom-up by fillFraction) ---
  const int bx = 80, by = 4, bw = 24, bh = 56;
  u8g2.drawFrame(bx, by, bw, bh);
  float frac = fillFraction(f);
  int fillH = (int)((bh - 4) * frac);
  int innerTop = by + (bh - 2) - fillH;
  if (fillH > 0) u8g2.drawBox(bx + 2, innerTop, bw - 4, fillH);

  // --- outer-edge ticks: recmax (top), prefill (true height), recmin (bottom) ---
  u8g2.setFont(u8g2_font_5x8_tf);
  char t[6];
  // recmax at top edge
  if (f.towerMaxValid) snprintf(t, sizeof(t), "%.0f", f.towerMax); else strncpy(t, "--", sizeof(t));
  u8g2.drawHLine(bx + bw, by + 1, 4);
  u8g2.drawStr(bx + bw + 6, by + 4, t);
  // recmin at bottom edge
  if (f.towerMinValid) snprintf(t, sizeof(t), "%.0f", f.towerMin); else strncpy(t, "--", sizeof(t));
  u8g2.drawHLine(bx + bw, by + bh - 1, 4);
  u8g2.drawStr(bx + bw + 6, by + bh, t);
  // prefill at its true height within [effMin, effMax]
  float lo = effMin(f), hi = effMax(f), span = hi - lo;
  if (span > 0.0f) {
    float pfrac = (f.deficient - lo) / span;
    if (pfrac < 0.0f) pfrac = 0.0f; if (pfrac > 1.0f) pfrac = 1.0f;
    int py = by + (bh - 2) - (int)((bh - 4) * pfrac);
    u8g2.drawHLine(bx + bw, py, 4);
    snprintf(t, sizeof(t), "%.0f", f.deficient);
    u8g2.drawStr(bx + bw + 6, py + 3, t);
  }
}
```

- [ ] **Step 2: Wire it into the dispatch as style 5**

```cpp
  switch (gDisplayStyle) {
    case 1:  drawRetro(f);       break;
    case 2:  drawBigNumber(f);   break;
    case 3:  drawStatusCards(f); break;
    case 4:  drawRadialRing(f);  break;
    case 5:  drawSplitPanel(f);  break;
    default: drawBigNumber(f);   break;
  }
```

- [ ] **Step 3: Compile check**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2`
Expected: clean compile.

- [ ] **Step 4: Commit**

```bash
git add pump_side_ai2/display.cpp
git commit -m "pump_side_ai2: add Split Panel OLED style

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Finalize cycle range, default, and comments

Lock the 1–5 range, set Big Number as the factory default, and fix stale comments/constants.

**Files:**
- Modify: `pump_side_ai2/display.cpp`, `pump_side_ai2/display.h`, `pump_side_ai2/config.h`

- [ ] **Step 1: Set the boot clamp to the final range**

At [display.cpp:446](../../../pump_side_ai2/display.cpp):

```cpp
  if (gDisplayStyle < 1 || gDisplayStyle > 5) gDisplayStyle = DISPLAY_STYLE;
```

- [ ] **Step 2: Set the cycle wrap to 5**

In `displayCycleStyle()` ([display.cpp:481-482](../../../pump_side_ai2/display.cpp)):

```cpp
  int next = gDisplayStyle + 1;
  if (next > 5) next = 1;
```

- [ ] **Step 3: Update `config.h` default + comment**

At [config.h:10-12](../../../pump_side_ai2/config.h) replace the style comment and default:

```cpp
// ---- OLED display style (1=Retro, 2=Big Number, 3=Status Cards,
//       4=Radial Ring, 5=Split Panel). Button short-press cycles 1..5;
//       selection persists to NVS. This is the factory default. ----
#define DISPLAY_STYLE 2
```

- [ ] **Step 4: Remove the now-unused `DISPLAY_PAGE_ROTATE_MS`**

Confirm it is unreferenced, then delete it:
```bash
cd pump_side_ai2 && grep -rn "DISPLAY_PAGE_ROTATE_MS" .
```
Expected: only its definition at [config.h:73](../../../pump_side_ai2/config.h). Delete that line (it was Animated-only). Leave `DISPLAY_THR_TOGGLE_MS` — used by Retro and Big Number.

- [ ] **Step 5: Update `display.h` doc comment**

At [display.h:19](../../../pump_side_ai2/display.h):

```cpp
void displayCycleStyle(); // advance OLED style 1->..->5->1 and persist to NVS. Call from loop thread (button).
```

- [ ] **Step 6: Update the `display.cpp` file header**

Fix the header block ([display.cpp:1-8](../../../pump_side_ai2/display.cpp)) so it no longer says "Three styles … Style B (animated) is the default." Replace the style sentence with:

```
// Five styles selectable via the button (1=Retro, 2=Big Number, 3=Status
// Cards, 4=Radial Ring, 5=Split Panel); the default is DISPLAY_STYLE in
// config.h. A missing panel is detected at begin() and turns every
// subsequent tick into a one-comparison no-op.
```

- [ ] **Step 7: Compile check + safety tests**

Run:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 pump_side_ai2
bash pump_side_ai2/test/run.sh
```
Expected: clean compile; test harness all-pass (removals didn't touch safety logic).

- [ ] **Step 8: Commit**

```bash
git add pump_side_ai2/display.cpp pump_side_ai2/display.h pump_side_ai2/config.h
git commit -m "pump_side_ai2: finalize 5-style cycle, Big Number default, cleanup

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: On-device verification

No code changes — flash and confirm on the real panel. Record results; if a style is mispositioned, nudge coordinates in the relevant `drawXxx` and re-flash (that's a follow-up commit, not a plan failure).

**Files:** none (hardware test)

- [ ] **Step 1: Flash the board** using the project's normal upload workflow (`arduino-cli upload …` with the correct port, or the IDE).

- [ ] **Step 2: Default style** — after a fresh flash (or NVS wipe), the panel boots into **Big Number** once WiFi+MQTT are up.

- [ ] **Step 3: Cycle order** — short-press the button and confirm the order: **Retro → Big Number → Status Cards → Radial Ring → Split Panel → Retro**. Reboot and confirm the last-selected style is restored (NVS).

- [ ] **Step 4: Data correctness** — with live tower data:
  - The level number matches across styles.
  - Fill bar (Big Number, Split Panel) and ring (Radial Ring) track the level within the range.
  - The five setpoints in Big Number's top row and in Status Cards' SET card match config/NVS (`recL/low/pref/max/recH` = towerMin/MIN/DEFICIENT/MAX/towerMax).
  - Big Number's top row toggles values ⇄ labels every ~15s.
  - No "cm" or unit appears anywhere.

- [ ] **Step 5: Overlays** — pull WiFi/MQTT → the connecting screen appears over whichever style is active. While pumping → the whole screen flashes on every style.

- [ ] **Step 6: Edge cases** — with no valid water reading the hero number shows `--.-`; before tower stats arrive, `recL/recH` show `--` and the fill math falls back to pump-side thresholds without glitching.

- [ ] **Step 7: Note any pixel nudges needed** and, if any, apply + commit as `pump_side_ai2: tune <style> layout on hardware`.

---

## Notes for the implementer

- **Font sizes are approximate.** `u8g2_font_logisoso24_tn` is the confirmed big digit font (Dashboard used it). If a specific style wants larger, try `u8g2_font_logisoso28_tn`/`_32_tn` but only if it compiles and fits — otherwise stay on 24. The mockups' "~36px" is a visual target, not a font name.
- **Coordinates come from mockups** validated at true 128×64; U8g2 glyph widths differ slightly from the browser, so expect small nudges in Task 7. Getting the layout structurally right (which card where, left-aligned hero, tick positions) matters more than pixel-exactness in this plan.
- **`#define BN_SET` in Task 3** is a local macro `#undef`'d immediately after use — kept local so it can't leak. If the reviewer prefers a static helper function instead, that's an acceptable equivalent.
- **Do not touch** `renderOnce()`'s bus probe, splash dwell, connecting-screen branch, or the pump-running flash — new styles must not special-case any of them.
