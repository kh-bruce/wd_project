// =====================================================================
// door_control.h — garage door relay (momentary pulse, no feedback).
// Pulses the up/down/stop relay for RELAY_PULSE_MS then resets it.
// One pulse at a time: commands during a pulse or its RELAY_GAP_MS
// cool-down are ignored (first click wins).
// Driven ONLY from the loop thread (via the command queue).
// =====================================================================
#ifndef DOOR_CONTROL_H
#define DOOR_CONTROL_H

#include <Arduino.h>

void doorInit();                 // pinMode + idle the relays
void doorCommand(const String &dir); // "up" | "down" | "stop"
void doorTick();                 // call every loop(): reset pulse when elapsed

#endif // DOOR_CONTROL_H
