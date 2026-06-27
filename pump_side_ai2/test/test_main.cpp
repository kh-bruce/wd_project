// =====================================================================
// test_main.cpp — host-side unit tests for pump_side_ai2 safety logic.
//
// Compiles the REAL commands.cpp / pump_control.cpp / failsafe.cpp against
// the fake Arduino layer in fakes/. ntp_time is stubbed here (its real impl
// drags in WiFi/NTPClient); the only time predicates the pump logic calls
// are isBadTime()/isTimeInRange(), which we control via g_stubBadTime etc.
//
// Build & run:  test/run.sh   (or see that script for the clang++ line)
// =====================================================================
#include "Arduino.h"
#include "../config.h"
#include "../commands.h"
#include "../pump_control.h"
#include "../failsafe.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// ---- Globals the fakes declare extern ----
unsigned long g_mockMillis = 0;
std::map<uint8_t, int> g_pinState;
std::map<std::string, float> g_nvsFloat;
FakeSerial Serial;
FakeESP ESP;
bool g_fwLineStart = true;   // firmware-log line-gutter tracking (see Arduino.h)

// ---- ntp_time stub (control the time-window predicates) ----
bool g_stubBadTime    = false;  // quiet hours?
bool g_stubTimeInRange = false; // prefill window?
bool isBadTime()     { return g_stubBadTime; }
bool isTimeInRange() { return g_stubTimeInRange; }
// other ntp_time symbols are not referenced by the units under test.

// ---- mockReset: zero everything so tests are independent ----
void mockReset() {
  // Start the clock well past 0 so pumpOnSinceMs/lastWaterMs are non-zero and
  // millis()-based guards (which special-case 0) behave like they do in flight.
  g_mockMillis = 100000;
  g_pinState.clear();
  g_nvsFloat.clear();
  g_stubBadTime = false;
  g_stubTimeInRange = false;
  // reset command queue + water ingress
  cmdHead = cmdTail = 0;
  latestWater = 0.0f; latestWaterValid = false; lastWaterMs = 0;
  // Force a clean pump module state. The firmware never re-runs setup(), so
  // pumpInit() intentionally does NOT reset pump_status or cancel the static
  // overheat/manual timers inside pump_control.cpp. For test isolation we
  // drive the public API into a known-idle state, then flush any armed
  // one-shot timers by ticking once far in the future and resetting the clock.
  bad_conn_mode = false;
  pump_status = RUNNING;                 // so pump_stop() won't early-return
  g_pinState[cfg::PIN_PUMP_RELAY] = cfg::RELAY_ON;
  pump_stop();                           // cancels overheat-trip & manual timers
  unsigned long save = g_mockMillis;
  g_mockMillis += 60UL * 60 * 1000;      // +1h: fire any stale one-shot timers
  pumpTick();                            // flush overheat-recover etc.
  g_mockMillis = save;

  pumpInit();      // pins idle, thresholds default (NVS cleared above)
  pump_status = STOPPED;
  g_pinState[cfg::PIN_PUMP_RELAY] = cfg::RELAY_OFF;
  failsafeInit();  // lastWaterMs = now, bad_conn_mode = false
}

// ---- tiny test framework ----
//
// Output is designed to read like a story:
//   ── test name ─ one-line description of the scenario
//      • STEP  what the test just did (an action)
//      ✓/✗ CHECK  an assertion, with the value observed
//
// The firmware's own Serial logs ([WARN]/[PHYSICAL]/…) are hidden by default
// so the narration stands out. Set VERBOSE=1 to interleave them:
//     VERBOSE=1 bash test/run.sh
// =====================================================================
static int g_tests = 0, g_fails = 0;
static const char* g_curr = "";
bool g_verbose = false;          // read from env in main(); gates FakeSerial

// ANSI colors (auto-off if not a TTY would be nicer, but keep simple)
#define C_DIM   "\033[2m"
#define C_GRN   "\033[32m"
#define C_RED   "\033[31m"
#define C_CYN   "\033[36m"
#define C_BLD   "\033[1m"
#define C_RST   "\033[0m"

