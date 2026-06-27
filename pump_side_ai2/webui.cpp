#include "webui.h"
#include "config.h"
#include "commands.h"
#include "mqtt_mgr.h"
#include "logging.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebSrv.h>

// Minimal local fallback. Handlers run on the AsyncTCP task, so they ONLY
// enqueue commands / record water — never touch hardware, timers, or state.
static AsyncWebServer server(80);

static const char STATUS_PAGE[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>WD Pump (fallback)</title>
<style>body{font-family:system-ui,sans-serif;background:#0a2035;color:#e6f6ff;margin:0;padding:20px}
h1{font-size:18px}pre{background:#05131e;padding:12px;border-radius:8px;overflow:auto}
button{font-size:15px;padding:10px 14px;margin:4px;border:0;border-radius:8px;background:#22d3ee;color:#06121f;cursor:pointer}
a{color:#22d3ee}</style>
<h1>WD Pump — local fallback</h1>
<p>Use this only when MQTT/Home Assistant is unavailable.</p>
<div>
<button onclick="g('manualpump=1')">Pump Run 5min</button>
<button onclick="g('manualpumpstop=1')">Pump Stop</button>
</div><div>
<button onclick="g('frontdoor=up')">Door Up</button>
<button onclick="g('frontdoor=down')">Door Down</button>
<button onclick="g('frontdoor=stop')">Door Stop</button>
</div>
<pre id=s>loading…</pre>
<script>
function g(q){fetch('/get?'+q).then(()=>setTimeout(load,300))}
function load(){fetch('/status.json').then(r=>r.json()).then(d=>{document.getElementById('s').textContent=JSON.stringify(d,null,2)})}
load();setInterval(load,3000);
</script>
)HTML";

void webuiInit() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", STATUS_PAGE);
  });

  server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *req) {
    // Serve the loop-produced cache; never build JSON on this (AsyncTCP) task.
    char buf[640];
    copyStatusCache(buf, sizeof(buf));
    req->send(200, "application/json", buf);
  });

  // Fallback command endpoint — ENQUEUE ONLY (loop processes it).
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *req) {
    String msg = "ok";
    if (req->hasParam("message")) {
      String v = req->getParam("message")->value();
      char b[24]; v.toCharArray(b, sizeof(b));
      char* endp = nullptr; float val = strtof(b, &endp);
      recordWater(val, endp != b);
      msg = "water=" + v;
    } else if (req->hasParam("frontdoor")) {
      String d = req->getParam("frontdoor")->value(); d.toLowerCase();
      if      (d == "up")   enqueueCommand(CMD_DOOR_UP);
      else if (d == "down") enqueueCommand(CMD_DOOR_DOWN);
      else if (d == "stop") enqueueCommand(CMD_DOOR_STOP);
      msg = "door=" + d;
    } else if (req->hasParam("manualpump")) {
      enqueueCommand(CMD_PUMP_RUN);  msg = "manualpump";
    } else if (req->hasParam("manualpumpstop")) {
      enqueueCommand(CMD_PUMP_STOP); msg = "manualpumpstop";
    } else if (req->hasParam("setmaxlevel")) {
      enqueueCommand(CMD_SET_MAX, req->getParam("setmaxlevel")->value().toFloat());
      msg = "setmax";
    } else if (req->hasParam("setminlevel")) {
      enqueueCommand(CMD_SET_MIN, req->getParam("setminlevel")->value().toFloat());
      msg = "setmin";
    } else if (req->hasParam("setdeficientlevel")) {
      enqueueCommand(CMD_SET_DEFICIENT, req->getParam("setdeficientlevel")->value().toFloat());
      msg = "setdeficient";
    } else {
      msg = "no command";
    }
    req->send(200, "text/plain", msg);
  });

  server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
}
