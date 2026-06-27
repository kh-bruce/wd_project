// =====================================================================
// config.h — all non-secret tunables for the pump-side controller.
// Secrets (WiFi/MQTT credentials) live in arduino_secrets.h.
// =====================================================================
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ---- Device identity / network ----
namespace cfg {
  // Static IP for this board (the 1F pump+garage controller).
  // Single source of truth — used by WiFi config AND the HA discovery URL.
  static const IPAddress DEVICE_IP(192, 168, 1, 217);
  static const IPAddress GATEWAY(192, 168, 1, 200);
  static const IPAddress SUBNET(255, 255, 255, 0);
  static const IPAddress DNS(192, 168, 1, 200);
  static const char* DEVICE_URL = "http://192.168.1.217/"; // for HA configuration_url

  // ---- Watchdog ----
  static const int WDT_TIMEOUT_S = 30; // panic enabled

  // ---- GPIO (relays are ACTIVE-LOW: LOW = energized/on) ----
  static const uint8_t PIN_STATUS_LED = 2;
  static const uint8_t PIN_PUMP_RELAY = 4;
  static const uint8_t PIN_DOOR_UP    = 16;
  static const uint8_t PIN_DOOR_DOWN  = 17;
  static const uint8_t PIN_DOOR_STOP  = 18;
  static const uint8_t RELAY_ON  = LOW;
  static const uint8_t RELAY_OFF = HIGH;

  // ---- Water level thresholds (defaults; max/min/deficient persisted to NVS) ----
  static const float DEFAULT_MAX_LEVEL       = 120.0f; // 實測最大值 83；2023/11 外部最大壓力測試 122
  static const float DEFAULT_MIN_LEVEL       = 70.0f;  // 實測最小值 46
  static const float DEFAULT_DEFICIENT_LEVEL = 85.0f;  // prefill 目標水位（使用者可調，存 NVS）

  // ---- Time windows (hours, local UTC+8) ----
  static const int PREFILL_HOUR_START = 20; // 預先補水區間開始
  static const int PREFILL_HOUR_END   = 23; // 預先補水區間結束
  static const int QUIET_HOUR_START   = 23; // badtime 開始
  static const int QUIET_HOUR_END     = 6;  // badtime 結束

  // ---- Timing (ms) ----
  static const unsigned long OVERHEAT_TRIP_MS     = 20UL * 60 * 1000; // 多久後啟動過熱保護
  static const unsigned long OVERHEAT_RECOVER_MS  = 10UL * 60 * 1000; // 過熱停機散熱時間
  static const unsigned long MANUAL_PUMP_MS       = 5UL * 60 * 1000;  // manual run duration
  static const unsigned long PUMP_MAX_ON_MS       = 25UL * 60 * 1000; // absolute hard cap (defense in depth)
  static const unsigned long RELAY_PULSE_MS       = 200;              // 遙控器點擊停留
  static const unsigned long BAD_CONN_DELAY_MS    = 60UL * 1000;      // no water -> bad-conn
  static const unsigned long PREFILL_CHECK_MS     = 5UL * 60 * 1000;
  static const unsigned long HEAP_CHECK_MS        = 60UL * 60 * 1000;
  static const unsigned long MQTT_RECONNECT_MS    = 5000;
  static const unsigned long WIFI_RETRY_MS        = 10000;
  static const unsigned long STATUS_PUBLISH_MS    = 2000;

  // ---- Blink intervals (ms) ----
  static const int BLINK_NORMAL_MS   = 1000; // waiting / pump on
  static const int BLINK_OVERHEAT_MS = 250;  // overheat protecting
  static const int BLINK_BADCONN_MS  = 50;   // no-conn mode
}

// ---- Pump state machine ----
enum PumpStatus {
  RUNNING,            // 運作中
  STOPPED,            // 已停止
  OVERHEAT_PROTECTION // 過熱保護
};

// ---- MQTT topics ----
namespace topic {
  static const char* AVAIL          = "wd/pump/avail";
  static const char* STATUS         = "wd/pump/state/status";
  static const char* PUMP_STATUS    = "wd/pump/state/pump_status";
  static const char* MIN_LEVEL      = "wd/pump/state/min_level";
  static const char* MAX_LEVEL      = "wd/pump/state/max_level";
  static const char* DEFICIENT_LEVEL = "wd/pump/state/deficient_level";
  static const char* BAD_CONN       = "wd/pump/state/bad_conn";
  static const char* RSSI           = "wd/pump/state/rssi"; // WiFi signal (dBm), retain=true
  static const char* SUB_WATER      = "wd/tower/state/water"; // tower publishes retain=false (failsafe depends on it)
  static const char* CMD_WILDCARD   = "wd/pump/cmd/#";
  static const char* CMD_SETMAX     = "wd/pump/cmd/set_max_level";
  static const char* CMD_SETMIN     = "wd/pump/cmd/set_min_level";
  static const char* CMD_SETDEFICIENT = "wd/pump/cmd/set_deficient_level";
  static const char* CMD_MANUALPUMP = "wd/pump/cmd/manual_pump";
  static const char* CMD_DOOR       = "wd/pump/cmd/door";
}

// Set to 0 to define HA entities manually in YAML instead of auto-discovery.
#define USE_HA_DISCOVERY 1

#endif // CONFIG_H
