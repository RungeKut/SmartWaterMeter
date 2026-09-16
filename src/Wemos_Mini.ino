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
 *   - OTA with confirmation watchdog (see FailsafeOTA.h)
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
#include "Log.h"
#include "ConfigStore.h"
#include "MeterCounter.h"
#include "TemperatureSensors.h"
#include "StatusLED.h"
#include "TelnetSerial.h"
#include "FailsafeOTA.h"
#include "MqttClient.h"
#include "FilterGuard.h"

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
MqttClient mqtt;
uint8_t deviceMac[6];

AsyncWebServer server(80);
FilterGuard filterGuard;

// Алерты фильтра отправляются НЕ из обработчика события: письмо через
// ESP_Mail_Client блокирует loop() на секунды, а обработчик вызывается
// из середины разбора датчиков. Складываем в маленькое кольцо и
// разбираем в конце цикла.
#define FILTER_ALERT_QUEUE 4
struct FilterAlert { FilterGuard::Event ev; float tempC; };
FilterAlert filterAlerts[FILTER_ALERT_QUEUE];
uint8_t filterAlertHead = 0, filterAlertTail = 0;
AsyncWebSocket ws("/ws");

char apSSID[32];
char deviceName[32];
char defaultDeviceName[32];   // SmartWaterMeter-XXXXXX по MAC

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
uint32_t lastBusRescan = 0;
uint32_t lastEepromSave = 0;
uint32_t lastEmailSend = 0;
uint32_t lastMeterPulse = 0;
bool metersDirty = false;

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
void wsBroadcastTelemetry(const JsonDocument &doc);
void handleWsMessage(AsyncWebSocketClient *client, const String &msg);
void sendFullState(AsyncWebSocketClient *client);
void setupHttpRoutes();
void keepOrSet(char *dst, JsonVariantConst src, size_t dstSize);
void fillSystemState(JsonDocument &doc);
void fillSensorState(JsonDocument &doc);
void sanitizeDeviceName(const char *src, size_t srcSize, char *dst, size_t dstSize);
String jsonTemp(float t);
void applyDeviceName();
void onMqttCommand(const String &cmd, const String &value);
void onFilterEvent(FilterGuard::Event ev, float tempC);
void handleFilterAlerts();
void fillMqttPayload(MqttClient::Payload &p);

// ==================== Helpers ====================

// Записать значение только если оно непустое. Используется для паролей:
// SPA не получает их с устройства, поэтому пустое поле означает
// «не менять», а не «стереть».
void keepOrSet(char *dst, JsonVariantConst src, size_t dstSize) {
  const char *val = src.is<const char*>() ? src.as<const char*>() : nullptr;
  if (val != nullptr && val[0] != '\0') {
    strlcpy(dst, val, dstSize);
  }
}

// Температура для JSON. Отсутствующий датчик отдаётся как null, а не
// как -127: DEVICE_DISCONNECTED_C — это признак «нет данных», а не
// измерение. Иначе Home Assistant показывал бы -127 °C, а Prometheus
// клал бы эту точку в графики и портил средние и алерты.
String jsonTemp(float t) {
  if (t == DEVICE_DISCONNECTED_C || t < -50) return "null";
  return String(t, 1);
}

// Приводит имя к виду, пригодному для hostname: латиница, цифры, дефис.
// Пробелы, точки и подчёркивания становятся дефисом, всё остальное
// (включая кириллицу) отбрасывается. Дефисы по краям убираются.
//
// srcSize обязателен: поле в EEPROM может не содержать терминатора —
// в байтах за прежним размером структуры лежит 0xFF.
void sanitizeDeviceName(const char *src, size_t srcSize, char *dst, size_t dstSize) {
  size_t j = 0;
  for (size_t i = 0; i < srcSize && src[i] != '\0' && j + 1 < dstSize; i++) {
    char c = src[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      dst[j++] = c;
    } else if (c == '-' || c == '_' || c == ' ' || c == '.') {
      if (j > 0 && dst[j - 1] != '-') dst[j++] = '-';
    }
  }
  while (j > 0 && dst[j - 1] == '-') j--;
  dst[j] = '\0';
}

// Имя из EEPROM, иначе сгенерированное по MAC.
// Заодно чистит сохранённое значение: при первой загрузке после
// расширения структуры там лежит мусор из неинициализированной flash.
void applyDeviceName() {
  char clean[sizeof(config.data.deviceName)];
  sanitizeDeviceName(config.data.deviceName, sizeof(config.data.deviceName),
                     clean, sizeof(clean));
  strlcpy(config.data.deviceName, clean, sizeof(config.data.deviceName));

  if (clean[0] != '\0') {
    strlcpy(deviceName, clean, sizeof(deviceName));
  } else {
    strlcpy(deviceName, defaultDeviceName, sizeof(deviceName));
  }
  // Точка доступа называется так же — иначе устройство пришлось бы
  // искать в сети под двумя разными именами
  strlcpy(apSSID, deviceName, sizeof(apSSID));
}

