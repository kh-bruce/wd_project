// =====================================================================
// pump_control.h — pump state machine, overheat protection, thresholds.
// ALL functions here run on the loop thread only (invoked from setup(),
// loop(), command-queue drain, or timer ticks serviced in loop()).
// =====================================================================
#ifndef PUMP_CONTROL_H
#define PUMP_CONTROL_H

#include <Arduino.h>
#include "config.h"

// ---- Shared pump state (owned by loop thread) ----
extern PumpStatus    pump_status;
extern unsigned long pumpStatusChangedMs; // millis() of last state change
extern unsigned long pumpOnSinceMs;       // millis() when pump last turned ON (0 if off)
extern int           overheatRounds;      // completed overheat cooldowns this fill (display "+N")

// ---- Thresholds (loaded from / persisted to NVS) ----
extern float MAX_WATER_LEVEL;
extern float MIN_WATER_LEVEL;
extern float DEFICIENT_WATER_LEVEL;

// ---- Last command (for status/diagnostics) ----
extern String        lastCommand;
extern unsigned long lastCommandMs;

void pumpInit();              // pinMode, load thresholds from NVS, idle pump
void pumpTick();              // call every loop(): overheat/recover/maxon timers

// Core control (loop thread only)
void request_pump_to(PumpStatus status);
void pump_run();
void pump_stop();
void pump_overheat_protect();
void manual_pump_start();
void manual_pump_stop();

// Water-level evaluation against thresholds (loop thread)
void check_water_level(float desiredMinWaterLevel);

// Threshold setters (validate + persist to NVS)
void applySetMax(float f);
void applySetMin(float f);
void applySetDeficient(float f);   // prefill target, user-set, within [min,max]

// Blink LED
void setBlinkInterval(int intervalMs);
void blinkTick();

const char* pumpStatusStr();
void record_command(const String &name);

#endif // PUMP_CONTROL_H
