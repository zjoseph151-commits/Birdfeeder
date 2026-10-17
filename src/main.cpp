#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <PubSubClient.h>
#include "esp_heap_caps.h"
#include "esp_camera.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and update your local settings."
#endif

#ifndef BIRD_SERVER_PORT
#define BIRD_SERVER_PORT 5000
#endif

#ifndef BIRD_MQTT_PORT
#define BIRD_MQTT_PORT 1883
#endif

// XIAO ESP32-S3 Sense camera pins
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// HC-SR501 PIR motion sensor
#define PIR_SENSOR_PIN        2
#define MOTION_ACTIVE_LEVEL   HIGH

const char* DEVICE_ID = "birdfeeder-01";
const char* FIRMWARE_VERSION = "0.4.0";

const char* WIFI_SSID = BIRD_WIFI_SSID;
const char* WIFI_PASSWORD = BIRD_WIFI_PASSWORD;
const char* SERVER_HOST = BIRD_SERVER_HOST;
const uint16_t SERVER_PORT = BIRD_SERVER_PORT;
const char* SERVER_PATH = "/upload";

const char* MQTT_HOST = BIRD_MQTT_HOST;
const uint16_t MQTT_PORT = BIRD_MQTT_PORT;

const bool ENABLE_TEST_TRIGGER = false;
const uint32_t TEST_TRIGGER_INTERVAL_MS = 30000;

const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
const uint32_t MQTT_CONNECT_TIMEOUT_MS = 8000;
const uint32_t HTTP_UPLOAD_TIMEOUT_MS = 60000;
const uint16_t MQTT_BUFFER_SIZE = 768;
const uint16_t MQTT_KEEPALIVE_SECONDS = 15;
const uint16_t MQTT_SOCKET_TIMEOUT_SECONDS = 5;
const uint8_t MQTT_PUBLISH_RETRY_COUNT = 2;
const uint32_t MQTT_PUBLISH_RETRY_DELAY_MS = 150;
const uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;
const uint32_t STATUS_INTERVAL_MS = 60000;
const size_t MQTT_COMMAND_BUFFER_SIZE = 256;
const uint16_t STREAM_HTTP_PORT = 81;
const uint32_t STREAM_DEFAULT_SECONDS = 60;
const uint32_t STREAM_MAX_SECONDS = 180;
const uint32_t STREAM_FRAME_INTERVAL_MS = 200;
const uint32_t STREAM_CLIENT_WRITE_TIMEOUT_MS = 2000;
const char* STREAM_HTTP_PATH = "/stream";
const char* STREAM_BOUNDARY = "birdframe";
const size_t UPLOAD_BUFFER_SIZE = 2048;
const size_t MAX_CLIP_BYTES = 6UL * 1024UL * 1024UL;
const uint32_t FRAME_INTERVAL_MS = 200;      // 5 FPS playback target
const uint32_t CLIP_FRAME_COUNT = 50;        // Fixed frame count per trigger
const uint32_t CAPTURE_START_DELAY_MS = 150;
const uint8_t FRAME_CAPTURE_RETRIES = 3;
const uint8_t UPLOAD_RETRY_COUNT = 3;
const uint32_t UPLOAD_RETRY_DELAY_MS = 1500;
const uint32_t PIR_COLD_BOOT_WARMUP_MS = 45000;
const uint32_t MOTION_CONFIRM_MS = 120;
const uint32_t SENSOR_RESET_MS = 750;
const uint32_t RETRIGGER_BLOCK_MS = 5000;

const framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_SVGA;
const int CAMERA_JPEG_QUALITY = 6;          // Lower number = better JPEG quality
const int CAMERA_BRIGHTNESS = -2;            // -2 to 2
const int CAMERA_CONTRAST = -2;              // -2 to 2
const int CAMERA_SATURATION = 2;            // -2 to 2
const int CAMERA_SHARPNESS = 2;             // -2 to 2
const int CAMERA_DENOISE = 1;               // 0 off, 1 on
const int CAMERA_WHITE_BALANCE = 1;         // 0 off, 1 on
const int CAMERA_AWB_GAIN = 1;              // 0 off, 1 on
const int CAMERA_GAIN_CONTROL = 1;          // 0 off, 1 on
const int CAMERA_EXPOSURE_CONTROL = 1;      // 0 off, 1 on
const int CAMERA_AEC2 = 1;                  // 0 off, 1 on
const int CAMERA_AE_LEVEL = 0;              // -2 to 2
const int CAMERA_BAD_PIXEL_CORRECT = 1;     // 0 off, 1 on
const int CAMERA_WHITE_PIXEL_CORRECT = 1;   // 0 off, 1 on
const int CAMERA_RAW_GAMMA = 1;             // 0 off, 1 on
const int CAMERA_LENS_CORRECT = 1;          // 0 off, 1 on

bool cameraReady = false;
bool recording = false;
bool waitingForSensorReset = false;
bool standbyMode = false;
bool streamActive = false;
bool streamServerRunning = false;
bool pendingCommandAvailable = false;
bool pendingCommandOverflow = false;

WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
WiFiServer streamServer(STREAM_HTTP_PORT, 1);
WiFiClient streamClient;

RTC_DATA_ATTR uint32_t clipCounter = 0;
RTC_DATA_ATTR uint32_t mqttMessageCounter = 0;
uint32_t motionActiveSinceMs = 0;
uint32_t sensorResetSinceMs = 0;
uint32_t lastRecordingFinishedMs = 0;
uint32_t lastTestTriggerMs = 0;
uint32_t pirReadyAtMs = 0;
uint32_t lastMqttReconnectAttemptMs = 0;
uint32_t lastStatusMs = 0;
uint32_t streamStartMs = 0;
uint32_t streamDurationMs = 0;
uint32_t nextStreamFrameMs = 0;
uint32_t streamFrameCount = 0;
int lastUploadHttpStatus = 0;
char pendingCommandPayload[MQTT_COMMAND_BUFFER_SIZE];

