#include "door_control.h"
#include "config.h"
#include "logging.h"

// Momentary pulse relay: energize the chosen pin, then reset all after
// RELAY_PULSE_MS. One pulse at a time — commands arriving during a pulse
// (or its RELAY_GAP_MS cool-down) are ignored, so a rapid burst of clicks
// can't truncate the active pulse: first click wins. No position feedback.
// Loop-thread only.
static unsigned long pulseStartMs = 0;
static unsigned long pulseEndMs   = 0;
static bool          pulseActive  = false;

void doorInit() {
  pinMode(cfg::PIN_DOOR_UP,   OUTPUT); digitalWrite(cfg::PIN_DOOR_UP,   cfg::RELAY_OFF);
  pinMode(cfg::PIN_DOOR_DOWN, OUTPUT); digitalWrite(cfg::PIN_DOOR_DOWN, cfg::RELAY_OFF);
  pinMode(cfg::PIN_DOOR_STOP, OUTPUT); digitalWrite(cfg::PIN_DOOR_STOP, cfg::RELAY_OFF);
  pulseActive  = false;
  pulseStartMs = 0;
  pulseEndMs   = 0;
}

static void resetRelays() {
  digitalWrite(cfg::PIN_DOOR_UP,   cfg::RELAY_OFF);
  digitalWrite(cfg::PIN_DOOR_DOWN, cfg::RELAY_OFF);
  digitalWrite(cfg::PIN_DOOR_STOP, cfg::RELAY_OFF);
}

void doorCommand(const String &dir) {
  uint8_t pin;
  if      (dir == "up")   pin = cfg::PIN_DOOR_UP;
  else if (dir == "down") pin = cfg::PIN_DOOR_DOWN;
  else if (dir == "stop") pin = cfg::PIN_DOOR_STOP;
  else { logWarning("Door command rejected: " + dir); return; }

  if (pulseActive || millis() - pulseEndMs < cfg::RELAY_GAP_MS) {
    logWarning("Door command ignored (pulse busy): " + dir);
    return;
  }

  logPhysical("Frontdoor relay: " + dir);
  resetRelays();                 // ensure only one energized
  digitalWrite(pin, cfg::RELAY_ON);
  pulseStartMs = millis();
  pulseActive  = true;
}

void doorTick() {
  if (pulseActive && millis() - pulseStartMs >= cfg::RELAY_PULSE_MS) {
    resetRelays();
    pulseActive = false;
    pulseEndMs  = millis();
    logPhysical("Frontdoor relay reset (all OFF)");
  }
}
