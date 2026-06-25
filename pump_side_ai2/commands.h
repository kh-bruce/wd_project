// =====================================================================
// commands.h — thread-safe command queue.
//
// Web (AsyncTCP task) and MQTT callbacks MUST NOT touch hardware, timers,
// or the pump state machine directly. They only enqueue a Command here.
// loop() drains the queue on the loop task, which is the SOLE owner of
// all GPIO / pump_status / timers. This makes the control path provably
// single-threaded and removes the data races on arduino-timer.
// =====================================================================
#ifndef COMMANDS_H
#define COMMANDS_H

#include <Arduino.h>

enum CmdType {
  CMD_DOOR_UP,
  CMD_DOOR_DOWN,
  CMD_DOOR_STOP,
  CMD_PUMP_RUN,    // manual 5-min run
  CMD_PUMP_STOP,   // manual stop
  CMD_SET_MAX,     // arg = new max level
  CMD_SET_MIN      // arg = new min level
};

struct Command {
  CmdType type;
  float   arg;
};

// Small ring buffer guarded by a portMUX critical section.
static const int CMD_QUEUE_SIZE = 16;
extern Command cmdQueue[CMD_QUEUE_SIZE];
extern volatile int cmdHead;
extern volatile int cmdTail;
extern portMUX_TYPE cmdMux;

// Water level ingress (set by MQTT/web handlers, read by loop). Guarded by
// the same mux. lastWaterMs is the SINGLE source of truth for the failsafe.
extern volatile float        latestWater;
extern volatile bool         latestWaterValid;
extern volatile unsigned long lastWaterMs;

// Called from any thread (web/MQTT). Non-blocking; drops if full.
inline void enqueueCommand(CmdType type, float arg = 0) {
  portENTER_CRITICAL(&cmdMux);
  int next = (cmdHead + 1) % CMD_QUEUE_SIZE;
  if (next != cmdTail) {           // not full
    cmdQueue[cmdHead].type = type;
    cmdQueue[cmdHead].arg  = arg;
    cmdHead = next;
  }
  portEXIT_CRITICAL(&cmdMux);
}

// Record a freshly received water level from any thread. The raw text is
// parsed by the caller (single parse at ingress). Resets the failsafe clock
// on ANY received value (receiving a message == proof of liveness).
inline void recordWater(float value, bool valid) {
  portENTER_CRITICAL(&cmdMux);
  latestWater = value;
  latestWaterValid = valid;
  lastWaterMs = millis();
  portEXIT_CRITICAL(&cmdMux);
}

// Pop one command (loop thread). Returns false if empty.
inline bool dequeueCommand(Command &out) {
  bool got = false;
  portENTER_CRITICAL(&cmdMux);
  if (cmdTail != cmdHead) {
    out = cmdQueue[cmdTail];
    cmdTail = (cmdTail + 1) % CMD_QUEUE_SIZE;
    got = true;
  }
  portEXIT_CRITICAL(&cmdMux);
  return got;
}

#endif // COMMANDS_H
