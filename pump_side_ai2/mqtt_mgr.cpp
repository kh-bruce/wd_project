#include "mqtt_mgr.h"
#include "config.h"
#include "commands.h"
#include "pump_control.h"
#include "failsafe.h"
#include "ntp_time.h"
#include "wifi_mgr.h"
#include "logging.h"
#include "arduino_secrets.h"
#include <WiFi.h>        // WiFi.RSSI()
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>   // feed the WDT around the blocking mqtt.connect()

static WiFiClient mqttWifiClient;
static PubSubClient mqtt(mqttWifiClient);
static unsigned long lastMqttReconnectAttempt = 0;
static bool wasMqttConnected = false;

bool mqttIsConnected() { return mqtt.connected(); }

// ---- Inbound callback: ONLY enqueue commands / record water. ----
// Runs on the PubSubClient (loop-driven) context; we still keep it pure-enqueue
// so the control path stays single-owner in loop().
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[32];
  unsigned int n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';
  String t = String(topic);
  String v = String(buf);

  if (t == topic::SUB_WATER) {
    // Parse once at ingress. Any received message (even "0"/garbage) is proof
    // of liveness and resets the failsafe clock via recordWater().
    char* endp = nullptr;
    float val = strtof(buf, &endp);
    bool valid = (endp != buf); // at least one numeric char parsed
    recordWater(val, valid);
    return;
  }
  if (t == topic::SUB_AVG_MAX || t == topic::SUB_AVG_MIN) {
    // DISPLAY-ONLY tower stats. Parse for the OLED but do NOT call recordWater()
    // — these are retained, so a replay is not proof of liveness; feeding the
    // failsafe clock here would falsely keep it alive.
    char* endp = nullptr;
    float val = strtof(buf, &endp);
    bool valid = (endp != buf);
    if (t == topic::SUB_AVG_MAX) recordTowerMax(val, valid);
    else                         recordTowerMin(val, valid);
    return;
  }
  if (t == topic::CMD_DOOR) {
    String d = v; d.toLowerCase();
    if      (d == "up")   enqueueCommand(CMD_DOOR_UP);
    else if (d == "down") enqueueCommand(CMD_DOOR_DOWN);
    else if (d == "stop") enqueueCommand(CMD_DOOR_STOP);
    return;
  }
  if (t == topic::CMD_MANUALPUMP) {
    if      (v == "RUN"  || v == "1") enqueueCommand(CMD_PUMP_RUN);
    else if (v == "STOP" || v == "0") enqueueCommand(CMD_PUMP_STOP);
    return;
  }
  if (t == topic::CMD_SETMAX) { enqueueCommand(CMD_SET_MAX, v.toFloat()); return; }
  if (t == topic::CMD_SETMIN) { enqueueCommand(CMD_SET_MIN, v.toFloat()); return; }
  if (t == topic::CMD_SETDEFICIENT) { enqueueCommand(CMD_SET_DEFICIENT, v.toFloat()); return; }
}

// ---- Status JSON (ArduinoJson -> fixed buffer) ----
size_t buildStatusJson(char *buf, size_t buflen) {
  // ArduinoJson v6: StaticJsonDocument (stack). If you upgrade to v7, change
  // this to `JsonDocument doc;` (StaticJsonDocument is removed in v7).
  StaticJsonDocument<512> doc;
  unsigned long nowMs = millis();

  doc["water"]        = latestWaterValid ? latestWater : 0.0f;
  doc["water_valid"]  = latestWaterValid;
  doc["pump_status"]  = pumpStatusStr();
  doc["minutes_since_change"] = (nowMs - pumpStatusChangedMs) / 60000.0;
  doc["bad_conn_mode"] = bad_conn_mode ? 1 : 0;
  doc["bad_conn_count"] = bad_conn_count;

  int hours = 0; String formatted = "time not synced";
  getLocalTimeFromCache(hours, formatted);
  doc["time"]        = formatted;
  doc["is_badtime"]  = isBadTime() ? "True" : "False";
  doc["min_level"]   = MIN_WATER_LEVEL;
  doc["max_level"]   = MAX_WATER_LEVEL;
  doc["deficient_level"] = DEFICIENT_WATER_LEVEL;
  doc["prefill_from"] = cfg::PREFILL_HOUR_START;
  doc["prefill_to"]   = cfg::PREFILL_HOUR_END;
  doc["uptime_s"]    = nowMs / 1000;
  doc["last_command"] = lastCommand;
  unsigned long waterMs;
  portENTER_CRITICAL(&cmdMux);
  waterMs = lastWaterMs;
  portEXIT_CRITICAL(&cmdMux);
  doc["last_water_update_s"] = (nowMs - waterMs) / 1000.0;

  return serializeJson(doc, buf, buflen);
}

