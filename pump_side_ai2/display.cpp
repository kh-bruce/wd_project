// =====================================================================
// display.cpp — 0.96" SSD1306 (128x64) OLED renderer.
//
// Loop-thread only. U8g2 full-buffer over hardware I2C. The whole U8g2/Wire
// dependency is contained here. Three styles selectable via DISPLAY_STYLE in
// config.h; Style B (animated) is the default. A missing panel is detected at
// begin() and turns every subsequent tick into a one-comparison no-op.
// =====================================================================
#include "display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>

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
static void displayTask(void*);   // defined below; spawned at the end of displayInit()

// ---- Snapshot of everything one frame needs (filled on the loop thread) ----
struct Frame {
  float        water;
  bool         waterValid;
  PumpStatus   pump;
  unsigned long pumpOnSinceMs;
  unsigned long pumpStatusChangedMs;  // millis() of last pump-state change
  float        minLevel, maxLevel, deficient;  // pump-side thresholds (config/NVS)
  float        towerMax, towerMin;             // tower session extremes (measured)
  bool         towerMaxValid, towerMinValid;
  bool         badConn;
  bool         wifiUp;
  long         rssi;
  char         ssid[20];
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
  f.pumpOnSinceMs = pumpOnSinceMs;
  f.pumpStatusChangedMs = pumpStatusChangedMs;
  f.minLevel      = MIN_WATER_LEVEL;
  f.maxLevel      = MAX_WATER_LEVEL;
  f.deficient     = DEFICIENT_WATER_LEVEL;
  f.badConn       = bad_conn_mode;
  f.wifiUp        = wifiConnected();
  f.rssi          = f.wifiUp ? WiFi.RSSI() : 0;
  if (f.wifiUp) { String s = WiFi.SSID(); s.toCharArray(f.ssid, sizeof(f.ssid)); }
  else          { strncpy(f.ssid, "--", sizeof(f.ssid)); }
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
// Style A — Info Dashboard
// =====================================================================
#if DISPLAY_STYLE == 1
static void drawDashboard(const Frame &f) {
  // --- top status bar ---
  u8g2.setFont(u8g2_font_6x10_tf);
  // WiFi indicator
  u8g2.drawStr(0, 8, f.wifiUp ? "WiFi" : "wifi?");
  // MQTT / link state
  if (f.badConn)        u8g2.drawStr(34, 8, "!LINK!");
  else if (f.mqttUp)    u8g2.drawStr(34, 8, "MQTT");
  else                  u8g2.drawStr(34, 8, "mqtt?");
  // clock (right-aligned-ish)
  u8g2.drawStr(86, 8, f.hhmm);
  u8g2.drawHLine(0, 11, 128);

  // --- hero water number ---
  u8g2.setFont(u8g2_font_logisoso24_tn);
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%.1f", f.water);
  else              strncpy(num, "--.-", sizeof(num));
  u8g2.drawStr(2, 44, num);

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(2, 52, "WATER");

  // --- pump badge ---
  const char *badge;
  switch (f.pump) {
    case RUNNING:             badge = " RUN "; break;
    case OVERHEAT_PROTECTION: badge = "HEAT!"; break;
    default:                  badge = "STOP "; break;
  }
  // blink the HEAT badge
  bool show = (f.pump != OVERHEAT_PROTECTION) || ((f.nowMs / 400) % 2 == 0);
  u8g2.setFont(u8g2_font_6x10_tf);
  int bw = 6 * 5 + 6;
  int bx = 128 - bw - 2;
  if (show) {
    u8g2.drawRBox(bx, 22, bw, 14, 2);
    u8g2.setDrawColor(0);
    u8g2.drawStr(bx + 3, 32, badge);
    u8g2.setDrawColor(1);
  } else {
    u8g2.drawRFrame(bx, 22, bw, 14, 2);
  }

  // --- bottom: tower session min/max (measured) + prefill target ---
  u8g2.setFont(u8g2_font_5x8_tf);
  char bot[26];
  char loStr[6], hiStr[6];
  if (f.towerMinValid) snprintf(loStr, sizeof(loStr), "%.0f", f.towerMin); else strncpy(loStr, "--", sizeof(loStr));
  if (f.towerMaxValid) snprintf(hiStr, sizeof(hiStr), "%.0f", f.towerMax); else strncpy(hiStr, "--", sizeof(hiStr));
  snprintf(bot, sizeof(bot), "min%s max%s fil%.0f", loStr, hiStr, f.deficient);
  u8g2.drawStr(0, 63, bot);
  if (!f.waterValid) u8g2.drawStr(98, 52, "STALE");
}
#endif

// =====================================================================
// Style B — Animated water tank + spinning pump + page rotation (DEFAULT)
// =====================================================================
#if DISPLAY_STYLE == 2
// Draw a centrifugal-pump impeller: outer casing + outlet nub + hub + 3
// backward-curved vanes. The vane set rotates while RUNNING, is static when
// STOPPED, and is replaced by a blinking X on bad-connection.
static void drawImpeller(int px, int py, int rOut, int rHub,
                         PumpStatus pump, bool badConn, unsigned long nowMs) {
  // Casing (double ring for a heavier, more detailed look) + outlet nub.
  u8g2.drawCircle(px, py, rOut);
  u8g2.drawCircle(px, py, rOut - 2);
  // outlet spout at upper-right (typical volute discharge)
  u8g2.drawBox(px + rOut - 3, py - rOut + 1, 5, 4);

  if (badConn) {
    if ((nowMs / 300) % 2 == 0) {
      int d = rOut - 3;
      u8g2.drawLine(px - d, py - d, px + d, py + d);
      u8g2.drawLine(px - d, py + d, px + d, py - d);
    }
    u8g2.drawDisc(px, py, rHub);
    return;
  }

  // Rotation phase: finer stepping (24 positions/rev) to match the higher fps,
  // so the vanes turn smoothly rather than snapping between coarse angles.
  float rot = 0.0f;
  if (pump == RUNNING) {
    int step = (nowMs / 45) % 24;          // 24 positions per revolution
    rot = step * (2.0f * PI / 24.0f);
  }

  const int rTip = rOut - 3;               // vane tip radius
  const int rRoot = rHub + 1;              // vane root radius
  const float curve = 0.6f;                // tangential bend at the tip (radians)
  for (int b = 0; b < 3; b++) {
    float base = rot + b * (2.0f * PI / 3.0f);
    // root point (near hub)
    int x0 = px + (int)(rRoot * cosf(base));
    int y0 = py + (int)(rRoot * sinf(base));
    // mid point (straight radial)
    float rMid = (rRoot + rTip) * 0.5f;
    int x1 = px + (int)(rMid * cosf(base));
    int y1 = py + (int)(rMid * sinf(base));
    // tip point (bent backward-curved relative to spin)
    int x2 = px + (int)(rTip * cosf(base + curve));
    int y2 = py + (int)(rTip * sinf(base + curve));
    u8g2.drawLine(x0, y0, x1, y1);
    u8g2.drawLine(x1, y1, x2, y2);
  }
  u8g2.drawDisc(px, py, rHub);             // hub on top of the vane roots
}

static void drawTankPage(const Frame &f) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 8, "WATER LEVEL");
  char num[8];
  if (f.waterValid) snprintf(num, sizeof(num), "%.1f", f.water);
  else              strncpy(num, "--.-", sizeof(num));
  u8g2.drawStr(92, 8, num);

  // Tank outline (left side). Narrowed to x:2..78 to make room for the larger
  // pump impeller on the right. y:14..52 (leaves a row for big min/max labels).
  const int tx = 2, ty = 14, tw = 76, th = 38;
  u8g2.drawFrame(tx, ty, tw, th);

  // Fill from the bottom up, proportional to level within [min,max].
  float frac = fillFraction(f);
  int fillH = (int)((th - 4) * frac);
  int fy = ty + (th - 2) - fillH;
  if (fillH > 0) u8g2.drawBox(tx + 2, fy, tw - 4, fillH);

  // 1px shimmer line on the surface while pumping.
  bool pumping = (f.pump == RUNNING);
  if (pumping && fillH > 0 && (f.nowMs / 150) % 2 == 0) {
    u8g2.setDrawColor(0);
    u8g2.drawHLine(tx + 2, fy, tw - 4);
    u8g2.setDrawColor(1);
  }

  // min/max labels under the tank — tower's measured session extremes.
  // Bigger 6x10 font; max on the left, min on the right of the tank's width.
  u8g2.setFont(u8g2_font_6x10_tf);
  char lab[8];
  if (f.towerMinValid) snprintf(lab, sizeof(lab), "min%.0f", f.towerMin);
  else                 strncpy(lab, "min--", sizeof(lab));
  u8g2.drawStr(tx, 63, lab);
  if (f.towerMaxValid) snprintf(lab, sizeof(lab), "max%.0f", f.towerMax);
  else                 strncpy(lab, "max--", sizeof(lab));
  // right-align against the tank's right edge (each glyph is 6px wide)
  u8g2.drawStr(tx + tw - (int)strlen(lab) * 6, 63, lab);

  // --- pump impeller (right side): larger, centrifugal-vane style ---
  const int px = 104, py = 27;   // center
  const int rOut = 13;           // casing radius
  const int rHub = 3;            // hub radius
  drawImpeller(px, py, rOut, rHub, f.pump, f.badConn, f.nowMs);

  u8g2.setFont(u8g2_font_5x8_tf);
  const char *st;
  unsigned long rm = pumpRunMin(f);
  char pl[10];
  switch (f.pump) {
    case RUNNING:             snprintf(pl, sizeof(pl), "RUN%lum", rm); st = pl; break;
    case OVERHEAT_PROTECTION: st = "HEAT!"; break;
    default:                  st = "STOP"; break;
  }
  // label below the impeller, centered-ish under it
  u8g2.drawStr(px - 12, 50, st);

  if (f.badConn) {
    u8g2.setFont(u8g2_font_5x8_tf);
    if ((f.nowMs / 300) % 2 == 0) u8g2.drawStr(2, 8, "!LINK LOST!");
  }
}