bool initCamera();
bool ensureWiFiConnected();
void initMqtt();
void serviceMqtt();
bool ensureMqttConnected();
void onMqttMessage(char* topic, uint8_t* payload, unsigned int length);
void buildMqttTopic(const char* messageType, char* topic, size_t topicSize);
uint32_t nextMqttCount();
bool timeReached(uint32_t deadlineMs);
bool parseCommandPayload(const char* payload, char* command, size_t commandSize, uint32_t& durationSeconds);
void processPendingMqttCommand();
bool publishCommandResponse(const char* command, bool success, const char* detail, uint32_t durationSeconds, const char* streamUrl);
bool publishMqttPayload(const char* messageType, const char* payload, bool retained);
bool publishAvailability(const char* state, bool retained);
bool publishStatusEvent(const char* eventName);
bool publishClipTelemetry(const char* eventName, const char* filename, bool captureOk, bool uploadOk, uint32_t frameCount, size_t clipBytes, uint32_t captureDurationMs, uint8_t uploadAttempts, int httpStatus);
void handleTriggers();
bool recordAndUploadVideo();
bool captureClipToMemory(uint8_t* clipData, size_t clipCapacity, uint32_t& frameCount, size_t& totalBytes, uint32_t& captureDurationMs);
bool captureFrameToBuffer(uint8_t* clipData, size_t clipCapacity, size_t& totalBytes);
bool uploadClipBuffer(const uint8_t* clipData, size_t clipSize, const char* serverFilename, uint32_t frameCount, uint32_t captureDurationMs);
bool motionDetectedNow();
void applyCameraSensorProfile(sensor_t* sensor);
void printServerTarget();
void flushCameraFrames(uint8_t count);
void buildStreamUrl(char* url, size_t urlSize);
bool startStreamMode(uint32_t durationSeconds);
void stopStreamMode(const char* reason);
void handleStreamMode();
void acceptStreamClient();
bool writeStreamAll(WiFiClient& client, const uint8_t* data, size_t len);
bool writeStreamText(WiFiClient& client, const char* value);
void sendStreamFrame();
bool writeAll(WiFiClient& client, const uint8_t* data, size_t len);
bool writeText(WiFiClient& client, const String& text);
bool streamBufferToClient(WiFiClient& client, const uint8_t* data, size_t len, size_t& bytesSent);
bool waitForHttpResponse(WiFiClient& client, int& statusCode, String& responseBody);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Bird Feeder Video Uploader");
  initMqtt();

  pinMode(PIR_SENSOR_PIN, INPUT_PULLDOWN);
  pirReadyAtMs = millis() + PIR_COLD_BOOT_WARMUP_MS;
  Serial.printf("PIR warmup: %lu seconds\n",
                static_cast<unsigned long>(PIR_COLD_BOOT_WARMUP_MS / 1000UL));

  cameraReady = initCamera();
  if (!cameraReady) {
    Serial.println("Camera initialization failed");
  } else {
    Serial.println("Camera ready");
  }

  if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASSWORD) == 0) {
    Serial.println("WiFi credentials are empty");
  } else {
    const bool wifiOk = ensureWiFiConnected();
    printServerTarget();
    if (wifiOk) {
      publishAvailability("online", true);
      publishStatusEvent("boot");
    }
  }

  lastTestTriggerMs = millis();
  lastStatusMs = millis();
}

void loop() {
  serviceMqtt();
  processPendingMqttCommand();
  handleStreamMode();
  if (!streamActive && !standbyMode) {
    handleTriggers();
  }
  delay(10);
}

void handleTriggers() {
  if (!cameraReady || recording || !timeReached(pirReadyAtMs)) {
    return;
  }

  const uint32_t now = millis();

  if (ENABLE_TEST_TRIGGER && (now - lastTestTriggerMs >= TEST_TRIGGER_INTERVAL_MS)) {
    lastTestTriggerMs = now;
    Serial.println("Test trigger");
    recordAndUploadVideo();
    return;
  }

  const bool motionActive = motionDetectedNow();

  if (waitingForSensorReset) {
    if (!motionActive) {
      if (sensorResetSinceMs == 0) {
        sensorResetSinceMs = now;
      } else if (now - sensorResetSinceMs >= SENSOR_RESET_MS) {
        waitingForSensorReset = false;
        sensorResetSinceMs = 0;
        Serial.println("Sensor reset complete");
      }
    } else {
      sensorResetSinceMs = 0;
    }
    return;
  }

  if (lastRecordingFinishedMs != 0 && (now - lastRecordingFinishedMs < RETRIGGER_BLOCK_MS)) {
    return;
  }

  if (motionActive) {
    if (motionActiveSinceMs == 0) {
      motionActiveSinceMs = now;
    } else if (now - motionActiveSinceMs >= MOTION_CONFIRM_MS) {
      Serial.println("Motion confirmed");
      recordAndUploadVideo();
      motionActiveSinceMs = 0;
    }
  } else {
    motionActiveSinceMs = 0;
  }
}

bool motionDetectedNow() {
  return digitalRead(PIR_SENSOR_PIN) == MOTION_ACTIVE_LEVEL;
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = CAMERA_FRAME_SIZE;
  config.jpeg_quality = CAMERA_JPEG_QUALITY;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init failed: 0x%X\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    applyCameraSensorProfile(sensor);
  }

  return true;
}

