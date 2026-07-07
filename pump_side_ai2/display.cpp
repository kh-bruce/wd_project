// =====================================================================
// display.cpp — 0.96" SSD1306 (128x64) OLED renderer.
//
// Loop-thread only. U8g2 full-buffer over hardware I2C. The whole U8g2/Wire
// dependency is contained here. Five styles selectable via the button
// (1=Retro, 2=Big Number, 3=Status Cards, 4=Radial Ring, 5=Split Panel); the
// default is DISPLAY_STYLE in config.h. A missing panel is detected at begin()
// and turns every subsequent tick into a one-comparison no-op.
// =====================================================================
#include "display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <Preferences.h>

#include "config.h"
#include "arduino_secrets.h" // SECRET_MQTT_HOST / SECRET_MQTT_PORT (LINK row)
#include "logging.h"
#include "commands.h"      // waterSnapshot()
#include "pump_control.h"  // pump_status, pumpOnSinceMs, thresholds
#include "failsafe.h"      // bad_conn_mode
#include "ntp_time.h"      // getLocalTimeFromCache()
#include "wifi_mgr.h"      // wifiConnected()
#include "mqtt_mgr.h"      // mqttIsConnected()

// Full-buffer, no-rotation, hardware-I2C. The panel in use is a 1.3" "IIC V2.2"
// module, which is an SH1106 controller (132 columns, panel mapped to cols
// 2..129). Using the SSD1306 driver shifts the image left 2px and clips the
// leftmost characters — the SH1106 driver handles the column offset correctly.
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /*reset=*/U8X8_PIN_NONE);

static bool gDisplayPresent = false;
static TaskHandle_t gDisplayTaskHandle = nullptr;
static unsigned long gSplashUntilMs = 0;   // keep the boot splash up until this millis()

// Active style (1/2/3), runtime-switchable via the button. Written by the loop
// thread (displayCycleStyle) and read by the render task; a plain aligned int
// is atomic on ESP32, and a one-frame-stale read during a switch is harmless.
// Persisted to NVS so it survives a reboot. DISPLAY_STYLE (config.h) is the
// factory default when nothing is stored yet.
static volatile int gDisplayStyle = DISPLAY_STYLE;
static Preferences gDisplayPrefs;   // same NVS namespace as pump_control ("wdpump")

static void displayTask(void*);   // defined below; spawned at the end of displayInit()

// ---- Snapshot of everything one frame needs (filled on the loop thread) ----
struct Frame {
  float        water;
  bool         waterValid;
  PumpStatus   pump;
  int          rounds;                 // completed overheat cooldowns this fill
  unsigned long pumpOnSinceMs;
  unsigned long pumpStatusChangedMs;  // millis() of last pump-state change
  float        minLevel, maxLevel, deficient;  // pump-side thresholds (config/NVS)
  float        towerMax, towerMin;             // tower session extremes (measured)
  bool         towerMaxValid, towerMinValid;
  bool         badConn;
  bool         wifiUp;
  long         rssi;
  char         ip[20];
  bool         mqttUp;
  bool         timeValid;
  int          hour;
  char         hhmm[6];   // "HH:MM"
  char         hhmmss[9]; // "HH:MM:SS"
  unsigned long uptimeS;
  unsigned long nowMs;
};

