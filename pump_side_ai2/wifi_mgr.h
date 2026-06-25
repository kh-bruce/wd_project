// =====================================================================
// wifi_mgr.h — non-blocking WiFi connect + auto-reconnect.
// setup() kicks off a connection and waits briefly but NEVER blocks the
// board into a dead state; loop() keeps it alive via serviceWifi().
// =====================================================================
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <Arduino.h>

void wifiBegin();        // (re)issue a connection attempt — non-blocking
void serviceWifi();      // call every loop(): reconnect if dropped
bool wifiConnected();

#endif // WIFI_MGR_H