static void drawSystemPage(const Frame &f) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 8, "SYSTEM");
  u8g2.drawStr(92, 8, f.hhmm);
  u8g2.drawHLine(0, 11, 128);

  u8g2.setFont(u8g2_font_6x10_tf);
  char line[26];

  // WiFi + animated signal bars
  snprintf(line, sizeof(line), "WiFi %ld", f.rssi);
  u8g2.drawStr(0, 24, line);
  // bars: map RSSI (-90..-50) to 0..4
  int bars = 0;
  if (f.wifiUp) {
    if (f.rssi >= -55) bars = 4;
    else if (f.rssi >= -65) bars = 3;
    else if (f.rssi >= -75) bars = 2;
    else if (f.rssi >= -85) bars = 1;
    else bars = 0;
  }
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 3;
    int bx = 90 + i * 7;
    if (i < bars) u8g2.drawBox(bx, 24 - bh, 5, bh);
    else          u8g2.drawFrame(bx, 24 - bh, 5, bh);
  }

  u8g2.drawStr(0, 38, f.mqttUp ? "MQTT connected" : "MQTT  ...");

  unsigned long up = f.uptimeS;
  snprintf(line, sizeof(line), "Up   %luh %02lum", up / 3600, (up % 3600) / 60);
  u8g2.drawStr(0, 52, line);

  u8g2.setFont(u8g2_font_5x8_tf);
  snprintf(line, sizeof(line), "fill %.0f (prefill %d-%dh)",
           f.deficient, cfg::PREFILL_HOUR_START, cfg::PREFILL_HOUR_END);
  u8g2.drawStr(0, 63, line);
}