void applyCameraSensorProfile(sensor_t* sensor) {
  if (sensor == nullptr) {
    return;
  }

  sensor->set_framesize(sensor, CAMERA_FRAME_SIZE);
  sensor->set_quality(sensor, CAMERA_JPEG_QUALITY);
  sensor->set_brightness(sensor, CAMERA_BRIGHTNESS);
  sensor->set_contrast(sensor, CAMERA_CONTRAST);
  sensor->set_saturation(sensor, CAMERA_SATURATION);
  sensor->set_sharpness(sensor, CAMERA_SHARPNESS);
  sensor->set_denoise(sensor, CAMERA_DENOISE);
  sensor->set_whitebal(sensor, CAMERA_WHITE_BALANCE);
  sensor->set_awb_gain(sensor, CAMERA_AWB_GAIN);
  sensor->set_gain_ctrl(sensor, CAMERA_GAIN_CONTROL);
  sensor->set_exposure_ctrl(sensor, CAMERA_EXPOSURE_CONTROL);
  sensor->set_aec2(sensor, CAMERA_AEC2);
  sensor->set_ae_level(sensor, CAMERA_AE_LEVEL);
  sensor->set_bpc(sensor, CAMERA_BAD_PIXEL_CORRECT);
  sensor->set_wpc(sensor, CAMERA_WHITE_PIXEL_CORRECT);
  sensor->set_raw_gma(sensor, CAMERA_RAW_GAMMA);
  sensor->set_lenc(sensor, CAMERA_LENS_CORRECT);

  Serial.printf("Camera sensor profile: brightness %d, contrast %d, saturation %d, sharpness %d, denoise %d\n",
                CAMERA_BRIGHTNESS,
                CAMERA_CONTRAST,
                CAMERA_SATURATION,
                CAMERA_SHARPNESS,
                CAMERA_DENOISE);
  Serial.printf("Camera auto controls: white balance %d, AWB gain %d, gain %d, exposure %d, AEC2 %d, AE level %d\n",
                CAMERA_WHITE_BALANCE,
                CAMERA_AWB_GAIN,
                CAMERA_GAIN_CONTROL,
                CAMERA_EXPOSURE_CONTROL,
                CAMERA_AEC2,
                CAMERA_AE_LEVEL);
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASSWORD) == 0) {
    Serial.println("WiFi credentials missing");
    return false;
  }

  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs < WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.print(".");
    delay(250);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi connect failed, status=%d\n", WiFi.status());
    return false;
  }

  Serial.print("WiFi connected, IP=");
  Serial.println(WiFi.localIP());
  return true;
}

void initMqtt() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttClient.setCallback(onMqttMessage);
}

void serviceMqtt() {
  if (mqttClient.connected()) {
    mqttClient.loop();
  } else if (millis() - lastMqttReconnectAttemptMs >= MQTT_RECONNECT_INTERVAL_MS) {
    lastMqttReconnectAttemptMs = millis();
    ensureMqttConnected();
  }

  if (mqttClient.connected() && millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = millis();
    publishStatusEvent("heartbeat");
  }
}

bool ensureMqttConnected() {
  if (mqttClient.connected()) {
    mqttClient.loop();
    return true;
  }

  if (strlen(MQTT_HOST) == 0) {
    Serial.println("MQTT host is empty");
    return false;
  }

  if (!ensureWiFiConnected()) {
    Serial.println("MQTT skipped because WiFi is unavailable");
    return false;
  }

  char availabilityTopic[96];
  buildMqttTopic("availability", availabilityTopic, sizeof(availabilityTopic));

  char clientId[80];
  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "%s-%04X%08X",
           DEVICE_ID,
           static_cast<unsigned int>((chipId >> 32) & 0xFFFF),
           static_cast<unsigned int>(chipId & 0xFFFFFFFF));

  Serial.print("Connecting to MQTT");
  const uint32_t startMs = millis();
  while (!mqttClient.connected() && (millis() - startMs < MQTT_CONNECT_TIMEOUT_MS)) {
    if (mqttClient.connect(clientId, availabilityTopic, 0, true, "offline")) {
      char commandsTopic[96];
      buildMqttTopic("commands", commandsTopic, sizeof(commandsTopic));
      if (!mqttClient.subscribe(commandsTopic)) {
        Serial.println("MQTT command subscription failed");
        mqttClient.disconnect();
        return false;
      }
      mqttClient.publish(availabilityTopic, "online", true);
      Serial.print(", client=");
      Serial.println(clientId);
      Serial.print("MQTT subscribed ");
      Serial.println(commandsTopic);
      mqttClient.loop();
      return true;
    }

    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.printf("MQTT connect failed, state=%d\n", mqttClient.state());
  return false;
}

void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  char commandsTopic[96];
  buildMqttTopic("commands", commandsTopic, sizeof(commandsTopic));
  if (strcmp(topic, commandsTopic) != 0 || pendingCommandAvailable) {
    return;
  }

  pendingCommandOverflow = length >= sizeof(pendingCommandPayload);
  if (!pendingCommandOverflow) {
    memcpy(pendingCommandPayload, payload, length);
    pendingCommandPayload[length] = '\0';
  }
  pendingCommandAvailable = true;
}

void buildMqttTopic(const char* messageType, char* topic, size_t topicSize) {
  snprintf(topic, topicSize, "home/devices/%s/%s", DEVICE_ID, messageType);
}

uint32_t nextMqttCount() {
  if (mqttMessageCounter == UINT32_MAX) {
    mqttMessageCounter = 0;
  }
  mqttMessageCounter++;
  return mqttMessageCounter;
}