// Tests register themselves into a list at static-init time, but DON'T run
// until main() — so main() can read VERBOSE, print a banner, and run in order.
struct RegisteredTest { const char* name; void(*fn)(); };
RegisteredTest g_registry[128];
int g_regCount = 0;
static int registerTest(const char* n, void(*fn)()) {
  g_registry[g_regCount] = {n, fn}; return g_regCount++;
}
#define TEST(name) static void name(); \
  static int reg_inst_##name = registerTest(#name, name); \
  static void name()

// Describe the scenario this test exercises (printed under the test header).
#define DESC(text) printf("   " C_DIM "%s" C_RST "\n", text)

// Narrate an action the test takes.
#define STEP(...) do { printf("      " C_CYN "•" C_RST " "); \
                       printf(__VA_ARGS__); printf("\n"); } while(0)

// Assert + show the result inline so passes are visible, not just failures.
#define CHECK(cond, msg) do { \
  if (cond) { printf("      " C_GRN "✓" C_RST " %s\n", msg); } \
  else      { printf("      " C_RED  "✗ FAIL:" C_RST " %s\n", msg); g_fails++; } \
} while(0)

// helpers to read observable state
static bool relayOn()  { return g_pinState[cfg::PIN_PUMP_RELAY] == cfg::RELAY_ON; }
static bool relayOff() { return g_pinState[cfg::PIN_PUMP_RELAY] == cfg::RELAY_OFF; }

// Human-readable current pump state, for narration.
static const char* stateName() {
  switch (pump_status) {
    case RUNNING: return "RUNNING"; case STOPPED: return "STOPPED";
    case OVERHEAT_PROTECTION: return "OVERHEAT"; default: return "?";
  }
}
// Minutes since boot (mock clock) — easier to read than raw ms.
static double clockMin() { return (g_mockMillis - 100000) / 60000.0; }

// =====================================================================
// TESTS
// =====================================================================

// --- Water-level threshold logic ---
TEST(below_min_starts_pump) {
  DESC("Water reads BELOW the min threshold -> controller should start the pump.");
  STEP("tower reports water = 50 (min=%.0f max=%.0f)", MIN_WATER_LEVEL, MAX_WATER_LEVEL);
  recordWater(50.0f, true);          // below MIN (70)
  check_water_level(MIN_WATER_LEVEL);
  STEP("after check_water_level -> pump is %s, relay %s", stateName(), relayOn()?"ON":"OFF");
  CHECK(relayOn(), "pump should run when water below min");
  CHECK(pump_status == RUNNING, "status RUNNING below min");
}

TEST(above_max_stops_pump) {
  DESC("Pump is running; water climbs ABOVE max -> controller should stop it.");
  STEP("water = 50 -> start pumping");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL); // get it running
  STEP("pump now %s; water rises to 130 (> max %.0f)", stateName(), MAX_WATER_LEVEL);
  recordWater(130.0f, true);         // above MAX (120)
  check_water_level(MIN_WATER_LEVEL);
  STEP("after check_water_level -> pump is %s, relay %s", stateName(), relayOn()?"ON":"OFF");
  CHECK(relayOff(), "pump should stop when water above max");
  CHECK(pump_status == STOPPED, "status STOPPED above max");
}

TEST(between_thresholds_no_change) {
  DESC("Water sits in the dead-band (min<water<max) -> no change either way.");
  STEP("water = 90 (between min %.0f and max %.0f), pump starts %s",
       MIN_WATER_LEVEL, MAX_WATER_LEVEL, stateName());
  recordWater(90.0f, true);          // between 70 and 120
  check_water_level(MIN_WATER_LEVEL);
  STEP("after check_water_level -> pump still %s", stateName());
  CHECK(relayOff() && pump_status == STOPPED, "no action in deadband (was off)");
}