// Системная телеметрия — одинаковая в fullState и в периодическом sensors,
// чтобы Dashboard обновлялся вживую, а не только при переподключении.
void fillSystemState(JsonDocument &doc) {
  doc["device"] = deviceName;
  doc["uptime_sec"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["wifi"] = wifiConnected ? "connected" : "disconnected";
  doc["wifi_rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  doc["ap_mode"] = apModeActive;
  doc["ap_ssid"] = apSSID;
  doc["ip"] = apModeActive ? WiFi.softAPIP().toString()
                           : WiFi.localIP().toString();
  doc["time_valid"] = timeValid;
  if (ptm) {
    doc["time_hour"] = ptm->tm_hour;
    doc["time_min"]  = ptm->tm_min;
    doc["time_sec"]  = ptm->tm_sec;
    doc["time_year"] = ptm->tm_year + 1900;
    doc["time_mon"]  = ptm->tm_mon + 1;
    doc["time_mday"] = ptm->tm_mday;
  }
  doc["ota_pending"] = failsafe.isPending();
  doc["ota_remaining"] = failsafe.remainingSec();
  doc["mqtt_enabled"] = mqtt.isEnabled();
  doc["mqtt_connected"] = mqtt.isConnected();
}

// Температуры, счётчики и карта шины OneWire
void fillSensorState(JsonDocument &doc) {
  JsonObject t = doc["temperatures"].to<JsonObject>();
  t["cold"]   = tempSensors.getTempHVS();
  t["hot"]    = tempSensors.getTempGVS();
  t["return"] = tempSensors.getTempReturn();
  t["supply"] = tempSensors.getTempSupply();

  JsonObject m = doc["meters"].to<JsonObject>();
  m["hot_m3"]  = config.data.meterHotM3;
  m["cold_m3"] = config.data.meterColdM3;

  JsonObject fg = doc["filter"].to<JsonObject>();
  fg["enabled"] = config.data.filterEnabled;
  fg["closed"] = filterGuard.isClosed();
  fg["sensorLost"] = filterGuard.isSensorLost();
  fg["trips"] = filterGuard.trips();
  fg["closedSec"] = filterGuard.closedSec();
  fg["tempOn"] = serialized(String(filterGuard.onThreshold(), 1));
  fg["tempOff"] = serialized(String(filterGuard.offThreshold(), 1));
  if (filterGuard.hasMaxTemp()) fg["maxTemp"] = serialized(String(filterGuard.maxTempC(), 1));
  else                          fg["maxTemp"] = nullptr;

  doc["calibrating"] = tempSensors.isCalibrating();
  doc["calibrate_index"] = tempSensors.getCalibrateIndex();
  doc["calibrate_remaining"] = tempSensors.getCalibrateRemainingSec();

  JsonArray sensors = doc["sensorMapping"].to<JsonArray>();
  for (int i = 0; i < NUM_SENSORS; i++) {
    JsonObject sm = sensors.add<JsonObject>();
    sm["name"] = TemperatureSensors::sensorName(i);
    sm["found"] = tempSensors.isFound(i);
    sm["temp"] = tempSensors.getTemp(i);
    // Адрес отдаём всегда, в том числе для отсутствующих датчиков:
    // по нему пользователь найдёт нужный датчик и подключит его
    sm["assigned"] = tempSensors.hasAssignedAddr(i);
    const uint8_t* ea = tempSensors.getExpectedAddr(i);
    if (ea && tempSensors.hasAssignedAddr(i)) {
      char eaStr[17];
      for (int j = 0; j < 8; j++) sprintf(eaStr + j * 2, "%02X", ea[j]);
      eaStr[16] = 0;
      sm["address"] = eaStr;
    }
  }

  JsonArray bus = doc["busDevices"].to<JsonArray>();
  for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
    JsonObject b = bus.add<JsonObject>();
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
}

// Команды из Home Assistant (кнопки и числовые настройки).
// Тема вида smartwatermeter/<MAC3>/cmd/<команда>.
void onMqttCommand(const String &cmd, const String &value) {
  if (cmd == "restart") {
    needsRestart = true;
  } else if (cmd == "confirm_ota") {
    failsafe.confirm();
  } else if (cmd == "debounce_closed" || cmd == "debounce_open") {
    long v = value.toInt();
    if (v < 0) v = 0;
    if (v > 60000) v = 60000;
    if (cmd == "debounce_closed") config.data.debounceClosedMs = (uint16_t)v;
    else                          config.data.debounceOpenMs = (uint16_t)v;
    config.save();
    Log.printf("[MQTT] %s set to %ld ms\n", cmd.c_str(), v);
  } else if (cmd == "filter_temp_on" || cmd == "filter_temp_off") {
    float v = value.toFloat();
    if (cmd == "filter_temp_on") {
      if (v > 0 && v <= FILTER_TEMP_MAX_C) config.data.filterTempOnC = v;
    } else {
      if (v > 0 && v < config.data.filterTempOnC) config.data.filterTempOffC = v;
    }
    config.save();
    Log.printf("[MQTT] %s set to %.1f C\n", cmd.c_str(), v);
  } else {
    Log.printf("[MQTT] Unknown command: %s\n", cmd.c_str());
  }
}

// Снимок состояния для MQTT. Держим сборку здесь, чтобы модуль
// MqttClient не зависел от остального проекта.
void fillMqttPayload(MqttClient::Payload &p) {
  p.tempCold   = tempSensors.getTempHVS();
  p.tempHot    = tempSensors.getTempGVS();
  p.tempSupply = tempSensors.getTempSupply();
  p.tempReturn = tempSensors.getTempReturn();
  p.tempColdOk   = tempSensors.isFound(0);
  p.tempHotOk    = tempSensors.isFound(1);
  p.tempReturnOk = tempSensors.isFound(2);
  p.tempSupplyOk = tempSensors.isFound(3);

  p.meterHotM3  = config.data.meterHotM3;
  p.meterColdM3 = config.data.meterColdM3;
  p.reedHotClosed  = meterHot.isClosed();
  p.reedColdClosed = meterCold.isClosed();

  p.rssi = wifiConnected ? WiFi.RSSI() : 0;
  p.uptimeSec = millis() / 1000;
  p.freeHeap = ESP.getFreeHeap();

  p.debounceClosedMs = config.data.debounceClosedMs;
  p.debounceOpenMs = config.data.debounceOpenMs;

  p.filterEnabled = config.data.filterEnabled;
  p.filterClosed = filterGuard.isClosed();
  p.filterSensorLost = filterGuard.isSensorLost();
  p.filterTrips = filterGuard.trips();
  p.filterTempOnC = filterGuard.onThreshold();
  p.filterTempOffC = filterGuard.offThreshold();
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  bootMillis = millis();

  led.begin();

  // Unique device name from MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);
  memcpy(deviceMac, mac, 6);
  snprintf(defaultDeviceName, sizeof(defaultDeviceName), "%s-%02X%02X%02X",
    AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

  config.begin();
  applyDeviceName();   // имя из EEPROM, иначе сгенерированное по MAC
  Log.printf("\n\n=== %s ===\n", deviceName);
  tempSensors.begin();

  // Check all sensors present
  bool allSensorsFound = true;
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (!tempSensors.isFound(i)) allSensorsFound = false;
  }
  if (!allSensorsFound) led.setSensorErr();

  meterHot.begin(PIN_METER_HOT, true, &config);
  meterCold.begin(PIN_METER_COLD, false, &config);

  // Реле защиты фильтра. Инициализируется рано и в безопасном
  // состоянии: клапан нормально открытый, вода на фильтр идёт.
  filterGuard.begin(PIN_FILTER_RELAY, &config, onFilterEvent);

  timeClient.begin();

  wifiConnected = connectToWiFi();

  // OTA
  ArduinoOTA.setHostname(deviceName);
  ArduinoOTA.onStart([]() {
    Log.println("[OTA] Start");
  });
  ArduinoOTA.onEnd([]() {
    Log.println("[OTA] Done — setting failsafe flag");
    failsafe.updateFirmware();
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Log.printf("[OTA] %u%%\r", (p * 100) / t);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Log.printf("[OTA] Error: %u\n", e);
    // При ошибке OTA не выставляем флаг — всё остаётся как было
  });
  ArduinoOTA.begin();

  Log.setSink(&telnet);   // с этого момента логи уходят и в Telnet
  telnet.begin();
  failsafe.begin();

  if (!LittleFS.begin()) {
    Log.println("[FS] LittleFS mount failed!");
  } else {
    Log.println("[FS] LittleFS mounted");
  }

  // WebSocket
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                 AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Log.printf("[WS] Client #%u connected\n", client->id());
      // Вторая линия защиты от обрыва: если очередь всё-таки переполнилась
      // (например, плата ушла в долгую отправку почты), лучше потерять
      // кадр телеметрии, чем соединение — обрыв перезагружал страницу
      // настроек поверх незаконченного ввода.
      client->setCloseClientOnQueueFull(false);
      sendFullState(client);
    } else if (type == WS_EVT_DISCONNECT) {
      Log.printf("[WS] Client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        // data не гарантированно NUL-terminated — берём ровно len байт
        String payload;
        payload.concat((const char*)data, len);
        handleWsMessage(client, payload);
      }
    }
  });
  server.addHandler(&ws);

  setupHttpRoutes();
  server.begin();

  // MQTT: версия прошивки = дата сборки, её видно в карточке устройства HA
  mqtt.begin(&config, deviceMac, deviceName, __DATE__, onMqttCommand);

  // mDNS: устройство доступно как http://<имя>.local
  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", 80);
    Log.printf("[mDNS] http://%s.local\n", deviceName);
  } else {
    Log.println("[mDNS] start failed");
  }

  if (!wifiConnected) {
    startAPMode();
  } else {
    led.setConnected();
  }

  if (strlen(config.data.smtpEmail) > 0 && wifiConnected) {
    trySendEmail("Power On", String(deviceName) + " is Power On");
  }

  Log.println("[Setup] Ready");
}

