// =====================================================================
// logging.h — lightweight logging to Serial (+ optional MQTT later).
// The old 256-entry RAM ring buffer + /logs.json + SSE log viewer are
// removed; Home Assistant + Serial cover diagnostics now.
// =====================================================================
#ifndef LOGGING_H
#define LOGGING_H

#include <Arduino.h>

inline void logPhysical(const String &s) { Serial.println("[PHYSICAL] " + s); }
inline void logError   (const String &s) { Serial.println("[ERROR] "    + s); }
inline void logWarning (const String &s) { Serial.println("[WARN] "     + s); }
inline void logVerbose (const String &s) { Serial.println("[VERBOSE] "  + s); }

#endif // LOGGING_H
