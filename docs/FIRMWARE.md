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

1. Boot, initialize camera, WiFi, and MQTT, and start the PIR warmup timer.
2. Publish retained `online` availability and a JSON status message.
3. On confirmed PIR motion or `record_now`, allocate `MAX_CLIP_BYTES` in PSRAM/RAM.
4. Capture `CLIP_FRAME_COUNT` JPEG frames into the buffer.
5. Upload the raw MJPEG buffer to `/upload` and publish JSON clip telemetry.
6. Free the buffer and wait for PIR idle before another motion trigger.
7. Stay online, publish a status heartbeat every 60 seconds, and handle MQTT commands.

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
home/devices/birdfeeder-01/responses
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

## MQTT Commands

The ESP32 subscribes to `home/devices/birdfeeder-01/commands` and publishes JSON replies to `home/devices/birdfeeder-01/responses`. Publish commands without the MQTT retain flag so an old command cannot replay when the ESP32 reconnects. Plain text command names also work.

| Command | Behavior |
| --- | --- |
| `record_now` | Capture and upload one clip, even in standby. Standby remains active afterward. |
| `stay_awake` | Resume normal camera and PIR operation from standby. The ESP32 is continuously powered, so this has no expiry. A legacy `duration_seconds` field is accepted but ignored. |
| `sleep_now` | Stop streaming, release the camera, and pause PIR recording. MQTT stays connected; this is standby, not ESP32 deep sleep. |
| `stream_on` | Serve MJPEG at `http://<esp32-ip>:81/stream`. Default 60 seconds, capped at 180 seconds. PIR recording is paused, then resumes when the stream times out. A second `stream_on` is rejected while streaming, so the timeout cannot be extended. |

Example on the Pi:

```bash
mosquitto_sub -h localhost -p 1883 -v -t 'home/devices/birdfeeder-01/responses'
mosquitto_pub -h localhost -p 1883 -t 'home/devices/birdfeeder-01/commands' -m '{"device":"birdfeeder-01","command":"stream_on","duration_seconds":60}'
```

Open the returned `stream_url` on a device that can reach the ESP32 LAN address. The stream is temporary and does not upload or save video. After timeout, normal PIR recording resumes. `sleep_now` leaves the device reachable via MQTT.

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
const int CAMERA_JPEG_QUALITY = 5;
```

For smaller uploads:

```cpp
const int CAMERA_JPEG_QUALITY = 12;
const uint32_t CLIP_FRAME_COUNT = 40;
```

For longer clips:

```cpp
const uint32_t CLIP_FRAME_COUNT = 75;
const int CAMERA_JPEG_QUALITY = 10;
```

Check actual clip sizes and PSRAM headroom before increasing the frame count or `MAX_CLIP_BYTES`.

## Troubleshooting

`Recording skipped: could not allocate ... clip buffer`

PSRAM is not enabled or the requested buffer is too large. The ESP32 should publish `clip_no_memory` telemetry if MQTT is reachable.

`Clip buffer full`

The clip exceeded `MAX_CLIP_BYTES`. Reduce quality, frame count, or resolution.

`HTTP body send failed`

The ESP32 did not send the full buffer. Check WiFi signal, server availability, and clip size.

`MQTT connect failed`

Confirm Mosquitto is running on the Pi, the Pi LAN IP is correct in `include/secrets.h`, and port `1883` is reachable from the ESP32 network.

`MQTT command subscription failed`

Check that the broker is reachable and accepts subscriptions to the device command topic.
