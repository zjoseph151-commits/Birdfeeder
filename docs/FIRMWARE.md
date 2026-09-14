# Firmware Notes

## Build Target

PlatformIO environment:

```ini
[env:seeed_xiao_esp32s3]
platform = espressif32
board = seeed_xiao_esp32s3
framework = arduino
```

The firmware depends on PSRAM for the clip buffer:

```ini
board_build.arduino.memory_type = qio_opi
board_build.psram_type = opi
build_flags =
  -DBOARD_HAS_PSRAM
```

The MQTT dependency is:

```ini
lib_deps =
  knolleary/PubSubClient@^2.8
```

## Local Configuration

Copy the template:

```powershell
Copy-Item include\secrets.example.h include\secrets.h
```

Update `include/secrets.h`:

```cpp
#define BIRD_WIFI_SSID "your-2.4ghz-wifi"
#define BIRD_WIFI_PASSWORD "your-wifi-password"
#define BIRD_SERVER_HOST "raspberry-pi-lan-ip"
#define BIRD_SERVER_PORT 5000
#define BIRD_MQTT_HOST "raspberry-pi-lan-ip"
#define BIRD_MQTT_PORT 1883
```

The ESP32 must use the Raspberry Pi LAN IP or hostname. `localhost` only points back to the ESP32 from the ESP32's point of view.

## Capture Flow

1. Wake from PIR motion or cold boot.
2. Initialize camera, WiFi, and MQTT.
3. Publish retained `online` availability and a JSON status message.
4. Allocate `MAX_CLIP_BYTES` in PSRAM/RAM.
5. Capture `CLIP_FRAME_COUNT` JPEG frames into the buffer.
6. Upload the raw MJPEG buffer to `/upload`.
7. Publish JSON clip telemetry.
8. Free the buffer.
9. Wait for PIR idle.
10. Publish sleep status and retained `offline` availability.
11. Return to deep sleep.

## MQTT Contract

Device ID:

```text
birdfeeder-01
```

Published topics:

```text
home/devices/birdfeeder-01/status
home/devices/birdfeeder-01/availability
home/devices/birdfeeder-01/telemetry
```

Availability payloads are retained plain text:

```text
online
offline
```

Status JSON includes the Raspberry Pi platform health fields:

```json
{"device":"birdfeeder-01","type":"status","count":1,"uptime_ms":1000,"wifi_rssi":-55}
```

Telemetry JSON includes the same health fields plus clip details:

```json
{"device":"birdfeeder-01","type":"telemetry","count":2,"uptime_ms":12000,"wifi_rssi":-55,"event":"clip_complete","filename":"bird_00000012_0001.mjpeg","frames":50,"bytes":800000,"capture_duration_ms":9860,"capture_ok":true,"upload_ok":true,"upload_attempts":1,"http_status":200}
```

The firmware intentionally does not subscribe to commands yet.

## Upload Format

The ESP32 sends raw concatenated JPEG frames with these headers:

```text
Content-Type: application/octet-stream
X-Video-Format: raw-mjpeg
X-Frame-Count: <frame count>
X-Frame-Interval-Ms: <interval>
X-Video-Duration-Ms: <capture duration>
X-Filename: bird_<seconds>_<counter>.mjpeg
```

The Flask server saves the request body, extracts JPEG frames, and writes an MJPEG AVI.

## Common Tuning Changes

For better quality:

```cpp
const int CAMERA_JPEG_QUALITY = 8;
```

For smaller uploads:

```cpp
const int CAMERA_JPEG_QUALITY = 12;
const uint32_t CLIP_FRAME_COUNT = 40;
```

For longer clips:

```cpp
const uint32_t CLIP_FRAME_COUNT = 75;
const size_t MAX_CLIP_BYTES = 6UL * 1024UL * 1024UL;
```

Only increase `MAX_CLIP_BYTES` if PSRAM allocation still succeeds.

## Troubleshooting

`Recording skipped: could not allocate ... clip buffer`

PSRAM is not enabled or the requested buffer is too large. The ESP32 should publish `clip_no_memory` telemetry if MQTT is reachable.

`Clip buffer full`

The clip exceeded `MAX_CLIP_BYTES`. Reduce quality, frame count, or resolution.

`HTTP body send failed`

The ESP32 did not send the full buffer. Check WiFi signal, server availability, and clip size.

`MQTT connect failed`

Confirm Mosquitto is running on the Pi, the Pi LAN IP is correct in `include/secrets.h`, and port `1883` is reachable from the ESP32 network.

`No motion at boot, entering deep sleep`

Normal behavior after cold boot and PIR warmup.
