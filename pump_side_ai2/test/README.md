# pump_side_ai2 — 主機端單元測試（非實機）

在 Mac/Linux 上用 `clang++` 直接跑，**不需要 ESP32、不需要 arduino-cli**。
編譯的是**真正的** `commands.cpp` / `pump_control.cpp` / `failsafe.cpp`，只把
Arduino 核心 API（`millis`/`digitalWrite`/`Preferences`/`arduino-timer`…）換成
`fakes/` 裡的假實作。時間由可控的 mock clock 提供（`mockAdvance(ms)`），所以
失聯保護 60 秒、過熱 20/10 分、最大開泵 25 分這些 `millis()` 邏輯都能**瞬間**
跑完、可重現。

## 跑法

```bash
bash test/run.sh            # 一般：每個測試講故事，隱藏 firmware log
VERBOSE=1 bash test/run.sh  # 連 firmware 自己的 Serial log 一起顯示（dim 的 fw| 行）
```

預期最後一行：`═══ 19 tests, 0 passed ═══`。

### 輸出怎麼讀

```
── below_min_starts_pump            <- 測試名稱
   Water reads BELOW the min ...     <- 這個測試在驗證什麼情境（DESC）
      • tower reports water = 50      <- 測試做的動作（STEP）
      • after check_water_level -> pump is RUNNING, relay ON
      ✓ pump should run when water below min   <- 一條斷言（綠 ✓ 過 / 紅 ✗ 失敗）
      ✓ status RUNNING below min
   PASS
```

`VERBOSE=1` 時會多出 dim 的 `fw| ...` 行，那是 firmware 自己印到 Serial 的東西
（`[WARN]`/`[PHYSICAL]`/`[check_water_level]`…），方便對照「程式內部實際發生了什麼」。

## 涵蓋範圍（對應 README「實機驗證清單」的邏輯部分）

| 區塊 | 測到的行為 |
|---|---|
| 水位門檻 | 低於 min 開泵、高於 max 停泵、deadband 不動、0/invalid 不動作 |
| **失聯保護** | 視窗內不誤觸發、視窗後觸發並強制停泵、恢復供水後清除、latch 期間拒絕開泵 |
| **過熱狀態機** | 運轉滿時觸發、復原時**依當下水位**決定是否重開、冷卻期間 force-stop 不中斷冷卻 |
| 最大開泵上限 | 卡開無過熱時的絕對防線會強制停泵 |
| 門檻 setter / NVS | set max/min 驗證與持久化、重開機（pumpInit）後保留 |
| 命令佇列 | FIFO 順序與參數、滿了丟棄不損毀 |

## 測不到的部分（仍需實機或整合測試）

- MQTT 實際連線 / LWT / HA discovery（`mqtt_mgr.cpp` 未編入；需 broker）
- WiFi 重連、NTP 同步、Web fallback、看門狗
- FreeRTOS 真實多執行緒競態（主機是單執行緒；portMUX 為 no-op，
  測的是「命令佇列協定正確」而非真實 race）

## 結構

```
test/
  run.sh          # clang++ 編譯 + 執行
  test_main.cpp   # 測試 + ntp_time stub + mock 全域
  fakes/
    Arduino.h        # 可控 millis、假 GPIO、最小 String、Serial、portMUX no-op
    arduino-timer.h  # 對齊 contrem/arduino-timer 的 in/every/cancel/tick
    Preferences.h    # 記憶體版 NVS
```
