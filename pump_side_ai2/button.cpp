#include "button.h"
#include <Arduino.h>
#include "config.h"
#include "commands.h"      // enqueueCommand, CMD_PUMP_RUN/STOP
#include "pump_control.h"  // pump_status (to decide toggle direction)
#include "display.h"       // displayCycleStyle
#include "logging.h"

// GPIO0 is active-LOW (INPUT_PULLUP): pressed == LOW.
static bool          gStableDown   = false;  // debounced button state
static bool          gRawDown      = false;  // last raw read
static unsigned long gRawChangeMs  = 0;      // when the raw read last changed
static unsigned long gPressStartMs = 0;      // when the debounced press began
static bool          gLongFired    = false;  // long-press action already fired this press
static unsigned long gBootMs       = 0;      // for the boot-ignore window

void buttonInit() {
  pinMode(cfg::PIN_BUTTON, INPUT_PULLUP);
  gBootMs = millis();
  gRawDown = (digitalRead(cfg::PIN_BUTTON) == LOW);
  gStableDown = gRawDown;
  gRawChangeMs = gBootMs;
}

// Long press fires the pump toggle the instant the hold crosses the threshold
// (not on release), so the user gets feedback while still holding. Short press
// fires on release when the hold never reached the long-press threshold.
void buttonTick() {
  unsigned long now = millis();

  // GPIO0 is a strapping pin: ignore it right after boot so a button still held
  // from reset can't be read as a UI press.
  if (now - gBootMs < cfg::BUTTON_BOOT_IGNORE_MS) return;

  bool raw = (digitalRead(cfg::PIN_BUTTON) == LOW);

  // Debounce: a raw change must stay stable for BUTTON_DEBOUNCE_MS before we
  // treat it as a real edge.
  if (raw != gRawDown) {
    gRawDown = raw;
    gRawChangeMs = now;
  }
  if (now - gRawChangeMs < cfg::BUTTON_DEBOUNCE_MS) return; // not settled yet
  if (raw == gStableDown) {
    // Steady state. If it's a steady press, check for the long-press threshold.
    if (gStableDown && !gLongFired &&
        now - gPressStartMs >= cfg::BUTTON_LONGPRESS_MS) {
      gLongFired = true;
      // Toggle: run if stopped, otherwise stop. Enqueue only (loop drains it).
      if (pump_status == STOPPED) enqueueCommand(CMD_PUMP_RUN);
      else                        enqueueCommand(CMD_PUMP_STOP);
      logPhysical("Button long-press: toggle pump");
    }
    return;
  }

  // Debounced edge.
  gStableDown = raw;
  if (gStableDown) {
    // Press begins.
    gPressStartMs = now;
    gLongFired = false;
  } else {
    // Release. A short press is one that never reached the long-press action.
    if (!gLongFired) {
      displayCycleStyle();
      logPhysical("Button short-press: cycle OLED style");
    }
  }
}
