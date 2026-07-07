#include "pump_control.h"
#include "config.h"
#include "logging.h"
#include "ntp_time.h"
#include "failsafe.h"
#include "commands.h"   // latestWater / latestWaterValid (water ingress)
#include <Preferences.h>
#include <arduino-timer.h>

// ---- State (loop thread only) ----
PumpStatus    pump_status = STOPPED;
unsigned long pumpStatusChangedMs = 0;
unsigned long pumpOnSinceMs = 0;

float MAX_WATER_LEVEL = cfg::DEFAULT_MAX_LEVEL;
float MIN_WATER_LEVEL = cfg::DEFAULT_MIN_LEVEL;
float DEFICIENT_WATER_LEVEL = cfg::DEFAULT_DEFICIENT_LEVEL;

String        lastCommand = "none";
unsigned long lastCommandMs = 0;

static Preferences prefs;

// Timers (ticked from pumpTick(), i.e. loop thread)
static auto timer_overheatTrip    = timer_create_default(); // run -> overheat after OVERHEAT_TRIP_MS
static auto timer_overheatRecover = timer_create_default(); // overheat -> recover after OVERHEAT_RECOVER_MS
static auto timer_blink           = timer_create_default();

static bool blinkState = true;

// Consecutive overheat rounds within the current fill episode (written on the
// loop thread only; OLED task reads it — aligned int, atomic on ESP32).
// Drives the resume cap in overheatRecover_cb.
int overheatRounds = 0;

const char* pumpStatusStr() {
  switch (pump_status) {
    case RUNNING:             return "RUNNING";
    case STOPPED:             return "STOPPED";
    case OVERHEAT_PROTECTION: return "OVERHEAT_PROTECTION";
    default:                  return "Unknown";
  }
}

void record_command(const String &name) {
  lastCommand = name;
  lastCommandMs = millis();
  logVerbose("Command: " + name);
}

// ---- Blink ----
static bool blink_cb(void *) {
  blinkState = !blinkState;
  digitalWrite(cfg::PIN_STATUS_LED, blinkState ? HIGH : LOW);
  return true;
}
void setBlinkInterval(int intervalMs) {
  timer_blink.cancel();
  timer_blink.every(intervalMs, blink_cb);
}
void blinkTick() { timer_blink.tick(); }

void applySetMax(float f) {
  if (f > MIN_WATER_LEVEL) {
    MAX_WATER_LEVEL = f;
    prefs.putFloat("max", MAX_WATER_LEVEL); // persist to NVS
    logWarning("Max level set to " + String(MAX_WATER_LEVEL, 1));
  } else {
    logWarning("Max level rejected (must > min " + String(MIN_WATER_LEVEL, 1) + ")");
  }
}
void applySetMin(float f) {
  if (f < MAX_WATER_LEVEL) {
    MIN_WATER_LEVEL = f;
    prefs.putFloat("min", MIN_WATER_LEVEL);
    logWarning("Min level set to " + String(MIN_WATER_LEVEL, 1));
  } else {
    logWarning("Min level rejected (must < max " + String(MAX_WATER_LEVEL, 1) + ")");
  }
}
// Deficient = prefill target. User-set (no longer auto-derived). Must sit
// within [min, max] so prefill behaves sensibly.
void applySetDeficient(float f) {
  if (f >= MIN_WATER_LEVEL && f <= MAX_WATER_LEVEL) {
    DEFICIENT_WATER_LEVEL = f;
    prefs.putFloat("deficient", DEFICIENT_WATER_LEVEL);
    logWarning("Deficient level set to " + String(DEFICIENT_WATER_LEVEL, 1));
  } else {
    logWarning("Deficient level rejected (must be within min " +
               String(MIN_WATER_LEVEL, 1) + "..max " + String(MAX_WATER_LEVEL, 1) + ")");
  }
}

// ---- Pump primitives (atomic on the loop thread) ----
static bool overheatTrip_cb(void *) {
  logWarning("Pump overheat protection triggered");
  request_pump_to(OVERHEAT_PROTECTION);
  return false; // one-shot
}
static bool overheatRecover_cb(void *);

void pump_run() {
  if (digitalRead(cfg::PIN_PUMP_RELAY) == cfg::RELAY_ON) return; // already on
  timer_overheatTrip.cancel();
  digitalWrite(cfg::PIN_PUMP_RELAY, cfg::RELAY_ON);
  pump_status = RUNNING;
  pumpStatusChangedMs = millis();
  pumpOnSinceMs = millis();
  logPhysical("Pump relay ON");
  timer_overheatTrip.in(cfg::OVERHEAT_TRIP_MS, overheatTrip_cb);
}

void pump_stop() {
  // During overheat cooldown the relay is already OFF and timer_overheatRecover
  // is counting down the thermal protection. A force-stop (failsafe trip or
  // manual stop) must NOT cancel that cooldown or flip the state to STOPPED,
  // or the motor could be restarted hot. Leave OVERHEAT_PROTECTION intact.
  if (pump_status == OVERHEAT_PROTECTION) return;
  if (digitalRead(cfg::PIN_PUMP_RELAY) == cfg::RELAY_OFF) return; // already off
  timer_overheatTrip.cancel();
  digitalWrite(cfg::PIN_PUMP_RELAY, cfg::RELAY_OFF);
  pump_status = STOPPED;
  pumpStatusChangedMs = millis();
  pumpOnSinceMs = 0;
  overheatRounds = 0; // a real stop ends the fill episode
  logPhysical("Pump relay OFF");
}