TEST(zero_reading_takes_no_action) {
  DESC("A 0 reading means 'no usable data' — link is alive but pump must NOT act.");
  STEP("tower reports water = 0 (valid=true)");
  recordWater(0.0f, true);           // 0 == no usable data
  check_water_level(MIN_WATER_LEVEL);
  STEP("pump is %s (must stay off despite 0 < min)", stateName());
  CHECK(relayOff(), "0 reading must NOT drive pump even though link alive");
}

TEST(invalid_reading_takes_no_action) {
  DESC("An invalid reading (valid=false) must be ignored for pump control.");
  STEP("tower value = 50 but flagged valid=false");
  recordWater(50.0f, false);         // valid=false
  check_water_level(MIN_WATER_LEVEL);
  STEP("pump is %s", stateName());
  CHECK(relayOff(), "invalid reading must take no action");
}

// --- Failsafe (SAFETY CRITICAL) ---
TEST(failsafe_trips_after_window_and_stops_pump) {
  DESC("Tower goes silent. After the bad-conn window the pump MUST force-stop.");
  STEP("water = 50 -> pump starts");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  CHECK(relayOn(), "precondition: pump running");
  STEP("no more water messages; advance clock %.1f min past the window",
       cfg::BAD_CONN_DELAY_MS / 60000.0);
  mockAdvance(cfg::BAD_CONN_DELAY_MS + 1);   // no fresh water for the window
  serviceFailsafe();
  STEP("serviceFailsafe -> bad_conn=%s, pump %s", bad_conn_mode?"ON":"off", stateName());
  CHECK(bad_conn_mode, "bad_conn_mode latched after stale window");
  CHECK(relayOff(), "failsafe force-stops the pump");
}

TEST(failsafe_does_not_trip_early) {
  DESC("Just UNDER the window the failsafe must stay quiet (no false trips).");
  STEP("water = 50 -> pump starts");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  STEP("advance to 1 ms BEFORE the %.1f-min window expires", cfg::BAD_CONN_DELAY_MS / 60000.0);
  mockAdvance(cfg::BAD_CONN_DELAY_MS - 1);    // just under the window
  serviceFailsafe();
  STEP("serviceFailsafe -> bad_conn=%s, pump %s", bad_conn_mode?"ON":"off", stateName());
  CHECK(!bad_conn_mode, "must NOT trip before the full window elapses");
  CHECK(relayOn(), "pump still running just under window");
}

TEST(failsafe_recovers_when_water_resumes) {
  DESC("After a trip, the first fresh water message clears bad-conn mode.");
  STEP("water = 50 -> pump starts");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  STEP("go silent past the window -> trip");
  mockAdvance(cfg::BAD_CONN_DELAY_MS + 1); serviceFailsafe();
  CHECK(bad_conn_mode, "precondition: tripped");
  STEP("tower comes back: fresh water = 50 (resets the liveness clock)");
  recordWater(50.0f, true);                   // fresh data resumes (resets clock)
  serviceFailsafe();
  STEP("serviceFailsafe -> bad_conn=%s", bad_conn_mode?"ON":"off");
  CHECK(!bad_conn_mode, "recovers once fresh water resumes");
}

TEST(latched_failsafe_refuses_to_run_pump) {
  DESC("While failsafe is latched, even a below-min reading must NOT start pump.");
  STEP("trip the failsafe first (no water past the window)");
  mockAdvance(cfg::BAD_CONN_DELAY_MS + 1); serviceFailsafe();
  CHECK(bad_conn_mode, "precondition: latched");
  STEP("now a 'low water = 50' arrives — normally that would start the pump");
  recordWater(50.0f, true);
  check_water_level(MIN_WATER_LEVEL);
  STEP("pump is %s (must stay off while latched)", stateName());
  CHECK(relayOff(), "while latched, refuse to run even below min");
}

