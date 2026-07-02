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
#define INPIN 32             // ADC1_CH4 (GPIO32)。ADC1 不受 WiFi 影響（WiFi 用 ADC2）
#define DEPTH_COUNT 9
#define TIMES2AVG 300        // 每湊滿這麼多次 analogRead 產生一個批次平均值
#define EMA_ALPHA 0.30f      // 對「9-slot 視窗平均」再做一層 EMA；alpha 越小越平滑、反應越慢

// 取樣已搬到獨立的 FreeRTOS task（samplingTask, core 1），不再跑在 loop() 上，
// 所以 loop() 裡的 MQTT/WiFi socket 阻塞「不會」再凍結 ADC 取樣（Fix 3）。
//
// 以下四個是「跨 task 共享」變數：只由 samplingTask 寫、只由 loop()/sendData 讀。
// 每個都是自然對齊的單字組純量（float/bool）→ 在 Xtensa LX6 上 load/store 為單一
// 指令，天生 atomic；ESP32 內部 SRAM 不經 per-core cache，兩核共享，無 cache 一致性
// 問題。因此 volatile（強制編譯器實際讀寫、不快取在暫存器）就足夠，不需要 portMUX/mutex。
// 三個 float 各自發到「獨立」的 MQTT topic，不需要被當成同一筆快照一起讀，
// 且 avg_max/avg_min 是 smoothedLevel 的單調極值，撕裂讀也不會破壞 min<=level<=max。
volatile float smoothedLevel = 0.0f;
volatile float avg_max = 0.0f;
volatile float avg_min = 401.0f;
volatile bool  haveReading = false;   // true once at least one batch has produced a value

// samplingTask 私有狀態（只有該 task 碰，刻意不加 volatile）。
static float depth[DEPTH_COUNT] = {};
static int   depth_index = 0;
static bool  over1loop = false;
static float temp = 0.0f;
static int   sampleCount = 0;

TaskHandle_t samplingTaskHandle = nullptr;

// ---- Timers (取樣 timer 已移除，改由 samplingTask 負責) ----
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
// Sensor collection (sampling logic unchanged; now driven by samplingTask)
// ===========================================================================
// ADC1/GPIO32 的唯一擁有者。只能由 samplingTask 呼叫。
void collectDateOnce() {
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

    // 先在 task-local 算好，EMA 的 read-modify-write 讀的是上一個 smoothedLevel。
    float newLevel;
    if (!haveReading) {
      newLevel = windowAvg;   // 第一筆直接帶入，避免從 0 慢慢爬上來
    } else {
      newLevel = EMA_ALPHA * windowAvg + (1.0f - EMA_ALPHA) * smoothedLevel;
    }

    // 寫入順序很重要：先發佈水位，極值次之，最後才升起 haveReading 閘門，
    // 確保 consumer 看到 haveReading==true 時 smoothedLevel 已是新值。
    smoothedLevel = newLevel;
    if (avg_max < newLevel) avg_max = newLevel;
    if (avg_min > newLevel) avg_min = newLevel;
    haveReading = true;
  }
}

// 專責 ADC 取樣的 task。優先權 = loopTask(1)，同核 round-robin 分時，
// 因此永遠不會餓死唯一餵看門狗的 loopTask。vTaskDelayUntil 提供無漂移的固定
// 節奏，且每次迭代「一定」block（讓出 core），所以 loopTask/IDLE1 都跑得到、
// 看門狗不會誤觸。loop() 裡 mqtt.connect() 阻塞時 loopTask 進入 Blocked，
// core 1 讓給本 task → 取樣持續不中斷（這就是 Fix 3 的核心）。
void samplingTask(void*) {
  const TickType_t kPeriod = pdMS_TO_TICKS(5);   // ~5ms → 300 取樣的批次約 1.5s
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    collectDateOnce();
    vTaskDelayUntil(&last, kPeriod);   // 必須維持每次迭代無條件讓出
  }
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
#if ESP_IDF_VERSION_MAJOR >= 5
  // ESP32 Arduino core 3.x / IDF v5+: the core already inits the TWDT, so
  // reconfigure it instead of init (init returns "already initialized").
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = (uint32_t)WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = false, // panic disabled (matches original tower behavior)
  };
  esp_task_wdt_reconfigure(&wdtConfig);
#else
  // ESP32 Arduino core 2.x / IDF v4: (timeout_s, panic).
  esp_task_wdt_init(WDT_TIMEOUT, false); // panic disabled (matches original tower behavior)
#endif
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

  // 在 samplingTask 建立前，於本 task 先觸發一次 ADC 的 lazy init，
  // 確保第一次初始化是單執行緒完成（保險用）。
  analogRead(INPIN);

  // 取樣 timer 已移除；sendData / uptime 仍由 loop() 的 arduino-timer 驅動。
  timer_send.every(1000, sendData);
  timer_uptime.every(1000, publishUptime);

  // 在 core 1 以 loopTask 同優先權(1) 啟動 ADC 取樣 task —— 詳見 samplingTask 註解。
  xTaskCreatePinnedToCore(samplingTask, "sample", 4096, NULL, 1,
                          &samplingTaskHandle, APP_CPU_NUM);

  Serial.println("Tower side (MQTT) startup complete");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  } else if (!mqtt.connected()) {
    mqttReconnect();
  } else {
    mqtt.loop();              // 可能阻塞在 socket；samplingTask 仍持續取樣
  }

  timer_send.tick();
  timer_uptime.tick();
  reset_wdt();
}
