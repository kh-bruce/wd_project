/**
  2023水鴨計畫 — PUMP SIDE (1F pump + garage) — clean rewrite

  Architecture (multi-file; Arduino compiles all tabs in this folder together):
    config.h        all non-secret tunables (GPIO/topics/thresholds/timing)
    commands.h/.cpp thread-safe command queue + water ingress (portMUX)
    wifi_mgr        non-blocking WiFi connect + auto-reconnect
    ntp_time        background (non-blocking) NTP + cached local time
    pump_control    pump state machine, overheat, thresholds (NVS), blink
    failsafe        timestamp-based 60s bad-connection failsafe
    door_control    garage door momentary-pulse relay
    mqtt_mgr        MQTT connect/LWT/reconnect, HA discovery, status (ArduinoJson)
    webui           minimal local fallback (status page + /get enqueue-only)

  SAFETY MODEL: the loop task is the SOLE owner of all hardware, timers, and
  pump state. Web/MQTT handlers only enqueueCommand()/recordWater(); loop()
  drains them. The failsafe is a millis() timestamp check — no timer races.

  Cross-device dependency: the tower MUST publish wd/tower/state/water with
  retain=false, else a reconnect replays a stale value and falsely keeps the
  failsafe alive. (tower_side_ai publishes retain=false — verified.)
**/
#include <esp_task_wdt.h>
#include <arduino-timer.h>
#include <WiFi.h>
#include "config.h"
#include "logging.h"
#include "commands.h"
#include "wifi_mgr.h"
#include "ntp_time.h"
#include "pump_control.h"
#include "failsafe.h"
#include "door_control.h"
#include "mqtt_mgr.h"
#include "webui.h"
#include "display.h"

static auto timer_heap          = timer_create_default();
static auto timer_prefill       = timer_create_default();
static auto timer_mqtt_status   = timer_create_default();

static bool heapCheck(void *) {
  logVerbose("Heap: " + String(ESP.getFreeHeap()) + " free, min " + String(ESP.getMinFreeHeap()));
  return true;
}

// Prefill: during the evening window, top up toward DEFICIENT_WATER_LEVEL.
static bool prefillCheck(void *) {
  if (isTimeInRange()) check_water_level(DEFICIENT_WATER_LEVEL);
  return true;
}

static bool publishStatusTimer(void *) {
  publishStatusMqtt();
  return true;
}

// Execute one queued command on the loop thread (sole hardware owner).
static void runCommand(const Command &c) {
  switch (c.type) {
    case CMD_DOOR_UP:   doorCommand("up");   record_command("door:up");   break;
    case CMD_DOOR_DOWN: doorCommand("down"); record_command("door:down"); break;
    case CMD_DOOR_STOP: doorCommand("stop"); record_command("door:stop"); break;
    case CMD_PUMP_RUN:  manual_pump_start();  record_command("manualpump"); break;
    case CMD_PUMP_STOP: manual_pump_stop();   record_command("manualpumpstop"); break;
    case CMD_SET_MAX:   applySetMax(c.arg);   record_command("setmax:" + String(c.arg, 1)); break;
    case CMD_SET_MIN:   applySetMin(c.arg);   record_command("setmin:" + String(c.arg, 1)); break;
    case CMD_SET_DEFICIENT: applySetDeficient(c.arg); record_command("setdeficient:" + String(c.arg, 1)); break;
  }
}

static void drainCommands() {
  Command c;
  while (dequeueCommand(c)) runCommand(c);
}

// Track water-ingress so we run the threshold logic once per new value
// (loop thread), instead of in the handler.
static unsigned long processedWaterMs = 0;

static void serviceWaterLevel() {
  unsigned long wMs;
  portENTER_CRITICAL(&cmdMux);
  wMs = lastWaterMs;
  portEXIT_CRITICAL(&cmdMux);
  if (wMs != processedWaterMs) {
    processedWaterMs = wMs;
    check_water_level(MIN_WATER_LEVEL);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Configuring WDT...");
#if ESP_IDF_VERSION_MAJOR >= 5
  // ESP32 Arduino core 3.x / IDF v5+: esp_task_wdt_init takes a config struct.
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = (uint32_t)cfg::WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true, // panic -> reboot
  };
  esp_task_wdt_init(&wdtConfig);
#else
  // ESP32 Arduino core 2.x / IDF v4: (timeout_s, panic).
  esp_task_wdt_init(cfg::WDT_TIMEOUT_S, true); // panic -> reboot
#endif
  esp_task_wdt_add(NULL);

  pumpInit();
  doorInit();
  failsafeInit();
  displayInit();   // optional OLED; harmless no-op if no panel is wired

  // WiFi: kick off, wait briefly (watchdog-fed), but never lock up on failure.
  wifiBegin();
  unsigned long wifiStart = millis();
  while (!wifiConnected() && millis() - wifiStart < 15000) {
    delay(250);
    esp_task_wdt_reset();
  }
  if (wifiConnected()) {
    Serial.print("IP Address: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not up yet — continuing; loop() will keep retrying");
  }

  mqttInit();   // primes the status cache BEFORE the web server can serve it
  webuiInit();
  ntpInit();

  timer_heap.every(cfg::HEAP_CHECK_MS, heapCheck);
  timer_prefill.every(cfg::PREFILL_CHECK_MS, prefillCheck);
  timer_mqtt_status.every(cfg::STATUS_PUBLISH_MS, publishStatusTimer);

  logWarning("System startup complete");
}

void loop() {
  serviceWifi();          // non-blocking WiFi keepalive
  mqttService();          // gated on WiFi; reconnect + mqtt.loop()
  esp_task_wdt_reset();   // cap the network phase: keep one slow phase from
                          // starving the watchdog of the others (defense-in-depth)
  drainCommands();        // execute queued web/MQTT commands (loop thread)
  serviceWaterLevel();    // run threshold logic on new water values
  serviceFailsafe();      // 60s timestamp check -> force-stop if stale

  pumpTick();             // overheat/recover/max-on timers + blink
  blinkTick();
  doorTick();
  ntpTick();
  // OLED now redraws on its own FreeRTOS task (see display.cpp) — loop() no
  // longer flushes I2C, so the keepalive can't be starved by a slow sendBuffer.
  timer_heap.tick();
  timer_prefill.tick();
  timer_mqtt_status.tick();

  esp_task_wdt_reset();
}
