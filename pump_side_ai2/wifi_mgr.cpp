#include "wifi_mgr.h"
#include <WiFi.h>
#include "config.h"
#include "logging.h"
#include "arduino_secrets.h"

static const char* ssid     = SECRET_WIFI_SSID;
static const char* password = SECRET_WIFI_PASS;
static unsigned long lastWifiAttempt = 0;
static bool wifiWasConnected = false;

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

// Heavy one-time setup (mode/static-IP/auto-reconnect) runs ONCE; retries just
// re-associate without powering the radio off — so we don't fight the SDK's
// auto-reconnect, tear down the netif, or destabilize AsyncWebServer.
void wifiBegin() {
  static bool configured = false;
  if (!configured) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.config(cfg::DEVICE_IP, cfg::GATEWAY, cfg::SUBNET, cfg::DNS);
    configured = true;
  }
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  lastWifiAttempt = millis();
}

void serviceWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      logVerbose("WiFi connected: " + WiFi.localIP().toString());
    }
    return;
  }
  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi lost — will keep retrying");
    logWarning("WiFi disconnected");
  }
  // Let the SDK auto-reconnect; only re-issue WiFi.begin() if stuck a while and
  // not already mid-association (avoid stomping an in-flight attempt).
  if (millis() - lastWifiAttempt >= cfg::WIFI_RETRY_MS &&
      WiFi.status() != WL_IDLE_STATUS) {
    Serial.println("Retrying WiFi.begin()...");
    wifiBegin();
  }
}