static void drawAnimated(const Frame &f) {
  static unsigned long lastPageMs = 0;
  static uint8_t page = 0;
  if (lastPageMs == 0) lastPageMs = f.nowMs;
  if (f.nowMs - lastPageMs >= cfg::DISPLAY_PAGE_ROTATE_MS) {
    lastPageMs = f.nowMs;
    page = (page + 1) % 2;
  }
  if (page == 0) drawTankPage(f);
  else           drawSystemPage(f);
}
#endif

// =====================================================================
// Style C — Retro Terminal
// =====================================================================
#if DISPLAY_STYLE == 3
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

  // pump — show time spent in the CURRENT state after every state name.
  char age[10];
  pumpStateAge(f, age, sizeof(age));
  switch (f.pump) {
    case RUNNING:
      snprintf(line, sizeof(line), "PUMP : RUNNING %s", age); break;
    case OVERHEAT_PROTECTION:
      snprintf(line, sizeof(line), "PUMP : OVERHEAT %s", age); break;
    default:
      snprintf(line, sizeof(line), "PUMP : STOPPED %s", age); break;
  }
  // blink the OVERHEAT line
  if (f.pump != OVERHEAT_PROTECTION || (f.nowMs / 400) % 2 == 0)
    u8g2.drawStr(0, 35, line);

  // LINK row alternates (same period as SET/REC): WiFi SSID+signal, then MQTT.
  bool showPhaseB = (f.nowMs / cfg::DISPLAY_THR_TOGGLE_MS) % 2 == 1;
  if (!showPhaseB) {
    if (f.wifiUp) snprintf(line, sizeof(line), "WIFI : %s %ld", f.ssid, f.rssi);
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

  // uptime + blinking cursor
  unsigned long up = f.uptimeS;
  snprintf(line, sizeof(line), "UP   : %02lu:%02lu:%02lu",
           up / 3600, (up % 3600) / 60, up % 60);
  u8g2.drawStr(0, 62, line);
  if ((f.nowMs / 500) % 2 == 0) u8g2.drawStr(118, 62, "_");
}
#endif

// =====================================================================
// Public API
// =====================================================================
void displayInit() {
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

#if DISPLAY_STYLE == 1
  drawDashboard(f);
#elif DISPLAY_STYLE == 2
  drawAnimated(f);
#elif DISPLAY_STYLE == 3
  drawRetro(f);
#else
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, "DISPLAY_STYLE?");
#endif

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