// --- Overheat state machine ---
TEST(overheat_trips_after_run_duration) {
  DESC("Pump running too long -> overheat protection kicks in and cuts the relay.");
  STEP("water = 50 -> pump starts (t=%.1f min)", clockMin());
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  CHECK(relayOn(), "precondition: running");
  STEP("run continuously for %.0f min (overheat trip time)", cfg::OVERHEAT_TRIP_MS / 60000.0);
  mockAdvance(cfg::OVERHEAT_TRIP_MS + 1);
  pumpTick();   // services overheat-trip timer
  STEP("pumpTick at t=%.1f min -> pump is %s, relay %s", clockMin(), stateName(), relayOn()?"ON":"OFF");
  CHECK(pump_status == OVERHEAT_PROTECTION, "enters overheat after trip time");
  CHECK(relayOff(), "relay off during overheat");
}

TEST(overheat_recovers_and_reevaluates_low_water) {
  DESC("After cooldown, if water is STILL low the pump restarts (re-evaluates).");
  STEP("water = 50 -> pump runs to overheat");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  mockAdvance(cfg::OVERHEAT_TRIP_MS + 1); pumpTick();           // -> overheat
  CHECK(pump_status == OVERHEAT_PROTECTION, "precondition overheat");
  STEP("cool down %.0f min; water is still 50 (< min)", cfg::OVERHEAT_RECOVER_MS / 60000.0);
  mockAdvance(cfg::OVERHEAT_RECOVER_MS + 1); pumpTick();        // -> recover
  // water is still 50 (< min) and not bad time -> should restart
  STEP("after recover -> pump is %s", stateName());
  CHECK(pump_status == RUNNING, "recover re-evaluates: still low -> restart");
}

TEST(overheat_recovers_and_stays_off_when_water_ok) {
  DESC("After cooldown, if water has recovered the pump must NOT blindly restart.");
  STEP("water = 50 -> pump runs to overheat");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  mockAdvance(cfg::OVERHEAT_TRIP_MS + 1); pumpTick();           // -> overheat
  STEP("during cooldown the tower reports water = 100 (now above min)");
  recordWater(100.0f, true);                                    // now plenty of water
  mockAdvance(cfg::OVERHEAT_RECOVER_MS + 1); pumpTick();        // -> recover
  STEP("after recover -> pump is %s (should stay off, water is fine)", stateName());
  CHECK(pump_status == STOPPED, "recover does NOT blindly restart when water ok");
  CHECK(relayOff(), "relay stays off when water ok after recover");
}

TEST(force_stop_during_cooldown_does_not_cancel_recovery) {
  DESC("A stop arriving mid-cooldown must NOT cut the thermal cooldown short.");
  STEP("water = 50 -> pump runs to overheat (cooldown begins)");
  recordWater(50.0f, true); check_water_level(MIN_WATER_LEVEL);
  mockAdvance(cfg::OVERHEAT_TRIP_MS + 1); pumpTick();           // -> overheat
  STEP("a failsafe/manual pump_stop() arrives while still cooling");
  pump_stop();   // a failsafe/manual stop arriving mid-cooldown
  STEP("pump is %s (must remain OVERHEAT, not flip to STOPPED early)", stateName());
  CHECK(pump_status == OVERHEAT_PROTECTION, "stop must NOT exit cooldown early");
  STEP("let the full %.0f-min cooldown elapse", cfg::OVERHEAT_RECOVER_MS / 60000.0);
  mockAdvance(cfg::OVERHEAT_RECOVER_MS + 1); pumpTick();
  STEP("after cooldown -> pump is %s", stateName());
  CHECK(pump_status != OVERHEAT_PROTECTION, "cooldown still completes on schedule");
}