// Cached status JSON: produced ONLY by the loop thread (publishStatusMqtt /
// refreshStatusCache), read by the web handler via copyStatusCache() under
// statusMux. Avoids building the JSON on the AsyncTCP task, which would read
// loop-owned state (incl. the String lastCommand) cross-thread and risk a
// heap use-after-free.
static char statusCache[640];
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

void refreshStatusCache() {
  char tmp[640];
  size_t n = buildStatusJson(tmp, sizeof(tmp));
  portENTER_CRITICAL(&statusMux);
  memcpy(statusCache, tmp, n + 1); // include NUL
  portEXIT_CRITICAL(&statusMux);
}

size_t copyStatusCache(char *out, size_t outlen) {
  portENTER_CRITICAL(&statusMux);
  size_t n = strlcpy(out, statusCache, outlen);
  portEXIT_CRITICAL(&statusMux);
  return n;
}

void publishStatusMqtt() {
  refreshStatusCache(); // keep the web cache fresh on the loop thread
  if (!mqtt.connected()) return;
  char buf[640];
  buildStatusJson(buf, sizeof(buf));
  mqtt.publish(topic::STATUS, buf, true);
  // Flat state topics for the dedicated HA entities.
  mqtt.publish(topic::PUMP_STATUS, pumpStatusStr(), true);
  char num[16];
  dtostrf(MIN_WATER_LEVEL, 0, 1, num); mqtt.publish(topic::MIN_LEVEL, num, true);
  dtostrf(MAX_WATER_LEVEL, 0, 1, num); mqtt.publish(topic::MAX_LEVEL, num, true);
  dtostrf(DEFICIENT_WATER_LEVEL, 0, 1, num); mqtt.publish(topic::DEFICIENT_LEVEL, num, true);
  mqtt.publish(topic::BAD_CONN, bad_conn_mode ? "ON" : "OFF", true);
  snprintf(num, sizeof(num), "%d", (int)WiFi.RSSI()); mqtt.publish(topic::RSSI, num, true);
}

// ---- HA discovery (table-driven) ----
#if USE_HA_DISCOVERY
static const char* DEV =
  "\"dev\":{\"ids\":[\"wd_pump\"],\"name\":\"WD Pump (1F)\",\"mdl\":\"ESP32\",\"mf\":\"wd_project\",\"cu\":\"http://192.168.1.217/\"}";
static const char* AV =
  "\"avty_t\":\"wd/pump/avail\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";

