# Hardware Notes

## Controller

Seeed Studio XIAO ESP32S3 Sense with the camera module installed.

Important setup details:

- Use a 2.4 GHz WiFi network.
- Keep the WiFi antenna attached.
- Enable PSRAM through PlatformIO configuration.
- Use USB-C while developing.
- Use a regulated 5V source for deployment.

## Camera Pins

The firmware uses the XIAO ESP32S3 Sense camera pin mapping from the working Arduino sketch:

```cpp
XCLK  = GPIO 10
SIOD  = GPIO 40
SIOC  = GPIO 39
Y9    = GPIO 48
Y8    = GPIO 11
Y7    = GPIO 12
Y6    = GPIO 14
Y5    = GPIO 16
Y4    = GPIO 18
Y3    = GPIO 17
Y2    = GPIO 15
VSYNC = GPIO 38
HREF  = GPIO 47
PCLK  = GPIO 13
```

## PIR Sensor

Current sensor: HC-SR501 PIR.

```text
HC-SR501 VCC  -> XIAO 5V
HC-SR501 GND  -> XIAO GND
HC-SR501 OUT  -> XIAO GPIO 2
```

The firmware configures GPIO 2 as `INPUT_PULLDOWN` and treats motion as `HIGH`.

The HC-SR501 has onboard sensitivity and hold-time trim pots. For bird feeder use, start with a shorter hold time so the ESP32 can return to deep sleep sooner after an upload.

## Power Notes

The ESP32 enters deep sleep between PIR triggers. The camera and WiFi are shut down before sleep. Real-world current draw may still be higher than the ESP32 datasheet value because the XIAO Sense camera hardware and PIR module remain powered.

