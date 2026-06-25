/**
  2023水鴨計畫 — PUMP SIDE (1F pump + garage)
  board: nodemcu (esp32)

  MQTT migration (was: water level arrived via HTTP /get?message=).
  This sketch ADDS an MQTT client ALONGSIDE the existing web server:
    - subscribes to wd/tower/state/water  -> feeds check_water_level(.., true),
      which resets the 60s bad-connection failsafe exactly like the old HTTP path
    - subscribes to wd/pump/cmd/#          -> pump / door / setmax / setmin commands
    - publishes wd/pump/state/status (makeStatusJson) + flat state topics
    - publishes Home Assistant MQTT Discovery so entities auto-appear
  The web UI + /get endpoints are KEPT as a local fallback control surface.

  IMPORTANT: all mqtt.* calls happen on the loop() thread only (never inside an
  AsyncWebServer callback) — arduino-timer / pump control are not thread-safe.
**/
#include <esp_task_wdt.h>
#define WDT_TIMEOUT 30
#define WOOOOOOOOOF 18 // must smaller then WDT_TIMEOUT
int last = 0;
bool stop_wdt = false; // set to true to reset devicesser
#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266) clearyncTCP.h >
#endif
#include <ESPAsyncWebSrv.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include "arduino_secrets.h"
AsyncWebServer server(80);
AsyncEventSource events("/events");
const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASS;

// ---- MQTT ----
WiFiClient mqttWifiClient;
PubSubClient mqtt(mqttWifiClient);
unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
bool wasMqttConnected = false;
// Discovery is published once per (re)connect. Set to 0 to use manual HA YAML instead.
#define USE_HA_DISCOVERY 1

const char* TOPIC_AVAIL          = "wd/pump/avail";
const char* TOPIC_STATUS         = "wd/pump/state/status";
const char* TOPIC_PUMP_STATUS    = "wd/pump/state/pump_status";
const char* TOPIC_MIN_LEVEL      = "wd/pump/state/min_level";
const char* TOPIC_MAX_LEVEL      = "wd/pump/state/max_level";
const char* TOPIC_BAD_CONN       = "wd/pump/state/bad_conn";
const char* TOPIC_SUB_WATER      = "wd/tower/state/water";
const char* TOPIC_CMD_WILDCARD   = "wd/pump/cmd/#";
const char* TOPIC_CMD_SETMAX     = "wd/pump/cmd/set_max_level";
const char* TOPIC_CMD_SETMIN     = "wd/pump/cmd/set_min_level";
const char* TOPIC_CMD_MANUALPUMP = "wd/pump/cmd/manual_pump";
const char* TOPIC_CMD_DOOR       = "wd/pump/cmd/door";

void applySetMax(float f);
void applySetMin(float f);
void publishPumpState();
const char *PARAM_INPUT_WATERLEVEL = "message"; //test
const char *PARAM_INPUT_FRONTDOOR = "frontdoor";
const char *PARAM_INPUT_SETMAXLEVEL = "setmaxlevel";
const char *PARAM_INPUT_SETMINLEVEL = "setminlevel";
const char *PARAM_INPUT_MANUALPUMP = "manualpump";
const char *PARAM_INPUT_MANUALPUMPSTOP = "manualpumpstop";
String message = "null";
unsigned long lastWaterLevelUpdateMs = 0;
#include <arduino-timer.h>
auto timer_blink = timer_create_default();
auto timer_1 = timer_create_default(); // auto start pump when bootup
auto timer_2 = timer_create_default(); // 過熱保護
auto timer_3 = timer_create_default(); // 過熱保護復歸
auto timer_relay = timer_create_default(); // 鐵門 frontdoor relay
auto timer_bad_connection = timer_create_default(); // how long till enter "no conn mode"
auto timer_ntp = timer_create_default(); // 20230808 ntp 時間功能
auto timer_manual_pump = timer_create_default(); // manual pump auto-stop
auto timer_events = timer_create_default(); // SSE debounce / pacing
auto timer_heap = timer_create_default(); // heap monitoring
auto timer_prefill = timer_create_default(); // prefill time window checks
auto timer_mqtt_status = timer_create_default(); // MQTT status publish (loop thread)

Timer<>::Task statusPushTask;
Timer<>::Task prefillTask;
unsigned long lastStatusSentMs = 0;
const unsigned long STATUS_DEBOUNCE_MS = 1000; // max 1 push per second
bool statusPushScheduled = false;
bool prefillTimerStarted = false;
const unsigned long PREFILL_CHECK_INTERVAL_MS = 5 * 60 * 1000; // 5 minutes
bool isTimeInRange(void *argument /* optional argument given to in/at/every */);
const int timer_heapcheck_interval = 60 * 60 * 1000; // ms // heap monitoring interval

void set_timer_blink_interval_to(int interval);
const int normal_blink_interval = 1000; // ms // when normal -> waiting & pump is on
const int overheated_blink_interval = 250; // ms // when over heat protecting
const int badconnmode_blink_interval = 50; // ms // when "no conn mode" active
const int timer_1_delay = (WDT_TIMEOUT + 1) * 1000; // ms // how long after bootup
const int timer_2_interval = 20 * 60 * 1000; // ms // 多久時間後啟動過熱保護
const int timer_3_interval = 10 * 60 * 1000; // ms // 過熱保護的停機散熱時間
const int relay_open_interval = 200; // 控制遙控器點擊的停留時間
const int timer_bad_connection_delay =  60 * 1000; // ms // how long till enter "no conn mode"
const long manual_pump_duration_ms = 5 * 60 * 1000; // manual run duration
float MAX_WATER_LEVEL = 120; // 實測最大值 83 // 2023111月底外部最大壓力測試 122
float MIN_WATER_LEVEL = 70; // 實測最小值 46
float DEFICIENT_WATER_LEVEL = 70; // 預先補水啟動補水之水位 // Deficient // Insufficient
const int isTimeInRange_min = 20; // 預先補水功能 補水時間區間開始
const int isTimeInRange_max = 23; // 預先補水功能 補水時間區間結束
const int isBadTimeBetween_from = 23; // badtime start at (default 22)
const int isBadTimeBetween_to = 6; // badtime ends at (default 6)
bool blink = true;
bool bad_conn_mode = false;
#define GPIO4PUMP 4 // motor // 麵包板7
#define GPIO4UP 16 //up // 麵包板8
#define GPIO4DOWN 17 //down // 麵包板9
#define GPIO4STOP 18 //stop // 麵包板11
// 右邊6負極
enum pumpStatus {
  RUNNING, // 運作中
  STOPPED, // 已停止
  OVERHEAT_PROTECTION // 過熱保護
};
typedef enum pumpStatus PumpStatus;
PumpStatus pump_status = STOPPED; // 初始化為已停止狀態
long ms = 0;

// 20230808 ntp 時間功能
// #include <NTPClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h> // restore include to define NTPClient
WiFiUDP ntpUDP;
const char* ntpServers[] = {
  "pool.ntp.org",
  "time.nist.gov",
  "time.google.com",
  "time.windows.com"
};
NTPClient timeClient(ntpUDP, "time.nist.gov");
int currentNtpServerIndex = 0;
String lastTimeServer = "";
unsigned long lastTimeSyncMs = 0;
unsigned long lastTimeEpoch = 0;
unsigned long lastCommandMs = 0;
String lastCommand = "none";
bool hasTimeSync = false;
int ntpAttemptIndex = 0;
Timer<>::Task ntpTask;
extern int bad_conn_count;
extern int cannotcanceltimerrrrrr;
// Fallback direct IPs to avoid DNS issues
const char* ntpServersIpFallback[] = {
  "129.6.15.28",   // time-a-g.nist.gov
  "216.239.35.0",  // time.google.com (one of the anycast IPs)
  "133.243.238.244" // ntp.nict.jp (Japan NICT)
};