static void snapshot(Frame &f) {
  waterSnapshot(f.water, f.waterValid);
  towerStatsSnapshot(f.towerMax, f.towerMaxValid, f.towerMin, f.towerMinValid);
  f.pump          = pump_status;
  f.rounds        = overheatRounds;
  f.pumpOnSinceMs = pumpOnSinceMs;
  f.pumpStatusChangedMs = pumpStatusChangedMs;
  f.minLevel      = MIN_WATER_LEVEL;
  f.maxLevel      = MAX_WATER_LEVEL;
  f.deficient     = DEFICIENT_WATER_LEVEL;
  f.badConn       = bad_conn_mode;
  f.wifiUp        = wifiConnected();
  f.rssi          = f.wifiUp ? WiFi.RSSI() : 0;
  if (f.wifiUp) { String s = WiFi.localIP().toString(); s.toCharArray(f.ip, sizeof(f.ip)); }
  else          { strncpy(f.ip, "--", sizeof(f.ip)); }
  f.mqttUp        = mqttIsConnected();
  f.nowMs         = millis();
  f.uptimeS       = f.nowMs / 1000;

  int h = 0; String t;
  f.timeValid = getLocalTimeFromCache(h, t);
  f.hour = h;
  if (f.timeValid && t.length() >= 8) {
    // t == "HH:MM:SS"
    t.toCharArray(f.hhmmss, sizeof(f.hhmmss));
    f.hhmm[0] = t[0]; f.hhmm[1] = t[1]; f.hhmm[2] = ':';
    f.hhmm[3] = t[3]; f.hhmm[4] = t[4]; f.hhmm[5] = '\0';
  } else {
    strncpy(f.hhmm,   "--:--",    sizeof(f.hhmm));
    strncpy(f.hhmmss, "--:--:--", sizeof(f.hhmmss));
  }
}

// Effective max/min used BOTH for the displayed numbers and the fill math, so
// they always agree. Prefer the tower's measured session extremes; fall back to
// the pump-side thresholds until the (retained) tower stats have arrived.
static float effMax(const Frame &f) { return f.towerMaxValid ? f.towerMax : f.maxLevel; }
static float effMin(const Frame &f) { return f.towerMinValid ? f.towerMin : f.minLevel; }

// Fraction (0..1) of the live level within the effective [min,max].
static float fillFraction(const Frame &f) {
  if (!f.waterValid) return 0.0f;
  float lo = effMin(f), hi = effMax(f);
  float span = hi - lo;
  if (span <= 0.0f) return 0.0f;
  float frac = (f.water - lo) / span;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  return frac;
}

// Minutes the pump has been running (0 if not running / unknown).
static unsigned long pumpRunMin(const Frame &f) {
  if (f.pump != RUNNING || f.pumpOnSinceMs == 0) return 0;
  return (f.nowMs - f.pumpOnSinceMs) / 60000UL;
}

// Pump state tag with the overheat-round suffix: while running or cooling the
// name gets "+N" appended (N = completed cooldowns this fill, so the first run
// shows "+0"); stopped shows the plain name.
static void pumpTag(const Frame &f, char *out, size_t n,
                    const char *run, const char *heat, const char *stop) {
  if (f.pump == RUNNING)                  snprintf(out, n, "%s+%d", run, f.rounds);
  else if (f.pump == OVERHEAT_PROTECTION) snprintf(out, n, "%s+%d", heat, f.rounds);
  else                                    snprintf(out, n, "%s", stop);
}

// Format how long the pump has been in its CURRENT state, compactly, into out:
// "<Ns>" under a minute, "<Nm>" under an hour, else "<Nh Mm>". Works for any
// state (run/stop/overheat) since it keys off pumpStatusChangedMs.
static void pumpStateAge(const Frame &f, char *out, size_t n) {
  unsigned long s = (f.pumpStatusChangedMs == 0) ? 0
                    : (f.nowMs - f.pumpStatusChangedMs) / 1000UL;
  if (s < 60)        snprintf(out, n, "%lus", s);
  else if (s < 3600) snprintf(out, n, "%lum", s / 60);
  else               snprintf(out, n, "%luh %lum", s / 3600, (s % 3600) / 60);
}

