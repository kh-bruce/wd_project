// =====================================================================
// mqtt_mgr.h — MQTT client: connect/LWT/reconnect, HA discovery,
// status publishing (ArduinoJson), and the inbound callback.
// The callback runs on the AsyncTCP/PubSubClient context — it ONLY
// enqueues commands / records water, never touches hardware directly.
// =====================================================================
#ifndef MQTT_MGR_H
#define MQTT_MGR_H

#include <Arduino.h>

void mqttInit();             // setServer/buffer/callback; call in setup()
void mqttService();          // call every loop(): reconnect (gated) + mqtt.loop()
void publishStatusMqtt();    // publish status JSON + flat state topics (loop thread)
bool mqttIsConnected();

// Build the status JSON (ArduinoJson) into the provided buffer; returns length.
// LOOP THREAD ONLY (reads loop-owned state incl. String lastCommand).
size_t buildStatusJson(char *buf, size_t buflen);

// Refresh the loop-produced status cache (call from loop thread).
void refreshStatusCache();
// Copy the cached status JSON (safe to call from the web/AsyncTCP task).
size_t copyStatusCache(char *out, size_t outlen);

#endif // MQTT_MGR_H