// Serve the UI from flash to avoid heap churn each request
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>iHOME Remote</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;600&display=swap');
    :root {
      --bg: #05131e;
      --card: rgba(10, 35, 54, 0.78);
      --accent: #22d3ee;
      --accent-strong: #0ea5e9;
      --text: #e6f6ff;
      --muted: #b7d9ec;
      --shadow: 0 14px 45px rgba(0,0,0,0.35);
      --radius: 18px;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: 'Space Grotesk', system-ui, sans-serif;
      background: radial-gradient(circle at 20% 20%, rgba(34,211,238,0.12), transparent 34%),
          radial-gradient(circle at 78% 10%, rgba(14,165,233,0.12), transparent 32%),
          linear-gradient(135deg, #04101c 0%, #0a2035 100%);
      color: var(--text);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 24px;
    }
    .shell {
      width: min(900px, 100%);
      background: var(--card);
      border: 1px solid rgba(255,255,255,0.09);
      border-radius: var(--radius);
      padding: 28px;
      box-shadow: var(--shadow);
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-bottom: 18px;
    }
    .header-actions { display: inline-flex; align-items: center; gap: 10px; flex-wrap: wrap; }
    .title {
      font-size: 24px;
      font-weight: 600;
      letter-spacing: 0.4px;
    }
    .status {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 8px 14px;
      border-radius: 999px;
      background: rgba(255,255,255,0.07);
      color: var(--muted);
      font-size: 14px;
      border: 1px solid rgba(255,255,255,0.08);
    }
    .status .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--muted);
      box-shadow: 0 0 12px rgba(255,255,255,0.15);
    }
    .status.on { color: var(--text); }
    .status.on .dot { background: var(--accent); box-shadow: 0 0 12px rgba(251,146,60,0.55); }
    .status.hold .dot { background: var(--accent-strong); box-shadow: 0 0 12px rgba(249,115,22,0.6); }
    .status.action { cursor: pointer; border: 1px solid rgba(255,255,255,0.1); background: rgba(255,255,255,0.05); gap: 6px; }
    .status.action:hover { border-color: rgba(255,255,255,0.2); background: rgba(255,255,255,0.08); }
    .status.action svg { width: 14px; height: 14px; fill: var(--text); opacity: 0.9; }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
      gap: 14px;
    }
    .card {
      background: rgba(255,255,255,0.08);
      border: 1px solid rgba(255,255,255,0.10);
      border-radius: 16px;
      padding: 18px;
      display: flex;
      flex-direction: column;
      gap: 12px;
      transition: transform 0.2s ease, border 0.2s ease;
    }
    .card:hover { transform: translateY(-2px); border-color: rgba(255,255,255,0.1); }
    .label { color: var(--muted); font-size: 14px; }
    .button {
      width: 100%;
      padding: 14px;
      border-radius: 12px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: transform 0.12s ease, box-shadow 0.12s ease, border 0.12s ease;
      border: 1px solid transparent;
      color: #0c111b;
      background: var(--accent);
      box-shadow: 0 10px 26px rgba(0,0,0,0.25);
    }
    .button:active { transform: translateY(1px); }
    .button.accent { background: var(--accent); }
    .button.outline {
      background: rgba(255,255,255,0.06);
      color: var(--text);
      border: 1px solid rgba(255,255,255,0.22);
      box-shadow: none;
    }
    .button.small { width: auto; padding: 10px 12px; font-size: 14px; }
    .toast {
      margin-top: 6px;
      font-size: 13px;
      color: var(--muted);
      min-height: 18px;
    }
    .stats {
      margin-top: 18px;
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
      gap: 12px;
      margin-bottom: 18px;
    }
    .log-panel {
      margin-top: 18px;
      background: rgba(255,255,255,0.07);
      border: 1px solid rgba(255,255,255,0.10);
      border-radius: 14px;
      padding: 14px;
      box-shadow: var(--shadow);
    }
    .log-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      margin-bottom: 10px;
    }
    .log-title {
      font-size: 16px;
      font-weight: 600;
      color: var(--text);
    }
    .log-box {
      background: #0d1117;
      border: 1px solid rgba(255,255,255,0.12);
      border-radius: 10px;
      padding: 12px;
      color: #c9d1d9;
      font-family: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
      font-size: 11px;
      line-height: 1.5;
      white-space: pre-wrap;
      word-break: break-word;
    }
    .log-entry { display: block; padding: 2px 0; border-bottom: 1px solid rgba(255,255,255,0.05); }
    .log-entry:last-child { border-bottom: none; }
    .log-level { font-weight: 600; padding: 1px 6px; border-radius: 3px; margin-right: 6px; font-size: 9px; text-transform: uppercase; display: inline-block; min-width: 62px; text-align: center; }
    .log-level.physical { background: #10b98155; color: #6ee7b7; }
    .log-level.error { background: #f8514966; color: #ff7b72; }
    .log-level.warning { background: #d29922aa; color: #f0e68c; }
    .log-level.verbose { background: #388bfd44; color: #79c0ff; }
    .log-time { color: #8b949e; margin-right: 4px; }
    .log-action { color: #c9d1d9; }
    .stat {
      background: rgba(255,255,255,0.07);
      border: 1px solid rgba(255,255,255,0.10);
      border-radius: 14px;
      padding: 14px;
      backdrop-filter: blur(5px);
    }
    .stat.water { grid-column: span 2; }
    .stat-label { color: var(--muted); font-size: 13px; margin-bottom: 4px; }
    .stat-value { font-size: 20px; font-weight: 600; color: var(--text); }
    .stat-foot { color: var(--muted); font-size: 12px; margin-top: 6px; line-height: 1.4; }
    @media (max-width: 640px) {
      body { padding: 16px; }
      .shell { padding: 22px; }
      .header { flex-direction: column; align-items: flex-start; gap: 10px; }
      .header-actions { width: 100%; justify-content: flex-start; }
      .header-actions .status { padding: 8px 12px; }
      .title { font-size: 20px; }
      .grid { grid-template-columns: 1fr; }
      .stats { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="header"><div class="title">iHOME Remote</div><div class="header-actions"><div id="header-status" class="status"><span class="dot"></span>LOADING</div><button class="status action" id="refresh-btn" aria-label="Refresh"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4a8 8 0 1 1-7.75 10h1.7a6.3 6.3 0 1 0 .06-4.99l2.19-2.18V12H3V6.06l2.02 2.02A8 8 0 0 1 12 4Z"></path></svg><span>Refresh</span></button></div></div>

    <div class="grid">
      <div class="card">
        <div class="label">Garage Door</div>
        <button class="button accent" onclick="sendCommand('up')">Up</button>
        <button class="button accent" onclick="sendCommand('down')">Down</button>
        <button class="button outline" onclick="sendCommand('stop')">Stop</button>
        <div class="toast" id="door-toast"></div>
      </div>
      <div class="card">
        <div class="label">Pump (GPIO4PUMP)</div>
        <button class="button accent" onclick="sendCommand('pump')">Run 5 minutes</button>
        <button class="button outline" onclick="sendCommand('pumpStop')">Stop now</button>
        <div class="toast" id="pump-toast"></div>
      </div>
    </div>

    <div class="toast" id="refresh-toast"></div>

    <div class="stats">
      <div class="stat water"><div class="stat-label">Water Level</div><div id="stat-water" class="stat-value">--</div><div id="stat-range" class="stat-foot">Auto range: --</div></div>
      <div class="stat"><div class="stat-label">Pump</div><div id="stat-pump" class="stat-value">--</div><div id="stat-pump-foot" class="stat-foot">Time since last change: --</div></div>
      <div class="stat"><div class="stat-label">Bad Connection</div><div id="stat-bad" class="stat-value">--</div><div id="stat-bad-foot" class="stat-foot">Count: -- | Cancel fail: --</div></div>
      <div class="stat"><div class="stat-label">Local Time</div><div id="stat-time" class="stat-value">--</div><div id="stat-time-foot" class="stat-foot">isBadtime: -- | Prefill: --</div></div>
      <div class="stat"><div class="stat-label">Uptime</div><div id="stat-uptime" class="stat-value">--</div><div id="stat-uptime-foot" class="stat-foot"></div></div>
    </div>

    <div class="log-panel">
      <div class="log-header">
        <div class="log-title">Logs <span id="log-count" style="font-weight:400;font-size:13px;color:var(--muted);">(0/0/0)</span></div>
        <div class="log-controls" style="display:flex;gap:6px;align-items:center;">
          <span style="position:relative;display:inline-block;">
            <input id="log-keyword" type="text" placeholder="Filter..." style="padding:6px 24px 6px 10px;border-radius:6px;font-size:12px;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);color:var(--text);width:80px;">
            <button id="log-keyword-clear" style="position:absolute;right:2px;top:50%;transform:translateY(-50%);background:none;border:none;color:var(--muted);font-size:14px;cursor:pointer;padding:0 4px;line-height:1;">&#10005;</button>
          </span>
          <select id="log-filter" style="padding:6px 10px;border-radius:6px;font-size:12px;cursor:pointer;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);color:var(--text);">
            <option value="3">All</option>
            <option value="2" selected>Warn+</option>
            <option value="1">Error+</option>
            <option value="0">Physical</option>
          </select>
          <button id="log-clear-btn" style="padding:6px 10px;border-radius:6px;font-size:12px;cursor:pointer;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.15);color:var(--text);">Clear Logs</button>
        </div>
      </div>
      <div id="log-output" class="log-box">Loading logs...</div>
    </div>
  </div>
  <script>
    const LOG_BUFFER_SIZE = 256;
    const actions = {
      up: '/get?frontdoor=up',
      down: '/get?frontdoor=down',
      stop: '/get?frontdoor=stop',
      pump: '/get?manualpump=1',
      pumpStop: '/get?manualpumpstop=1'
    };

    const toasts = {
      up: document.getElementById('door-toast'),
      down: document.getElementById('door-toast'),
      stop: document.getElementById('door-toast'),
      pump: document.getElementById('pump-toast'),
      pumpStop: document.getElementById('pump-toast'),
      refresh: document.getElementById('refresh-toast')
    };

    function showToast(key, text) {
      const el = toasts[key];
      if (!el) return;
      el.textContent = text;
      setTimeout(() => { if (el.textContent === text) el.textContent = ''; }, 2600);
    }

    async function sendCommand(key) {
      const url = actions[key];
      if (!url) return;
      try {
        showToast(key, 'Sending...');
        const res = await fetch(url, { method: 'GET' });
        if (!res.ok) throw new Error('Request failed');
        showToast(key, 'Done');
      } catch (err) {
        showToast(key, 'Error: ' + err.message);
      }
    }

    const els = {
      headerStatus: document.getElementById('header-status'),
      water: document.getElementById('stat-water'),
      range: document.getElementById('stat-range'),
      pump: document.getElementById('stat-pump'),
      pumpFoot: document.getElementById('stat-pump-foot'),
      bad: document.getElementById('stat-bad'),
      badFoot: document.getElementById('stat-bad-foot'),
      time: document.getElementById('stat-time'),
      timeFoot: document.getElementById('stat-time-foot'),
      uptime: document.getElementById('stat-uptime'),
      uptimeFoot: document.getElementById('stat-uptime-foot')
    };

    const logOutput = document.getElementById('log-output');
    const logClearBtn = document.getElementById('log-clear-btn');
    const logCountEl = document.getElementById('log-count');
    const logFilter = document.getElementById('log-filter');
    const logKeyword = document.getElementById('log-keyword');
    const logKeywordClear = document.getElementById('log-keyword-clear');
    let allLogs = [];
    let currentFilterLevel = 2;
    let currentKeyword = '';
    let lastReceivedHead = null;

    function formatLogTime(ms, time) {
      const msStr = String(ms);
      const timeStr = time && time !== 'time not synced' ? time : '--------';
      return msStr + 'ms | ' + timeStr;
    }

    function renderLogs(logs, filterLevel, keyword) {
      if (!logOutput) return;
      const kw = (keyword || '').toLowerCase();
      const filtered = logs.filter(log => log.lv <= filterLevel && (!kw || log.action.toLowerCase().includes(kw)));
      if (!logs || logs.length === 0) {
        logOutput.innerHTML = '<span style="color:#8b949e;">No logs yet...</span>';
        if (logCountEl) logCountEl.textContent = '(0/' + LOG_BUFFER_SIZE + ')';
        return;
      }
      if (logCountEl) logCountEl.textContent = '(' + filtered.length + '/' + logs.length + '/' + LOG_BUFFER_SIZE + ')';
      if (filtered.length === 0) {
        logOutput.innerHTML = '<span style="color:#8b949e;">No logs match filter...</span>';
        return;
      }
      const reversed = [...filtered].reverse();
      let html = '';
      for (const log of reversed) {
        const levelClass = log.level.toLowerCase();
        const timeStr = formatLogTime(log.ms, log.time);
        html += '<div class="log-entry">';
        html += '<span class="log-level ' + levelClass + '">' + log.level + '</span>';
        html += '<span class="log-time">' + timeStr + '</span>';
        html += '<span class="log-action">' + log.action + '</span>';
        html += '</div>';
      }
      logOutput.innerHTML = html;
    }

    async function fetchLogs() {
      try {
        const res = await fetch('/logs.json', { cache: 'no-store' });
        if (!res.ok) throw new Error('Logs fetch failed');
        allLogs = await res.json();
        renderLogs(allLogs, currentFilterLevel, currentKeyword);
      } catch (err) {
        if (logOutput) logOutput.innerHTML = '<span style="color:#ff7b72;">Error: ' + err.message + '</span>';
      }
    }

    async function clearLogs() {
      try {
        const res = await fetch('/clearlogs', { cache: 'no-store' });
        if (!res.ok) throw new Error('Clear failed');
        allLogs = [];
        fetchLogs();
      } catch (err) {
        if (logOutput) logOutput.innerHTML = '<span style="color:#ff7b72;">Error: ' + err.message + '</span>';
      }
    }

    if (logClearBtn) {
      logClearBtn.addEventListener('click', clearLogs);
    }

    if (logFilter) {
      logFilter.addEventListener('change', (e) => {
        currentFilterLevel = parseInt(e.target.value, 10);
        renderLogs(allLogs, currentFilterLevel, currentKeyword);
      });
    }

    if (logKeyword) {
      logKeyword.addEventListener('input', (e) => {
        currentKeyword = e.target.value;
        renderLogs(allLogs, currentFilterLevel, currentKeyword);
      });
    }
    if (logKeywordClear && logKeyword) {
      logKeywordClear.addEventListener('click', () => {
        logKeyword.value = '';
        currentKeyword = '';
        renderLogs(allLogs, currentFilterLevel, currentKeyword);
        logKeyword.focus();
      });
    }

    let lastStatus = {};
    function appendLogs(newLogs) {
      if (!Array.isArray(newLogs) || newLogs.length === 0) return;
      allLogs = allLogs.concat(newLogs);
      renderLogs(allLogs, currentFilterLevel, currentKeyword);
    }

    function updateText(el, text, key) {
      if (!el || text === undefined || text === null) return;
      if (lastStatus[key] !== text) {
        el.textContent = text;
        lastStatus[key] = text;
      }
    }

    function updateStatusClass(statusText) {
      if (!els.headerStatus) return;
      els.headerStatus.classList.remove('on', 'hold');
      if (statusText === 'RUNNING') els.headerStatus.classList.add('on');
      else if (statusText === 'OVERHEAT_PROTECTION') els.headerStatus.classList.add('hold');
    }

    function formatUptime(sec) {
      const s = Number(sec) || 0;
      const days = Math.floor(s / 86400);
      const hours = Math.floor((s % 86400) / 3600);
      const mins = Math.floor((s % 3600) / 60);
      const secs = Math.floor(s % 60);
      if (days > 0) return `${days}d ${hours}h ${mins}m`;
      if (hours > 0) return `${hours}h ${mins}m`;
      if (mins > 0) return `${mins}m ${secs}s`;
      return `${secs}s`;
    }

    function applyStatus(data) {
      if (!data) return;
      updateText(els.water, data.water, 'water');
      if (data.min_level !== undefined && data.max_level !== undefined) {
        let rangeText = `Auto range: ${data.min_level} - ${data.max_level}`;
        if (data.last_water_update_s !== undefined) {
          rangeText += ` | Last update: ${formatUptime(data.last_water_update_s)}`;
        }
        updateText(els.range, rangeText, 'range');
      }
      updateText(els.pump, data.pump_status, 'pump');
      if (data.minutes_since_change !== undefined) updateText(els.pumpFoot, `Time since last change: ${data.minutes_since_change} min`, 'pumpFoot');
      if (data.bad_conn_mode !== undefined) updateText(els.bad, String(data.bad_conn_mode), 'bad');
      if (data.bad_conn_count !== undefined && data.bad_cancel_fail !== undefined) {
        updateText(els.badFoot, `Count: ${data.bad_conn_count} | Cancel fail: ${data.bad_cancel_fail}`, 'badFoot');
      }
      if (data.time) updateText(els.time, data.time, 'time');
      if (data.is_badtime !== undefined && data.deficient_level !== undefined && data.prefill_from !== undefined && data.prefill_to !== undefined) {
        updateText(els.timeFoot, `isBadtime: ${data.is_badtime} | Prefill: ${data.prefill_from}:00-${data.prefill_to}:00 (${data.deficient_level})`, 'timeFoot');
      }
      if (data.uptime_s !== undefined) updateText(els.uptime, formatUptime(data.uptime_s), 'uptime');
      if (data.last_command !== undefined) {
        const cmdAgo = data.last_command_since_s !== undefined ? formatUptime(data.last_command_since_s) : 'n/a';
        updateText(els.uptimeFoot, `Last command: ${data.last_command} (${cmdAgo} ago)`, 'uptimeFoot');
      }
      if (els.headerStatus && data.pump_status) {
        const headerText = '<span class="dot"></span>' + data.pump_status;
        if (lastStatus.header !== headerText) {
          els.headerStatus.innerHTML = headerText;
          lastStatus.header = headerText;
        }
      }
      if (data.pump_status && lastStatus.pumpStatusClass !== data.pump_status) {
        updateStatusClass(data.pump_status);
        lastStatus.pumpStatusClass = data.pump_status;
      }
    }

    async function fetchStatus(withToast = false) {
      try {
        if (withToast) showToast('refresh', 'Refreshing...');
        const res = await fetch('/status.json', { cache: 'no-store' });
        if (!res.ok) throw new Error('Status fetch failed');
        const data = await res.json();
        applyStatus(data);
        if (withToast) showToast('refresh', 'Done');
      } catch (err) {
        if (withToast) showToast('refresh', 'Error: ' + err.message);
      }
    }
    const refreshBtn = document.getElementById('refresh-btn');
    if (refreshBtn) {
      refreshBtn.addEventListener('click', () => {
        fetchStatus(true);
        fetchLogs();
      });
    }

    const es = new EventSource('/events');
    es.addEventListener('status', (e) => {
      try {
        const payload = JSON.parse(e.data);
        if (payload.status) applyStatus(payload.status);
        if (payload.log_head !== undefined) {
          if (lastReceivedHead !== null && payload.log_head !== lastReceivedHead) {
            fetchLogs();
            lastReceivedHead = payload.log_head;
            return;
          }
          lastReceivedHead = payload.log_head;
        }
        if (payload.logs_reset) {
          fetchLogs();
          return;
        }
        if (payload.logs) appendLogs(payload.logs);
      } catch (err) { console.error(err); }
    });
    es.addEventListener('error', () => { /* optional: toast/log */ });
    window.addEventListener('beforeunload', () => es.close());

    fetchLogs();
    fetchStatus(false);
  </script>
</body>
</html>
)rawliteral";

// === LOG SYSTEM ===
// Memory calculation for ESP32:
// - ESP32 has ~320KB RAM, WiFi/stack uses ~100-150KB
// - Using fixed char[64] instead of String to avoid heap fragmentation
// - Per entry: 4 (enum) + 4 (ms) + 4 (epoch) + 64 (action) = 76 bytes
// - Safe allocation: ~5KB -> 64 entries (4,864 bytes total)
enum LogLevel { LOG_PHYSICAL = 0, LOG_ERROR = 1, LOG_WARNING = 2, LOG_VERBOSE = 3 };
const int LOG_BUFFER_SIZE = 256;  // Increased buffer size for more logs
const int LOG_ACTION_SIZE = 64;  // Max chars per action message
void scheduleStatusPush();
struct LogEntry {
  LogLevel level;              // 4 bytes
  unsigned long timestampMs;   // 4 bytes
  unsigned long epochTime;     // 4 bytes
  char action[LOG_ACTION_SIZE]; // 64 bytes (fixed, no heap fragmentation)
};  // Total: 76 bytes per entry
LogEntry logBuffer[LOG_BUFFER_SIZE];
int logHead = 0;
int logCount = 0;
int lastSentLogHead = 0;
int lastSentLogCount = 0;

void addLog(LogLevel level, const String &action) {
  LogEntry &entry = logBuffer[logHead];
  entry.level = level;
  entry.timestampMs = millis();
  // Calculate epoch from cached time
  if (lastTimeEpoch > 0 && lastTimeSyncMs > 0) {
    entry.epochTime = lastTimeEpoch + ((entry.timestampMs - lastTimeSyncMs) / 1000);
  } else {
    entry.epochTime = 0;
  }
  // Copy action with truncation to fixed buffer
  strncpy(entry.action, action.c_str(), LOG_ACTION_SIZE - 1);
  entry.action[LOG_ACTION_SIZE - 1] = '\0';
  logHead = (logHead + 1) % LOG_BUFFER_SIZE;
  if (logCount < LOG_BUFFER_SIZE) logCount++;
  scheduleStatusPush();
}

void logPhysical(const String &action) { addLog(LOG_PHYSICAL, action); Serial.println("[PHYSICAL] " + action); }
void logError(const String &action) { addLog(LOG_ERROR, action); Serial.println("[ERROR] " + action); }
void logWarning(const String &action) { addLog(LOG_WARNING, action); Serial.println("[WARN] " + action); }
void logVerbose(const String &action) { addLog(LOG_VERBOSE, action); Serial.println("[VERBOSE] " + action); }

void clearLogs() {
  logHead = 0;
  logCount = 0;
}

String getLogsJson() {
  String json = "[";
  json.reserve(LOG_BUFFER_SIZE * 100); // Pre-allocate to reduce fragmentation
  int start = (logCount < LOG_BUFFER_SIZE) ? 0 : logHead;
  bool first = true;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_BUFFER_SIZE;
    LogEntry &e = logBuffer[idx];
    if (!first) json += ",";
    first = false;
    String levelStr = (e.level == LOG_PHYSICAL) ? "PHYSICAL" : (e.level == LOG_ERROR) ? "ERROR" : (e.level == LOG_WARNING) ? "WARNING" : "VERBOSE";
    String timeStr = formatTimeFromEpoch(e.epochTime);
    // Escape quotes in action
    String escapedAction = String(e.action);
    escapedAction.replace("\"", "'");
    json += "{\"level\":\"" + levelStr + "\",";
    json += "\"lv\":" + String(e.level) + ",";  // numeric level for filtering
    json += "\"ms\":" + String(e.timestampMs) + ",";
    json += "\"time\":\"" + timeStr + "\",";
    json += "\"action\":\"" + escapedAction + "\"}";
  }
  json += "]";
  return json;
}
// === END LOG SYSTEM ===

// Build status JSON for REST/SSE reuse
String makeStatusJson() {
  String pumpStatusText = "Unknown";
  switch (pump_status) {
    case RUNNING: pumpStatusText = "RUNNING"; break;
    case STOPPED: pumpStatusText = "STOPPED"; break;
    case OVERHEAT_PROTECTION: pumpStatusText = "OVERHEAT_PROTECTION"; break;
    default: break;
  }
  unsigned long nowMs = millis();
  float minutesSinceChange = (nowMs - ms) / 1000.0 / 60.0;
  int hoursCached = 0;
  String formattedTime = "time not synced";
  getLocalTimeFromCache(hoursCached, formattedTime);
  String badTimeText = isBadTime() ? "True" : "False";
  unsigned long timeSinceSyncMs = lastTimeSyncMs > 0 ? nowMs - lastTimeSyncMs : 0;
  unsigned long lastCmdSinceMs = lastCommandMs > 0 ? nowMs - lastCommandMs : 0;

  String json;
  json.reserve(900);
  json += "{";
  json += "\"water\":\"" + message + "\",";
  json += "\"pump_status\":\"" + pumpStatusText + "\",";
  json += "\"minutes_since_change\":" + String(minutesSinceChange, 2) + ",";
  json += "\"bad_conn_mode\":" + String(bad_conn_mode ? 1 : 0) + ",";
  json += "\"bad_conn_count\":" + String(bad_conn_count) + ",";
  json += "\"bad_cancel_fail\":" + String(cannotcanceltimerrrrrr) + ",";
  json += "\"time\":\"" + formattedTime + "\",";
  json += "\"time_since_sync_s\":" + String(timeSinceSyncMs / 1000.0, 2) + ",";
  json += "\"is_badtime\":\"" + badTimeText + "\",";
  json += "\"min_level\":" + String(MIN_WATER_LEVEL, 2) + ",";
  json += "\"max_level\":" + String(MAX_WATER_LEVEL, 2) + ",";
  json += "\"deficient_level\":" + String(DEFICIENT_WATER_LEVEL, 2) + ",";
  json += "\"prefill_from\":" + String(isTimeInRange_min) + ",";
  json += "\"prefill_to\":" + String(isTimeInRange_max) + ",";
  json += "\"uptime_s\":" + String(nowMs / 1000) + ",";
  json += "\"last_command\":\"" + lastCommand + "\",";
  json += "\"last_command_since_s\":" + String(lastCmdSinceMs / 1000.0, 2) + ",";
  json += "\"last_water_update_s\":" + String((nowMs - lastWaterLevelUpdateMs) / 1000.0, 2);
  json += "}";
  return json;
}

// Collect incremental logs since last SSE send; if overflow detected, request reset on client
void collectNewLogs(String &logsJson, bool &reset) {
  reset = false;
  logsJson = "[]";
  if (logCount == 0) return;

  int diff = logCount - lastSentLogCount;
  if (diff <= 0) return; // nothing new

  if (diff > LOG_BUFFER_SIZE) {
    // buffer wrapped before we could send; ask client to refetch full
    reset = true;
    lastSentLogHead = logHead;
    lastSentLogCount = logCount;
    return;
  }

  // start index of new entries
  int startIdx = (logHead - diff + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
  logsJson = "[";
  bool first = true;
  for (int i = 0; i < diff; i++) {
    int idx = (startIdx + i) % LOG_BUFFER_SIZE;
    LogEntry &e = logBuffer[idx];
    if (!first) logsJson += ",";
    first = false;
    String levelStr = (e.level == LOG_PHYSICAL) ? "PHYSICAL" : (e.level == LOG_ERROR) ? "ERROR" : (e.level == LOG_WARNING) ? "WARNING" : "VERBOSE";
    String timeStr = formatTimeFromEpoch(e.epochTime);
    String escapedAction = String(e.action);
    escapedAction.replace("\"", "'");
    logsJson += "{\"level\":\"" + levelStr + "\",";
    logsJson += "\"lv\":" + String(e.level) + ",";
    logsJson += "\"ms\":" + String(e.timestampMs) + ",";
    logsJson += "\"time\":\"" + timeStr + "\",";
    logsJson += "\"action\":\"" + escapedAction + "\"}";
  }
  logsJson += "]";

  lastSentLogHead = logHead;
  lastSentLogCount = logCount;
}

// Debounced SSE status push (max 1/sec)
void sendStatusSse() {
  String statusJson = makeStatusJson();
  String logsJson;
  bool resetLogs = false;
  collectNewLogs(logsJson, resetLogs);

  String payload;
  payload.reserve(statusJson.length() + logsJson.length() + 64);
  payload += "{";
  payload += "\"status\":" + statusJson + ",";
  payload += "\"logs_reset\":" + String(resetLogs ? 1 : 0) + ",";
  payload += "\"logs\":" + logsJson + ",";
  payload += "\"log_head\":" + String(logHead) + ",";
  payload += "\"log_count\":" + String(logCount);
  payload += "}";

  events.send(payload.c_str(), "status", millis());

  // NOTE: do NOT publish to MQTT here. sendStatusSse() can be reached from a web
  // handler (addLog -> scheduleStatusPush -> sendStatusSse) which runs on the
  // AsyncTCP task, and PubSubClient is not thread-safe. The MQTT status mirror is
  // published from loop() instead (publishStatusMqtt, ticked on timer_mqtt_status).

  lastStatusSentMs = millis();
  statusPushScheduled = false;
}

bool statusDeferred(void *) {
  sendStatusSse();
  return false; // one-shot
}

void goPrefillTimer() {
  if (prefillTimerStarted) return;
  timer_prefill.cancel(prefillTask);
  prefillTask = timer_prefill.every(PREFILL_CHECK_INTERVAL_MS, isTimeInRange);
  prefillTimerStarted = true;
}

void scheduleStatusPush() {
  unsigned long now = millis();
  unsigned long elapsed = now - lastStatusSentMs;
  if (elapsed >= STATUS_DEBOUNCE_MS) {
    sendStatusSse();
    return;
  }
  if (!statusPushScheduled) {
    unsigned long delayMs = STATUS_DEBOUNCE_MS - elapsed;
    timer_events.cancel(statusPushTask);
    statusPushTask = timer_events.in(delayMs, statusDeferred);
    statusPushScheduled = true;
  }
}

bool syncTimeWithServer(const char* server, int attempts = 4, int delayMs = 2000) {
  Serial.print("Trying NTP server: ");
  Serial.println(server);

  timeClient.end();
  timeClient = NTPClient(ntpUDP, server, 28800, 60000);
  timeClient.begin();

  for (int retry = 0; retry < attempts; retry++) {
    if (!stop_wdt) esp_task_wdt_reset();
    if (timeClient.forceUpdate()) {
      Serial.print("NTP sync successful via: ");
      Serial.println(server);
      Serial.print("Time: ");
      Serial.println(timeClient.getFormattedTime());
      lastTimeServer = server;
      lastTimeSyncMs = millis();
      lastTimeEpoch = timeClient.getEpochTime();
      hasTimeSync = true;
      // Trigger a prefill check right after the first successful sync
      isTimeInRange(nullptr);
      goPrefillTimer();
      return true;
    }
    delay(delayMs);
    if (!stop_wdt) esp_task_wdt_reset();
  }
  logWarning("NTP sync failed: " + String(server));
  return false;
}

String formatTimeFromEpoch(unsigned long epoch) {
  if (epoch == 0) return "time not synced";
  unsigned long secDay = epoch % 86400;
  int h = secDay / 3600;
  int m = (secDay % 3600) / 60;
  int s = secDay % 60;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

// Get local time (UTC+8) from cached NTP values; returns false if no sync yet.
bool getLocalTimeFromCache(int &hoursOut, String &formattedOut) {
  if (lastTimeEpoch == 0 || lastTimeSyncMs == 0) return false;
  unsigned long nowMs = millis();
  unsigned long nowEpoch = lastTimeEpoch + ((nowMs - lastTimeSyncMs) / 1000);
  hoursOut = (nowEpoch % 86400) / 3600;
  formattedOut = formatTimeFromEpoch(nowEpoch);
  return true;
}

bool ntpFastPoll(void *);
bool ntpSlowPoll(void *);
void scheduleNtpFast();
void scheduleNtpSlow();

// reset_bad_conn_timer 找錯中...
int bad_conn_count = 0;
int cannotcanceltimerrrrrr = 0;
bool overheat(void *argument /* optional argument given to in/at/every */) {
    Serial.println("overheat overheat overheat");
    logWarning("Pump overheat protection triggered");
    request_pump_to(OVERHEAT_PROTECTION);
    timer_2.cancel();
    Serial.println("log for timer1");
    return true; // to repeat the action - false to stop
}
void pump_run(long pumpDuration_ms = timer_2_interval){
  // Only execute if pump is currently off (GPIO4PUMP is HIGH)
  if (digitalRead(GPIO4PUMP) == LOW) {
      return; // Pump already on, do nothing
  }
  timer_2.cancel();
  digitalWrite(GPIO4PUMP, LOW);
  pump_status = RUNNING;
  ms = millis();
  logPhysical("Pump relay ON (duration: " + String(pumpDuration_ms/1000) + "s)");
  timer_2.in(pumpDuration_ms, overheat);
}
void pump_stop(){
    // Only execute if pump is currently on (GPIO4PUMP is LOW)
    if (digitalRead(GPIO4PUMP) == HIGH) {
        return; // Pump already off, do nothing
    }
    timer_2.cancel();
    timer_3.cancel();
    digitalWrite(GPIO4PUMP, HIGH);
    pump_status = STOPPED;
    ms = millis();
    logPhysical("Pump relay OFF");
}
bool recover_from_overheat(void *argument /* optional argument given to in/at/every */) {
    timer_3.cancel();

    set_timer_blink_interval_to(normal_blink_interval);
    Serial.println("recover from overheat (過熱保護復歸)");
    logVerbose("Recovered from overheat protection");
    pump_status = STOPPED;
    ms = millis();
    if (!isBadTime()) request_pump_to(RUNNING); // check_water_level(); // pump_run();
    // reset_bad_conn_timer(); // 以防進入 bad conn mode 然後又在這邊被打開... 不會發生因為request_pump_to()裡面有擋了
    Serial.println("log for timer2");
    return true; // to repeat the action - false to stop
}
void pump_overheat_protect(long overheatRecoveryDelay_ms = timer_3_interval){
  timer_3.cancel();
  set_timer_blink_interval_to(overheated_blink_interval);
  digitalWrite(GPIO4PUMP, HIGH);
  pump_status = OVERHEAT_PROTECTION;
  ms = millis();
  logPhysical("Pump forced OFF (overheat protection)");
  timer_3.in(overheatRecoveryDelay_ms, recover_from_overheat);
}
bool stop_manual_pump(void *argument /* optional argument given to in/at/every */) {
  timer_manual_pump.cancel();
  Serial.println("Manual pump run finished, stopping pump");
  logVerbose("Manual pump timer expired - stopping");
  pump_stop();
  return false; // one-shot
}
void manual_pump_start() {
  Serial.println("Manual pump start for 5 minutes (GPIO4PUMP)");
  logPhysical("Manual pump started (5 min)");
  timer_manual_pump.cancel();
  request_pump_to(RUNNING);
  timer_manual_pump.in(manual_pump_duration_ms, stop_manual_pump);
}
void request_pump_to(PumpStatus status) {

  if (bad_conn_mode) return;
  switch (status) {
    case RUNNING:
      if (pump_status == RUNNING){
        Serial.println("pump is running");
      }else if (pump_status == OVERHEAT_PROTECTION){
        Serial.println("pump is now in overheat protection");
      }else{
        Serial.println("pump_run");
        pump_run();
      }
      break;
    case STOPPED:
      pump_stop();
      // if (pump_status == STOPPED){
      //   Serial.println("pump is stopped");
      // }else{
      //   Serial.println("pump_stop");
      //   pump_stop();
      // }
      break;
    case OVERHEAT_PROTECTION:
      if (pump_status == OVERHEAT_PROTECTION){
      }else{
        Serial.println("pump_overheat_protect");
        pump_overheat_protect();
      }
      break;
    default:
      Serial.println("Unknown pump status");
      break;
  }
}
bool go_bad_conn_mode (void *argument /* optional argument given to in/at/every */) {
  Serial.println("[go_bad_conn_mode] triggered ! !");
  Serial.println("[go_bad_conn_mode] triggered ! !");
  logError("Bad connection mode activated - pump force stopped");
  bad_conn_count++;
  bad_conn_mode = true;
  pump_stop(); // force stop pump
  set_timer_blink_interval_to(badconnmode_blink_interval);
  Serial.println("log for timer3");
  return true;
}

void record_command(const String &name) {
  lastCommand = name;
  lastCommandMs = millis();
  logVerbose("Command: " + name);
}
void reset_bad_conn_timer() {
  Serial.println("log for reset_bad_conn_timer");
  static Timer<>::Task event;
  if (!timer_bad_connection.empty()) {
    timer_bad_connection.cancel(event);
    if (!timer_bad_connection.empty()) cannotcanceltimerrrrrr++;
  }
  // delay(1);
  event = timer_bad_connection.in(timer_bad_connection_delay, go_bad_conn_mode);
  if (bad_conn_mode) {
    set_timer_blink_interval_to(normal_blink_interval);
    logWarning("Bad connection mode recovered");
  }
  bad_conn_mode = false;
}
void check_water_level (float desiredMinWaterLevel, bool if_reset_bad_conn_timer) {
  if (message.toFloat() != 0) {
    float num = message.toFloat();
    Serial.println("[check_water_level] Water level: " + String(num) + ". (desiredMinWaterLevel: " + desiredMinWaterLevel + ", MAX_WATER_LEVEL: " + MAX_WATER_LEVEL + ")");
    // logVerbose("Water level: " + String(num, 1) + " (min:" + String(desiredMinWaterLevel, 1) + " max:" + String(MAX_WATER_LEVEL, 1) + ")");
    if (if_reset_bad_conn_timer) reset_bad_conn_timer();
    if (num < desiredMinWaterLevel){
      Serial.println(" !! BELOW desiredMinWaterLevel (" + String(desiredMinWaterLevel) + ") !! ");
      logWarning("Water BELOW min (" + String(num, 1) + " < " + String(desiredMinWaterLevel, 1) + ")");
      //requested the pump to start
      request_pump_to(RUNNING);
    }else if (num > MAX_WATER_LEVEL){
      Serial.println(" !! OVER MAX_WATER_LEVEL (" + String(MAX_WATER_LEVEL) + ") !! ");
      logVerbose("Water OVER max (" + String(num, 1) + " > " + String(MAX_WATER_LEVEL, 1) + ")");
      //requested the pump to stop
      request_pump_to(STOPPED); //lock from entering RUNNING status for a period of time after pump stopped
    }
    Serial.println("log for check_water_level3");
  } else {
    Serial.println("[check_water_level] Waiting for water level data from 4F. (or given data is not valid)");
  }
}
bool relay_reset(void *argument /* optional argument given to in/at/every */) {
  timer_relay.cancel();
  digitalWrite(GPIO4UP, HIGH);
  digitalWrite(GPIO4DOWN, HIGH);
  digitalWrite(GPIO4STOP, HIGH);
  logPhysical("Frontdoor relay reset (all HIGH)");
  Serial.println("log for timer4");
  return true;
}
void frontdoor_control(String value){ // frontdoor control/relay control
  logPhysical("Frontdoor relay: " + value);
  if (value == "up"){
    digitalWrite(GPIO4UP, LOW);
    timer_relay.cancel();
    timer_relay.in(relay_open_interval, relay_reset);
  }else if (value == "down"){
    digitalWrite(GPIO4DOWN, LOW);
    timer_relay.cancel();
    timer_relay.in(relay_open_interval, relay_reset);
  }else if (value == "stop"){
    digitalWrite(GPIO4STOP, LOW);
    timer_relay.cancel();
    timer_relay.in(relay_open_interval, relay_reset);
  }
}
bool blink_f (void *argument /* optional argument given to in/at/every */) {
  blink = !blink;
  if (blink) digitalWrite(2, HIGH);
  else digitalWrite(2, LOW);
  return true;
}
bool fill_up (void *argument /* optional argument given to in/at/every */) {
  request_pump_to(RUNNING);
  Serial.println("log for timer6");
  return true;
}
// Heap monitoring
bool heapCheck (void *argument /* optional argument given to in/at/every */) {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();
  logVerbose("Heap: " + String(freeHeap) + " bytes free, min: " + String(minFreeHeap) + " bytes");
  return true;
}
// 20230808 ntp 時間功能
bool isTimeInRange (void *argument /* optional argument given to in/at/every */) {
  int hours = 0;
  String formattedTime;
  if (!getLocalTimeFromCache(hours, formattedTime)) {
    Serial.println("log for timer7 (no time sync yet)");
    return true; // No time available yet
  }
  // Check if the time is between 20 and 22
  if (hours >= isTimeInRange_min && hours < isTimeInRange_max) {
    Serial.println("Current time is between 20:00 and 22:00, check_water_level with desiredMinWaterLevel as: " + String(DEFICIENT_WATER_LEVEL) + " (" + formattedTime + ")");
    check_water_level(DEFICIENT_WATER_LEVEL, false);
  } else {
    Serial.println("Current time is NOT between 20:00 and 22:00 (" + formattedTime + ")");
  }
  return true;
}
bool isBadTime() {
  int hours = 0;
  String tmp;
  if (!getLocalTimeFromCache(hours, tmp)) return false;

  // Check if current hour is in "bad time" range (23:00 - 06:00)
  // isBadTimeBetween_from = 23, isBadTimeBetween_to = 6
  if (hours >= isBadTimeBetween_from || hours < isBadTimeBetween_to) {
    Serial.println("isBadtime : True (cached epoch, hour: " + String(hours) + ")");
    return true;
  }

  Serial.println("isBadtime : False (cached epoch, hour: " + String(hours) + ")");
  return false;
}

// ===========================================================================
// Shared threshold setters — reused by BOTH the web /get handler and MQTT.
// (Same validation that used to be inline in the /get lambda.)
// ===========================================================================
void applySetMax(float f) {
  if (f > MIN_WATER_LEVEL) {
    MAX_WATER_LEVEL = f;
    DEFICIENT_WATER_LEVEL = (MAX_WATER_LEVEL - MIN_WATER_LEVEL) * 0.3 + MIN_WATER_LEVEL;
    logWarning("Max level set to " + String(MAX_WATER_LEVEL, 1));
  } else {
    logWarning("Max level rejected (must > min " + String(MIN_WATER_LEVEL, 1) + ")");
  }
}
void applySetMin(float f) {
  if (f < MAX_WATER_LEVEL) {
    MIN_WATER_LEVEL = f;
    DEFICIENT_WATER_LEVEL = (MAX_WATER_LEVEL - MIN_WATER_LEVEL) * 0.3 + MIN_WATER_LEVEL;
    logWarning("Min level set to " + String(MIN_WATER_LEVEL, 1));
  } else {
    logWarning("Min level rejected (must < max " + String(MAX_WATER_LEVEL, 1) + ")");
  }
}

// ===========================================================================
// MQTT — all calls run on the loop() thread (never from a web callback).
// ===========================================================================
const char* pumpStatusStr() {
  switch (pump_status) {
    case RUNNING: return "RUNNING";
    case STOPPED: return "STOPPED";
    case OVERHEAT_PROTECTION: return "OVERHEAT_PROTECTION";
    default: return "Unknown";
  }
}

// Publish the discrete state topics HA reads (retained). The big JSON status is
// published from sendStatusSse() on its existing 1/sec debounce.
void publishPumpState() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_PUMP_STATUS, pumpStatusStr(), true);
  char buf[16];
  dtostrf(MIN_WATER_LEVEL, 0, 1, buf); mqtt.publish(TOPIC_MIN_LEVEL, buf, true);
  dtostrf(MAX_WATER_LEVEL, 0, 1, buf); mqtt.publish(TOPIC_MAX_LEVEL, buf, true);
  mqtt.publish(TOPIC_BAD_CONN, bad_conn_mode ? "ON" : "OFF", true);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  // Copy payload to a small stack buffer (commands are tiny).
  char buf[32];
  unsigned int n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';
  String t = String(topic);
  String v = String(buf);

  if (t == TOPIC_SUB_WATER) {
    // THE critical wiring: incoming water == the failsafe heartbeat.
    // retain=false on this topic means a reconnect does NOT replay a stale value
    // (which would falsely keep the failsafe alive).
    message = v;
    lastWaterLevelUpdateMs = millis();
    // Reset the failsafe on ANY received message, even "0"/non-numeric: receiving
    // a message IS proof of liveness. check_water_level only resets the timer when
    // message.toFloat() != 0, so we reset here explicitly to cover 0/garbage too.
    reset_bad_conn_timer();
    check_water_level(MIN_WATER_LEVEL, false);
    scheduleStatusPush();
    return;
  }
  if (t == TOPIC_CMD_DOOR) {
    String d = v; d.toLowerCase();
    if (d == "up") frontdoor_control("up");
    else if (d == "down") frontdoor_control("down");
    else if (d == "stop") frontdoor_control("stop");
    record_command("frontdoor:" + d);
    scheduleStatusPush();
    return;
  }
  if (t == TOPIC_CMD_MANUALPUMP) {
    if (v == "RUN" || v == "1") {
      manual_pump_start();
      record_command("manualpump");
    } else if (v == "STOP" || v == "0") {
      logWarning("Manual pump stop requested (mqtt)");
      timer_manual_pump.cancel();
      request_pump_to(STOPPED);
      record_command("manualpumpstop");
    }
    scheduleStatusPush();
    return;
  }
  if (t == TOPIC_CMD_SETMAX) {
    applySetMax(v.toFloat());
    record_command("setmax:" + v);
    publishPumpState();
    scheduleStatusPush();
    return;
  }
  if (t == TOPIC_CMD_SETMIN) {
    applySetMin(v.toFloat());
    record_command("setmin:" + v);
    publishPumpState();
    scheduleStatusPush();
    return;
  }
}

#if USE_HA_DISCOVERY
// Publish one HA discovery config (retained). Built with snprintf into a reused
// buffer to protect the tight heap. The pump device block is shared.
void publishDiscovery() {
  const char* DEV = "\"dev\":{\"ids\":[\"wd_pump\"],\"name\":\"WD Pump (1F)\",\"mdl\":\"ESP32\",\"mf\":\"wd_project\",\"cu\":\"http://192.168.1.217/\"}";
  const char* AV  = "\"avty_t\":\"wd/pump/avail\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";
  char buf[640];

  // Pump status (enum sensor) — also carries the full status JSON as attributes.
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Pump Status\",\"uniq_id\":\"wd_pump_status\",\"stat_t\":\"%s\","
    "\"dev_cla\":\"enum\",\"options\":[\"RUNNING\",\"STOPPED\",\"OVERHEAT_PROTECTION\"],"
    "\"json_attr_t\":\"%s\",%s,%s}",
    TOPIC_PUMP_STATUS, TOPIC_STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/status/config", buf, true);

  // Bad connection (binary_sensor, problem)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Bad Connection\",\"uniq_id\":\"wd_pump_bad_conn\",\"stat_t\":\"%s\","
    "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_BAD_CONN, AV, DEV);
  mqtt.publish("homeassistant/binary_sensor/wd_pump/bad_conn/config", buf, true);

  // Max level (number, config)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Max Water Level\",\"uniq_id\":\"wd_pump_max_level\",\"cmd_t\":\"%s\","
    "\"stat_t\":\"%s\",\"min\":0,\"max\":200,\"step\":1,\"mode\":\"box\",\"ent_cat\":\"config\",%s,%s}",
    TOPIC_CMD_SETMAX, TOPIC_MAX_LEVEL, AV, DEV);
  mqtt.publish("homeassistant/number/wd_pump/max_level/config", buf, true);

  // Min level (number, config)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Min Water Level\",\"uniq_id\":\"wd_pump_min_level\",\"cmd_t\":\"%s\","
    "\"stat_t\":\"%s\",\"min\":0,\"max\":200,\"step\":1,\"mode\":\"box\",\"ent_cat\":\"config\",%s,%s}",
    TOPIC_CMD_SETMIN, TOPIC_MIN_LEVEL, AV, DEV);
  mqtt.publish("homeassistant/number/wd_pump/min_level/config", buf, true);

  // Manual pump (switch RUN/STOP, state from pump_status)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Manual Pump\",\"uniq_id\":\"wd_pump_manual\",\"cmd_t\":\"%s\","
    "\"pl_on\":\"RUN\",\"pl_off\":\"STOP\",\"stat_t\":\"%s\",\"stat_on\":\"RUNNING\",\"stat_off\":\"STOPPED\","
    "\"ic\":\"mdi:water-boiler\",%s,%s}",
    TOPIC_CMD_MANUALPUMP, TOPIC_PUMP_STATUS, AV, DEV);
  mqtt.publish("homeassistant/switch/wd_pump/manual/config", buf, true);

  // Garage door (cover, optimistic — pulse relay has no position feedback)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Garage Door\",\"uniq_id\":\"wd_pump_door\",\"dev_cla\":\"garage\",\"cmd_t\":\"%s\","
    "\"pl_open\":\"UP\",\"pl_cls\":\"DOWN\",\"pl_stop\":\"STOP\",\"opt\":true,%s,%s}",
    TOPIC_CMD_DOOR, AV, DEV);
  mqtt.publish("homeassistant/cover/wd_pump/door/config", buf, true);

  // Water (sensor, from status JSON)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Pump Water\",\"uniq_id\":\"wd_pump_water\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{value_json.water}}\",\"stat_cla\":\"measurement\",%s,%s}",
    TOPIC_STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/water/config", buf, true);

  // Uptime (diagnostic, from status JSON)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Uptime\",\"uniq_id\":\"wd_pump_uptime\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{value_json.uptime_s}}\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\","
    "\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/uptime/config", buf, true);

  // Local time (diagnostic, from status JSON)
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Local Time\",\"uniq_id\":\"wd_pump_time\",\"stat_t\":\"%s\","
    "\"val_tpl\":\"{{value_json.time}}\",\"ic\":\"mdi:clock-outline\",\"ent_cat\":\"diagnostic\",%s,%s}",
    TOPIC_STATUS, AV, DEV);
  mqtt.publish("homeassistant/sensor/wd_pump/time/config", buf, true);
}
#else
void publishDiscovery() {}
#endif

