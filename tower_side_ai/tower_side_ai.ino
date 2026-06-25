/**
  2023水鴨計畫 — TOWER SIDE (4F water-level sensor)
  board: nodemcu (esp32)

  MQTT migration (was Blynk). This sketch:
    - reads the analog water-level sensor (GPIO32), same averaging logic as before
    - publishes the water level + session max/min + uptime + rssi to MQTT
    - is PUBLISH-ONLY (no command subscriptions; HA sets thresholds on the pump directly)

  The pump subscribes to wd/tower/state/water and drives the pump from it.
  Blynk and the direct HTTP GET to the pump are both removed.
**/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <arduino-timer.h>
#include <esp_task_wdt.h>
#include <stdio.h>
#include "arduino_secrets.h"

// ---- Watchdog ----
#define WDT_TIMEOUT 25
unsigned long lastWdtReset = 0;

// ---- WiFi / MQTT ----
const char* ssid = SECRET_WIFI_SSID;
const char* pass = SECRET_WIFI_PASS;
WiFiClient   espClient;
PubSubClient mqtt(espClient);
unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;

// ---- MQTT topics ----
const char* TOPIC_AVAIL     = "wd/tower/avail";
const char* TOPIC_WATER     = "wd/tower/state/water";    // SAFETY-CRITICAL: retain=false
const char* TOPIC_AVG_MAX   = "wd/tower/state/avg_max";  // retain=true
const char* TOPIC_AVG_MIN   = "wd/tower/state/avg_min";  // retain=true
const char* TOPIC_UPTIME    = "wd/tower/state/uptime";   // retain=true
const char* TOPIC_RSSI      = "wd/tower/state/rssi";     // retain=true

// ---- Sensor ----
#define INPIN 32
#define DEPTH_COUNT 9
float depth[DEPTH_COUNT] = {};
int   depth_index = 0;
float avg_max = 0.0;
float avg_min = 401.0;
bool  over1loop = false;
bool  haveReading = false;   // true once at least one batch has produced a value

float temp = 0;
// 每湊滿這麼多次 analogRead 就產生一個新的批次平均值。
// 從 5000 大幅降低：arduino-timer 每次 loop() 最多只跑 collectDate 一次，
// 所以實際取樣率 = loop() 迭代率。批次越小，即使 WiFi/MQTT 偶爾阻塞拖慢
// loop()，也能每 1~2 秒就備好一個新的水位值，發佈才不會被餓死。
#define TIMES2AVG 300
int   sampleCount = 0;

// 解耦「發佈」與「湊滿一批取樣」：sendData 每秒都會發佈 smoothedLevel，
// 不再等 needUpdate。smoothedLevel 是對「9-slot 視窗平均」再做一層 EMA，
// 額外增加平滑度。alpha 越小越平滑、反應越慢。
float smoothedLevel = 0.0;
#define EMA_ALPHA 0.30f

// ---- Timers (replaces BlynkTimer) ----
auto timer_sensor = timer_create_default(); // collectDate, every 2ms
auto timer_send   = timer_create_default(); // sendData,    every 1000ms
auto timer_uptime = timer_create_default(); // uptime/rssi, every 1000ms

// ===========================================================================
// Watchdog
// ===========================================================================
void reset_wdt() {
  if (millis() - lastWdtReset >= 5000) {
    esp_task_wdt_reset();
    lastWdtReset = millis();
  }
}

// ===========================================================================
// MQTT
// ===========================================================================
bool publishFloat(const char* topic, float value, int decimals, bool retain) {
  char buf[24];
  dtostrf(value, 0, decimals, buf);
  return mqtt.publish(topic, buf, retain);
}

// Publish HA MQTT Discovery configs (retained) so the tower's entities
// auto-appear in Home Assistant grouped under one "WD Tower (4F)" device.
// Built with snprintf into a reused buffer to keep heap pressure low.
// Set USE_HA_DISCOVERY to 0 to define these entities manually in HA YAML instead.
#define USE_HA_DISCOVERY 1
#if USE_HA_DISCOVERY
void publishDiscovery() {
  const char* DEV = "\"dev\":{\"ids\":[\"wd_tower\"],\"name\":\"WD Tower (4F)\",\"mdl\":\"ESP32\",\"mf\":\"wd_project\"}";
  const char* AV  = "\"avty_t\":\"wd/tower/avail\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  char buf[480];

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Water Level\",\"uniq_id\":\"wd_tower_water\",\"stat_t\":\"%s\","
    "\"stat_cla\":\"measurement\",\"ic\":\"mdi:waves-arrow-up\",\"exp_aft\":90,%s,%s}",
    TOPIC_WATER, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_tower/water/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Water Avg Max\",\"uniq_id\":\"wd_tower_avg_max\",\"stat_t\":\"%s\","
    "\"ic\":\"mdi:arrow-collapse-up\",\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_AVG_MAX, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_tower/avg_max/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Water Avg Min\",\"uniq_id\":\"wd_tower_avg_min\",\"stat_t\":\"%s\","
    "\"ic\":\"mdi:arrow-collapse-down\",\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_AVG_MIN, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_tower/avg_min/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"Uptime\",\"uniq_id\":\"wd_tower_uptime\",\"stat_t\":\"%s\","
    "\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"stat_cla\":\"total_increasing\","
    "\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_UPTIME, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_tower/uptime/config", buf, true);

  snprintf(buf, sizeof(buf),
    "{\"name\":\"WiFi Signal\",\"uniq_id\":\"wd_tower_rssi\",\"stat_t\":\"%s\","
    "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\","
    "\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_RSSI, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_tower/rssi/config", buf, true);
}
#else
void publishDiscovery() {}
#endif