static void publishDiscovery() {
  char buf[640];

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Pump Status\",\"uniq_id\":\"wd_pump_status\",\"stat_t\":\"%s\","
    "\"dev_cla\":\"enum\",\"options\":[\"RUNNING\",\"STOPPED\",\"OVERHEAT_PROTECTION\"],"
    "\"json_attr_t\":\"%s\",%s,%s}", topic::PUMP_STATUS, topic::STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/status/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Bad Connection\",\"uniq_id\":\"wd_pump_bad_conn\",\"stat_t\":\"%s\","
    "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\",%s,%s}",
    topic::BAD_CONN, AV, DEV);
  mqtt.publish("homeassistant/binary_sensor/wd_pump/bad_conn/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Max Water Level\",\"uniq_id\":\"wd_pump_max_level\",\"cmd_t\":\"%s\","
    "\"stat_t\":\"%s\",\"min\":0,\"max\":200,\"step\":1,\"mode\":\"box\",\"ent_cat\":\"config\",%s,%s}",
    topic::CMD_SETMAX, topic::MAX_LEVEL, AV, DEV);
  mqtt.publish("homeassistant/number/wd_pump/max_level/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Min Water Level\",\"uniq_id\":\"wd_pump_min_level\",\"cmd_t\":\"%s\","
    "\"stat_t\":\"%s\",\"min\":0,\"max\":200,\"step\":1,\"mode\":\"box\",\"ent_cat\":\"config\",%s,%s}",
    topic::CMD_SETMIN, topic::MIN_LEVEL, AV, DEV);
  mqtt.publish("homeassistant/number/wd_pump/min_level/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Deficient Water Level\",\"uniq_id\":\"wd_pump_deficient_level\",\"cmd_t\":\"%s\","
    "\"stat_t\":\"%s\",\"min\":0,\"max\":200,\"step\":1,\"mode\":\"box\",\"ent_cat\":\"config\",%s,%s}",
    topic::CMD_SETDEFICIENT, topic::DEFICIENT_LEVEL, AV, DEV);
  mqtt.publish("homeassistant/number/wd_pump/deficient_level/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Manual Pump\",\"uniq_id\":\"wd_pump_manual\",\"cmd_t\":\"%s\","
    "\"pl_on\":\"RUN\",\"pl_off\":\"STOP\",\"stat_t\":\"%s\",\"stat_on\":\"RUNNING\",\"stat_off\":\"STOPPED\","
    "\"ic\":\"mdi:water-boiler\",%s,%s}", topic::CMD_MANUALPUMP, topic::PUMP_STATUS, AV, DEV);
  mqtt.publish("homeassistant/switch/wd_pump/manual/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Garage Door\",\"uniq_id\":\"wd_pump_door\",\"dev_cla\":\"garage\",\"cmd_t\":\"%s\","
    "\"pl_open\":\"UP\",\"pl_cls\":\"DOWN\",\"pl_stop\":\"STOP\",\"opt\":true,%s,%s}",
    topic::CMD_DOOR, AV, DEV);
  mqtt.publish("homeassistant/cover/wd_pump/door/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Pump Water\",\"uniq_id\":\"wd_pump_water\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{value_json.water}}\",\"stat_cla\":\"measurement\",%s,%s}",
    topic::STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/water/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Uptime\",\"uniq_id\":\"wd_pump_uptime\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{value_json.uptime_s}}\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\","
    "\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\",%s,%s}", topic::STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/uptime/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"WiFi Signal\",\"uniq_id\":\"wd_pump_rssi\",\"stat_t\":\"%s\","
    "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\","
    "\"ent_cat\":\"diagnostic\",%s,%s}", topic::RSSI, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/rssi/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Local Time\",\"uniq_id\":\"wd_pump_time\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{value_json.time}}\",\"ic\":\"mdi:clock-outline\",\"ent_cat\":\"diagnostic\",%s,%s}",
    topic::STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/time/config", buf, true);
}
#else
static void publishDiscovery() {}
#endif

static bool mqttReconnect() {
  if (millis() - lastMqttReconnectAttempt < cfg::MQTT_RECONNECT_MS) return false;
  lastMqttReconnectAttempt = millis();
  Serial.println("MQTT connecting...");
  // mqtt.connect() blocks (TCP connect + CONNACK wait, up to a few seconds).
  // Feed the WDT before and after so back-to-back failed laps can't eat the
  // watchdog window (matches the tower's hardened reconnect).
  esp_task_wdt_reset();
  bool ok = mqtt.connect(SECRET_MQTT_CLIENTID, SECRET_MQTT_USER, SECRET_MQTT_PASS,
                         topic::AVAIL, 1, true, "offline");
  esp_task_wdt_reset();
  if (ok) {
    Serial.println("MQTT connected");
    logVerbose("MQTT connected");
    mqtt.subscribe(topic::SUB_WATER, 1);
    mqtt.subscribe(topic::SUB_AVG_MAX, 1);  // display-only tower session max
    mqtt.subscribe(topic::SUB_AVG_MIN, 1);  // display-only tower session min
    mqtt.subscribe(topic::CMD_WILDCARD, 1);
    mqtt.publish(topic::AVAIL, "online", true);
    publishDiscovery();
    publishStatusMqtt();
  } else {
    Serial.print("MQTT connect failed, state="); Serial.println(mqtt.state());
  }
  return ok;
}

void mqttInit() {
  mqtt.setServer(SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  mqtt.setBufferSize(1024);
  // PubSubClient does NOT cap the underlying TCP connect(); on a weak link that
  // lets mqtt.connect() block for many seconds and stack into a 40s reconnect
  // stall (observed on 1F, not on the offloaded tower). Bound it on the
  // WiFiClient directly, and shrink the socket timeout, matching the tower fix.
  mqttWifiClient.setTimeout(2000);  // ms — caps blocking TCP connect
  mqtt.setSocketTimeout(2);         // s  — was 5; each failed reconnect lap is shorter
  mqtt.setKeepAlive(15);
  mqtt.setCallback(mqttCallback);
  refreshStatusCache(); // prime so /status.json works before the first publish
}

void mqttService() {
  if (!wifiConnected()) return; // don't attempt MQTT while WiFi is down
  if (!mqtt.connected()) {
    if (wasMqttConnected) { logWarning("MQTT disconnected"); wasMqttConnected = false; }
    mqttReconnect();
  } else {
    if (!wasMqttConnected) wasMqttConnected = true;
    mqtt.loop();
  }
}