bool mqttReconnect() {
  if (millis() - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL_MS) return false;
  lastMqttReconnectAttempt = millis();
  Serial.println("MQTT connecting...");
  bool ok = mqtt.connect(SECRET_MQTT_CLIENTID, SECRET_MQTT_USER, SECRET_MQTT_PASS,
                         TOPIC_AVAIL, 1, true, "offline");
  if (ok) {
    Serial.println("MQTT connected");
    logVerbose("MQTT connected");
    mqtt.subscribe(TOPIC_SUB_WATER, 1);
    mqtt.subscribe(TOPIC_CMD_WILDCARD, 1);
    mqtt.publish(TOPIC_AVAIL, "online", true);
    publishDiscovery();
    publishPumpState();
    mqtt.publish(TOPIC_STATUS, makeStatusJson().c_str(), true);
  } else {
    Serial.print("MQTT connect failed, state=");
    Serial.println(mqtt.state());
  }
  return ok;
}

// Publish the status JSON + flat state topics to MQTT. Ticked from loop() only
// (timer_mqtt_status), so it never races mqtt.loop() on another thread.
bool publishStatusMqtt(void*) {
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_STATUS, makeStatusJson().c_str(), true);
    publishPumpState();
  }
  return true; // repeat
}

void setup() {
  Serial.begin(115200);
  Serial.println("Configuring WDT...");
  esp_task_wdt_init(WDT_TIMEOUT, true); //enable panic so ESP32 restarts
  esp_task_wdt_add(NULL);
  last = millis();
  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);
  pinMode(GPIO4PUMP, OUTPUT);
  digitalWrite(GPIO4PUMP, HIGH);
  pinMode(GPIO4UP, OUTPUT); //up
  digitalWrite(GPIO4UP, HIGH);
  pinMode(GPIO4DOWN, OUTPUT); //down
  digitalWrite(GPIO4DOWN, HIGH);
  pinMode(GPIO4STOP, OUTPUT); //stop
  digitalWrite(GPIO4STOP, HIGH);

  String myip = "192.168.1.217";
  IPAddress staticIP(192, 168, 1, 217);
  IPAddress gateway(192, 168, 1, 200);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(staticIP, gateway, subnet);
  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    delay(500);
    if (!stop_wdt) esp_task_wdt_reset();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("ERROR! >> WiFi Failed!");
    // Disable auto-restart to avoid reboot loops; stay alive for troubleshooting.
    return;
  }
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  logVerbose("WiFi connected: " + WiFi.localIP().toString());
  if (WiFi.localIP().toString().equals(myip) == false) {
    Serial.println("ERROR! >> WRONG IP Address, please check Router's setting (now ip:" + WiFi.localIP().toString() + ")");
    // Disable auto-restart to avoid reboot loops; stay alive for troubleshooting.
    return;
  }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/info/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/logs.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", getLogsJson());
  });
  server.on("/clearlogs", HTTP_GET, [](AsyncWebServerRequest *request) {
    clearLogs();
    logVerbose("Logs cleared by user");
    request->send(200, "text/plain", "Logs cleared");
  });
  server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", makeStatusJson());
  });
  server.addHandler(&events);
  events.send("online", "ping", millis());
  // ==========================================================================
  // GET /get - Main command handler endpoint
  // ==========================================================================
  // Supported parameters:
  //   ?message=<value>       - Water level from 4F sensor (triggers pump logic)
  //   ?frontdoor=up|down|stop - Garage door relay control
  //   ?manualpump=1          - Start pump for 5 minutes
  //   ?manualpumpstop=1      - Stop pump immediately
  //   ?setmaxlevel=<value>   - Set MAX_WATER_LEVEL threshold
  //   ?setminlevel=<value>   - Set MIN_WATER_LEVEL threshold
  //
  // Logging & record_command() behavior:
  //   - record_command() updates lastCommand/lastCommandMs AND logs (VERBOSE)
  //   - Water level uses record_command("water") for tracking
  //   - Physical GPIO actions logged as PHYSICAL in their respective functions
  //   - Config changes (setmax/setmin) logged as WARNING
  // ==========================================================================
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    String temp = "";
    if (request->hasParam(PARAM_INPUT_WATERLEVEL)) {
      temp = request->getParam(PARAM_INPUT_WATERLEVEL)->value();
      message = temp;
      lastWaterLevelUpdateMs = millis();
      // record_command("water:" + temp);
    } else if (request->hasParam(PARAM_INPUT_FRONTDOOR)){
      String value = request->getParam(PARAM_INPUT_FRONTDOOR)->value();
      temp = "recived command: frontdoor= " + value;
      frontdoor_control(value);
      record_command("frontdoor:" + value);
    } else if (request->hasParam(PARAM_INPUT_MANUALPUMP)) {
      temp = "recived command: manual pump run";
      manual_pump_start();
      record_command("manualpump");
    } else if (request->hasParam(PARAM_INPUT_MANUALPUMPSTOP)) {
      temp = "recived command: manual pump stop";
      logWarning("Manual pump stop requested");
      timer_manual_pump.cancel();
      request_pump_to(STOPPED);
      record_command("manualpumpstop");
    } else if (request->hasParam(PARAM_INPUT_SETMAXLEVEL)){
      String value = request->getParam(PARAM_INPUT_SETMAXLEVEL)->value();
      temp = "recived command: set max water level to " + value;
      applySetMax(float(value.toInt()));
      record_command("setmax:" + value);
    } else if (request->hasParam(PARAM_INPUT_SETMINLEVEL)){
      String value = request->getParam(PARAM_INPUT_SETMINLEVEL)->value();
      temp = "recived command: set min water level to " + value;
      applySetMin(float(value.toInt()));
      record_command("setmin:" + value);
    } else {
      message = temp;
      temp = "No message sent";
    }
    Serial.println("\tHello, GET: " + temp);
    request->send(200, "text/plain", "Hello, GET: " + temp);
    check_water_level(MIN_WATER_LEVEL, true);
    scheduleStatusPush();
  });
  // Send a POST request to <IP>/post with a form field message set to <message>
  // server.on("/post", HTTP_POST, [](AsyncWebServerRequest *request) {
  //   String message;
  //   if (request->hasParam(PARAM_INPUT_WATERLEVEL, true)) {
  //     message = request->getParam(PARAM_INPUT_WATERLEVEL, true)->value();
  //     lastWaterLevelUpdateMs = millis();
  //   } else {
  //     message = "No message sent";
  //   }
  //   Serial.println("\tHello, POST: " + message);
  //   request->send(200, "text/plain", "Hello, POST: " + message);
  //   check_water_level(MIN_WATER_LEVEL, true);
  //   scheduleStatusPush();
  // });
  server.onNotFound([](AsyncWebServerRequest *request){
  request->send(404, "text/plain", "Not found");
  });
  server.begin();

  // MQTT (alongside the web server; all mqtt.* calls happen on the loop thread).
  mqtt.setServer(SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  mqtt.setBufferSize(1024); // makeStatusJson()/discovery exceed the 256B default
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(15);
  mqtt.setCallback(mqttCallback);
  mqttReconnect();
  timer_mqtt_status.every(2000, publishStatusMqtt); // MQTT status refresh (loop thread)

  timer_heap.every(timer_heapcheck_interval, heapCheck); // Heap monitoring every hour
  set_timer_blink_interval_to(normal_blink_interval);
  // timer_1.in(timer_1_delay, fill_up); // Auto pump start after boot (31 sec) [disabled]
  reset_bad_conn_timer(); // timer_bad_connection — arms the 60s failsafe at boot

  bool ntpSuccess = false;
  const int ntpServerCount = sizeof(ntpServers) / sizeof(ntpServers[0]);
  for (int i = 0; i < ntpServerCount && !ntpSuccess; i++) {
    ntpSuccess = syncTimeWithServer(ntpServers[i], 4, 2000);
    if (ntpSuccess) currentNtpServerIndex = i;
  }

  const int ntpIpFallbackCount = sizeof(ntpServersIpFallback) / sizeof(ntpServersIpFallback[0]);
  for (int i = 0; i < ntpIpFallbackCount && !ntpSuccess; i++) {
    Serial.println("DNS may be blocked, trying direct NTP IP fallback...");
    ntpSuccess = syncTimeWithServer(ntpServersIpFallback[i], 4, 2000);
    if (ntpSuccess) currentNtpServerIndex = -1; // indicates IP fallback
  }

  if (!ntpSuccess) {
    Serial.println("ERROR: All NTP servers (names and IPs) failed!");
  }

  if (ntpSuccess) {
    scheduleNtpSlow();
  } else {
    scheduleNtpFast();
  }

  logWarning("System startup complete");
  ms = millis();
}
void set_timer_blink_interval_to(int interval) {
  Serial.println("set_timer_blink_interval_to: " + interval);
  timer_blink.cancel();
  timer_blink.every(interval, blink_f);
}
void scheduleNtpFast() {
  timer_ntp.cancel(ntpTask);
  ntpTask = timer_ntp.every(2000, ntpFastPoll);
}
void scheduleNtpSlow() {
  timer_ntp.cancel(ntpTask);
  ntpTask = timer_ntp.every(60 * 60 * 1000, ntpSlowPoll);
}