bool timeReached(uint32_t deadlineMs) {
  return static_cast<int32_t>(millis() - deadlineMs) >= 0;
}

const char* findJsonValue(const char* payload, const char* key) {
  char pattern[40];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* cursor = strstr(payload, pattern);
  if (cursor == nullptr) {
    return nullptr;
  }
  cursor += strlen(pattern);
  while (isspace(static_cast<unsigned char>(*cursor))) {
    ++cursor;
  }
  if (*cursor++ != ':') {
    return nullptr;
  }
  while (isspace(static_cast<unsigned char>(*cursor))) {
    ++cursor;
  }
  return cursor;
}

bool parseCommandPayload(const char* payload, char* command, size_t commandSize, uint32_t& durationSeconds) {
  durationSeconds = 0;
  while (isspace(static_cast<unsigned char>(*payload))) {
    ++payload;
  }

  const bool json = *payload == '{';
  const char* cursor = json ? findJsonValue(payload, "command") : payload;
  if (cursor == nullptr || commandSize == 0) {
    return false;
  }
  if (json && *cursor++ != '"') {
    return false;
  }

  size_t used = 0;
  while (*cursor && (isalnum(static_cast<unsigned char>(*cursor)) || *cursor == '_')) {
    if (used + 1 >= commandSize) {
      return false;
    }
    command[used++] = static_cast<char>(tolower(static_cast<unsigned char>(*cursor++)));
  }
  command[used] = '\0';
  if (used == 0) {
    return false;
  }
  if (json) {
    if (*cursor != '"') {
      return false;
    }
    const char* duration = findJsonValue(payload, "duration_seconds");
    if (duration != nullptr) {
      if (*duration == '"') {
        ++duration;
      }
      if (!isdigit(static_cast<unsigned char>(*duration))) {
        return false;
      }
      char* end = nullptr;
      const unsigned long value = strtoul(duration, &end, 10);
      if (end == duration || value > UINT32_MAX || (*end != ',' && *end != '}' && *end != '"' && !isspace(static_cast<unsigned char>(*end)))) {
        return false;
      }
      durationSeconds = static_cast<uint32_t>(value);
    }
  } else {
    while (isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor != '\0') {
      return false;
    }
  }
  return true;
}

void processPendingMqttCommand() {
  if (!pendingCommandAvailable) {
    return;
  }
  pendingCommandAvailable = false;
  if (pendingCommandOverflow) {
    pendingCommandOverflow = false;
    publishCommandResponse("unknown", false, "payload_too_large", 0, nullptr);
    return;
  }

  char command[32];
  uint32_t requestedSeconds = 0;
  if (!parseCommandPayload(pendingCommandPayload, command, sizeof(command), requestedSeconds)) {
    publishCommandResponse("unknown", false, "malformed_command", 0, nullptr);
    return;
  }
  Serial.printf("MQTT command: %s\n", command);

  if (strcmp(command, "sleep_now") == 0) {
    stopStreamMode("stream_stopped");
    standbyMode = true;
    waitingForSensorReset = false;
    motionActiveSinceMs = 0;
    if (cameraReady) {
      esp_camera_deinit();
      cameraReady = false;
    }
    publishStatusEvent("standby");
    publishCommandResponse(command, true, "standby_mqtt_online", 0, nullptr);
    return;
  }

  if (strcmp(command, "stay_awake") == 0) {
    if (!cameraReady) {
      cameraReady = initCamera();
    }
    if (!cameraReady) {
      publishCommandResponse(command, false, "camera_not_ready", 0, nullptr);
      return;
    }
    standbyMode = false;
    lastRecordingFinishedMs = millis();
    publishStatusEvent("awake");
    publishCommandResponse(command, true, "awake_continuously", 0, nullptr);
    return;
  }

  if (strcmp(command, "record_now") == 0) {
    if (streamActive) {
      publishCommandResponse(command, false, "streaming_active", 0, nullptr);
      return;
    }
    if (!cameraReady) {
      cameraReady = initCamera();
    }
    if (!cameraReady) {
      publishCommandResponse(command, false, "camera_not_ready", 0, nullptr);
      return;
    }
    publishCommandResponse(command, true, "recording_started", 0, nullptr);
    publishStatusEvent("command_record_now");
    recordAndUploadVideo();
    if (standbyMode) {
      esp_camera_deinit();
      cameraReady = false;
    }
    return;
  }

  if (strcmp(command, "stream_on") == 0) {
    if (streamActive) {
      publishCommandResponse(command, false, "streaming_active", 0, nullptr);
      return;
    }
    if (!cameraReady) {
      cameraReady = initCamera();
    }
    if (!cameraReady) {
      publishCommandResponse(command, false, "camera_not_ready", 0, nullptr);
      return;
    }
    const uint32_t seconds = requestedSeconds == 0 ? STREAM_DEFAULT_SECONDS :
                             (requestedSeconds > STREAM_MAX_SECONDS ? STREAM_MAX_SECONDS : requestedSeconds);
    if (!startStreamMode(seconds)) {
      publishCommandResponse(command, false, "stream_start_failed", 0, nullptr);
      return;
    }
    standbyMode = false;
    char url[80];
    buildStreamUrl(url, sizeof(url));
    publishCommandResponse(command, true, "stream_started", seconds, url);
    return;
  }

  publishCommandResponse("unknown", false, "unknown_command", 0, nullptr);
}

