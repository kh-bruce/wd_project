// fakes/Preferences.h — in-memory NVS replacement.
// Persists within a process; mockReset() clears it so each test starts clean.
#ifndef FAKE_PREFERENCES_H
#define FAKE_PREFERENCES_H

#include "Arduino.h"
#include <map>

extern std::map<std::string, float> g_nvsFloat;

class Preferences {
public:
  bool begin(const char*, bool) { return true; }
  void end() {}
  float getFloat(const char* key, float def) {
    auto it = g_nvsFloat.find(key);
    return it == g_nvsFloat.end() ? def : it->second;
  }
  size_t putFloat(const char* key, float val) { g_nvsFloat[key] = val; return 4; }
};

#endif // FAKE_PREFERENCES_H