// =====================================================================
// Style C — Retro Terminal
// =====================================================================
static void drawRetro(const Frame &f) {
  u8g2.setFont(u8g2_font_5x8_tf);
  char line[28];

  u8g2.drawStr(0, 7, "WD-PUMP 1F");
  u8g2.drawStr(78, 7, f.badConn ? "[FAILSAFE]" : "[ONLINE]");
  // header rule
  for (int x = 0; x < 128; x += 4) u8g2.drawStr(x, 16, "=");

  // water + [####----] bar
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%05.1f", f.water);
  else              strncpy(num, "--.--", sizeof(num));
  char bar[11];
  int filled = (int)(fillFraction(f) * 8 + 0.5f);
  bar[0] = '[';
  for (int i = 0; i < 8; i++) bar[1 + i] = (i < filled) ? '#' : '-';
  bar[9] = ']'; bar[10] = '\0';
  snprintf(line, sizeof(line), "WATER: %s %s", num, bar);
  u8g2.drawStr(0, 26, line);

  // pump — state (+overheat round) then time spent in the CURRENT state.
  char age[10], tag[14];
  pumpStateAge(f, age, sizeof(age));
  pumpTag(f, tag, sizeof(tag), "RUNNING", "OVERHEAT", "STOPPED");
  snprintf(line, sizeof(line), "PUMP : %s %s", tag, age);
  // blink the OVERHEAT line
  if (f.pump != OVERHEAT_PROTECTION || (f.nowMs / 400) % 2 == 0)
    u8g2.drawStr(0, 35, line);

  // LINK row alternates (same period as SET/REC): WiFi IP+signal, then MQTT.
  bool showPhaseB = (f.nowMs / cfg::DISPLAY_THR_TOGGLE_MS) % 2 == 1;
  if (!showPhaseB) {
    if (f.wifiUp) snprintf(line, sizeof(line), "WIFI : %s %ld", f.ip, f.rssi);
    else          snprintf(line, sizeof(line), "WIFI : disconnected");
  } else {
    if (f.mqttUp) snprintf(line, sizeof(line), "MQTT : %s:%d",
                           SECRET_MQTT_HOST, (int)SECRET_MQTT_PORT);
    else          snprintf(line, sizeof(line), "MQTT : offline");
  }
  u8g2.drawStr(0, 44, line);

  // SET/REC row toggles in sync with the LINK row. SET = user thresholds
  // (min / fill / max); REC = measured record extremes shown as "lo << - - - >> hi".
  if (!showPhaseB) {
    snprintf(line, sizeof(line), "SET  : %.0f / %.0f / %.0f",
             f.minLevel, f.deficient, f.maxLevel);
  } else {
    char loStr[6], hiStr[6];
    if (f.towerMinValid) snprintf(loStr, sizeof(loStr), "%.0f", f.towerMin); else strncpy(loStr, "--", sizeof(loStr));
    if (f.towerMaxValid) snprintf(hiStr, sizeof(hiStr), "%.0f", f.towerMax); else strncpy(hiStr, "--", sizeof(hiStr));
    snprintf(line, sizeof(line), "REC  : %s << - - >> %s", loStr, hiStr);
  }
  u8g2.drawStr(0, 53, line);

  // bottom row alternates (same period as the LINK/SET rows): uptime, then
  // wall-clock time (NTP). Blinking cursor stays on both.
  if (!showPhaseB) {
    unsigned long up = f.uptimeS;
    snprintf(line, sizeof(line), "UP   : %02lu:%02lu:%02lu",
             up / 3600, (up % 3600) / 60, up % 60);
  } else {
    snprintf(line, sizeof(line), "TIME : %s", f.hhmmss);
  }
  u8g2.drawStr(0, 62, line);
  if ((f.nowMs / 500) % 2 == 0) u8g2.drawStr(118, 62, "_");
}

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

  // --- bottom: pump state (+overheat round) + run-min + clock ---
  u8g2.setFont(u8g2_font_5x8_tf);
  char bot[26], st[10];
  pumpTag(f, st, sizeof(st), "RUN", "HEAT", "STOP");
  snprintf(bot, sizeof(bot), "PUMP %s %lum   %s", st, pumpRunMin(f), f.hhmm);
  u8g2.drawStr(0, 63, bot);
}

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
  // Card is too narrow for "HEAT+N" in the big font, so the overheat round
  // rides on the title row ("PUMP+N") instead.
  u8g2.drawFrame(52, 38, 36, 24);
  u8g2.setFont(u8g2_font_5x8_tf);
  char lbl[10];
  pumpTag(f, lbl, sizeof(lbl), "PUMP", "PUMP", "PUMP");
  u8g2.drawStr(56, 46, lbl);
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
  char st[10];
  pumpTag(f, st, sizeof(st), "RUN", "HEAT", "STOP");
  u8g2.drawStr(0, 7, st);                                   // top-left: pump
  u8g2.drawStr(128 - (int)strlen(f.hhmm) * 5, 7, f.hhmm);  // top-right: clock
  u8g2.drawStr(0, 63, f.mqttUp ? "MQTT" : "mqtt?");        // bottom-left: link
  char rs[8];                                               // bottom-right: rssi
  snprintf(rs, sizeof(rs), "%ld", f.rssi);
  u8g2.drawStr(128 - (int)strlen(rs) * 5, 63, rs);
}

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
  char st[10], l1[16];
  pumpTag(f, st, sizeof(st), "RUN", "HEAT", "STOP");
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