// ==================== LOOP ====================
void loop() {
  led.tick();
  ArduinoOTA.handle();
  telnet.handle();
  failsafe.handle();
  MDNS.update();

  {
    MqttClient::Payload mp;
    fillMqttPayload(mp);
    mqtt.handle(mp);
  }
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
  // Phase 2: read results (next loop cycle)
  // Конверсия DS18B20 при 12 битах длится ~750 мс, результат читаем через
  // >=850 мс. С setWaitForConversion(false) запуск стоит ~2 мс, поэтому
  // период 1000 мс укладывается с запасом: цикл получается ~1010 мс.
  uint32_t sensorInterval = 1000;

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

    // Защита фильтра работает по свежему значению ХВС. isFound(0)
    // отличает настоящий холод от отвалившегося датчика: -127 нельзя
    // принимать за «вода холодная».
    filterGuard.handle(tempSensors.getTempHVS(), tempSensors.isFound(0));

    // Broadcast to all WebSocket clients.
    // Помимо датчиков шлём и системные поля — иначе uptime, heap, время
    // и IP на Dashboard замирали бы до переподключения WebSocket.
    //
    // Если слушать некому, документ не собираем вовсе. Раньше он
    // строился каждую секунду независимо от числа клиентов: это около
    // полутора килобайт аллокаций в секунду и лишняя фрагментация кучи
    // на плате, где свободно около 18 КБ.
    if (ws.count() > 0) {
      JsonDocument doc;
      doc["type"] = "sensors";
      fillSystemState(doc);
      fillSensorState(doc);
      wsBroadcastTelemetry(doc);
    }

    // Periodic bus rescan (every 30s) to detect newly connected/disconnected sensors
    if (now - lastBusRescan > 30000 && !tempSensors.isCalibrating()) {
      lastBusRescan = now;
      tempSensors.rescanBusLight();
    }
  }

  // ---- Счётчики: разбор событий геркона (каждый цикл) ----
  bool hotPulse = meterHot.process();
  bool coldPulse = meterCold.process();
  if (hotPulse || coldPulse) {
    metersDirty = true;
    lastMeterPulse = now;
    // Показания в HA обновляем сразу, не дожидаясь периодической
    // публикации: расход воды — событие, а не фон
    if (mqtt.isConnected()) {
      MqttClient::Payload mp;
      fillMqttPayload(mp);
      mqtt.publishState(mp);
    }
  }

  // Показания уезжают в EEPROM через 30 с после последнего импульса.
  // Расход воды идёт пачками: открыли кран — закрыли. Так теряется
  // только то, что пришлось на обрыв питания прямо во время
  // пользования, а число записей равно числу «сеансов» за сутки.
  if (metersDirty && now - lastMeterPulse > 30000) {
    metersDirty = false;
    config.save();
  }

  // ---- EEPROM periodic save (every 5 min, includes meter values) ----
  if (now - lastEepromSave > 300000) {
    lastEepromSave = now;
    config.save();
  }

  // ---- Email report schedule ----
  // reportEnabled — общий выключатель плановых писем. SMTP при этом
  // остаётся настроенным: Test Email и алерты фильтра продолжают работать.
  if (config.data.reportEnabled && wifiConnected
      && strlen(config.data.smtpEmail) > 0 && ptm != nullptr) {
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
    // Список шины актуализируется внутри startCalibration() — до того,
    // как снимаются базовые температуры.
    tempSensors.startCalibration(calibrateRequestedIndex, onCalibrateDone);
  }

  // ---- Алерты фильтра (письма блокирующие, поэтому здесь) ----
  handleFilterAlerts();

  // ---- Restart ----
  if (needsRestart && now > 10000) {
    Log.println("[System] Restart...");
    ESP.restart();
  }

  delay(10);
}