// --- Absolute max-on cap (defense in depth) ---
// NOTE: in normal operation overheat-trip (20m) fires before the max-on cap
// (25m), so the cap only matters if the overheat timer never armed/fired
// (e.g. a future regression). We simulate that by clearing the overheat timer
// effect: keep status RUNNING and pumpOnSinceMs old, then tick past the cap.
TEST(max_on_cap_force_stops_independently_of_overheat) {
  DESC("Absolute backstop: a pump stuck ON past the hard cap is force-stopped,");
  DESC("even if the overheat timer never fired (defense in depth).");
  STEP("cap = %.0f min, overheat trip = %.0f min -> overheat normally wins first",
       cfg::PUMP_MAX_ON_MS / 60000.0, cfg::OVERHEAT_TRIP_MS / 60000.0);
  CHECK(cfg::PUMP_MAX_ON_MS > cfg::OVERHEAT_TRIP_MS,
        "documents that overheat normally wins; cap is a pure backstop");
  // mockReset() left us idle with NO pump timers armed. Forge a "stuck on"
  // pump: RUNNING, relay on, on-time older than the absolute cap — exactly the
  // state the cap exists to catch (overheat timer somehow never fired). No
  // overheat timer is armed, so only the cap branch in pumpTick() can act.
  extern unsigned long pumpOnSinceMs;
  STEP("simulate a stuck-on pump: RUNNING with on-time older than the cap");
  mockAdvance(cfg::PUMP_MAX_ON_MS + 10);     // now() comfortably exceeds the cap
  pump_status = RUNNING;
  g_pinState[cfg::PIN_PUMP_RELAY] = cfg::RELAY_ON;
  pumpOnSinceMs = 1;                          // on since "long ago" -> over cap
  pumpTick();
  STEP("pumpTick -> pump is %s, relay %s", stateName(), relayOn()?"ON":"OFF");
  CHECK(relayOff(), "max-on cap force-stops a pump stuck on with no overheat");
  CHECK(pump_status == STOPPED, "cap leaves status STOPPED");
}

// --- Threshold setters + NVS persistence ---
TEST(set_max_persists_and_validates) {
  DESC("Setting max updates the value, writes it to NVS, and rejects max<=min.");
  STEP("set max = 150 (min is %.0f)", MIN_WATER_LEVEL);
  applySetMax(150.0f);
  CHECK(MAX_WATER_LEVEL == 150.0f, "max updated");
  CHECK(g_nvsFloat["max"] == 150.0f, "max persisted to NVS");
  STEP("try set max = 50, which is below min -> should be rejected");
  applySetMax(50.0f);   // below min(70) -> rejected
  CHECK(MAX_WATER_LEVEL == 150.0f, "max <= min rejected");
}

TEST(set_min_persists_and_validates) {
  DESC("Setting min updates the value, writes it to NVS, and rejects min>=max.");
  STEP("set min = 40 (max is %.0f)", MAX_WATER_LEVEL);
  applySetMin(40.0f);
  CHECK(MIN_WATER_LEVEL == 40.0f, "min updated");
  CHECK(g_nvsFloat["min"] == 40.0f, "min persisted to NVS");
  STEP("try set min = 200, which is above max -> should be rejected");
  applySetMin(200.0f);  // above max(120) -> rejected
  CHECK(MIN_WATER_LEVEL == 40.0f, "min >= max rejected");
}

TEST(set_deficient_persists_and_validates) {
  DESC("Deficient (prefill target) is now USER-SET, not auto-derived; it writes");
  DESC("to NVS and is rejected unless it sits within [min, max].");
  STEP("set deficient = 90 (min %.0f, max %.0f)", MIN_WATER_LEVEL, MAX_WATER_LEVEL);
  applySetDeficient(90.0f);
  CHECK(DEFICIENT_WATER_LEVEL == 90.0f, "deficient updated");
  CHECK(g_nvsFloat["deficient"] == 90.0f, "deficient persisted to NVS");
  STEP("try set deficient = 200 (above max) -> rejected");
  applySetDeficient(200.0f);
  CHECK(DEFICIENT_WATER_LEVEL == 90.0f, "deficient > max rejected");
  STEP("try set deficient = 10 (below min) -> rejected");
  applySetDeficient(10.0f);
  CHECK(DEFICIENT_WATER_LEVEL == 90.0f, "deficient < min rejected");
}

TEST(deficient_not_recomputed_when_min_max_change) {
  DESC("Changing min/max must NOT silently move the user's deficient value.");
  STEP("set deficient = 100, then change max=150 and min=40");
  applySetDeficient(100.0f);
  applySetMax(150.0f); applySetMin(40.0f);
  STEP("deficient is still %.0f (no auto-recalc)", DEFICIENT_WATER_LEVEL);
  CHECK(DEFICIENT_WATER_LEVEL == 100.0f, "deficient stays put when min/max change");
}