bool publishCommandResponse(const char* command, bool success, const char* detail, uint32_t durationSeconds, const char* streamUrl) {
  char payload[512];
  const int written = snprintf(payload, sizeof(payload),
    "{\"device\":\"%s\",\"type\":\"responses\",\"count\":%lu,\"uptime_ms\":%lu,"
    "\"wifi_rssi\":%d,\"command\":\"%s\",\"success\":%s,\"message\":\"%s\","
    "\"duration_seconds\":%lu,\"stream_url\":\"%s\"}",
    DEVICE_ID, static_cast<unsigned long>(nextMqttCount()), static_cast<unsigned long>(millis()),
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, command, success ? "true" : "false",
    detail, static_cast<unsigned long>(durationSeconds), streamUrl == nullptr ? "" : streamUrl);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return false;
  }
  return publishMqttPayload("responses", payload, false);
}

bool publishMqttPayload(const char* messageType, const char* payload, bool retained) {
  char topic[96];
  buildMqttTopic(messageType, topic, sizeof(topic));

  for (uint8_t attempt = 1; attempt <= MQTT_PUBLISH_RETRY_COUNT; ++attempt) {
    if (!ensureMqttConnected()) {
      return false;
    }

    if (mqttClient.publish(topic, payload, retained)) {
      mqttClient.loop();
      Serial.print("MQTT published ");
      Serial.println(topic);
      return true;
    }

    Serial.printf("MQTT publish failed for %s, attempt %u/%u, state=%d\n",
                  topic,
                  attempt,
                  MQTT_PUBLISH_RETRY_COUNT,
                  mqttClient.state());
    delay(MQTT_PUBLISH_RETRY_DELAY_MS);
  }

  return false;
}

bool publishAvailability(const char* state, bool retained) {
  return publishMqttPayload("availability", state, retained);
}

bool publishStatusEvent(const char* eventName) {
  char payload[512];
  const uint32_t count = nextMqttCount();
  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  const IPAddress ip = WiFi.localIP();

  const int written = snprintf(
    payload,
    sizeof(payload),
    "{\"device\":\"%s\",\"type\":\"status\",\"count\":%lu,\"uptime_ms\":%lu,"
    "\"wifi_rssi\":%d,\"firmware_version\":\"%s\",\"event\":\"%s\","
    "\"standby\":%s,\"stream_active\":%s,\"ip\":\"%u.%u.%u.%u\",\"free_heap\":%lu,"
    "\"free_psram\":%lu}",
    DEVICE_ID,
    static_cast<unsigned long>(count),
    static_cast<unsigned long>(millis()),
    rssi,
    FIRMWARE_VERSION,
    eventName,
    standbyMode ? "true" : "false",
    streamActive ? "true" : "false",
    static_cast<unsigned int>(ip[0]),
    static_cast<unsigned int>(ip[1]),
    static_cast<unsigned int>(ip[2]),
    static_cast<unsigned int>(ip[3]),
    static_cast<unsigned long>(ESP.getFreeHeap()),
    static_cast<unsigned long>(ESP.getFreePsram())
  );

  if (written < 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    Serial.println("MQTT status payload truncated");
    return false;
  }

  return publishMqttPayload("status", payload, false);
}

bool publishClipTelemetry(const char* eventName, const char* filename, bool captureOk, bool uploadOk, uint32_t frameCount, size_t clipBytes, uint32_t captureDurationMs, uint8_t uploadAttempts, int httpStatus) {
  char payload[640];
  const uint32_t count = nextMqttCount();
  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  const int written = snprintf(
    payload,
    sizeof(payload),
    "{\"device\":\"%s\",\"type\":\"telemetry\",\"count\":%lu,\"uptime_ms\":%lu,"
    "\"wifi_rssi\":%d,\"event\":\"%s\",\"filename\":\"%s\",\"frames\":%lu,"
    "\"bytes\":%lu,\"capture_duration_ms\":%lu,\"capture_ok\":%s,"
    "\"upload_ok\":%s,\"upload_attempts\":%u,\"http_status\":%d,"
    "\"free_heap\":%lu,\"free_psram\":%lu}",
    DEVICE_ID,
    static_cast<unsigned long>(count),
    static_cast<unsigned long>(millis()),
    rssi,
    eventName,
    filename,
    static_cast<unsigned long>(frameCount),
    static_cast<unsigned long>(clipBytes),
    static_cast<unsigned long>(captureDurationMs),
    captureOk ? "true" : "false",
    uploadOk ? "true" : "false",
    uploadAttempts,
    httpStatus,
    static_cast<unsigned long>(ESP.getFreeHeap()),
    static_cast<unsigned long>(ESP.getFreePsram())
  );

  if (written < 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    Serial.println("MQTT telemetry payload truncated");
    return false;
  }

  return publishMqttPayload("telemetry", payload, false);
}

void buildStreamUrl(char* url, size_t urlSize) {
  const IPAddress ip = WiFi.localIP();
  snprintf(url, urlSize, "http://%u.%u.%u.%u:%u%s",
           ip[0], ip[1], ip[2], ip[3], STREAM_HTTP_PORT, STREAM_HTTP_PATH);
}

bool startStreamMode(uint32_t durationSeconds) {
  if (!ensureWiFiConnected()) {
    return false;
  }
  streamServer.begin();
  streamServer.setNoDelay(true);
  streamServerRunning = true;
  streamActive = true;
  streamStartMs = millis();
  streamDurationMs = durationSeconds * 1000UL;
  nextStreamFrameMs = millis();
  streamFrameCount = 0;
  waitingForSensorReset = false;
  sensorResetSinceMs = 0;
  motionActiveSinceMs = 0;
  char url[80];
  buildStreamUrl(url, sizeof(url));
  Serial.printf("Temporary stream: %s, timeout %lu seconds\n", url,
                static_cast<unsigned long>(durationSeconds));
  publishStatusEvent("stream_started");
  return true;
}