// ==================== WiFi ====================
bool connectToWiFi() {
  if (strlen(config.data.wifiSSID) == 0) {
    Log.println("[WiFi] SSID not set, AP mode");
    return false;
  }
  Log.printf("[WiFi] Connecting to '%s'...\n", config.data.wifiSSID);
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
    Log.print(".");
    if (WiFi.status() == WL_CONNECTED) break;
  }
  Log.println();
  if (WiFi.status() == WL_CONNECTED) {
    Log.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    apModeActive = false;
    return true;
  }
  Log.printf("[WiFi] Failed (status: %d)\n", WiFi.status());
  wifiConnected = false;
  return false;
}

void startAPMode() {
  Log.println("[WiFi] Starting AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(
    IPAddress(192, 168, 0, 1),
    IPAddress(192, 168, 0, 1),
    IPAddress(255, 255, 255, 0)
  );
  WiFi.softAP(apSSID, AP_PASS_DEFAULT);
  Log.printf("[WiFi] AP: %s (192.168.0.1)\n", apSSID);
  apModeActive = true;
  wifiConnected = false;
}

void WiFiupd() {
  if (apModeActive) {
    uint32_t now = millis();
    if (now - lastWiFiRetry > 600000) {
      lastWiFiRetry = now;
      Log.println("[WiFi] AP: attempting reconnection...");
      WiFi.softAPdisconnect(true);
      delay(100);
      WiFi.mode(WIFI_OFF);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.setSleepMode(WIFI_NONE_SLEEP);
      WiFi.hostname(deviceName);   // иначе после реконнекта роутер видит имя по умолчанию
      if (strlen(config.data.wifiSSID) > 0) {
        WiFi.begin(config.data.wifiSSID, config.data.wifiPass);
        int attempts = 0;
        while (attempts < 40) {
          delay(500);
          attempts++;
          if (WiFi.status() == WL_CONNECTED) break;
        }
        if (WiFi.status() == WL_CONNECTED) {
          Log.printf("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
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
    Log.println("[WiFi] Reconnecting...");
    if (strlen(config.data.wifiSSID) > 0) {
      WiFi.disconnect(true);
      delay(100);
      WiFi.mode(WIFI_OFF);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.setSleepMode(WIFI_NONE_SLEEP);
      WiFi.hostname(deviceName);
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
          Log.println("[WiFi] Too many failures, switching to AP mode");
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

// Телеметрия раз в секунду. Очередь клиента на ESP8266 — 8 сообщений;
// при переполнении библиотека по умолчанию РАЗРЫВАЕТ соединение, браузер
// через 3 секунды переподключается и получает fullState. Для страницы
// настроек это выглядело как самопроизвольный сброс формы.
//
// Пропущенный кадр телеметрии не стоит обрыва: следующий придёт через
// секунду. Поэтому при забитой очереди просто молчим. Важные ответы
// (saveConfigResult, результат калибровки) по-прежнему идут через
// wsBroadcastJson/wsSendJson без потерь.
void wsBroadcastTelemetry(const JsonDocument &doc) {
  if (ws.count() == 0) return;
  if (!ws.availableForWriteAll()) return;
  wsBroadcastJson(doc);
}

void sendFullState(AsyncWebSocketClient *client) {
  JsonDocument doc;
  doc["type"] = "fullState";

  fillSystemState(doc);
  fillSensorState(doc);

  JsonObject cfg = doc["config"].to<JsonObject>();
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
  cfg["debounceClosedMs"] = config.data.debounceClosedMs;
  cfg["debounceOpenMs"] = config.data.debounceOpenMs;
  cfg["deviceName"] = config.data.deviceName;      // пустое = имя по MAC
  cfg["mqttEnabled"] = config.data.mqttEnabled;
  cfg["mqttHost"] = config.data.mqttHost;
  cfg["mqttPort"] = config.data.mqttPort;
  cfg["mqttUser"] = config.data.mqttUser;
  cfg["mqttIntervalSec"] = config.data.mqttIntervalSec;
  cfg["reportEnabled"] = config.data.reportEnabled;
  cfg["filterEnabled"] = config.data.filterEnabled;
  cfg["filterTempOnC"] = serialized(String(config.data.filterTempOnC, 1));
  cfg["filterTempOffC"] = serialized(String(config.data.filterTempOffC, 1));
  cfg["filterRelayActiveLow"] = config.data.filterRelayActiveLow;
  cfg["filterNotifyMqtt"] = config.data.filterNotifyMqtt;
  cfg["filterNotifyEmail"] = config.data.filterNotifyEmail;
  cfg["filterMetricsEnabled"] = config.data.filterMetricsEnabled;
  cfg["filterRelayPin"] = PIN_FILTER_RELAY;
  // mqttPass не отдаём — как и остальные пароли
  cfg["defaultDeviceName"] = defaultDeviceName;
  // Пароли (wifiPass/smtpPass) намеренно не отдаём клиенту

  // Диагностика герконов. Кладём только в fullState (раз в 30 с по
  // keep-alive) — в секундной рассылке это лишние байты.
  JsonArray md = doc["meterDiag"].to<JsonArray>();
  MeterCounter* meters[2] = { &meterHot, &meterCold };
  const char* names[2] = { "Hot", "Cold" };
  for (int i = 0; i < 2; i++) {
    JsonObject d = md.add<JsonObject>();
    d["name"] = names[i];
    d["closed"] = meters[i]->isClosed();
    d["stateAgeSec"] = meters[i]->stateAgeSec();
    d["pulses"] = meters[i]->totalPulses();
    d["bounces"] = meters[i]->bounces();
    d["lastClosedMs"] = meters[i]->lastClosedMs();
    d["minClosedMs"] = meters[i]->minClosedMs();
    d["maxClosedMs"] = meters[i]->maxClosedMs();
    d["lastOpenMs"] = meters[i]->lastOpenMs();
    d["overflow"] = meters[i]->queueOverflow();
  }

  wsSendJson(client, doc);
}

void handleWsMessage(AsyncWebSocketClient *client, const String &msg) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Log.printf("[WS] Parse error: %s\n", err.c_str());
    return;
  }

  const char* type = doc["type"];
  if (type == nullptr) {
    Log.println("[WS] Message without 'type' field, ignored");
    return;
  }

  if (strcmp(type, "getFullState") == 0) {
    sendFullState(client);
  }
  else if (strcmp(type, "saveConfig") == 0) {
    JsonObject cfg = doc["config"];
    if (cfg.isNull()) {
      // Без объекта config все поля ушли бы в EEPROM пустыми
      Log.println("[WS] saveConfig without 'config' object, ignored");
      JsonDocument resp;
      resp["type"] = "saveConfigResult";
      resp["success"] = false;
      resp["message"] = "Malformed request";
      wsSendJson(client, resp);
      return;
    }

    strlcpy(config.data.wifiSSID, cfg["wifiSSID"] | "", sizeof(config.data.wifiSSID));
    // Поля паролей в SPA всегда приходят пустыми, если пользователь их не
    // трогал (сервер их не отдаёт). Пустое значение = «оставить как было»,
    // иначе сохранение любой настройки стирало бы пароли.
    keepOrSet(config.data.wifiPass, cfg["wifiPass"], sizeof(config.data.wifiPass));
    strlcpy(config.data.smtpHost, cfg["smtpHost"] | "", sizeof(config.data.smtpHost));
    config.data.smtpPort = cfg["smtpPort"] | 465;
    strlcpy(config.data.smtpEmail, cfg["smtpEmail"] | "", sizeof(config.data.smtpEmail));
    keepOrSet(config.data.smtpPass, cfg["smtpPass"], sizeof(config.data.smtpPass));
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
    // 0 = значения по умолчанию из MeterCounter.h
    config.data.debounceClosedMs = cfg["debounceClosedMs"] | 0;
    config.data.debounceOpenMs = cfg["debounceOpenMs"] | 0;

    // Имя устройства: чистим до пригодного для hostname.
    // Пустое значение = вернуться к имени по MAC.
    const char *nm = cfg["deviceName"] | "";
    char cleanName[sizeof(config.data.deviceName)];
    sanitizeDeviceName(nm, strlen(nm), cleanName, sizeof(cleanName));
    strlcpy(config.data.deviceName, cleanName, sizeof(config.data.deviceName));

    // MQTT
    config.data.mqttEnabled = cfg["mqttEnabled"] | false;
    strlcpy(config.data.mqttHost, cfg["mqttHost"] | "", sizeof(config.data.mqttHost));
    config.data.mqttPort = cfg["mqttPort"] | 1883;
    strlcpy(config.data.mqttUser, cfg["mqttUser"] | "", sizeof(config.data.mqttUser));
    keepOrSet(config.data.mqttPass, cfg["mqttPass"], sizeof(config.data.mqttPass));
    config.data.mqttIntervalSec = cfg["mqttIntervalSec"] | 0;

    // Защита фильтра
    config.data.reportEnabled = cfg["reportEnabled"] | true;
    config.data.filterEnabled = cfg["filterEnabled"] | false;
    config.data.filterTempOnC = cfg["filterTempOnC"] | FILTER_TEMP_ON_DEFAULT;
    config.data.filterTempOffC = cfg["filterTempOffC"] | FILTER_TEMP_OFF_DEFAULT;
    // Схлопнувшийся гистерезис заставил бы реле дребезжать. Клиент это
    // уже проверяет, но настройки приходят и из других мест.
    if (!(config.data.filterTempOnC > 0) || config.data.filterTempOnC > FILTER_TEMP_MAX_C)
      config.data.filterTempOnC = FILTER_TEMP_ON_DEFAULT;
    if (!(config.data.filterTempOffC > 0)
        || config.data.filterTempOffC >= config.data.filterTempOnC)
      config.data.filterTempOffC = config.data.filterTempOnC - FILTER_HYST_MIN_C;
    config.data.filterRelayActiveLow = cfg["filterRelayActiveLow"] | false;
    config.data.filterNotifyMqtt = cfg["filterNotifyMqtt"] | false;
    config.data.filterNotifyEmail = cfg["filterNotifyEmail"] | false;
    config.data.filterMetricsEnabled = cfg["filterMetricsEnabled"] | false;

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
    testBody += F("=== Temperatures ===\n");
    testBody += F("Cold: ");
    testBody += TemperatureSensors::formatTemp(tempSensors.getTempHVS());
    testBody += F(" C\n");
    testBody += F("Hot: ");
    testBody += TemperatureSensors::formatTemp(tempSensors.getTempGVS());
    testBody += F(" C\n");
    testBody += F("Heating Supply: ");
    testBody += TemperatureSensors::formatTemp(tempSensors.getTempSupply());
    testBody += F(" C\n");
    testBody += F("Heating Return: ");
    testBody += TemperatureSensors::formatTemp(tempSensors.getTempReturn());
    testBody += F(" C\n\n");
    testBody += F("=== Meter Readings ===\n");
    testBody += F("Hot: ");
    testBody += String(config.data.meterHotM3, 3);
    testBody += F(" m3\n");
    testBody += F("Cold: ");
    testBody += String(config.data.meterColdM3, 3);
    testBody += F(" m3\n\n");
    testBody += F("=== System ===\n");
    testBody += F("WiFi: ");
    testBody += String(wifiConnected ? "connected" : "disconnected");
    testBody += F("\n");
    testBody += F("IP: ");
    testBody += (wifiConnected ? WiFi.localIP().toString() : "N/A");
    testBody += F("\n");
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
  else if (strcmp(type, "assignSensor") == 0) {
    // Ручное назначение датчика с шины на канал — альтернатива нагреву
    int idx = doc["index"] | -1;
    int busIdx = doc["busIndex"] | -1;
    int displaced = -1;

    JsonDocument resp;
    resp["type"] = "assignResult";
    if (idx < 0 || busIdx < 0 ||
        !tempSensors.assignSensor(idx, (uint8_t)busIdx, &displaced)) {
      resp["success"] = false;
      resp["message"] = "Assign failed";
    } else {
      resp["success"] = true;
      if (displaced >= 0) {
        resp["message"] = String(TemperatureSensors::sensorName(idx))
                        + " assigned, "
                        + TemperatureSensors::sensorName(displaced)
                        + " released";
      } else {
        resp["message"] = String(TemperatureSensors::sensorName(idx)) + " assigned";
      }
    }
    wsSendJson(client, resp);
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
    json += F("\"device\":\"");
    json += String(deviceName);
    json += F("\",");
    json += F("\"uptime_sec\":");
    json += String(millis() / 1000);
    json += F(",");
    json += F("\"free_heap\":");
    json += String(ESP.getFreeHeap());
    json += F(",");
    json += F("\"wifi\":\"");
    json += String(wifiConnected ? "connected" : "disconnected");
    json += F("\",");
    json += F("\"wifi_rssi\":");
    json += String(wifiConnected ? WiFi.RSSI() : 0);
    json += F(",");
    json += F("\"ip\":\"");
    json += (apModeActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
    json += F("\",");
    json += F("\"time_valid\":");
    json += String(timeValid ? "true" : "false");
    json += F(",");
    json += F("\"temperatures\":{");
    json += F("\"cold\":");
    json += jsonTemp(tempSensors.getTempHVS());
    json += F(",");
    json += F("\"hot\":");
    json += jsonTemp(tempSensors.getTempGVS());
    json += F(",");
    json += F("\"supply\":");
    json += jsonTemp(tempSensors.getTempSupply());
    json += F(",");
    json += F("\"return\":");
    json += jsonTemp(tempSensors.getTempReturn());
    json += F("},");
    json += F("\"meters\":{");
    json += F("\"hot_m3\":");
    json += String(config.data.meterHotM3, 3);
    json += F(",");
    json += F("\"cold_m3\":");
    json += String(config.data.meterColdM3, 3);
    json += F("},");
    json += F("\"calibrating\":");
    json += String(tempSensors.isCalibrating() ? "true" : "false");
    json += F("}\n");
    request->send(200, "application/json", json);
  });

  // OTA confirmation endpoint (GET from browser)
  server.on("/confirm", HTTP_GET, [](AsyncWebServerRequest *request) {
    failsafe.confirm();
    String html = "<html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"3;url=/\"></head><body>";
    html += F("<h2>Прошивка подтверждена!</h2>");
    html += F("<p>Возврат на главную через 3 секунды...</p>");
    html += F("</body></html>");
    request->send(200, "text/html; charset=utf-8", html);
  });

  // Prometheus metrics
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request) {
    String body;

    // Отсутствующий датчик не экспортируется вовсе. Отдать -127 означало
    // бы положить в график настоящую точку и испортить средние и алерты;
    // в Prometheus отсутствие серии — штатный способ сказать «данных нет».
    body += F("# HELP smartwatermeter_temperature_celsius Temperature sensors\n");
    body += F("# TYPE smartwatermeter_temperature_celsius gauge\n");
    const char* tNames[4] = { "cold", "hot", "supply", "return" };
    float tVals[4] = {
      tempSensors.getTempHVS(), tempSensors.getTempGVS(),
      tempSensors.getTempSupply(), tempSensors.getTempReturn()
    };
    for (int i = 0; i < 4; i++) {
      if (tVals[i] == DEVICE_DISCONNECTED_C || tVals[i] < -50) continue;
      body += F("smartwatermeter_temperature_celsius{sensor=\"");
      body += tNames[i];
      body += F("\"} ");
      body += String(tVals[i], 1);
      body += F("\n");
    }

    // Показания счётчиков монотонно растут — это counter, а не gauge.
    // Только с типом counter имеют смысл rate() и increase(), то есть
    // расход за час или за сутки.
    body += F("# HELP smartwatermeter_water_m3_total Total water consumption\n");
    body += F("# TYPE smartwatermeter_water_m3_total counter\n");
    body += F("smartwatermeter_water_m3_total{type=\"hot\"} ");
    body += String(config.data.meterHotM3, 3);
    body += F("\n");
    body += F("smartwatermeter_water_m3_total{type=\"cold\"} ");
    body += String(config.data.meterColdM3, 3);
    body += F("\n");

    // Аптайм — gauge: он сбрасывается при перезагрузке
    // Метрики фильтра отдаются по галочке. Prometheus ничего не
    // «получает» — он опрашивает сам, поэтому «уведомлять через
    // Prometheus» здесь означает именно «выставлять серию наружу».
    if (config.data.filterMetricsEnabled) {
      body += F("# HELP smartwatermeter_filter_valve_closed Osmosis filter inlet valve is shut\n");
      body += F("# TYPE smartwatermeter_filter_valve_closed gauge\n");
      body += F("smartwatermeter_filter_valve_closed ");
      body += String(filterGuard.isClosed() ? 1 : 0);
      body += F("\n");

      // Срабатывания монотонно растут в пределах периода — counter,
      // чтобы работали rate() и increase()
      body += F("# HELP smartwatermeter_filter_trips_total Guard activations since boot\n");
      body += F("# TYPE smartwatermeter_filter_trips_total counter\n");
      body += F("smartwatermeter_filter_trips_total ");
      body += String(filterGuard.trips());
      body += F("\n");

      body += F("# HELP smartwatermeter_filter_closed_seconds_total Time with inlet shut\n");
      body += F("# TYPE smartwatermeter_filter_closed_seconds_total counter\n");
      body += F("smartwatermeter_filter_closed_seconds_total ");
      body += String(filterGuard.closedSec());
      body += F("\n");

      // Потерянный датчик — отдельная серия: без неё «клапан открыт»
      // выглядел бы как «всё хорошо», хотя защита ослепла
      body += F("# HELP smartwatermeter_filter_sensor_lost Cold sensor is not responding\n");
      body += F("# TYPE smartwatermeter_filter_sensor_lost gauge\n");
      body += F("smartwatermeter_filter_sensor_lost ");
      body += String(filterGuard.isSensorLost() ? 1 : 0);
      body += F("\n");
    }

    body += F("# HELP smartwatermeter_uptime_seconds System uptime\n");
    body += F("# TYPE smartwatermeter_uptime_seconds gauge\n");
    body += F("smartwatermeter_uptime_seconds ");
    body += String(millis() / 1000);
    body += F("\n");

    body += F("# HELP smartwatermeter_free_heap_bytes Free heap memory\n");
    body += F("# TYPE smartwatermeter_free_heap_bytes gauge\n");
    body += F("smartwatermeter_free_heap_bytes ");
    body += String(ESP.getFreeHeap());
    body += F("\n");

    // RSSI имеет смысл только в режиме клиента
    if (wifiConnected) {
      body += F("# HELP smartwatermeter_wifi_rssi_dbm WiFi signal strength\n");
      body += F("# TYPE smartwatermeter_wifi_rssi_dbm gauge\n");
      body += F("smartwatermeter_wifi_rssi_dbm ");
      body += String(WiFi.RSSI());
      body += F("\n");
    }

    body += F("# HELP smartwatermeter_sensor_present Sensor is currently readable\n");
    body += F("# TYPE smartwatermeter_sensor_present gauge\n");
    // Метки в нижнем регистре — те же значения, что у температур,
    // иначе серии не соединить по sensor в PromQL.
    // Порядок каналов: [0]=Cold, [1]=Hot, [2]=Return, [3]=Supply
    const char* chNames[NUM_SENSORS] = { "cold", "hot", "return", "supply" };
    for (int i = 0; i < NUM_SENSORS; i++) {
      body += F("smartwatermeter_sensor_present{sensor=\"");
      body += chNames[i];
      body += F("\"} ");
      body += tempSensors.isFound(i) ? "1" : "0";
      body += F("\n");
    }

    body += F("# HELP smartwatermeter_reed_closed Reed switch is currently closed\n");
    body += F("# TYPE smartwatermeter_reed_closed gauge\n");
    body += F("smartwatermeter_reed_closed{type=\"hot\"} ");
    body += String(meterHot.isClosed() ? 1 : 0);
    body += F("\n");
    body += F("smartwatermeter_reed_closed{type=\"cold\"} ");
    body += String(meterCold.isClosed() ? 1 : 0);
    body += F("\n");

    body += F("# HELP smartwatermeter_calibrating Whether calibration is in progress\n");
    body += F("# TYPE smartwatermeter_calibrating gauge\n");
    body += F("smartwatermeter_calibrating ");
    body += String(tempSensors.isCalibrating() ? "1" : "0");
    body += F("\n");

    request->send(200, "text/plain; version=0.0.4; charset=utf-8", body);
  });
}

// ==================== SMTP ====================
void trySendEmail(const String &subject, const String &body) {
  if (strlen(config.data.smtpEmail) == 0 || strlen(config.data.smtpRecipient) == 0) {
    Log.println("[SMTP] Email not configured, skipping");
    return;
  }
  Log.printf("[SMTP] Sending to %s...\n", config.data.smtpRecipient);

  smtp.callback(smtpCallback);

  Session_Config smtpConfig;
  smtpConfig.server.host_name = config.data.smtpHost;
  smtpConfig.server.port = config.data.smtpPort;
  smtpConfig.login.email = config.data.smtpEmail;
  smtpConfig.login.password = config.data.smtpPass;
  smtpConfig.login.user_domain = "";

  if (!smtp.connect(&smtpConfig)) {
    Log.printf("[SMTP] Connection error: %d %s\n",
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
    Log.printf("[SMTP] Error: %d %s\n",
      smtp.statusCode(), smtp.errorReason().c_str());
  } else {
    Log.println("[SMTP] Sent successfully");
  }
}

void sendDailyReport() {
  String body = String(deviceName) + " - Daily Report\n";
  body += F("================================\n\n");
  if (ptm) {
    body += F("Date: ");
    body += String(ptm->tm_mday);
    body += F(".");
    body += String(ptm->tm_mon + 1);
    body += F(".");
    body += String(ptm->tm_year + 1900);
    body += F("\n");
    body += F("Time: ");
    body += String(ptm->tm_hour);
    body += F(":");
    body += String(ptm->tm_min);
    body += F(":");
    body += String(ptm->tm_sec);
    body += F("\n\n");
  }
  body += F("=== Temperatures ===\n");
  body += F("Hot: ");
  body += TemperatureSensors::formatTemp(tempSensors.getTempGVS());
  body += F(" C\n");
  body += F("Cold: ");
  body += TemperatureSensors::formatTemp(tempSensors.getTempHVS());
  body += F(" C\n");
  body += F("Heating Supply: ");
  body += TemperatureSensors::formatTemp(tempSensors.getTempSupply());
  body += F(" C\n");
  body += F("Heating Return: ");
  body += TemperatureSensors::formatTemp(tempSensors.getTempReturn());
  body += F(" C\n\n");
  body += F("=== Meter Readings ===\n");
  body += F("Hot: ");
  body += String(config.data.meterHotM3, 3);
  body += F(" m3\n");
  body += F("Cold: ");
  body += String(config.data.meterColdM3, 3);
  body += F(" m3\n\n");
  if (config.data.filterEnabled) {
    body += F("=== Filter Guard ===\n");
    body += F("Trips this period: ");
    body += String(filterGuard.trips());
    body += F("\n");
    body += F("Max cold water: ");
    body += (filterGuard.hasMaxTemp() ? TemperatureSensors::formatTemp(filterGuard.maxTempC()) : String("n/a"));
    body += F(" C\n");
    body += F("Inlet shut for: ");
    body += String(filterGuard.closedSec() / 60);
    body += F(" min\n");
    body += F("Valve now: ");
    body += String(filterGuard.isClosed() ? "CLOSED" : "open");
    body += F("\n");
    if (filterGuard.isSensorLost()) body += "WARNING: cold sensor not responding\n";
    body += F("\n");
  }
  body += F("=== System Info ===\n");
  body += F("Uptime: ");
  body += String(millis() / 3600000);
  body += F(" hours\n");
  body += String("WiFi: ");
  body += (wifiConnected ? "Connected" : "Disconnected");
  body += F("\n");
  body += F("IP: ");
  body += (wifiConnected ? WiFi.localIP().toString() : "N/A");
  body += F("\n");
  trySendEmail("Daily Report - SmartWaterMeter", body);
  // Статистика фильтра — за период между отчётами, а не с загрузки
  filterGuard.resetStats();
}

// Вызывается из filterGuard.handle() в середине разбора датчиков.
// Здесь ничего блокирующего: только складываем событие и, если
// разрешено, толкаем состояние в MQTT — публикация асинхронная.
void onFilterEvent(FilterGuard::Event ev, float tempC) {
  uint8_t next = (uint8_t)((filterAlertHead + 1) % FILTER_ALERT_QUEUE);
  if (next != filterAlertTail) {
    filterAlerts[filterAlertHead].ev = ev;
    filterAlerts[filterAlertHead].tempC = tempC;
    filterAlertHead = next;
  }

  if (config.data.filterNotifyMqtt && mqtt.isConnected()) {
    MqttClient::Payload mp;
    fillMqttPayload(mp);
    mqtt.publishState(mp);
  }

  // Состояние на вкладке Dashboard должно смениться сразу, не дожидаясь
  // очередной секундной рассылки
  JsonDocument doc;
  doc["type"] = "filterEvent";
  doc["event"] = FilterGuard::eventName(ev);
  doc["closed"] = filterGuard.isClosed();
  doc["sensorLost"] = filterGuard.isSensorLost();
  doc["temp"] = serialized(String(tempC, 1));
  wsBroadcastJson(doc);
}

// Письма шлём из loop(): ESP_Mail_Client блокирует на секунды, и делать
// это внутри разбора датчиков значило бы ронять импульсы счётчиков.
void handleFilterAlerts() {
  if (filterAlertTail == filterAlertHead) return;
  if (!config.data.filterNotifyEmail) {     // очередь всё равно чистим
    filterAlertTail = filterAlertHead;
    return;
  }
  if (!wifiConnected || strlen(config.data.smtpEmail) == 0) {
    filterAlertTail = filterAlertHead;
    return;
  }

  FilterAlert a = filterAlerts[filterAlertTail];
  filterAlertTail = (uint8_t)((filterAlertTail + 1) % FILTER_ALERT_QUEUE);

  String subject = String("Filter Guard: ") + FilterGuard::eventName(a.ev);
  String body = String(deviceName) + " - Filter Guard\n";
  body += F("================================\n\n");
  body += F("Event: ");
  body += String(FilterGuard::eventName(a.ev));
  body += F("\n");
  body += F("Cold water: ");
  body += TemperatureSensors::formatTemp(a.tempC);
  body += F(" C\n");
  body += F("Thresholds: ");
  body += String(filterGuard.onThreshold(), 1);
  body += F(" / ");
  body += String(filterGuard.offThreshold(), 1);
  body += F(" C\n");
  body += F("Valve now: ");
  body += String(filterGuard.isClosed() ? "CLOSED" : "open");
  body += F("\n");
  body += F("Trips this period: ");
  body += String(filterGuard.trips());
  body += F("\n");
  trySendEmail(subject, body);
}

void onCalibrateDone(int sensorIndex, bool success) {
  calibrateLastResult = success;
  calibrateFeedbackTime = millis();
  if (success) {
    Log.printf("[CALIBRATE] Sensor %s calibrated!\n",
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
  Log.println(status.info());
  if (status.success()) {
    Log.printf("[SMTP] Sent: %d, Failed: %d\n",
      status.completedCount(), status.failedCount());
    for (size_t i = 0; i < smtp.sendingResult.size(); i++) {
      SMTP_Result result = smtp.sendingResult.getItem(i);
      Log.printf("[SMTP] Msg %d: %s\n", i + 1,
        result.completed ? "success" : "failed");
    }
    smtp.sendingResult.clear();
  }
}