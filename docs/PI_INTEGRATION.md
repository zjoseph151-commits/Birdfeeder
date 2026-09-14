# Raspberry Pi IoT Platform Integration

This project integrates with the Raspberry Pi IoT platform by publishing MQTT metadata from the ESP32 while keeping actual video transfer on the existing HTTP upload path.

## Integration Shape

- Video files: ESP32 uploads raw MJPEG over HTTP to the Birdfeeder Flask server.
- Metadata: ESP32 publishes MQTT directly to Mosquitto on the Raspberry Pi.
- Server MQTT: disabled by design; the Flask server does not publish MQTT.
- Device ID: `birdfeeder-01`
- Expected Pi project path: `/home/zack/projects/Birdfeeder`
- Expected video folder: `/home/zack/projects/Birdfeeder/bird_videos`

## MQTT Topics

```text
home/devices/birdfeeder-01/status
home/devices/birdfeeder-01/availability
home/devices/birdfeeder-01/telemetry
```

Availability uses retained plain text:

```text
online
offline
```

Status and telemetry use valid JSON and include the current platform health fields:

```json
{"device":"birdfeeder-01","type":"status","count":1,"uptime_ms":1000,"wifi_rssi":-55}
```

Extra telemetry fields are intentionally included so the Pi dashboard can show clip details in the expandable row.

## ESP32 Configuration

Update `include/secrets.h`:

```cpp
#define BIRD_WIFI_SSID "your-2.4ghz-wifi"
#define BIRD_WIFI_PASSWORD "your-wifi-password"
#define BIRD_SERVER_HOST "raspberry-pi-lan-ip"
#define BIRD_SERVER_PORT 5000
#define BIRD_MQTT_HOST "raspberry-pi-lan-ip"
#define BIRD_MQTT_PORT 1883
```

The ESP32 should not use `localhost` for either HTTP or MQTT.

## Pi Manual Verification

Watch live Birdfeeder MQTT traffic:

```bash
mosquitto_sub -h localhost -p 1883 -v -t 'home/devices/birdfeeder-01/#'
```

Publish a fake status message:

```bash
mosquitto_pub -h localhost -p 1883 \
  -t 'home/devices/birdfeeder-01/status' \
  -m '{"device":"birdfeeder-01","type":"status","count":1,"uptime_ms":1000,"wifi_rssi":-55}'
```

Publish fake clip telemetry:

```bash
mosquitto_pub -h localhost -p 1883 \
  -t 'home/devices/birdfeeder-01/telemetry' \
  -m '{"device":"birdfeeder-01","type":"telemetry","count":2,"uptime_ms":12000,"wifi_rssi":-55,"event":"clip_complete","filename":"bird_test.mjpeg","frames":50,"bytes":800000,"capture_duration_ms":9860,"capture_ok":true,"upload_ok":true,"upload_attempts":1,"http_status":200}'
```

Publish retained availability:

```bash
mosquitto_pub -h localhost -p 1883 -r \
  -t 'home/devices/birdfeeder-01/availability' \
  -m 'online'
```

Check platform logs:

```bash
tail -n 20 data/logs/mqtt_messages.log
tail -n 20 data/logs/mqtt_messages.jsonl
tail -n 20 data/logs/mqtt_messages.csv
```

Run the health monitor immediately:

```bash
/home/zack/projects/raspberry-pi/.venv/bin/python \
  /home/zack/projects/raspberry-pi/services/health-monitor/health_monitor.py
```

Print the health table:

```bash
/home/zack/projects/raspberry-pi/.venv/bin/python \
  /home/zack/projects/raspberry-pi/services/health-monitor/device_status.py
```

Check services:

```bash
systemctl status mqtt-listener.service --no-pager
systemctl status health-monitor.timer --no-pager
systemctl status health-monitor.service --no-pager
systemctl status iot-dashboard.service --no-pager
journalctl -u mqtt-listener.service -n 50 --no-pager
journalctl -u health-monitor.service -n 50 --no-pager
journalctl -u iot-dashboard.service -n 50 --no-pager
```

Open the dashboard:

```text
http://<raspberry-pi-lan-ip>:8080
```

Expected dashboard behavior:

- `birdfeeder-01` appears after a valid JSON status or telemetry message.
- Heartbeat freshness may become `STALE` between triggers because the ESP32 deep sleeps.
- Availability should show `ONLINE` while awake and `OFFLINE` after intentional sleep.
- Latest clip metadata appears in the expandable telemetry details.

## Server Verification

Run the Birdfeeder Flask server on the Pi:

```bash
cd /home/zack/projects/Birdfeeder
source .venv/bin/activate
python server/bird_server.py
```

Check health locally on the Pi:

```bash
curl http://127.0.0.1:5000/health
```

Check that videos are written:

```bash
ls -lh /home/zack/projects/Birdfeeder/bird_videos
```

## Deferred Work

- Add a systemd service for the Birdfeeder Flask server.
- Add retention/cleanup for old AVI files.
- Add command handling only if remote control becomes useful.
- Add battery voltage telemetry if the hardware exposes a safe ADC measurement.