bool mqttReconnect() {
  // One non-blocking attempt per interval (must stay well under WDT_TIMEOUT).
  if (millis() - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL_MS) return false;
  lastMqttReconnectAttempt = millis();

  Serial.print("MQTT connecting to ");
  Serial.print(SECRET_MQTT_HOST);
  Serial.print(":");
  Serial.println(SECRET_MQTT_PORT);

  // mqtt.connect() 會阻塞（TCP 連線 + 等 CONNACK，可達數秒）。在阻塞前後各餵一次
  // 看門狗，避免連續失敗的重連嘗試把 25s WDT 視窗吃滿。
  esp_task_wdt_reset();
  lastWdtReset = millis();

  // LWT: broker publishes "offline" (retained) if we drop uncleanly.
  bool ok = mqtt.connect(SECRET_MQTT_CLIENTID, SECRET_MQTT_USER, SECRET_MQTT_PASS,
                         TOPIC_AVAIL, 1, true, "offline");

  esp_task_wdt_reset();
  lastWdtReset = millis();
  if (ok) {
    Serial.println("MQTT connected");
    mqtt.publish(TOPIC_AVAIL, "online", true);
    publishDiscovery();
  } else {
    Serial.print("MQTT connect failed, state=");
    Serial.println(mqtt.state());
  }
  return ok;
}

// ===========================================================================
// Sensor collection (unchanged logic from the original sketch)
// ===========================================================================
bool collectDate(void*) {
  temp += analogRead(INPIN) / 10;
  sampleCount += 1;
  if (sampleCount >= TIMES2AVG) {
    float avg = temp / sampleCount;
    temp = 0;
    sampleCount = 0;

    depth[depth_index] = avg;
    if (depth_index >= DEPTH_COUNT - 1) {
      over1loop = true;
      depth_index = 0;
    } else {
      depth_index++;
    }

    // 對 9-slot 視窗求平均，再餵進 EMA 多平滑一層。
    double sum = 0;
    int n = over1loop ? DEPTH_COUNT : depth_index;
    if (n <= 0) n = 1;
    for (int i = 0; i < n; i++) sum += depth[i];
    float windowAvg = (float)(sum / n);

    if (!haveReading) {
      smoothedLevel = windowAvg;   // 第一筆直接帶入，避免從 0 慢慢爬上來
      haveReading = true;
    } else {
      smoothedLevel = EMA_ALPHA * windowAvg + (1.0f - EMA_ALPHA) * smoothedLevel;
    }

    if (avg_max < smoothedLevel) avg_max = smoothedLevel;
    if (avg_min > smoothedLevel) avg_min = smoothedLevel;
  }
  return true; // repeat
}

bool sendData(void*) {
  // 一律發佈目前最新的平滑水位（不再等湊滿一整批 5000 取樣）。
  // 只要還沒有任何讀值就先跳過，避免在開機初期送出 0。
  if (!haveReading) return true;

  // Publish the live water level to MQTT (retain=false — the pump's 60s failsafe
  // relies on NOT receiving stale retained values on reconnect).
  // 水位輸出精確到小數點下一位。
  bool ok = false;
  if (mqtt.connected()) {
    ok = publishFloat(TOPIC_WATER, smoothedLevel, 1, false);
    publishFloat(TOPIC_AVG_MAX, avg_max, 1, true);
    publishFloat(TOPIC_AVG_MIN, avg_min, 1, true);
  }
  Serial.printf("sendData: %.1f, max: %.1f, min: %.1f (mqtt pub=%d)\n", smoothedLevel, avg_max, avg_min, ok);
  return true; // repeat
}

bool publishUptime(void*) {
  if (mqtt.connected()) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
    mqtt.publish(TOPIC_UPTIME, buf, true);
    snprintf(buf, sizeof(buf), "%d", (int)WiFi.RSSI());
    mqtt.publish(TOPIC_RSSI, buf, true);
  }
  return true; // repeat
}

// ===========================================================================
// Setup / loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("Configuring WDT...");
  esp_task_wdt_init(WDT_TIMEOUT, false); // panic disabled (matches original tower behavior)
  esp_task_wdt_add(NULL);
  lastWdtReset = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);   // 讓 WiFi 斷線時自動於背景重連，縮短每次卡住的時間
  WiFi.persistent(false);
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    delay(500);
    esp_task_wdt_reset();
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed (will keep retrying in loop)");
  }

  // PubSubClient 不會限制底層 TCP connect 的逾時（ESP32 預設 ~3s 無上限保護），
  // 所以直接在 WiFiClient 上設逾時，弱訊號時 connect 阻塞才不會拖長。
  espClient.setTimeout(2000); // ms

  mqtt.setServer(SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  mqtt.setBufferSize(512);
  // socketTimeout 主要影響接收路徑（等 CONNACK / 讀封包）。從 5s 降到 2s，
  // 弱訊號時每次連線失敗的阻塞時間更短，避免多次重連疊加成 40~80s 的空窗。
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(15);
  mqttReconnect();

  timer_sensor.every(2, collectDate);
  timer_send.every(1000, sendData);
  timer_uptime.every(1000, publishUptime);

  Serial.println("Tower side (MQTT) startup complete");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  } else if (!mqtt.connected()) {
    mqttReconnect();
  } else {
    mqtt.loop();
  }

  timer_sensor.tick();
  timer_send.tick();
  timer_uptime.tick();
  reset_wdt();
}