void stopStreamMode(const char* reason) {
  if (!streamActive) {
    return;
  }
  streamClient.stop();
  if (streamServerRunning) {
    streamServer.end();
    streamServerRunning = false;
  }
  streamActive = false;
  nextStreamFrameMs = 0;
  waitingForSensorReset = false;
  sensorResetSinceMs = 0;
  motionActiveSinceMs = 0;
  lastRecordingFinishedMs = millis();
  Serial.printf("Temporary stream stopped: %s\n", reason);
  publishStatusEvent(reason);
}

void handleStreamMode() {
  if (!streamActive) {
    return;
  }
  if (millis() - streamStartMs >= streamDurationMs) {
    stopStreamMode("stream_timeout");
    return;
  }
  if (!streamClient || !streamClient.connected()) {
    streamClient.stop();
    acceptStreamClient();
    return;
  }
  sendStreamFrame();
}

void acceptStreamClient() {
  WiFiClient incoming = streamServer.accept();
  if (!incoming) {
    return;
  }
  char requestLine[80];
  size_t used = 0;
  bool requestLineComplete = false;
  const uint32_t requestStartMs = millis();
  while (used + 1 < sizeof(requestLine) && incoming.connected() &&
         millis() - requestStartMs < 250) {
    if (!incoming.available()) {
      delay(1);
      continue;
    }
    const char c = static_cast<char>(incoming.read());
    if (c == '\n') {
      requestLineComplete = true;
      break;
    }
    requestLine[used++] = c;
  }
  requestLine[used] = '\0';
  if (!requestLineComplete ||
      strncmp(requestLine, "GET /stream HTTP/1.", strlen("GET /stream HTTP/1.")) != 0) {
    writeStreamText(incoming, "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    incoming.stop();
    return;
  }
  while (incoming.available()) {
    incoming.read();
  }
  streamClient = incoming;
  streamClient.setNoDelay(true);
  const char* header =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=birdframe\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n\r\n";
  if (!writeStreamText(streamClient, header)) {
    streamClient.stop();
    return;
  }
  nextStreamFrameMs = millis();
  Serial.println("MJPEG stream client connected");
}

bool writeStreamAll(WiFiClient& client, const uint8_t* data, size_t len) {
  size_t sent = 0;
  uint32_t lastProgressMs = millis();
  while (sent < len && client.connected()) {
    if (millis() - streamStartMs >= streamDurationMs) {
      return false;
    }
    const size_t remaining = len - sent;
    const size_t chunk = remaining > UPLOAD_BUFFER_SIZE ? UPLOAD_BUFFER_SIZE : remaining;
    const size_t written = client.write(data + sent, chunk);
    if (written == 0) {
      if (millis() - lastProgressMs >= STREAM_CLIENT_WRITE_TIMEOUT_MS) {
        return false;
      }
      delay(1);
      continue;
    }
    sent += written;
    lastProgressMs = millis();
    mqttClient.loop();
    delay(0);
  }
  return sent == len;
}

bool writeStreamText(WiFiClient& client, const char* value) {
  return writeStreamAll(client, reinterpret_cast<const uint8_t*>(value), strlen(value));
}

void sendStreamFrame() {
  if (!timeReached(nextStreamFrameMs)) {
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("Stream frame capture failed");
    nextStreamFrameMs = millis() + STREAM_FRAME_INTERVAL_MS;
    return;
  }
  char header[128];
  snprintf(header, sizeof(header), "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
           STREAM_BOUNDARY, static_cast<unsigned int>(fb->len));
  const bool ok = writeStreamText(streamClient, header) &&
                  writeStreamAll(streamClient, fb->buf, fb->len) &&
                  writeStreamText(streamClient, "\r\n");
  esp_camera_fb_return(fb);
  if (!ok) {
    streamClient.stop();
    return;
  }
  ++streamFrameCount;
  if (streamFrameCount % 25U == 0) {
    Serial.printf("Streamed %lu frames\n", static_cast<unsigned long>(streamFrameCount));
  }
  nextStreamFrameMs = millis() + STREAM_FRAME_INTERVAL_MS;
}

bool recordAndUploadVideo() {
  recording = true;

  char filename[48];
  snprintf(filename, sizeof(filename), "bird_%08lu_%04lu.mjpeg",
           static_cast<unsigned long>(millis() / 1000UL),
           static_cast<unsigned long>(clipCounter));

  uint8_t* clipData = static_cast<uint8_t*>(
    heap_caps_malloc(MAX_CLIP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
  );
  if (clipData == nullptr) {
    clipData = static_cast<uint8_t*>(heap_caps_malloc(MAX_CLIP_BYTES, MALLOC_CAP_8BIT));
  }

  if (clipData == nullptr) {
    Serial.printf("Recording skipped: could not allocate %u byte clip buffer\n",
                  static_cast<unsigned int>(MAX_CLIP_BYTES));
    publishClipTelemetry("clip_no_memory", filename, false, false, 0, 0, 0, 0, 0);
    publishStatusEvent("clip_no_memory");
    recording = false;
    waitingForSensorReset = true;
    lastRecordingFinishedMs = millis();
    return false;
  }

  Serial.printf("Recording %s\n", filename);
  publishStatusEvent("capture_start");

  uint32_t frameCount = 0;
  size_t totalBytes = 0;
  uint32_t captureDurationMs = 0;
  const bool captureOk = captureClipToMemory(clipData, MAX_CLIP_BYTES, frameCount, totalBytes, captureDurationMs);

  Serial.printf("Frames: %lu\n", static_cast<unsigned long>(frameCount));
  Serial.printf("Bytes: %u\n", static_cast<unsigned int>(totalBytes));
  Serial.printf("Capture duration: %.2f sec\n", captureDurationMs / 1000.0f);

  bool uploadOk = false;
  uint8_t uploadAttempts = 0;
  lastUploadHttpStatus = 0;
  if (captureOk) {
    for (uint8_t attempt = 1; attempt <= UPLOAD_RETRY_COUNT; ++attempt) {
      Serial.printf("Upload attempt %u/%u\n", attempt, UPLOAD_RETRY_COUNT);
      uploadAttempts = attempt;
      lastUploadHttpStatus = 0;
      uploadOk = uploadClipBuffer(clipData, totalBytes, filename, frameCount, captureDurationMs);
      if (uploadOk) {
        break;
      }

      if (attempt < UPLOAD_RETRY_COUNT) {
        delay(UPLOAD_RETRY_DELAY_MS);
      }
    }
  } else {
    Serial.println("Capture failed");
  }

  if (uploadOk) {
    Serial.println("Upload complete");
    clipCounter++;
  } else {
    Serial.println("Upload failed");
  }

  heap_caps_free(clipData);
  publishClipTelemetry("clip_complete", filename, captureOk, uploadOk, frameCount, totalBytes, captureDurationMs, uploadAttempts, lastUploadHttpStatus);
  publishStatusEvent(uploadOk ? "upload_complete" : (captureOk ? "upload_failed" : "capture_failed"));
  lastRecordingFinishedMs = millis();
  waitingForSensorReset = true;
  sensorResetSinceMs = 0;
  recording = false;
  return uploadOk;
}

bool captureClipToMemory(uint8_t* clipData, size_t clipCapacity, uint32_t& frameCount, size_t& totalBytes, uint32_t& captureDurationMs) {
  delay(CAPTURE_START_DELAY_MS);
  flushCameraFrames(2);

  const uint32_t startMs = millis();
  uint32_t nextFrameMs = startMs;

  for (uint32_t frameIndex = 0; frameIndex < CLIP_FRAME_COUNT; ++frameIndex) {
    const int32_t waitMs = static_cast<int32_t>(nextFrameMs - millis());
    if (waitMs > 0) {
      delay(waitMs);
    }

    if (!captureFrameToBuffer(clipData, clipCapacity, totalBytes)) {
      return false;
    }

    frameCount++;
    if ((frameCount % 10U) == 0U) {
      Serial.printf("Captured %lu/%lu frames\n",
                    static_cast<unsigned long>(frameCount),
                    static_cast<unsigned long>(CLIP_FRAME_COUNT));
    }

    nextFrameMs += FRAME_INTERVAL_MS;
  }

  captureDurationMs = millis() - startMs;
  flushCameraFrames(1);
  return frameCount == CLIP_FRAME_COUNT;
}

bool captureFrameToBuffer(uint8_t* clipData, size_t clipCapacity, size_t& totalBytes) {
  for (uint8_t attempt = 0; attempt < FRAME_CAPTURE_RETRIES; ++attempt) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      delay(20);
      continue;
    }

    const size_t frameLen = fb->len;
    if (totalBytes + frameLen > clipCapacity) {
      Serial.printf("Clip buffer full: need %u more bytes, used %u of %u\n",
                    static_cast<unsigned int>(frameLen),
                    static_cast<unsigned int>(totalBytes),
                    static_cast<unsigned int>(clipCapacity));
      esp_camera_fb_return(fb);
      return false;
    }

    memcpy(clipData + totalBytes, fb->buf, frameLen);
    esp_camera_fb_return(fb);

    totalBytes += frameLen;
    return true;
  }

  Serial.println("Camera frame capture failed");
  return false;
}

