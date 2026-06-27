# pump_side_ai2 — 1F 水泵 + 鐵門控制器（重寫版）

水鴨計畫 1F 端 ESP32 控制器的乾淨重寫版。驅動水泵、鐵門繼電器，透過 MQTT +
Home Assistant 控制，並有安全關鍵的失聯保護（60 秒收不到水位就強制停泵）。

> ⚠️ **尚未實機驗證。** 在通過下方「實機驗證清單」前，`pump_side_ai/`（單檔版）
> 仍是部署版本。本版用獨立資料夾，不影響 ai 版。

---

## 架構（多檔 — Arduino 同資料夾多 tab 一起編譯）

| 檔案 | 職責 |
|---|---|
| `pump_side_ai2.ino` | `setup()` / `loop()` 編排 |
| `config.h` | GPIO 腳位、MQTT topic、門檻預設、各 interval、`PumpStatus` enum、裝置 IP（集中一處）|
| `arduino_secrets.h` | WiFi / MQTT 憑證（gitignored；由 `.example.h` 複製）|
| `commands.{h,cpp}` | **命令佇列**：portMUX 保護的環狀佇列 + 水位入口（`enqueueCommand`/`recordWater`/`dequeueCommand`）|
| `logging.h` | 輕量 Serial log |
| `wifi_mgr.{h,cpp}` | 非阻塞 WiFi 連線 + 自動重連 |
| `ntp_time.{h,cpp}` | 背景（非阻塞）NTP 同步 + 快取本地時間 + prefill/quiet 時窗判斷 |
| `pump_control.{h,cpp}` | 水泵狀態機、過熱保護、門檻 setter（存 NVS）、blink |
| `failsafe.{h,cpp}` | 時間戳比對失聯保護 |
| `door_control.{h,cpp}` | 鐵門點動繼電器 |
| `mqtt_mgr.{h,cpp}` | MQTT 連線/LWT/重連、HA discovery、狀態發布（ArduinoJson）、inbound callback |
| `webui.{h,cpp}` | 極簡本地 fallback（狀態頁 + `/get` 只入列命令）|

### 安全模型（核心）
**loop 執行緒是所有硬體、計時器、pump 狀態的唯一擁有者。** web / MQTT handler
（跑在 AsyncTCP 執行緒）**只**呼叫 `enqueueCommand()` / `recordWater()`；`loop()`
透過 `drainCommands()` 取出並執行。失聯保護是 `loop()` 裡的 `millis()` 時間戳比對。

---

## 與 ai 版（`pump_side_ai/`）的差異

| 面向 | ai 版 | ai2 版 |
|---|---|---|
| **結構** | 單一 `.ino`（~1708 行）| 19 個模組檔（~1339 行）|
| **執行緒安全** | web/MQTT handler 直接動硬體/計時器（AsyncTCP 執行緒）→ race，pump 可能卡開/卡關 | handler 只入列命令；loop 單一擁有者 |
| **失聯保護** | `timer_bad_connection` cancel/re-arm（cancel 會失敗，`cannotcanceltimerrrrrr` 計數）| `millis()` 時間戳比對，無 race |
| **過熱復原** | 無條件 `request_pump_to(RUNNING)` | 重新評估當下水位才決定 |
| **過熱冷卻** | force-stop 會中斷 10 分鐘冷卻 | 過熱中 stop 不取消 recover timer（保護冷卻）|
| **最大開泵時間** | 無 | 新增 `PUMP_MAX_ON_MS` 絕對上限防線 |
| **水位儲存** | `String message` 反覆 `toFloat()` | 入口解析一次成 `float` |
| **門檻 max/min** | 全域變數，**重開機還原** 120/70 | 存 NVS（Preferences），重開保留 |
| **deficient（prefill 目標）** | `(max-min)*0.3+min` 自動算 | **使用者可調**（HA number entity），存 NVS、重開保留 |
| **JSON** | String 拼接（跳脫 bug、heap 碎裂）| ArduinoJson |
| **status 發布** | SSE + MQTT 兩條，可能在 AsyncTCP 執行緒跑 | 只在 loop 產生 → mutex cache，web 端複製 |
| **Web UI** | 完整 SPA + SSE + 256 筆 log buffer（~19.5KB RAM）| 極簡 fallback（狀態頁 + `/get`）|

