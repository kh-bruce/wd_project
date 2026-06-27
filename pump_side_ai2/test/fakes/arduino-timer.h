// =====================================================================
// fakes/arduino-timer.h — host replica of contrem/arduino-timer, enough
// for pump_control.cpp. Driven by the mock millis() clock, so .tick()
// only fires callbacks once mockAdvance() has pushed time past their due.
//
// Real-library semantics we reproduce:
//   .in(ms, cb)     one-shot after ms
//   .every(ms, cb)  repeating; reschedules while cb returns true
//   .cancel()       cancels ALL tasks on this timer (matches how
//                   pump_control uses one timer per concern)
//   .tick()         fire all due tasks
// A callback returning false is treated as one-shot (not rescheduled).
// =====================================================================
#ifndef FAKE_ARDUINO_TIMER_H
#define FAKE_ARDUINO_TIMER_H

#include "Arduino.h"
#include <vector>

template <int MAXT = 16>
class Timer {
public:
  typedef bool (*handler_t)(void*);

  struct Task { bool active; unsigned long due; unsigned long interval; bool repeat; handler_t cb; };

  std::vector<Task> tasks;

  void in(unsigned long ms, handler_t cb) {
    tasks.push_back({true, millis() + ms, ms, false, cb});
  }
  void every(unsigned long ms, handler_t cb) {
    tasks.push_back({true, millis() + ms, ms, true, cb});
  }
  // Real API cancel() clears the whole timer object's tasks.
  void cancel() { tasks.clear(); }

  void tick() {
    unsigned long now = millis();
    // Fire any due tasks. Reschedule repeating ones; drop one-shots and any
    // task whose callback returns false.
    for (size_t i = 0; i < tasks.size(); ) {
      Task &t = tasks[i];
      if (t.active && now >= t.due) {
        bool keep = t.cb(nullptr);
        if (t.repeat && keep) {
          t.due += t.interval;
          ++i;
        } else {
          tasks.erase(tasks.begin() + i);
        }
      } else {
        ++i;
      }
    }
  }
};

inline Timer<> timer_create_default() { return Timer<>(); }

#endif // FAKE_ARDUINO_TIMER_H
