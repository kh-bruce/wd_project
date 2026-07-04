// =====================================================================
// fakes/Arduino.h — host-side replacement for the Arduino core, just
// enough to compile & run pump_control / failsafe / commands on a Mac.
//
// The clock is CONTROLLABLE: tests call mockAdvance(ms) to move time and
// mockReset() to zero everything between tests. millis() reads the mock
// clock, so all millis()-based logic (failsafe window, overheat trip,
// max-on cap) is exercised deterministically with no real waiting.
// =====================================================================
#ifndef FAKE_ARDUINO_H
#define FAKE_ARDUINO_H

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <map>

// ---- Pin levels / modes ----
#define HIGH 1
#define LOW  0
#define OUTPUT 1
#define INPUT  0
#define INPUT_PULLUP 2

typedef uint8_t byte;

// ---- Controllable clock ----
extern unsigned long g_mockMillis;
inline unsigned long millis() { return g_mockMillis; }
inline void mockAdvance(unsigned long ms) { g_mockMillis += ms; }

// ---- Fake GPIO (observable from tests) ----
extern std::map<uint8_t, int> g_pinState;
inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t pin, int val) { g_pinState[pin] = val; }
inline int  digitalRead(uint8_t pin) {
  auto it = g_pinState.find(pin);
  return it == g_pinState.end() ? LOW : it->second;
}
inline int  analogRead(uint8_t) { return 0; }

inline void delay(unsigned long ms) { mockAdvance(ms); }
inline void yield() {}

// ---- Minimal Arduino String ----
class String {
public:
  std::string s;
  String() {}
  String(const char* c) : s(c?c:"") {}
  String(const std::string& x) : s(x) {}
  String(int v)          { s = std::to_string(v); }
  String(unsigned long v){ s = std::to_string(v); }
  String(long v)         { s = std::to_string(v); }
  String(float v, int dec=2) { char buf[32]; snprintf(buf,sizeof buf,"%.*f",dec,v); s=buf; }
  String(double v, int dec=2){ char buf[32]; snprintf(buf,sizeof buf,"%.*f",dec,(float)v); s=buf; }
  String operator+(const String& o) const { return String(s + o.s); }
  String operator+(const char* o)  const { return String(s + (o?o:"")); }
  String& operator+=(const String& o){ s += o.s; return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o)   const { return s == (o?o:""); }
  const char* c_str() const { return s.c_str(); }
  float toFloat() const { return s.empty()?0.f:(float)atof(s.c_str()); }
  unsigned int length() const { return (unsigned)s.size(); }
};
inline String operator+(const char* a, const String& b){ return String(std::string(a?a:"") + b.s); }

// ---- Serial ----
// The firmware logs heavily to Serial. During tests that's noise that buries
// the test narration, so it's muted unless g_verbose is set (VERBOSE=1). Muted
// lines are dimmed and prefixed so you can still tell firmware spoke.
extern bool g_verbose;
// All firmware output is dimmed and indented under an "fw|" gutter. We track
// whether we're at the start of a line so the gutter is printed once per line,
// not once per print()/printf() fragment (avoids the doubled-prefix mess).
extern bool g_fwLineStart;
inline void fwEmit(const char* s) {
  if (!g_verbose) return;
  for (const char* p = s; *p; ++p) {
    if (g_fwLineStart) { ::printf("\033[2m        fw| "); g_fwLineStart = false; }
    if (*p == '\n') { ::printf("\033[0m\n"); g_fwLineStart = true; }
    else ::printf("%c", *p);
  }
}
struct FakeSerial {
  void begin(unsigned long) {}
  void println(const String& x){ fwEmit(x.c_str()); fwEmit("\n"); }
  void println(const char* x) { fwEmit(x?x:""); fwEmit("\n"); }
  void println() { fwEmit("\n"); }
  void print(const String& x)  { fwEmit(x.c_str()); }
  void print(const char* x)   { fwEmit(x?x:""); }
  void printf(const char* fmt, ...) {
    if (!g_verbose) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fwEmit(buf);
  }
};
extern FakeSerial Serial;

// ---- ESP (heap stats used by heapCheck; harmless stubs) ----
struct FakeESP {
  uint32_t getFreeHeap()    { return 200000; }
  uint32_t getMinFreeHeap() { return 150000; }
};
extern FakeESP ESP;

// ---- FreeRTOS portMUX (single-threaded host: no-ops) ----
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}

// ---- Test harness controls ----
void mockReset();   // zero clock + pins (defined in test_main)

#endif // FAKE_ARDUINO_H