bool ntpFastPoll(void *) {
  const int nameCount = sizeof(ntpServers) / sizeof(ntpServers[0]);
  const int ipCount = sizeof(ntpServersIpFallback) / sizeof(ntpServersIpFallback[0]);
  const int total = nameCount + ipCount;
  if (total == 0) return true;
  int idx = ntpAttemptIndex % total;
  ntpAttemptIndex++;
  bool ok = false;
  String server;
  if (idx < nameCount) {
    server = String(ntpServers[idx]);
    ok = syncTimeWithServer(ntpServers[idx], 2, 1500);
  } else {
    server = String(ntpServersIpFallback[idx - nameCount]);
    ok = syncTimeWithServer(ntpServersIpFallback[idx - nameCount], 2, 1500);
  }
  if (ok) {
    scheduleNtpSlow();
    return false; // stop fast poll; slow poll will continue
  }
  logError("NTP fast poll failed: " + server);
  return true;
}

bool ntpSlowPoll(void *) {
  const int nameCount = sizeof(ntpServers) / sizeof(ntpServers[0]);
  const int ipCount = sizeof(ntpServersIpFallback) / sizeof(ntpServersIpFallback[0]);
  const int total = nameCount + ipCount;
  if (total == 0) return true;
  int idx = ntpAttemptIndex % total;
  ntpAttemptIndex++;
  bool ok = false;
  String server;
  if (idx < nameCount) {
    server = String(ntpServers[idx]);
    ok = syncTimeWithServer(ntpServers[idx], 2, 1500);
  } else {
    server = String(ntpServersIpFallback[idx - nameCount]);
    ok = syncTimeWithServer(ntpServersIpFallback[idx - nameCount], 2, 1500);
  }
  return true;
}
void loop() {
  // MQTT serviced on the loop thread (one non-blocking reconnect attempt / 5s).
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (wasMqttConnected) { logWarning("MQTT disconnected"); wasMqttConnected = false; }
      mqttReconnect();
    } else {
      if (!wasMqttConnected) wasMqttConnected = true;
      mqtt.loop();
    }
  }

  timer_1.tick();
  timer_blink.tick();
  timer_2.tick();
  timer_3.tick();
  timer_relay.tick();
  timer_bad_connection.tick();
  timer_ntp.tick();
  timer_manual_pump.tick();
  timer_events.tick();
  timer_heap.tick();
  timer_prefill.tick();
  timer_mqtt_status.tick();
  // Reset watchdog every loop to avoid false triggers when work takes longer.
  if (!stop_wdt) {
    esp_task_wdt_reset();
    last = millis();
  }
}