TEST(thresholds_reload_from_nvs_on_init) {
  DESC("All three thresholds survive a reboot: pumpInit() reloads from NVS.");
  STEP("set max=140, min=55, deficient=80, then pumpInit() to simulate reboot");
  applySetMax(140.0f); applySetMin(55.0f); applySetDeficient(80.0f);
  pumpInit();   // simulates reboot: should reload persisted values
  STEP("after reboot -> max=%.0f, min=%.0f, deficient=%.0f",
       MAX_WATER_LEVEL, MIN_WATER_LEVEL, DEFICIENT_WATER_LEVEL);
  CHECK(MAX_WATER_LEVEL == 140.0f, "max survives reboot via NVS");
  CHECK(MIN_WATER_LEVEL == 55.0f,  "min survives reboot via NVS");
  CHECK(DEFICIENT_WATER_LEVEL == 80.0f, "deficient survives reboot via NVS");
}

// --- Command queue (thread-safety model: enqueue/dequeue ring buffer) ---
TEST(command_queue_fifo_and_full) {
  DESC("Web/MQTT handlers only enqueue; loop drains in FIFO order with args intact.");
  STEP("enqueue PUMP_RUN, then SET_MAX(99)");
  enqueueCommand(CMD_PUMP_RUN);
  enqueueCommand(CMD_SET_MAX, 99.0f);
  Command c;
  CHECK(dequeueCommand(c) && c.type == CMD_PUMP_RUN, "FIFO order 1");
  CHECK(dequeueCommand(c) && c.type == CMD_SET_MAX && c.arg == 99.0f, "FIFO order 2 + arg");
  CHECK(!dequeueCommand(c), "empty after draining");
}

TEST(command_queue_drops_when_full_without_corruption) {
  DESC("Overflowing the ring drops excess cleanly — no corruption of held items.");
  STEP("enqueue %d commands into a ring of capacity %d", CMD_QUEUE_SIZE + 5, CMD_QUEUE_SIZE);
  // ring holds CMD_QUEUE_SIZE-1 usable slots
  for (int i = 0; i < CMD_QUEUE_SIZE + 5; i++) enqueueCommand(CMD_DOOR_UP);
  int drained = 0; Command c;
  while (dequeueCommand(c)) { CHECK(c.type == CMD_DOOR_UP, "no corruption"); drained++; }
  STEP("drained %d commands (capacity-1 = %d)", drained, CMD_QUEUE_SIZE - 1);
  CHECK(drained == CMD_QUEUE_SIZE - 1, "drops excess, keeps capacity-1");
}

// =====================================================================
static void run(const char* n, void(*fn)()) {
  g_tests++; g_curr = n;
  int before = g_fails;
  printf("\n" C_BLD "── %s" C_RST "\n", n);
  mockReset();
  fn();
  bool ok = (g_fails == before);
  printf("   %s\n", ok ? C_GRN "PASS" C_RST : C_RED C_BLD "FAILED" C_RST);
}

#include <cstdlib>
int main() {
  const char* v = getenv("VERBOSE");
  g_verbose = (v && v[0] && v[0] != '0');

  printf(C_BLD "pump_side_ai2 — host-side unit tests" C_RST "\n");
  printf(C_DIM "no hardware; real control logic vs. fake Arduino layer. "
               "%s firmware logs (VERBOSE=1 to show)." C_RST "\n",
               g_verbose ? "showing" : "hiding");

  for (int i = 0; i < g_regCount; i++) run(g_registry[i].name, g_registry[i].fn);

  printf("\n" C_BLD "═══ %d tests, %d %s ═══" C_RST "\n",
         g_tests, g_fails,
         g_fails ? C_RED "assertion failures" C_RST C_BLD : C_GRN "passed" C_RST C_BLD);
  return g_fails == 0 ? 0 : 1;
}