void pump_overheat_protect() {
  timer_overheatRecover.cancel();
  setBlinkInterval(cfg::BLINK_OVERHEAT_MS);
  digitalWrite(cfg::PIN_PUMP_RELAY, cfg::RELAY_OFF);
  pump_status = OVERHEAT_PROTECTION;
  pumpStatusChangedMs = millis();
  pumpOnSinceMs = 0;
  logPhysical("Pump forced OFF (overheat protection)");
  timer_overheatRecover.in(cfg::OVERHEAT_RECOVER_MS, overheatRecover_cb);
}

static bool overheatRecover_cb(void *) {
  extern bool bad_conn_mode;
  // Don't override the fast bad-conn blink if the failsafe is latched.
  if (!bad_conn_mode) setBlinkInterval(cfg::BLINK_NORMAL_MS);
  logVerbose("Recovered from overheat protection");
  pump_status = STOPPED;
  pumpStatusChangedMs = millis();
  pumpOnSinceMs = 0;
  overheatRounds++;
  // Resume the interrupted fill: restart whenever water is still below max —
  // but cap the consecutive rounds so a sensor stuck at a valid mid-range
  // value can't cycle the pump forever. Past the cap, fall back to the
  // min-level check: dry-tank protection is never capped.
  if (!isBadTime()) {
    if (overheatRounds < cfg::OVERHEAT_MAX_ROUNDS) {
      check_water_level(MAX_WATER_LEVEL);
    } else {
      logWarning("Overheat resume cap reached - min-level check only");
      check_water_level(MIN_WATER_LEVEL);
    }
  }
  // No resume happened (past max / capped / blocked / quiet hours / no data):
  // the fill episode is over.
  if (pump_status != RUNNING) overheatRounds = 0;
  return false; // one-shot
}

void manual_pump_start() {
  logPhysical("Manual pump started");
  request_pump_to(RUNNING);
}

void manual_pump_stop() {
  logWarning("Manual pump stop requested");
  request_pump_to(STOPPED);
}

void request_pump_to(PumpStatus status) {
  if (bad_conn_mode) return; // failsafe latched: refuse to run
  switch (status) {
    case RUNNING:
      if (pump_status == STOPPED) pump_run();
      // ignore if already RUNNING or in OVERHEAT_PROTECTION
      break;
    case STOPPED:
      pump_stop();
      break;
    case OVERHEAT_PROTECTION:
      if (pump_status != OVERHEAT_PROTECTION) pump_overheat_protect();
      break;
    default: break;
  }
}

void check_water_level(float desiredMinWaterLevel) {
  // latestWater is parsed at ingress. Treat a 0 (or invalid) reading as "no
  // usable data" and take NO pump action — matching the original semantics
  // (message.toFloat() != 0). Liveness/failsafe is handled separately by
  // recordWater(), which resets on ANY message including 0. So a 0 reading
  // still proves the link is alive but never drives the pump.
  float num = latestWater;
  if (!latestWaterValid || num == 0.0f) {
    Serial.println("[check_water_level] no usable water data (0/invalid) — no action");
    return;
  }
  Serial.printf("[check_water_level] %.2f (min %.1f max %.1f)\n",
                num, desiredMinWaterLevel, MAX_WATER_LEVEL);
  if (num < desiredMinWaterLevel) {
    logWarning("Water BELOW threshold (" + String(num, 1) + " < " + String(desiredMinWaterLevel, 1) + ")");
    request_pump_to(RUNNING);
  } else if (num > MAX_WATER_LEVEL) {
    logVerbose("Water OVER max (" + String(num, 1) + " > " + String(MAX_WATER_LEVEL, 1) + ")");
    request_pump_to(STOPPED);
  }
}

void pumpInit() {
  pinMode(cfg::PIN_STATUS_LED, OUTPUT);
  digitalWrite(cfg::PIN_STATUS_LED, HIGH);
  pinMode(cfg::PIN_PUMP_RELAY, OUTPUT);
  digitalWrite(cfg::PIN_PUMP_RELAY, cfg::RELAY_OFF);

  // Boot state is STOPPED; stamp the change time so "time in state" counts from
  // boot. Without this, pump_stop()'s "already off" early-return means the
  // initial STOPPED state never stamps it and time-in-state stays frozen at 0.
  pumpStatusChangedMs = millis();

  // Load thresholds from NVS (fall back to defaults).
  prefs.begin("wdpump", false);
  MAX_WATER_LEVEL = prefs.getFloat("max", cfg::DEFAULT_MAX_LEVEL);
  MIN_WATER_LEVEL = prefs.getFloat("min", cfg::DEFAULT_MIN_LEVEL);
  DEFICIENT_WATER_LEVEL = prefs.getFloat("deficient", cfg::DEFAULT_DEFICIENT_LEVEL);
  Serial.printf("Thresholds loaded: max=%.1f min=%.1f deficient=%.1f\n",
                MAX_WATER_LEVEL, MIN_WATER_LEVEL, DEFICIENT_WATER_LEVEL);

  setBlinkInterval(cfg::BLINK_NORMAL_MS);
}

void pumpTick() {
  timer_overheatTrip.tick();
  timer_overheatRecover.tick();

  // Absolute max-on hard cap (defense in depth, independent of overheat timer).
  if (pump_status == RUNNING && pumpOnSinceMs != 0 &&
      millis() - pumpOnSinceMs > cfg::PUMP_MAX_ON_MS) {
    logError("Pump exceeded absolute max-on time — forcing OFF");
    pump_stop();
  }
}
