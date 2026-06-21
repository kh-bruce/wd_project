// Copy this file to `arduino_secrets.h` (same folder) and fill in real values.
// arduino_secrets.h is gitignored; this .example.h is the committed template.
#ifndef ARDUINO_SECRETS_H
#define ARDUINO_SECRETS_H

#define SECRET_WIFI_SSID     "iHome"
#define SECRET_WIFI_PASS     "REPLACE_ME"

// Use the NUMERIC LAN IP of the Docker host (zz0004) — NOT "zz0004.local".
// ESP32 + PubSubClient does not resolve mDNS .local names. Find it: `hostname -I`.
#define SECRET_MQTT_HOST     "192.168.1.xxx"
#define SECRET_MQTT_PORT     1883
#define SECRET_MQTT_USER     "wd_mqtt"
#define SECRET_MQTT_PASS     "REPLACE_ME"
#define SECRET_MQTT_CLIENTID "wd-tower"

#endif
