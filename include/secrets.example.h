#pragma once

// Copy this file to include/secrets.h and update the values for your network.
// The ESP32 must use the Raspberry Pi LAN IP or hostname, not localhost.

#define BIRD_WIFI_SSID "your-2.4ghz-wifi"
#define BIRD_WIFI_PASSWORD "your-wifi-password"

#define BIRD_SERVER_HOST "10.0.0.250"
#define BIRD_SERVER_PORT 5000

#define BIRD_MQTT_HOST "10.0.0.250"
#define BIRD_MQTT_PORT 1883