bool uploadClipBuffer(const uint8_t* clipData, size_t clipSize, const char* serverFilename, uint32_t frameCount, uint32_t captureDurationMs) {
  lastUploadHttpStatus = 0;

  if (!ensureWiFiConnected()) {
    Serial.println("Upload skipped because WiFi is unavailable");
    return false;
  }

  if (clipData == nullptr || clipSize == 0) {
    Serial.println("Upload skipped because clip buffer is empty");
    return false;
  }

  IPAddress serverIp;
  if (!WiFi.hostByName(SERVER_HOST, serverIp)) {
    Serial.print("DNS/IP lookup failed for ");
    Serial.println(SERVER_HOST);
    return false;
  }

  Serial.print("Resolved server IP: ");
  Serial.println(serverIp);

  const size_t uploadSize = clipSize;
  WiFiClient uploadClient;
  uploadClient.setTimeout(HTTP_UPLOAD_TIMEOUT_MS);
  uploadClient.setNoDelay(false);

  if (!uploadClient.connect(serverIp, SERVER_PORT, WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("HTTP socket connect failed");
    return false;
  }

  String request;
  request.reserve(320);
  request += "POST ";
  request += SERVER_PATH;
  request += " HTTP/1.1\r\nHost: ";
  request += SERVER_HOST;
  request += ":";
  request += String(SERVER_PORT);
  request += "\r\nUser-Agent: ESP32-BirdFeeder/1.0";
  request += "\r\nConnection: close";
  request += "\r\nContent-Type: application/octet-stream";
  request += "\r\nContent-Length: ";
  request += String(uploadSize);
  request += "\r\nX-Filename: ";
  request += serverFilename;
  request += "\r\nX-Video-Format: raw-mjpeg";
  request += "\r\nX-Frame-Count: ";
  request += String(frameCount);
  request += "\r\nX-Frame-Interval-Ms: ";
  request += String(FRAME_INTERVAL_MS);
  request += "\r\nX-Video-Duration-Ms: ";
  request += String(captureDurationMs);
  request += "\r\n\r\n";

  if (!writeText(uploadClient, request)) {
    Serial.println("HTTP header send failed");
    uploadClient.stop();
    return false;
  }

  size_t bytesSent = 0;
  const bool bodyOk = streamBufferToClient(uploadClient, clipData, clipSize, bytesSent);

  if (!bodyOk || bytesSent != uploadSize) {
    Serial.printf("HTTP body send failed: sent %u of %u bytes\n",
                  static_cast<unsigned int>(bytesSent),
                  static_cast<unsigned int>(uploadSize));
    uploadClient.stop();
    return false;
  }

  int statusCode = 0;
  String responseBody;
  if (!waitForHttpResponse(uploadClient, statusCode, responseBody)) {
    Serial.println("HTTP response timeout or parse failure");
    uploadClient.stop();
    return false;
  }
  lastUploadHttpStatus = statusCode;

  Serial.printf("HTTP upload size: %u bytes\n", static_cast<unsigned int>(uploadSize));
  Serial.printf("HTTP/1.1 %d %s\n", statusCode, statusCode == 200 ? "OK" : "Response");

  if (responseBody.length() > 0) {
    Serial.println(responseBody);
  }

  uploadClient.stop();
  return statusCode == 200;
}

void printServerTarget() {
  Serial.print("Server target: http://");
  Serial.print(SERVER_HOST);
  Serial.print(":");
  Serial.print(SERVER_PORT);
  Serial.println(SERVER_PATH);
  Serial.print("MQTT target: ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);
  Serial.print("MQTT base topic: home/devices/");
  Serial.println(DEVICE_ID);
  Serial.printf("Clip settings: %lu frames, %lu ms interval, %lu x quality %d\n",
                static_cast<unsigned long>(CLIP_FRAME_COUNT),
                static_cast<unsigned long>(FRAME_INTERVAL_MS),
                static_cast<unsigned long>(CAMERA_FRAME_SIZE),
                CAMERA_JPEG_QUALITY);
  Serial.printf("Clip buffer: %u bytes in PSRAM/RAM, no SD card required\n",
                static_cast<unsigned int>(MAX_CLIP_BYTES));
  Serial.println("Power mode: continuously awake; sleep_now enters MQTT standby");
  Serial.printf("PIR input: GPIO %d powered from 5V, active level %s\n",
                PIR_SENSOR_PIN,
                MOTION_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW");
  Serial.println("Note: 127.0.0.1 only works on the server PC itself, not from the ESP32");
}

void flushCameraFrames(uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      break;
    }
    esp_camera_fb_return(fb);
    delay(5);
  }
}

bool writeAll(WiFiClient& client, const uint8_t* data, size_t len) {
  size_t totalWritten = 0;
  uint32_t lastProgressMs = millis();

  while (totalWritten < len) {
    if (!client.connected()) {
      return false;
    }

    const size_t written = client.write(data + totalWritten, len - totalWritten);
    if (written == 0) {
      if (millis() - lastProgressMs > HTTP_UPLOAD_TIMEOUT_MS) {
        return false;
      }
      delay(1);
      continue;
    }

    totalWritten += written;
    lastProgressMs = millis();
    delay(0);
  }

  return true;
}

bool writeText(WiFiClient& client, const String& text) {
  return writeAll(client, reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

bool streamBufferToClient(WiFiClient& client, const uint8_t* data, size_t len, size_t& bytesSent) {
  uint32_t lastProgressMs = millis();

  while (bytesSent < len) {
    const size_t remaining = len - bytesSent;
    const size_t chunkSize = remaining > UPLOAD_BUFFER_SIZE ? UPLOAD_BUFFER_SIZE : remaining;

    if (!writeAll(client, data + bytesSent, chunkSize)) {
      return false;
    }

    bytesSent += chunkSize;
    lastProgressMs = millis();

    if ((bytesSent % (UPLOAD_BUFFER_SIZE * 16)) == 0) {
      Serial.printf("Upload progress: %u bytes\n", static_cast<unsigned int>(bytesSent));
    }

    if (millis() - lastProgressMs > HTTP_UPLOAD_TIMEOUT_MS) {
      return false;
    }
    delay(0);
  }

  return true;
}

bool waitForHttpResponse(WiFiClient& client, int& statusCode, String& responseBody) {
  const uint32_t startMs = millis();
  while (!client.available() && client.connected() && (millis() - startMs < HTTP_UPLOAD_TIMEOUT_MS)) {
    delay(10);
  }

  if (!client.available()) {
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  if (!statusLine.startsWith("HTTP/1.1 ")) {
    return false;
  }

  statusCode = statusLine.substring(9, 12).toInt();

  int contentLength = -1;
  while (client.available() || client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      break;
    }

    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    }
  }

  responseBody = "";
  if (contentLength > 0) {
    responseBody.reserve(contentLength);
  }

  const uint32_t bodyStartMs = millis();
  while ((contentLength < 0 && client.connected()) ||
         (contentLength >= 0 && responseBody.length() < static_cast<unsigned int>(contentLength))) {
    while (client.available()) {
      responseBody += static_cast<char>(client.read());
      if (contentLength >= 0 && responseBody.length() >= static_cast<unsigned int>(contentLength)) {
        return true;
      }
    }

    if (!client.connected()) {
      break;
    }

    if (millis() - bodyStartMs > HTTP_UPLOAD_TIMEOUT_MS) {
      break;
    }
    delay(10);
  }

  return true;
}
