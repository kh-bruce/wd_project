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
  CMD_PUMP_RUN,    // manual run (until max/failsafe/overheat/max-on stops it)
  CMD_PUMP_STOP,   // manual stop
  CMD_SET_MAX,       // arg = new max level
  CMD_SET_MIN,       // arg = new min level
  CMD_SET_DEFICIENT  // arg = new deficient (prefill target) level
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

// Tower session max/min, for DISPLAY ONLY (the OLED). These come from
// retain=true topics, so they must NOT touch lastWaterMs / the failsafe clock
// (a retained replay is not proof of liveness). Guarded by cmdMux.
extern volatile float        towerAvgMax;
extern volatile bool         towerAvgMaxValid;
extern volatile float        towerAvgMin;
extern volatile bool         towerAvgMinValid;

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

// Read the latest water level + valid flag as one consistent snapshot. The
// pair is written together under cmdMux in recordWater(), so readers (e.g. the
// OLED on the loop thread) must take the same lock to avoid a torn read of a
// new "valid" against a stale value. Mirrors the cmdMux read idiom in
// serviceWaterLevel(). Copies only — no work inside the critical section.
inline void waterSnapshot(float &valueOut, bool &validOut) {
  portENTER_CRITICAL(&cmdMux);
  valueOut = latestWater;
  validOut = latestWaterValid;
  portEXIT_CRITICAL(&cmdMux);
}

// Record tower session max/min (DISPLAY ONLY). Deliberately does NOT update
// lastWaterMs — these arrive retained and must not feed the liveness failsafe.
inline void recordTowerMax(float value, bool valid) {
  portENTER_CRITICAL(&cmdMux);
  towerAvgMax = value;
  towerAvgMaxValid = valid;
  portEXIT_CRITICAL(&cmdMux);
}
inline void recordTowerMin(float value, bool valid) {
  portENTER_CRITICAL(&cmdMux);
  towerAvgMin = value;
  towerAvgMinValid = valid;
  portEXIT_CRITICAL(&cmdMux);
}

// Read the tower session max/min as one snapshot (loop thread / OLED).
inline void towerStatsSnapshot(float &maxOut, bool &maxValid,
                               float &minOut, bool &minValid) {
  portENTER_CRITICAL(&cmdMux);
  maxOut = towerAvgMax;  maxValid = towerAvgMaxValid;
  minOut = towerAvgMin;  minValid = towerAvgMinValid;
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