// =====================================================================
// Public API
// =====================================================================
void displayInit() {
  // Restore the saved style (falls back to the DISPLAY_STYLE default). Reads a
  // separate handle on the same "wdpump" NVS namespace pump_control uses.
  gDisplayPrefs.begin("wdpump", false);
  gDisplayStyle = gDisplayPrefs.getInt("style", DISPLAY_STYLE);
  if (gDisplayStyle < 1 || gDisplayStyle > 5) gDisplayStyle = DISPLAY_STYLE;

  Wire.begin(cfg::I2C_SDA, cfg::I2C_SCL);
  // EMINOTE: relays/boost/12V-RF on this board couple noise onto I2C. Run the
  // bus SLOW (100kHz) for margin, and hard-cap every transaction so an
  // EMI-wedged bus can never block loopTask past the watchdog. Without the
  // timeout the IDF I2C lock waits portMAX_DELAY -> a stuck SDA hangs forever.
  Wire.setClock(100000);
  Wire.setTimeOut(20);    // ms per transaction (default is effectively unbounded)
  u8g2.setI2CAddress(cfg::OLED_ADDR << 1);  // U8g2 wants the 8-bit address.
  u8g2.setBusClock(100000);                 // make U8g2 keep the slow clock at each START
  gDisplayPresent = u8g2.begin();           // false if no panel responds.
  if (!gDisplayPresent) {
    logWarning("OLED not found at 0x3C — continuing headless");
    return;
  }
  // One-frame boot splash (shown during the WiFi connect wait that follows).
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(16, 28, "WATER DUCK");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(20, 44, "pump 1F  boot");
  u8g2.sendBuffer();
  gSplashUntilMs = millis() + 2000;   // hold the splash on screen for >=2s

  // Hand the panel over to a dedicated task. From here on, ALL U8g2/Wire access
  // happens on displayTask — the splash above is the last flush on the setup
  // thread, and loop() never touches the display — so I2C has a single owner.
  xTaskCreatePinnedToCore(displayTask, "oled", 4096, NULL, 1,
                          &gDisplayTaskHandle, APP_CPU_NUM);
}

// Advance to the next style (1->..->5->1) and persist it. Called from the loop
// thread (button handler); the render task picks up the new value next frame.
void displayCycleStyle() {
  int next = gDisplayStyle + 1;
  if (next > 5) next = 1;
  gDisplayStyle = next;
  gDisplayPrefs.putInt("style", next);   // survive reboot
  logVerbose("OLED style -> " + String(next));
}

// Startup / reconnect screen: shown whenever WiFi or MQTT is not up (both at
// boot and if either link drops later). Staged checklist — WiFi then MQTT —
// with an animated "..." on the step we're currently waiting on so it's clear
// the controller is alive and working through the sequence.
static void drawConnecting(const Frame &f) {
  // Animated dots for the in-progress step: "", ".", "..", "..." on a ~300ms
  // cycle. Time-driven (no stored state), so it works fine on the render task.
  static const char* DOTS[4] = { "", ".", "..", "..." };
  const char* dots = DOTS[(f.nowMs / 300) % 4];

  // Title
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(10, 12, "WATER DUCK 1F");
  u8g2.drawHLine(0, 16, 128);

  u8g2.setFont(u8g2_font_6x10_tf);

  // WiFi step: ticked once associated; the pending step shows the dots.
  char line[24];
  snprintf(line, sizeof(line), "[%c] WiFi%s",
           f.wifiUp ? 'v' : ' ', f.wifiUp ? "" : dots);
  u8g2.drawStr(6, 34, line);

  // MQTT step. Only animate MQTT once WiFi is up (that's the step we're on);
  // while WiFi is still connecting, MQTT just shows an empty box.
  snprintf(line, sizeof(line), "[%c] MQTT%s",
           f.mqttUp ? 'v' : ' ', (f.wifiUp && !f.mqttUp) ? dots : "");
  u8g2.drawStr(6, 50, line);
}