**刻意保留（行為一致，只換實作）**：失聯保護 60 秒、過熱保護（20 分觸發 / 10 分冷卻）、
prefill 時窗、鐵門點動 200ms、MQTT topic、HA discovery entity、LWT、WiFi 非阻塞重連、
NTP 背景同步。

---

## 燒錄需求

**Arduino library**（Library Manager 安裝）：
- PubSubClient（knolleary，≥ 2.8）
- arduino-timer（contrem）
- **ArduinoJson — 本版用 v6（已實測 6.21.2）**。注意：`StaticJsonDocument<512>`
  在 v6 可用；若升級到 **v7** 要把 `mqtt_mgr.cpp` 的 `buildStatusJson` 改成
  `JsonDocument doc;`（v7 移除了 StaticJsonDocument）。
- ESPAsyncWebSrv + AsyncTCP
- NTPClient

**Secrets**：`cp arduino_secrets.example.h arduino_secrets.h`，填入 `SECRET_MQTT_PASS`。
`SECRET_MQTT_HOST` 已是 `192.168.1.215`（zz0004），WiFi SSID 已是 `iHome`。
板子用 **數字 IP**，不要用 `.local`（ESP32 不解析 mDNS）。

**板子**：NodeMCU-32S（ESP32）。靜態 IP `192.168.1.217`。

---

## 實機驗證清單（安全關鍵 — 全部通過才取代 ai 版）

1. **編譯** 過（上述 library 齊全）。
2. **基本上線**：Serial 看到 `Connecting to WiFi SSID: iHome` → `IP Address: 192.168.1.217`
   → `MQTT connected`；`mosquitto_sub -t 'wd/#'` 看到 `wd/pump/state/status`；
   HA 自動出現 **WD Pump (1F)** 裝置 + entity。
3. **執行緒安全壓測**：同時（a）HA 連發 manual_pump RUN/STOP + door 命令、（b）tower 持續發水位，
   觀察數分鐘 → pump GPIO 與 `pump_status` 一致、不卡開/卡關、不重開機。
4. **失聯保護**：pump 運轉中關掉 tower / broker → 60 秒內強制停泵、`wd/pump/state/bad_conn` 變 ON；
   恢復供水 → 自動清除回正常。
5. **過熱 + 復原**：縮短 interval 測 overheat → 冷卻 → 復原會依**當下水位**決定是否重開
   （不盲目開）；冷卻期間觸發失聯，冷卻不被中斷。
6. **NVS 持久化**：HA 改 max/min/deficient → 重開機 → 值保留（不還原預設）。
7. **fallback**：broker 關閉時，`http://192.168.1.217/` 極簡頁可開、`/get?manualpump=1` 仍能入列執行。
8. **看門狗**：全程不得出現 `task_wdt: ... Aborting`。
9. 全部通過後，才以 ai2 取代 ai（更名或切換部署）。

---

## MQTT topics（state，皆 retain=true）

| topic | 內容 |
|---|---|
| `wd/pump/state/status` | 完整狀態 JSON（water / pump_status / uptime_s / time …）|
| `wd/pump/state/pump_status` | `RUNNING` / `STOPPED` / `OVERHEAT_PROTECTION` |
| `wd/pump/state/min_level`、`.../max_level` | 目前門檻 |
| `wd/pump/state/deficient_level` | prefill 目標水位（使用者可調 → HA「Deficient Water Level」number）|
| `wd/pump/state/bad_conn` | 失聯保護 `ON` / `OFF` |
| `wd/pump/state/rssi` | WiFi 訊號強度（dBm）→ HA「WiFi Signal」診斷 entity |

可訂閱：`wd/tower/state/water`（tower 水位）。命令：`wd/pump/cmd/#`。
HA discovery 全部掛在 **WD Pump (1F)** 裝置下。

---

## 跨裝置依賴（重要）

tower 必須以 **retain=false** 發 `wd/tower/state/water`。否則 pump 重連時 broker 會重播
舊水位 → 假性餵活失聯保護（明明 tower 已死卻不觸發停泵）。`tower_side_ai` 已是 retain=false。
