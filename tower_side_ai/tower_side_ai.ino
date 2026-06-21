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
bool  needUpdate = false;
bool  over1loop = false;

float temp = 0;
#define TIMES2AVG 5000 // 取幾次做平均
int   sampleCount = 0;

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

bool mqttReconnect() {
  // One non-blocking attempt per interval (must stay well under WDT_TIMEOUT).
  if (millis() - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL_MS) return false;
  lastMqttReconnectAttempt = millis();

  Serial.print("MQTT connecting to ");
  Serial.print(SECRET_MQTT_HOST);
  Serial.print(":");
  Serial.println(SECRET_MQTT_PORT);

  // LWT: broker publishes "offline" (retained) if we drop uncleanly.
  bool ok = mqtt.connect(SECRET_MQTT_CLIENTID, SECRET_MQTT_USER, SECRET_MQTT_PASS,
                         TOPIC_AVAIL, 1, true, "offline");
  if (ok) {
    Serial.println("MQTT connected");
    mqtt.publish(TOPIC_AVAIL, "online", true);
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
    needUpdate = true;
  }
  return true; // repeat
}

bool sendData(void*) {
  if (!needUpdate) return true;

  double sum = 0, avg = 0;
  if (over1loop) {
    for (int i = 0; i < DEPTH_COUNT; i++) sum += depth[i];
    avg = sum / DEPTH_COUNT;
  } else {
    for (int i = 0; i < depth_index; i++) sum += depth[i];
    avg = (depth_index > 0) ? sum / depth_index : 0;
  }

  if (avg_max < avg) avg_max = avg;
  if (avg_min > avg) avg_min = avg;

  needUpdate = false;

  // Publish the live water level to MQTT (retain=false — the pump's 60s failsafe
  // relies on NOT receiving stale retained values on reconnect).
  bool ok = false;
  if (mqtt.connected()) {
    ok = publishFloat(TOPIC_WATER, (float)avg, 4, false);
    publishFloat(TOPIC_AVG_MAX, avg_max, 4, true);
    publishFloat(TOPIC_AVG_MIN, avg_min, 4, true);
  }
  Serial.printf("sendData: %.4f, max: %.4f, min: %.4f (mqtt pub=%d)\n", avg, avg_max, avg_min, ok);
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

  mqtt.setServer(SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(5);
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