// Render one frame: probe the bus, snapshot loop state, draw, and flush over
// I2C. Runs ONLY on displayTask (below) — the ~100ms sendBuffer() that used to
// stall loop() now blocks this dedicated task instead. Frame pacing is handled
// by the task's vTaskDelayUntil, so there is no millis() throttle here.
static void renderOnce() {
  unsigned long now = millis();

  // Bus liveness probe (cheap, ~every 2s): if the panel stops ACKing — e.g. an
  // EMI-wedged bus or a yanked cable — latch the display OFF so every later
  // tick becomes a no-op instead of repeatedly stalling on a dead bus. The
  // probe itself is bounded by Wire.setTimeOut(20) set in displayInit().
  static unsigned long lastProbeMs = 0;
  if (now - lastProbeMs >= 2000) {
    lastProbeMs = now;
    Wire.beginTransmission(cfg::OLED_ADDR);
    if (Wire.endTransmission() != 0) {
      gDisplayPresent = false;
      logWarning("OLED bus not responding — display disabled");
      return;
    }
  }

  // Hold the boot splash on screen for its minimum dwell before drawing
  // anything else. The splash was flushed in displayInit(); we simply don't
  // overwrite the buffer yet. (Bus probe above still runs.)
  if (gSplashUntilMs != 0) {
    if (now < gSplashUntilMs) return;
    gSplashUntilMs = 0;   // window elapsed; resume normal rendering
  }

  Frame f;
  snapshot(f);

  u8g2.clearBuffer();

  // Until BOTH WiFi and MQTT are up, show the connecting screen instead of the
  // main UI (whose water data isn't meaningful without the tower's MQTT feed).
  // Re-evaluated every frame, so this also covers a mid-run drop of either link.
  if (!f.wifiUp || !f.mqttUp) {
    drawConnecting(f);
    u8g2.sendBuffer();
    return;
  }

  switch (gDisplayStyle) {
    case 1:  drawRetro(f);       break;
    case 2:  drawBigNumber(f);   break;
    case 3:  drawStatusCards(f); break;
    case 4:  drawRadialRing(f);  break;
    case 5:  drawSplitPanel(f);  break;
    default: drawBigNumber(f);   break;   // default -> Big Number
  }

  // Pump-running alert: flash the WHOLE screen between normal and inverted,
  // at the SAME rate as the onboard status LED while RUNNING (BLINK_NORMAL_MS).
  // One XOR-fill over the full buffer flips every pixel; applies to all styles.
  if (f.pump == RUNNING &&
      (now / (unsigned long)cfg::BLINK_NORMAL_MS) % 2 == 1) {
    u8g2.setDrawColor(2);            // XOR mode
    u8g2.drawBox(0, 0, 128, 64);     // invert every pixel
    u8g2.setDrawColor(1);            // restore
  }

  u8g2.sendBuffer();
}

// Dedicated OLED task. The I2C flush is the only place we block, and it happens
// here — never on loop() — so a ~100ms sendBuffer() can no longer starve the
// MQTT keepalive. Pinned to APP_CPU_NUM like the tower's samplingTask; NOT
// registered with the task WDT (only loop() is). Every iteration yields via
// vTaskDelayUntil, which also sets the frame cadence (DISPLAY_FRAME_MS).
static void displayTask(void*) {
  const TickType_t period = pdMS_TO_TICKS(cfg::DISPLAY_FRAME_MS);
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    if (gDisplayPresent) renderOnce();
    vTaskDelayUntil(&last, period);
  }
}
