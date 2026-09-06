/******************************************************************
 * SmartWaterMeter - ESP8266 water & heat metering controller
 *
 * Hardware: Wemos D1 mini (ESP8266)
 * Features:
 *   - 4x DS18B20 temperature sensors (Cold/Hot/Supply/Return)
 *   - 2x pulse water meters with interrupt debouncing
 *   - Async Web Server + WebSocket for real-time SPA frontend
 *   - JSON API (/api.json) + Prometheus (/metrics)
 *   - SMTP email reports on schedule
 *   - Telnet serial console over WiFi
 *   - Failsafe OTA with automatic rollback (A/B slots)
 *   - WiFi STA with AP fallback
 ******************************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <time.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>

// Async Web Server
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// Project headers
#include "secrets.h"
#include "ConfigStore.h"
#include "MeterCounter.h"
#include "TemperatureSensors.h"
#include "StatusLED.h"
#include "TelnetSerial.h"
#include "FailsafeOTA.h"

// SMTP
#include <ESP_Mail_Client.h>
SMTPSession smtp;
void smtpCallback(SMTP_Status status);

// NTP
#include <NTPClient.h>
#define NTP_INTERVAL 60000
#define NTP_ADDRESS  "europe.pool.ntp.org"
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_ADDRESS, 10800, NTP_INTERVAL);

// ==================== Globals ====================
ConfigStore config;
MeterCounter meterHot;
MeterCounter meterCold;
TemperatureSensors tempSensors(ONE_WIRE_BUS, &config);
StatusLED led;
TelnetSerial telnet;
FailsafeOTA failsafe;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

char apSSID[32];
char deviceName[32];

time_t epochTime;
struct tm *ptm = nullptr;
uint32_t bootMillis = 0;
time_t localEpoch = 0;
bool timeValid = false;
bool wifiConnected = false;
bool apModeActive = false;
bool needsRestart = false;
int wifiReconnectFails = 0;
uint32_t lastWiFiRetry = 0;

// Timing
uint32_t lastSensorRead = 0;
uint32_t lastSensorConv = 0;
bool sensorConvPending = false;
uint32_t lastEepromSave = 0;
uint32_t lastEmailSend = 0;
uint32_t lastMeterFlush = 0;

// Calibration
bool calibrateRequested = false;
int calibrateRequestedIndex = -1;
uint32_t calibrateFeedbackTime = 0;
bool calibrateLastResult = false;

// Prototypes
void WiFiupd();
void updateLocalTime();
bool connectToWiFi();
void startAPMode();
void trySendEmail(const String &subject, const String &body);
void sendDailyReport();
void onCalibrateDone(int sensorIndex, bool success);
void telnetConfirmFirmware();
void wsSendJson(AsyncWebSocketClient *client, const JsonDocument &doc);
void wsBroadcastJson(const JsonDocument &doc);
void handleWsMessage(AsyncWebSocketClient *client, const String &msg);
void sendFullState(AsyncWebSocketClient *client);
void setupHttpRoutes();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  bootMillis = millis();

  led.begin();

  // Unique device name from MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSSID, sizeof(apSSID), "%s-%02X%02X%02X",
    AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
  snprintf(deviceName, sizeof(deviceName), "%s-%02X%02X%02X",
    AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
  Serial.printf("\n\n=== %s ===\n", deviceName);

  config.begin();
  tempSensors.begin();

  // Check all sensors present
  bool allSensorsFound = true;
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (!tempSensors.isFound(i)) allSensorsFound = false;
  }
  if (!allSensorsFound) led.setSensorErr();

  meterHot.begin(PIN_METER_HOT, true, &config);
  meterCold.begin(PIN_METER_COLD, false, &config);

  timeClient.begin();

  wifiConnected = connectToWiFi();

  // OTA
  ArduinoOTA.setHostname(deviceName);
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Done"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u%%\r", (p * 100) / t);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Error: %u\n", e);
  });
  ArduinoOTA.begin();

  telnet.begin();
  failsafe.begin();

  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS mount failed!");
  } else {
    Serial.println("[FS] LittleFS mounted");
  }

  // WebSocket
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                 AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Serial.printf("[WS] Client #%u connected\n", client->id());
      sendFullState(client);
    } else if (type == WS_EVT_DISCONNECT) {
      Serial.printf("[WS] Client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        handleWsMessage(client, String((char*)data).substring(0, len));
      }
    }
  });
  server.addHandler(&ws);

  setupHttpRoutes();
  server.begin();

  if (!wifiConnected) {
    startAPMode();
  } else {
    led.setConnected();
  }

  if (strlen(config.data.smtpEmail) > 0 && wifiConnected) {
    trySendEmail("Power On", String(deviceName) + " is Power On");
  }

  Serial.println("[Setup] Ready");
}

// ==================== LOOP ====================
void loop() {
  led.tick();
  ArduinoOTA.handle();
  telnet.handle();
  failsafe.handle();
  ws.cleanupClients();

  uint32_t now = millis();

  WiFiupd();
  updateLocalTime();

  // LED mode selection
  static LEDMode lastLedMode = LED_BOOTING;
  LEDMode desiredMode;
  if (tempSensors.isCalibrating()) {
    desiredMode = LED_CALIBRATE;
  } else if (apModeActive) {
    desiredMode = LED_AP_MODE;
  } else if (!wifiConnected) {
    desiredMode = LED_WIFI_LOST;
  } else {
    bool allFound = true;
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (!tempSensors.isFound(i)) { allFound = false; break; }
    }
    desiredMode = allFound ? LED_CONNECTED : LED_SENSOR_ERR;
  }
  if (desiredMode != lastLedMode) {
    lastLedMode = desiredMode;
    led.setMode(desiredMode);
  }

  // ---- DS18B20: asynchronous conversion cycle ----
  // Phase 1: start conversion (triggers ~750ms on bus)
  // Phase 2: read results (next loop cycle, ~1s later)
  uint32_t sensorInterval = tempSensors.isCalibrating() ? 1000 : 1000;

  if (!sensorConvPending && now - lastSensorRead > sensorInterval) {
    // Phase 1: start new conversion
    lastSensorRead = now;
    tempSensors.startConversion();
    sensorConvPending = true;
    lastSensorConv = now;
  }

  if (sensorConvPending && now - lastSensorConv > 850) {
    // Phase 2: read results (>=850ms after start)
    sensorConvPending = false;
    tempSensors.readTemperatures();

    // Periodically rescan the 1-wire bus to detect new/removed sensors
    static uint32_t lastRescan = 0;
    if (now - lastRescan > 30000) {
      lastRescan = now;
      tempSensors.rescanBus();
      // After rescan, start conversion immediately so next read cycle
      // has fresh data for all devices
      tempSensors.startConversion();
      // Re-arm the async cycle: next read will happen ~850ms from now
      lastSensorConv = now;
    }

    // Broadcast to all WebSocket clients
    JsonDocument doc;
    doc["type"] = "sensors";
    JsonObject t = doc.createNestedObject("temperatures");
    t["cold"]   = tempSensors.getTempHVS();
    t["hot"]    = tempSensors.getTempGVS();
    t["return"] = tempSensors.getTempReturn();
    t["supply"] = tempSensors.getTempSupply();
    // Include bus devices for live calibrate table
    JsonArray bus = doc.createNestedArray("busDevices");
    for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
      JsonObject b = bus.createNestedObject();
      b["index"] = i;
      b["temp"] = tempSensors.getRawTemp(i);
      if (tempSensors.isCalibrating()) {
        b["baseTemp"] = tempSensors.getCalibrateBaseTemp(i);
        b["delta"] = tempSensors.getCalibrateDelta(i);
      }
      const uint8_t* addr = tempSensors.getAllAddr(i);
      if (addr) {
        char addrStr[17];
        for (int j = 0; j < 8; j++) {
          sprintf(addrStr + j * 2, "%02X", addr[j]);
        }
        addrStr[16] = '\0';
        b["address"] = addrStr;
      }
    }
    wsBroadcastJson(doc);
  }

  // ---- Meters ----
  if (now - lastMeterFlush > 10000) {
    lastMeterFlush = now;
    meterHot.flush();
    meterCold.flush();
  }

  // ---- EEPROM periodic save ----
  if (now - lastEepromSave > 300000) {
    lastEepromSave = now;
    config.save();
  }

  // ---- Email report schedule ----
  if (wifiConnected && strlen(config.data.smtpEmail) > 0 && ptm != nullptr) {
    if (ptm->tm_hour == config.data.reportHour
        && ptm->tm_min == config.data.reportMinute
        && now - lastEmailSend > 60000) {
      bool shouldSend = false;
      switch (config.data.reportSchedule) {
        case 0: shouldSend = true; break;
        case 1: if (ptm->tm_wday == config.data.reportDay) shouldSend = true; break;
        case 2: if (ptm->tm_mday == config.data.reportDay) shouldSend = true; break;
      }
      if (shouldSend) {
        lastEmailSend = now;
        sendDailyReport();
      }
    }
  }

  // ---- Calibration request ----
  if (calibrateRequested && !tempSensors.isCalibrating()) {
    calibrateRequested = false;
    tempSensors.startCalibration(calibrateRequestedIndex, onCalibrateDone);
    // Rescan bus for calibration mode
    tempSensors.rescanBus();
  }

  // ---- Restart ----
  if (needsRestart && now > 10000) {
    Serial.println("[System] Restart...");
    ESP.restart();
  }

  delay(10);
}

// ==================== WiFi ====================
bool connectToWiFi() {
  if (strlen(config.data.wifiSSID) == 0) {
    Serial.println("[WiFi] SSID not set, AP mode");
    return false;
  }
  Serial.printf("[WiFi] Connecting to '%s'...\n", config.data.wifiSSID);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname(deviceName);
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  WiFi.begin(config.data.wifiSSID, config.data.wifiPass);
  int attempts = 0;
  while (attempts < 60) {
    delay(500);
    attempts++;
    Serial.print(".");
    if (WiFi.status() == WL_CONNECTED) break;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    apModeActive = false;
    return true;
  }
  Serial.printf("[WiFi] Failed (status: %d)\n", WiFi.status());
  wifiConnected = false;
  return false;
}

void startAPMode() {
  Serial.println("[WiFi] Starting AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
  WiFi.softAP(apSSID, AP_PASS_DEFAULT);
  Serial.printf("[WiFi] AP: %s (192.168.4.1)\n", apSSID);
  apModeActive = true;
  wifiConnected = false;
}

void WiFiupd() {
  if (apModeActive) {
    uint32_t now = millis();
    if (now - lastWiFiRetry > 600000) {
      lastWiFiRetry = now;
      Serial.println("[WiFi] AP: attempting reconnection...");
      WiFi.softAPdisconnect(true);
      delay(100);
      WiFi.mode(WIFI_OFF);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.setSleepMode(WIFI_NONE_SLEEP);
      if (strlen(config.data.wifiSSID) > 0) {
        WiFi.begin(config.data.wifiSSID, config.data.wifiPass);
        int attempts = 0;
        while (attempts < 40) {
          delay(500);
          attempts++;
          if (WiFi.status() == WL_CONNECTED) break;
        }
        if (WiFi.status() == WL_CONNECTED) {
          Serial.printf("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
          wifiConnected = true;
          apModeActive = false;
          wifiReconnectFails = 0;
          led.setConnected();
          return;
        } else {
          startAPMode();
        }
      }
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiReconnectFails = 0;
    timeClient.update();
    epochTime = timeClient.getEpochTime();
    if (epochTime > 100000) {
      localEpoch = epochTime;
      bootMillis = millis();
      timeValid = true;
    }
    return;
  }

  uint32_t now = millis();
  if (now - lastWiFiRetry > 30000) {
    lastWiFiRetry = now;
    wifiConnected = false;
    Serial.println("[WiFi] Reconnecting...");
    if (strlen(config.data.wifiSSID) > 0) {
      WiFi.disconnect(true);
      delay(100);
      WiFi.mode(WIFI_OFF);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.setSleepMode(WIFI_NONE_SLEEP);
      WiFi.begin(config.data.wifiSSID, config.data.wifiPass);
      int attempts = 0;
      while (attempts < 40) {
        delay(500);
        attempts++;
        if (WiFi.status() == WL_CONNECTED) break;
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        wifiReconnectFails = 0;
      } else {
        wifiReconnectFails++;
        if (wifiReconnectFails >= 3) {
          Serial.println("[WiFi] Too many failures, switching to AP mode");
          startAPMode();
        }
      }
    }
  }
}

// ==================== Local Time ====================
void updateLocalTime() {
  if (timeValid) {
    time_t now = localEpoch + (millis() - bootMillis) / 1000;
    ptm = gmtime(&now);
  }
}

// ==================== WebSocket ====================
void wsSendJson(AsyncWebSocketClient *client, const JsonDocument &doc) {
  String json;
  serializeJson(doc, json);
  if (client && client->status() == WS_CONNECTED) {
    client->text(json);
  }
}

void wsBroadcastJson(const JsonDocument &doc) {
  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

void sendFullState(AsyncWebSocketClient *client) {
  JsonDocument doc;
  doc["type"] = "fullState";

  doc["device"] = deviceName;
  doc["uptime_sec"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["wifi"] = wifiConnected ? "connected" : "disconnected";
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ap_mode"] = apModeActive;
  doc["ap_ssid"] = apSSID;
  doc["time_valid"] = timeValid;
  if (ptm) {
    doc["time_hour"] = ptm->tm_hour;
    doc["time_min"]  = ptm->tm_min;
    doc["time_sec"]  = ptm->tm_sec;
    doc["time_year"] = ptm->tm_year + 1900;
    doc["time_mon"]  = ptm->tm_mon + 1;
    doc["time_mday"] = ptm->tm_mday;
  }

  JsonObject t = doc.createNestedObject("temperatures");
  t["cold"]   = tempSensors.getTempHVS();
  t["hot"]    = tempSensors.getTempGVS();
  t["return"] = tempSensors.getTempReturn();
  t["supply"] = tempSensors.getTempSupply();

  JsonObject m = doc.createNestedObject("meters");
  m["hot_m3"]  = config.data.meterHotM3;
  m["cold_m3"] = config.data.meterColdM3;

  doc["calibrating"] = tempSensors.isCalibrating();
  doc["calibrate_index"] = tempSensors.getCalibrateIndex();

  JsonObject cfg = doc.createNestedObject("config");
  cfg["wifiSSID"] = config.data.wifiSSID;
  cfg["smtpHost"] = config.data.smtpHost;
  cfg["smtpPort"] = config.data.smtpPort;
  cfg["smtpEmail"] = config.data.smtpEmail;
  cfg["smtpRecipient"] = config.data.smtpRecipient;
  cfg["reportHour"] = config.data.reportHour;
  cfg["reportMinute"] = config.data.reportMinute;
  cfg["reportSchedule"] = config.data.reportSchedule;
  cfg["reportDay"] = config.data.reportDay;
  cfg["meterHotM3"] = config.data.meterHotM3;
  cfg["meterColdM3"] = config.data.meterColdM3;
  cfg["litersPerPulseHot"] = config.data.litersPerPulseHot;
  cfg["litersPerPulseCold"] = config.data.litersPerPulseCold;

  JsonArray sensors = doc.createNestedArray("sensorMapping");
  for (int i = 0; i < NUM_SENSORS; i++) {
    JsonObject s = sensors.createNestedObject();
    s["name"] = TemperatureSensors::sensorName(i);
    s["found"] = tempSensors.isFound(i);
    s["temp"] = tempSensors.getTemp(i);
  }

  JsonArray bus = doc.createNestedArray("busDevices");
  for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
    JsonObject b = bus.createNestedObject();
    b["index"] = i;
    b["temp"] = tempSensors.getRawTemp(i);
    if (tempSensors.isCalibrating()) {
      b["baseTemp"] = tempSensors.getCalibrateBaseTemp(i);
      b["delta"] = tempSensors.getCalibrateDelta(i);
    }
    // Format address as hex string
    const uint8_t* addr = tempSensors.getAllAddr(i);
    if (addr) {
      char addrStr[17];
      for (int j = 0; j < 8; j++) {
        sprintf(addrStr + j * 2, "%02X", addr[j]);
      }
      addrStr[16] = '\0';
      b["address"] = addrStr;
    }
  }

  doc["ota_pending"] = failsafe.isPending();
  if (failsafe.isPending()) {
    doc["ota_remaining"] = failsafe.remainingSec();
  }

  wsSendJson(client, doc);
}

void handleWsMessage(AsyncWebSocketClient *client, const String &msg) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.printf("[WS] Parse error: %s\n", err.c_str());
    return;
  }

  const char* type = doc["type"];

  if (strcmp(type, "getFullState") == 0) {
    sendFullState(client);
  }
  else if (strcmp(type, "saveConfig") == 0) {
    JsonObject cfg = doc["config"];

    strlcpy(config.data.wifiSSID, cfg["wifiSSID"] | "", sizeof(config.data.wifiSSID));
    strlcpy(config.data.wifiPass, cfg["wifiPass"] | "", sizeof(config.data.wifiPass));
    strlcpy(config.data.smtpHost, cfg["smtpHost"] | "", sizeof(config.data.smtpHost));
    config.data.smtpPort = cfg["smtpPort"] | 465;
    strlcpy(config.data.smtpEmail, cfg["smtpEmail"] | "", sizeof(config.data.smtpEmail));
    strlcpy(config.data.smtpPass, cfg["smtpPass"] | "", sizeof(config.data.smtpPass));
    strlcpy(config.data.smtpRecipient, cfg["smtpRecipient"] | "", sizeof(config.data.smtpRecipient));
    config.data.reportHour = cfg["reportHour"] | 9;
    config.data.reportMinute = cfg["reportMinute"] | 0;
    config.data.reportSchedule = cfg["reportSchedule"] | 0;
    config.data.reportDay = cfg["reportDay"] | 0;
    config.data.meterHotM3 = cfg["meterHotM3"] | 0.0f;
    config.data.meterColdM3 = cfg["meterColdM3"] | 0.0f;
    config.data.litersPerPulseHot = cfg["litersPerPulseHot"] | 1.0f;
    if (config.data.litersPerPulseHot <= 0) config.data.litersPerPulseHot = 1.0;
    config.data.litersPerPulseCold = cfg["litersPerPulseCold"] | 1.0f;
    if (config.data.litersPerPulseCold <= 0) config.data.litersPerPulseCold = 1.0;

    config.save();
    needsRestart = true;

    JsonDocument resp;
    resp["type"] = "saveConfigResult";
    resp["success"] = true;
    resp["message"] = "Config saved, restarting...";
    wsSendJson(client, resp);
  }
  else if (strcmp(type, "restart") == 0) {
    needsRestart = true;
  }
  else if (strcmp(type, "confirmOta") == 0) {
    failsafe.confirm();
    JsonDocument resp;
    resp["type"] = "confirmOtaResult";
    resp["success"] = true;
    wsSendJson(client, resp);
  }
  else if (strcmp(type, "testEmail") == 0) {
    String testBody = String(deviceName) + " - Test email\n\n";
    testBody += "=== Temperatures ===\n";
    testBody += "Cold: " + TemperatureSensors::formatTemp(tempSensors.getTempHVS()) + " C\n";
    testBody += "Hot: " + TemperatureSensors::formatTemp(tempSensors.getTempGVS()) + " C\n";
    testBody += "Heating Supply: " + TemperatureSensors::formatTemp(tempSensors.getTempSupply()) + " C\n";
    testBody += "Heating Return: " + TemperatureSensors::formatTemp(tempSensors.getTempReturn()) + " C\n\n";
    testBody += "=== Meter Readings ===\n";
    testBody += "Hot: " + String(config.data.meterHotM3, 3) + " m3\n";
    testBody += "Cold: " + String(config.data.meterColdM3, 3) + " m3\n\n";
    testBody += "=== System ===\n";
    testBody += "WiFi: " + String(wifiConnected ? "connected" : "disconnected") + "\n";
    testBody += "IP: " + (wifiConnected ? WiFi.localIP().toString() : "N/A") + "\n";
    trySendEmail("Test", testBody);
    JsonDocument resp;
    resp["type"] = "testEmailResult";
    resp["success"] = true;
    resp["message"] = "Test email sent";
    wsSendJson(client, resp);
  }
  else if (strcmp(type, "startCalibration") == 0) {
    int idx = doc["index"] | -1;
    if (idx >= 0 && idx < NUM_SENSORS && !tempSensors.isCalibrating()) {
      calibrateRequested = true;
      calibrateRequestedIndex = idx;
      JsonDocument resp;
      resp["type"] = "calibrationStarted";
      resp["index"] = idx;
      wsSendJson(client, resp);
    }
  }
  else if (strcmp(type, "cancelCalibration") == 0) {
    tempSensors.cancelCalibration();
    calibrateLastResult = false;
    calibrateFeedbackTime = millis();
    JsonDocument resp;
    resp["type"] = "calibrationCancelled";
    wsSendJson(client, resp);
  }
}

// ==================== HTTP Routes ====================
void setupHttpRoutes() {
  // SPA frontend from LittleFS (no-cache for fresh updates on reflash)
  server.serveStatic("/", LittleFS, "/")
    .setDefaultFile("index.html")
    .setCacheControl("no-cache, no-store, must-revalidate");

  // JSON API
  server.on("/api.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"device\":\"" + String(deviceName) + "\",";
    json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"wifi\":\"" + String(wifiConnected ? "connected" : "disconnected") + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"time_valid\":" + String(timeValid ? "true" : "false") + ",";
    json += "\"temperatures\":{";
    json += "\"cold\":" + String(tempSensors.getTempHVS(), 1) + ",";
    json += "\"hot\":" + String(tempSensors.getTempGVS(), 1) + ",";
    json += "\"supply\":" + String(tempSensors.getTempSupply(), 1) + ",";
    json += "\"return\":" + String(tempSensors.getTempReturn(), 1);
    json += "},";
    json += "\"meters\":{";
    json += "\"hot_m3\":" + String(config.data.meterHotM3, 3) + ",";
    json += "\"cold_m3\":" + String(config.data.meterColdM3, 3);
    json += "},";
    json += "\"calibrating\":" + String(tempSensors.isCalibrating() ? "true" : "false");
    json += "}\n";
    request->send(200, "application/json", json);
  });

  // Prometheus metrics
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request) {
    String body;
    body += "# HELP smartwatermeter_temperature Temperature sensors\n";
    body += "# TYPE smartwatermeter_temperature gauge\n";
    body += "smartwatermeter_temperature{sensor=\"cold\"} " + String(tempSensors.getTempHVS(), 1) + "\n";
    body += "smartwatermeter_temperature{sensor=\"hot\"} " + String(tempSensors.getTempGVS(), 1) + "\n";
    body += "smartwatermeter_temperature{sensor=\"supply\"} " + String(tempSensors.getTempSupply(), 1) + "\n";
    body += "smartwatermeter_temperature{sensor=\"return\"} " + String(tempSensors.getTempReturn(), 1) + "\n";
    body += "# HELP smartwatermeter_meter Water meter readings in m3\n";
    body += "# TYPE smartwatermeter_meter gauge\n";
    body += "smartwatermeter_meter{type=\"hot\"} " + String(config.data.meterHotM3, 3) + "\n";
    body += "smartwatermeter_meter{type=\"cold\"} " + String(config.data.meterColdM3, 3) + "\n";
    body += "# HELP smartwatermeter_uptime_seconds System uptime\n";
    body += "# TYPE smartwatermeter_uptime_seconds counter\n";
    body += "smartwatermeter_uptime_seconds " + String(millis() / 1000) + "\n";
    body += "# HELP smartwatermeter_free_heap_bytes Free heap memory\n";
    body += "# TYPE smartwatermeter_free_heap_bytes gauge\n";
    body += "smartwatermeter_free_heap_bytes " + String(ESP.getFreeHeap()) + "\n";
    body += "# HELP smartwatermeter_wifi_rssi WiFi signal strength\n";
    body += "# TYPE smartwatermeter_wifi_rssi gauge\n";
    body += "smartwatermeter_wifi_rssi " + String(WiFi.RSSI()) + "\n";
    body += "# HELP smartwatermeter_calibrating Whether calibration is in progress\n";
    body += "# TYPE smartwatermeter_calibrating gauge\n";
    body += "smartwatermeter_calibrating " + String(tempSensors.isCalibrating() ? "1" : "0") + "\n";
    request->send(200, "text/plain; charset=utf-8", body);
  });
}

// ==================== SMTP ====================
void trySendEmail(const String &subject, const String &body) {
  if (strlen(config.data.smtpEmail) == 0 || strlen(config.data.smtpRecipient) == 0) {
    Serial.println("[SMTP] Email not configured, skipping");
    return;
  }
  Serial.printf("[SMTP] Sending to %s...\n", config.data.smtpRecipient);

  Session_Config smtpConfig;
  smtpConfig.server.host_name = config.data.smtpHost;
  smtpConfig.server.port = config.data.smtpPort;
  smtpConfig.login.email = config.data.smtpEmail;
  smtpConfig.login.password = config.data.smtpPass;
  smtpConfig.login.user_domain = "";

  if (!smtp.connect(&smtpConfig)) {
    Serial.printf("[SMTP] Connection error: %d %s\n",
      smtp.statusCode(), smtp.errorReason().c_str());
    return;
  }

  SMTP_Message message;
  message.sender.name = deviceName;
  message.sender.email = config.data.smtpEmail;
  message.subject = subject;
  message.addRecipient("User", config.data.smtpRecipient);
  message.text.content = body.c_str();
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success
                          | esp_mail_smtp_notify_failure
                          | esp_mail_smtp_notify_delay;

  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.printf("[SMTP] Error: %d %s\n",
      smtp.statusCode(), smtp.errorReason().c_str());
  } else {
    Serial.println("[SMTP] Sent successfully");
  }
}

void sendDailyReport() {
  String body = String(deviceName) + " - Daily Report\n";
  body += "================================\n\n";
  if (ptm) {
    body += "Date: " + String(ptm->tm_mday) + "." + String(ptm->tm_mon + 1)
          + "." + String(ptm->tm_year + 1900) + "\n";
    body += "Time: " + String(ptm->tm_hour) + ":" + String(ptm->tm_min)
          + ":" + String(ptm->tm_sec) + "\n\n";
  }
  body += "=== Temperatures ===\n";
  body += "Hot: " + TemperatureSensors::formatTemp(tempSensors.getTempGVS()) + " C\n";
  body += "Cold: " + TemperatureSensors::formatTemp(tempSensors.getTempHVS()) + " C\n";
  body += "Heating Supply: " + TemperatureSensors::formatTemp(tempSensors.getTempSupply()) + " C\n";
  body += "Heating Return: " + TemperatureSensors::formatTemp(tempSensors.getTempReturn()) + " C\n\n";
  body += "=== Meter Readings ===\n";
  body += "Hot: " + String(config.data.meterHotM3, 3) + " m3\n";
  body += "Cold: " + String(config.data.meterColdM3, 3) + " m3\n\n";
  body += "=== System Info ===\n";
  body += "Uptime: " + String(millis() / 3600000) + " hours\n";
  body += String("WiFi: ") + (wifiConnected ? "Connected" : "Disconnected") + "\n";
  body += "IP: " + (wifiConnected ? WiFi.localIP().toString() : "N/A") + "\n";
  trySendEmail("Daily Report - SmartWaterMeter", body);
}

void onCalibrateDone(int sensorIndex, bool success) {
  calibrateLastResult = success;
  calibrateFeedbackTime = millis();
  if (success) {
    Serial.printf("[CALIBRATE] Sensor %s calibrated!\n",
      TemperatureSensors::sensorName(sensorIndex));
  }
  JsonDocument doc;
  doc["type"] = "calibrationResult";
  doc["index"] = sensorIndex;
  doc["success"] = success;
  wsBroadcastJson(doc);
}

void telnetConfirmFirmware() {
  failsafe.confirm();
}

void smtpCallback(SMTP_Status status) {
  Serial.println(status.info());
  if (status.success()) {
    Serial.printf("[SMTP] Sent: %d, Failed: %d\n",
      status.completedCount(), status.failedCount());
    for (size_t i = 0; i < smtp.sendingResult.size(); i++) {
      SMTP_Result result = smtp.sendingResult.getItem(i);
      Serial.printf("[SMTP] Msg %d: %s\n", i + 1,
        result.completed ? "success" : "failed");
    }
    smtp.sendingResult.clear();
  }
}