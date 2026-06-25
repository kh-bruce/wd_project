// =====================================================================
// webui.h — minimal local fallback web interface.
// A tiny status page (GET /) and a /get command endpoint for use when the
// MQTT broker / Home Assistant is unavailable. Handlers run on the AsyncTCP
// task, so they ONLY enqueue commands / record water — never touch hardware.
// The old full SPA, SSE /events, and log endpoints are removed.
// =====================================================================
#ifndef WEBUI_H
#define WEBUI_H

#include <Arduino.h>

void webuiInit();   // register routes + server.begin(); call in setup()

#endif // WEBUI_H
