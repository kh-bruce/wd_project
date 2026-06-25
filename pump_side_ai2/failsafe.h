// =====================================================================
// failsafe.h — bad-connection failsafe (SAFETY CRITICAL).
//
// If no fresh water level arrives within BAD_CONN_DELAY_MS (tower offline,
// broker down, or WiFi lost), force-stop the pump and latch bad-conn mode.
//
// Rewritten from the old timer_bad_connection cancel/re-arm dance to a plain
// millis() timestamp comparison checked once per loop() — no arduino-timer,
// no cancel races, no cannotcanceltimerrrrrr. lastWaterMs (in commands.h) is
// the single source of truth, updated by recordWater() from any thread.
// =====================================================================
#ifndef FAILSAFE_H
#define FAILSAFE_H

#include <Arduino.h>

extern bool bad_conn_mode;
extern int  bad_conn_count;

void failsafeInit();          // arm at boot (treat boot as "last water = now")
void serviceFailsafe();       // call every loop(): trip or recover based on lastWaterMs

#endif // FAILSAFE_H
