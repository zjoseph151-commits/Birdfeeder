# Bird Feeder Camera

Motion-triggered bird feeder camera using a Seeed Studio XIAO ESP32S3 Sense, HC-SR501 PIR sensor, MQTT control and metadata, and a local Flask video upload server.

The ESP32 stays powered and connected, records a fixed-length MJPEG clip into PSRAM on PIR motion or MQTT command, uploads the raw MJPEG bytes to the Raspberry Pi Flask server, and publishes MQTT status and clip telemetry. The server converts the raw MJPEG upload into an AVI file for playback.

## Project Layout

```text
.
|-- platformio.ini              PlatformIO firmware configuration
|-- include/secrets.example.h   Local config template
|-- include/secrets.h           Local private config, ignored by git
|-- src/main.cpp                ESP32 Arduino firmware
|-- server/bird_server.py       Flask upload server
|-- requirements.txt            Python server dependencies
|-- docs/HARDWARE.md            Wiring and hardware notes
|-- docs/FIRMWARE.md            Firmware settings and MQTT notes
|-- docs/SERVER.md              Server setup notes
|-- docs/PI_INTEGRATION.md      Raspberry Pi IoT platform integration
`-- bird_videos/                Generated videos, ignored by git
```

Legacy Arduino sketches and notes are still in the root folder as reference material.

## Hardware

- Seeed Studio XIAO ESP32S3 Sense with camera module
- HC-SR501 PIR motion sensor
- 2.4 GHz WiFi network
- USB-C for programming and testing
- Regulated 5V wall supply

Current PIR wiring:

```text
HC-SR501 VCC  -> XIAO 5V
HC-SR501 GND  -> XIAO GND
HC-SR501 OUT  -> XIAO GPIO 2
```

The HC-SR501 output is treated as active `HIGH`.

## Firmware Build And Upload

1. Install the PlatformIO VS Code extension.
2. Open this folder in VS Code: `C:\Users\zjose\Documents\Birdfeeder`
3. Copy `include/secrets.example.h` to `include/secrets.h`.
4. Update `include/secrets.h` with the 2.4 GHz WiFi credentials and Raspberry Pi LAN address.
5. Use PlatformIO: `Build`, then `Upload`, then `Monitor`.

The ESP32 must use the Raspberry Pi LAN IP or hostname for both HTTP and MQTT. Do not use `127.0.0.1` or `localhost` in ESP32 firmware.

The configured PlatformIO board is `seeed_xiao_esp32s3`. PSRAM is required because the clip is stored in a RAM buffer before upload.

## Raspberry Pi Server Setup

Expected Pi project path:

```bash
/home/zack/projects/Birdfeeder
```

From that folder:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python server/bird_server.py
```

The server listens on port `5000` and, at the expected Pi path, saves AVI files into:

```text
/home/zack/projects/Birdfeeder/bird_videos
```

The ESP32 should upload to:

```text
http://<raspberry-pi-lan-ip>:5000/upload
```

## Raspberry Pi MQTT Integration

Device ID:

```text
birdfeeder-01
```

The ESP32 publishes directly to the Pi MQTT broker using the preferred platform topic contract:

```text
home/devices/birdfeeder-01/status
home/devices/birdfeeder-01/availability
home/devices/birdfeeder-01/telemetry
```

Availability is retained plain text: `online` while connected and `offline` if the MQTT connection fails unexpectedly. Status and telemetry are compact JSON objects that include `device`, `type`, `count`, `uptime_ms`, and `wifi_rssi`, plus extra clip details for the dashboard expandable view. The ESP32 publishes a status heartbeat every 60 seconds.

Commands go to `home/devices/birdfeeder-01/commands`; responses appear on `home/devices/birdfeeder-01/responses`. `stream_on` serves a temporary MJPEG stream at `http://<esp32-ip>:81/stream` for at most 180 seconds.

## Current Firmware Behavior

- PIR recording is gated during the 45-second cold boot warmup; MQTT remains available.
- ESP32 connects to WiFi and MQTT.
- ESP32 publishes retained `online` availability and a boot status message.
- PIR `HIGH` triggers recording after confirmation.
- ESP32 records `50` SVGA JPEG frames at a `200 ms` interval.
- Clip bytes are stored in a `6 MB` PSRAM/RAM buffer.
- ESP32 uploads the buffer with a manual HTTP POST.
- ESP32 publishes MQTT clip telemetry with frame count, byte count, capture time, upload result, and HTTP status.
- Server converts raw MJPEG to AVI.
- ESP32 stays online and waits for PIR idle before accepting the next motion trigger.
- `sleep_now` pauses PIR recording and releases the camera while keeping MQTT online; `stay_awake` resumes normal operation.
- `record_now` records one clip from either normal or standby mode. `stream_on` pauses PIR recording until its timeout, then resumes it.

## Tuning

Common firmware constants live near the top of `src/main.cpp`:

- `CLIP_FRAME_COUNT`: number of frames per trigger
- `FRAME_INTERVAL_MS`: time between captured frames
- `CAMERA_FRAME_SIZE`: current value is `FRAMESIZE_SVGA`
- `CAMERA_JPEG_QUALITY`: lower number means better quality and larger clips
- `MAX_CLIP_BYTES`: maximum in-memory clip size before upload
- `PIR_COLD_BOOT_WARMUP_MS`: PIR stabilization delay on power-up

If uploads become unreliable, first reduce clip size by increasing `CAMERA_JPEG_QUALITY` to `12` or reducing `CLIP_FRAME_COUNT`.

## Notes

- ESP32 only supports 2.4 GHz WiFi.
- Arduino/PlatformIO PSRAM support must work, or the firmware will fail to allocate the clip buffer.
- Occasional `cam_hal: FB-OVF` messages have been observed. They are acceptable if captures still complete at the expected frame count.
- Generated videos are ignored by git.
- Retention/cleanup for old videos is intentionally deferred.
