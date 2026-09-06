/******************************************************************
 * SmartWaterMeter Olimp
 * 
 * ?????????: Wemos Mini (ESP8266)
 * 
 * ???????:
 * - 4 ??????? DS18B20 (???, ???, ??????/??????? ?????????)
 * - 2 ?????????? ???????? (???, ???) ? ????????? ? EEPROM
 * - ???-????????? (GyverPortal) ? ???????????? ??????
 * - ????? ????? ??????? ??? ?????????????? ????????? WiFi/SMTP
 * - ???????? email ? ??????????? ?? ??????????
 ******************************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <time.h>
#include <EEPROM.h>

// OTA-прошивка по WiFi
#include <ArduinoOTA.h>

// ==================== ???????????????? ?????? ====================
#include "secrets.h"

// ==================== ?????? ??????? ====================
#include "ConfigStore.h"
#include "MeterCounter.h"
#include "TemperatureSensors.h"
#include "StatusLED.h"
#include "TelnetSerial.h"
#include "FailsafeOTA.h"

// ==================== SMTP ====================
#include <ESP_Mail_Client.h>
SMTPSession smtp;
void smtpCallback(SMTP_Status status);

// ==================== GyverPortal ====================
#include <GyverPortal.h>
GyverPortal ui;

// ==================== NTP ====================
#include <NTPClient.h>
#define NTP_OFFSET   60 * 60
#define NTP_INTERVAL 60 * 1000
#define NTP_ADDRESS  "europe.pool.ntp.org"
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);

// ==================== ?????????? ??????? ====================
ConfigStore config;
MeterCounter meterHot;
MeterCounter meterCold;
TemperatureSensors tempSensors(ONE_WIRE_BUS, &config);
StatusLED led;
TelnetSerial telnet;
FailsafeOTA failsafe;

// AP SSID и имя устройства с суффиксом MAC-адреса
char apSSID[32];
char deviceName[32];

// ==================== ????????? ====================
GPdate valDate;
GPtime valTime;
time_t epochTime;
struct tm *ptm;

uint32_t lastSensorRead = 0;
uint32_t lastEepromSave = 0;
uint32_t lastEmailSend = 0;
uint32_t lastMeterFlush = 0;
uint32_t lastWiFiRetry = 0;
uint32_t bootMillis = 0;
time_t localEpoch = 0;       // текущее время (NTP или локальное от millis)
bool timeValid = false;      // true если время было получено через NTP хоть раз

bool wifiConnected = false;
bool apModeActive = false;
bool needsRestart = false;
int wifiReconnectFails = 0;

#define HISTORY_SIZE 60
float tempHistory[NUM_SENSORS][HISTORY_SIZE];
uint8_t historyIndex = 0;
uint8_t historyCount = 0;

// Калибровка датчиков
bool calibrateRequested = false;
int calibrateRequestedIndex = -1;
uint32_t calibrateFeedbackTime = 0;
bool calibrateLastResult = false;

// ==================== Прототипы ====================
void buildPage(GyverPortal& p);
void action(GyverPortal& p);
void buildMain(GyverPortal& p);
void buildMainUpdate(GyverPortal& p);
void buildSettings(GyverPortal& p);
void actionSettings(GyverPortal& p);
void buildCalibrate(GyverPortal& p);
void onCalibrateDone(int sensorIndex, bool success);
void handleApiJson(GyverPortal& p);
void handleMetrics(GyverPortal& p);
void WiFiupd();
void updateLocalTime();
bool connectToWiFi();
void startAPMode();
void trySendEmail(const String &subject, const String &body);
void sendDailyReport();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.printf("\n\n=== %s ===\n", deviceName);
  bootMillis = millis();
  
  led.begin();
  
  // Генерация уникального AP SSID и имени устройства на основе MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSSID, sizeof(apSSID), "%s-%02X%02X%02X",
    AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
  snprintf(deviceName, sizeof(deviceName), "%s-%02X%02X%02X",
    AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
  Serial.printf("[System] AP SSID: %s\n", apSSID);
  Serial.printf("[System] Device name: %s\n", deviceName);
  
  config.begin();
  tempSensors.begin();
  
  // Проверка, все ли датчики найдены
  bool allSensorsFound = true;
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (!tempSensors.isFound(i)) allSensorsFound = false;
  }
  if (!allSensorsFound) {
    led.setSensorErr();
  }
  
  meterHot.begin(PIN_METER_HOT, true, &config);
  meterCold.begin(PIN_METER_COLD, false, &config);
  
  timeClient.begin();
  timeClient.setTimeOffset(10800);
  
wifiConnected = connectToWiFi();

  // OTA — доступна и в STA, и в AP-режиме
  ArduinoOTA.setHostname(deviceName);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Done");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error: %u\n", error);
  });
  ArduinoOTA.begin();
  Serial.printf("[OTA] Ready on %s.local\n", deviceName);

  // Telnet Serial
  telnet.begin();

  // Fail-safe OTA — проверка после загрузки
  failsafe.begin();

  ui.attachBuild(buildPage);
  ui.attach(action);
  ui.start();

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
  ui.tick();
  led.tick();
  ArduinoOTA.handle();
  telnet.handle();
  failsafe.handle();
  
  uint32_t now = millis();
  
  WiFiupd();
  updateLocalTime();
  
  // Управление LED — вычисляем нужный режим, меняем только при изменении
  {
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
  }
  
  // Двухфазный опрос датчиков: читаем готовые данные, отправляем новый запрос
  uint32_t sensorInterval = tempSensors.isCalibrating() ? 1000 : 5000;
  if (now - lastSensorRead > sensorInterval) {
    lastSensorRead = now;
    
    // Фаза 1: читаем данные предыдущего измерения (они уже готовы, т.к. прошло >750мс)
    tempSensors.readTemperatures();
    
    // Фаза 2: отправляем новый запрос на измерение (данные будут готовы через ~750мс)
    tempSensors.startConversion();
    
    for (int i = 0; i < NUM_SENSORS; i++) {
      tempHistory[i][historyIndex] = tempSensors.getTemp(i);
    }
    historyIndex = (historyIndex + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) historyCount++;
    
    Serial.printf("[TEMP] Cold:%.1f Hot:%.1f SUP:%.1f RET:%.1f\n",
      tempSensors.getTempHVS(), tempSensors.getTempGVS(),
      tempSensors.getTempSupply(), tempSensors.getTempReturn());
    
    // Пересканирование шины в том же такте (5-10ms, синхронно с опросом)
    tempSensors.rescanBus();
  }
  
  if (now - lastMeterFlush > 10000) {
    lastMeterFlush = now;
    meterHot.flush();
    meterCold.flush();
  }
  
  if (now - lastEepromSave > 300000) {
    lastEepromSave = now;
    config.save();
  }
  
  if (wifiConnected && strlen(config.data.smtpEmail) > 0) {
    if (ptm != nullptr && ptm->tm_hour == config.data.reportHour && ptm->tm_min == config.data.reportMinute && now - lastEmailSend > 60000) {
      bool shouldSend = false;
      switch (config.data.reportSchedule) {
        case 0:  // daily
          shouldSend = true;
          break;
        case 1:  // weekly — ptm->tm_wday: 0=Sun..6=Sat
          if (ptm->tm_wday == config.data.reportDay) shouldSend = true;
          break;
        case 2:  // monthly
          if (ptm->tm_mday == config.data.reportDay) shouldSend = true;
          break;
      }
      if (shouldSend) {
        lastEmailSend = now;
        sendDailyReport();
      }
    }
  }
  
  // Обработка запроса калибровки
  if (calibrateRequested && !tempSensors.isCalibrating()) {
    calibrateRequested = false;
    tempSensors.startCalibration(calibrateRequestedIndex, onCalibrateDone);
  }
  
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
  
  // Полный сброс WiFi-стека
  WiFi.persistent(false);
  WiFi.disconnect(true);  // true = сбросить сохранённые конфиги
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname(deviceName);
  
  // Принудительное сканирование эфира — чтобы ESP увидела доступные сети
  Serial.println("[WiFi] Scanning networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("[WiFi] Found %d networks\n", n);
  bool ssidFound = false;
  for (int i = 0; i < n; i++) {
    String found = WiFi.SSID(i);
    Serial.printf("  %s\n", found.c_str());
    if (found == config.data.wifiSSID) {
      ssidFound = true;
    }
  }
  if (!ssidFound) {
    Serial.println("[WiFi] WARNING: SSID not found in scan! Check range/channel.");
  }
  delay(100);
  
  // Принудительно: 802.11 b/g/n
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  
  WiFi.begin(config.data.wifiSSID, config.data.wifiPass);
  
  // Ручной таймаут 30 секунд
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
  
  Serial.printf("[WiFi] Failed to connect (status: %d)\n", WiFi.status());
  // Диагностика: выводим auth_mode сети
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == config.data.wifiSSID) {
      Serial.printf("[WiFi] Target network encryption: %d RSSI: %d\n",
        WiFi.encryptionType(i), WiFi.RSSI(i));
      break;
    }
  }
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
  // Если в AP-режиме — раз в 10 минут пробуем переподключиться к роутеру
  if (apModeActive) {
    uint32_t now = millis();
    if (now - lastWiFiRetry > 600000) {  // 10 минут
      lastWiFiRetry = now;
      Serial.println("[WiFi] AP mode: attempting reconnection to router...");
      
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
          Serial.printf("[WiFi] Reconnect failed, returning to AP mode (status: %d)\n", WiFi.status());
          startAPMode();  // перезапускаем AP (т.к. softAPdisconnect его выключил)
        }
      }
    }
    return;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiReconnectFails = 0;  // сброс счётчика при успехе
    timeClient.update();
    
    // Если NTP дал валидное время — сохраняем как точку отсчёта
    epochTime = timeClient.getEpochTime();
    if (epochTime > 100000) {  // 1973+ год — валидное время
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
        Serial.printf("[WiFi] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        wifiConnected = true;
        wifiReconnectFails = 0;
      } else {
        wifiReconnectFails++;
        Serial.printf("[WiFi] Reconnect failed (status: %d, fails: %d)\n", WiFi.status(), wifiReconnectFails);
        // После 3 неудачных попыток подряд — переходим в AP-режим
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
    // Время было получено через NTP — считаем от localEpoch + прошедшие millis
    time_t now = localEpoch + (millis() - bootMillis) / 1000;
    ptm = gmtime(&now);
    if (ptm != nullptr) {
      valTime = GPtime(ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
      valDate = GPdate(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
    }
  } else {
    // Времени нет — показываем аптайм
    uint32_t sec = millis() / 1000;
    valTime = GPtime(sec / 3600, (sec % 3600) / 60, sec % 60);
    valDate = GPdate(0, 0, 0);
  }
}

// ==================== GyverPortal: Router ====================
void buildPage(GyverPortal& p) {
  if (p.uri("/api.json")) {
    handleApiJson(p);
  } else if (p.uri("/metrics")) {
    handleMetrics(p);
  } else if (p.uri("/settings")) {
    buildSettings(p);
  } else if (p.uri("/calibrate")) {
    buildCalibrate(p);
  } else {
    buildMain(p);
  }
}

void action(GyverPortal& p) {
  // JSON API и Prometheus — обрабатываем до всего, т.к. это не HTML
  if (p.uri("/api.json")) {
    handleApiJson(p);
    return;
  }
  if (p.uri("/metrics")) {
    handleMetrics(p);
    return;
  }
  // Настройки
  if (p.uri("/settings")) {
    actionSettings(p);
  }
  // Частичное обновление — URI всегда /GP_update, определяем по ID блока
  if (p.update()) {
    String updName = p.updateName();
    if (updName == "dateBlock" || updName == "wsBlock" || updName == "heatBlock" || updName == "statusBlock") {
      buildMainUpdate(p);
    } else if (updName == "busBlock" || updName == "mapBlock" || updName == "calBlock") {
      buildCalibrate(p);
    }
  }
}

// ==================== GyverPortal: Main Page ====================
void buildMain(GyverPortal& p) {
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE(deviceName, deviceName);
  GP.TITLE(deviceName, "t1");
  
  // Блок даты/времени — обновляется через GP.UPDATE
  GP.BLOCK_BEGIN(GP_THIN, "", "Date && Time");
  {
    String dtHtml;
    dtHtml += "Date: " + String(valDate.encodeDMY()) + "<br>";
    dtHtml += "Time: " + String(valTime.encode());
    GP.LABEL(dtHtml, "dateBlock");
  }
  GP.BLOCK_END();
  
  // Блок водоснабжения — обновляется через GP.UPDATE
  GP.BLOCK_BEGIN(GP_THIN, "", "Water Supply");
  {
    String wsHtml;
    wsHtml += "Hot ";
    wsHtml += "<font color='#ffb2b2'>" + String(config.data.meterHotM3, 3) + "</font>";
    wsHtml += " m3<br>";
    wsHtml += "<font color='#ffb2b2'>" + TemperatureSensors::formatTemp(tempSensors.getTempGVS()) + "</font>";
    wsHtml += " C<br><br>";
    wsHtml += "Cold ";
    wsHtml += "<font color='#b2b2ff'>" + String(config.data.meterColdM3, 3) + "</font>";
    wsHtml += " m3<br>";
    wsHtml += "<font color='#b2b2ff'>" + TemperatureSensors::formatTemp(tempSensors.getTempHVS()) + "</font>";
    wsHtml += " C";
    GP.LABEL(wsHtml, "wsBlock");
  }
  GP.BLOCK_END();
  
  // Блок отопления — обновляется через GP.UPDATE
  GP.BLOCK_BEGIN(GP_THIN, "", "Heating");
  {
    String heatHtml;
    heatHtml += "Supply ";
    heatHtml += "<font color='#ffb2b2'>" + TemperatureSensors::formatTemp(tempSensors.getTempSupply()) + "</font>";
    heatHtml += " C<br>";
    heatHtml += "Return ";
    heatHtml += "<font color='#b2b2ff'>" + TemperatureSensors::formatTemp(tempSensors.getTempReturn()) + "</font>";
    heatHtml += " C";
    GP.LABEL(heatHtml, "heatBlock");
  }
  GP.BLOCK_END();
  
  // Блок статуса — обновляется через GP.UPDATE
  GP.BLOCK_BEGIN(GP_THIN, "", "Status");
  {
    String stHtml;
    stHtml += wifiConnected ? "WiFi: connected" : "WiFi: disconnected";
    stHtml += "<br>";
    if (wifiConnected) {
      stHtml += "IP: " + WiFi.localIP().toString();
      stHtml += "<br>";
    }
    if (apModeActive) {
      stHtml += "AP: " + String(apSSID);
      stHtml += "<br>";
    }
    GP.LABEL(stHtml, "statusBlock");
  }
  GP.BLOCK_END();
  
  // Обработка GET-параметра restart
  if (p.getString("restart") == "1") {
    needsRestart = true;
  }
  
  // Обработка подтверждения новой прошивки
  if (p.getString("confirm") == "1") {
    failsafe.confirm();
  }
  
  // Блок подтверждения прошивки (только если ждём)
  if (failsafe.isPending()) {
    GP.BLOCK_BEGIN(GP_THIN, "", "Firmware Update");
    {
      String fwHtml;
      fwHtml += "New firmware detected!<br>";
      fwHtml += "Auto-rollback in <b>" + String(failsafe.remainingSec()) + "</b> seconds<br>";
      fwHtml += "<input type='button' value='Confirm' onclick=\"location.href='/?confirm=1';\">";
      GP.LABEL(fwHtml, "fwBlock");
    }
    GP.BLOCK_END();
  }
  
  GP.BLOCK_BEGIN(GP_THIN, "", "Controls");
  GP.BUTTON_LINK("/settings", "Settings");
  GP.BREAK();
  GP.BUTTON_LINK("/calibrate", "Calibrate Sensors");
  GP.BREAK();
  GP.BUTTON_LINK("/?restart=1", "Restart");
  GP.BLOCK_END();
  
  // Частичное обновление каждые 5 сек
  GP.UPDATE("dateBlock,wsBlock,heatBlock,statusBlock", 5000);
  GP.BUILD_END();
}

// Частичное обновление главной страницы
void buildMainUpdate(GyverPortal& p) {
  if (p.update("dateBlock")) {
    String html;
    html += "Date: " + String(valDate.encodeDMY()) + "<br>";
    html += "Time: " + String(valTime.encode());
    p.answer(html);
    return;
  }
  if (p.update("wsBlock")) {
    String html;
    html += "Hot ";
    html += "<font color='#ffb2b2'>" + String(config.data.meterHotM3, 3) + "</font>";
    html += " m3<br>";
    html += "<font color='#ffb2b2'>" + TemperatureSensors::formatTemp(tempSensors.getTempGVS()) + "</font>";
    html += " C<br><br>";
    html += "Cold ";
    html += "<font color='#b2b2ff'>" + String(config.data.meterColdM3, 3) + "</font>";
    html += " m3<br>";
    html += "<font color='#b2b2ff'>" + TemperatureSensors::formatTemp(tempSensors.getTempHVS()) + "</font>";
    html += " C";
    p.answer(html);
    return;
  }
  if (p.update("heatBlock")) {
    String html;
    html += "Supply ";
    html += "<font color='#ffb2b2'>" + TemperatureSensors::formatTemp(tempSensors.getTempSupply()) + "</font>";
    html += " C<br>";
    html += "Return ";
    html += "<font color='#b2b2ff'>" + TemperatureSensors::formatTemp(tempSensors.getTempReturn()) + "</font>";
    html += " C";
    p.answer(html);
    return;
  }
  if (p.update("statusBlock")) {
    String html;
    html += wifiConnected ? "WiFi: connected" : "WiFi: disconnected";
    html += "<br>";
    if (wifiConnected) {
      html += "IP: " + WiFi.localIP().toString();
      html += "<br>";
    }
    if (apModeActive) {
      html += "AP: " + String(apSSID);
      html += "<br>";
    }
    p.answer(html);
    return;
  }
}

// ==================== GyverPortal: Settings Page ====================
void buildSettings(GyverPortal& p) {
  // Обработка GET-параметра testemail (приходит от BUTTON_LINK до отрисовки)
  if (p.getString("testemail") == "1") {
    Serial.println("[Settings] Test email requested");
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
  }
  
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("Settings", deviceName);
  GP.TITLE("Settings", "t1");
  
  GP.FORM_BEGIN("/settings");
  
  GP.BLOCK_BEGIN(GP_THIN, "", "WiFi");
  GP.LABEL("SSID:");
  GP.TEXT("wifiSSID", "wifiSSID", config.data.wifiSSID, "", 32);
  GP.BREAK();
  GP.LABEL("Password:");
  GP.PASS("wifiPass", "wifiPass", config.data.wifiPass, "", 64);
  GP.BLOCK_END();
  
  GP.BLOCK_BEGIN(GP_THIN, "", "Email (SMTP)");
  GP.LABEL("SMTP Host:");
  GP.TEXT("smtpHost", "smtpHost", config.data.smtpHost, "", 32);
  GP.BREAK();
  GP.LABEL("Port:");
  GP.NUMBER("smtpPort", "smtpPort", config.data.smtpPort);
  GP.BREAK();
  GP.LABEL("Sender Email:");
  GP.TEXT("smtpEmail", "smtpEmail", config.data.smtpEmail, "", 48);
  GP.BREAK();
  GP.LABEL("SMTP Password:");
  GP.PASS("smtpPass", "smtpPass", config.data.smtpPass, "", 48);
  GP.BREAK();
  GP.LABEL("Recipient Email:");
  GP.TEXT("smtpRecipient", "smtpRecipient", config.data.smtpRecipient, "", 48);
  GP.BLOCK_END();
  
  GP.BLOCK_BEGIN(GP_THIN, "", "Report Schedule");
  GP.LABEL("Time (HH:MM):");
  GP.NUMBER("reportHour", "reportHour", config.data.reportHour);
  GP.LABEL(" : ");
  GP.NUMBER("reportMinute", "reportMinute", config.data.reportMinute);
  GP.BREAK();
  GP.LABEL("Repeat:");
  GP.SELECT("reportSchedule", "Daily,Weekly,Monthly", config.data.reportSchedule);
  GP.BREAK();
  GP.LABEL("Day:");
  GP.SELECT("reportDay", "Sunday,Monday,Tuesday,Wednesday,Thursday,Friday,Saturday", config.data.reportDay);
  GP.LABEL(" / ");
  GP.NUMBER("reportDayNum", "reportDayNum", config.data.reportDay);
  GP.BLOCK_END();
  
  GP.BLOCK_BEGIN(GP_THIN, "", "Meter Readings");
  GP.LABEL("Hot m3:");
  GP.NUMBER_F("meterHotM3", "meterHotM3", config.data.meterHotM3, 3);
  GP.BREAK();
  GP.LABEL("cube/pulse (Hot):");
  GP.NUMBER_F("litersPerPulseHot", "litersPerPulseHot", config.data.litersPerPulseHot, 3);
  GP.BREAK();
  GP.LABEL("Cold m3:");
  GP.NUMBER_F("meterColdM3", "meterColdM3", config.data.meterColdM3, 3);
  GP.BREAK();
  GP.LABEL("cube/pulse (Cold):");
  GP.NUMBER_F("litersPerPulseCold", "litersPerPulseCold", config.data.litersPerPulseCold, 3);
  GP.BLOCK_END();
  
  GP.BLOCK_BEGIN(GP_THIN, "", "");
  GP.SUBMIT("Save && Reboot");
  GP.BREAK();
  GP.BUTTON_LINK("/settings?testemail=1", "Test Email");
  GP.BREAK();
  GP.BUTTON_LINK("/", "Back");
  GP.BLOCK_END();
  
  GP.FORM_END();
  GP.BUILD_END();
}

void actionSettings(GyverPortal& p) {
  // Отладка
  String uri = p.uri();
  Serial.printf("[Settings] uri='%s' form=%d args=%d\n", uri.c_str(), p.form(), p.args());
  
  if (p.form("/settings")) {
    String ssid = p.getString("wifiSSID");
    String pass = p.getString("wifiPass");
    Serial.printf("[Settings] SSID='%s' Pass='%s' (%d chars)\n", ssid.c_str(), pass.c_str(), pass.length());
    strlcpy(config.data.wifiSSID, ssid.c_str(), sizeof(config.data.wifiSSID));
    strlcpy(config.data.wifiPass, pass.c_str(), sizeof(config.data.wifiPass));
    
    String host = p.getString("smtpHost");
    String email = p.getString("smtpEmail");
    String smtpPass = p.getString("smtpPass");
    String recipient = p.getString("smtpRecipient");
    uint16_t port = p.getInt("smtpPort");
    Serial.printf("[Settings] SMTP host='%s' port=%d email='%s'\n", host.c_str(), port, email.c_str());
    
    strlcpy(config.data.smtpHost, host.c_str(), sizeof(config.data.smtpHost));
    config.data.smtpPort = port;
    strlcpy(config.data.smtpEmail, email.c_str(), sizeof(config.data.smtpEmail));
    strlcpy(config.data.smtpPass, smtpPass.c_str(), sizeof(config.data.smtpPass));
    strlcpy(config.data.smtpRecipient, recipient.c_str(), sizeof(config.data.smtpRecipient));
    
    // Расписание отчёта
    config.data.reportHour = (uint8_t)constrain(p.getInt("reportHour"), 0, 23);
    config.data.reportMinute = (uint8_t)constrain(p.getInt("reportMinute"), 0, 59);
    config.data.reportSchedule = (uint8_t)p.getInt("reportSchedule");
    if (config.data.reportSchedule == 2) {  // monthly — число месяца
      int dn = p.getInt("reportDayNum");
      if (dn >= 1 && dn <= 31) config.data.reportDay = (uint8_t)dn;
    } else {  // daily / weekly — день недели
      config.data.reportDay = (uint8_t)p.getInt("reportDay");
    }
    Serial.printf("[Settings] Report: %02d:%02d schedule=%d day=%d\n",
      config.data.reportHour, config.data.reportMinute,
      config.data.reportSchedule, config.data.reportDay);
    
    // Показания счётчиков
    config.data.meterHotM3 = p.getFloat("meterHotM3");
    config.data.meterColdM3 = p.getFloat("meterColdM3");
    config.data.litersPerPulseHot = p.getFloat("litersPerPulseHot");
    config.data.litersPerPulseCold = p.getFloat("litersPerPulseCold");
    if (config.data.litersPerPulseHot <= 0) config.data.litersPerPulseHot = 1.0;
    if (config.data.litersPerPulseCold <= 0) config.data.litersPerPulseCold = 1.0;
    Serial.printf("[Settings] Meters: Hot=%.3f (%.3f L/pulse) Cold=%.3f (%.3f L/pulse)\n",
      config.data.meterHotM3, config.data.litersPerPulseHot,
      config.data.meterColdM3, config.data.litersPerPulseCold);
    
    config.save();
    needsRestart = true;
  }
}

// ==================== GyverPortal: Calibrate Page ====================
void buildCalibrate(GyverPortal& p) {
  // Обновляем список датчиков на шине
  tempSensors.refreshAddrList();
  
  // Обработка GET-параметров (приходят от BUTTON_LINK)
  String calAction = p.getString("cal");
  if (calAction == "start") {
    int idx = p.getInt("idx");
    if (idx >= 0 && idx < NUM_SENSORS && !tempSensors.isCalibrating()) {
      tempSensors.startCalibration(idx, onCalibrateDone);
    }
  } else if (calAction == "cancel") {
    tempSensors.cancelCalibration();
    calibrateLastResult = false;
    calibrateFeedbackTime = millis();
  }
  
  // ========== Частичное обновление через GP_update ==========
  // Если это запрос GP_update — возвращаем только содержимое запрошенных блоков
  if (p.update("busBlock")) {
    String html;
    for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
      const uint8_t* addr = tempSensors.getAllAddr(i);
      if (addr) {
        String shortAddr = String(addr[6], HEX) + String(addr[7], HEX);
        html += "#" + String(i) + " .." + shortAddr + " " + TemperatureSensors::formatTemp(tempSensors.getRawTemp(i)) + "C<br>";
      }
    }
    p.answer(html);
    return;
  }
  if (p.update("mapBlock")) {
    String html;
    for (int i = 0; i < NUM_SENSORS; i++) {
      html += String(tempSensors.sensorName(i)) + ": ";
      if (tempSensors.isFound(i)) {
        html += "<font color='#00ff00'>" + TemperatureSensors::formatTemp(tempSensors.getTemp(i)) + "C</font><br>";
      } else {
        html += "<font color='#ff0000'>---</font><br>";
      }
    }
    p.answer(html);
    return;
  }
  if (p.update("calBlock")) {
    String html;
    if (tempSensors.isCalibrating()) {
      int idx = tempSensors.getCalibrateIndex();
      html += "Heat until +" + String(CALIBRATE_THRESHOLD, 1) + "C  Timeout:60s<br>";
      for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
        float delta = tempSensors.getCalibrateDelta(i);
        html += "#" + String(i) + ": " + TemperatureSensors::formatTemp(tempSensors.getRawTemp(i)) + "C";
        if (delta >= CALIBRATE_THRESHOLD) {
          html += " <font color='#00ff00'>DONE!</font>";
        } else if (delta > 0) {
          html += " <font color='#ffff00'>+" + String(delta, 1) + "C</font>";
        }
        html += "<br>";
      }
      html += "<input type='button' value='Cancel' onclick=\"location.href='/calibrate?cal=cancel';\">";
    } else {
      html += "1. Click a sensor<br>2. Heat +" + String(CALIBRATE_THRESHOLD, 1) + "C<br>3. Wait<br>";
      for (int i = 0; i < NUM_SENSORS; i++) {
        String label = String(tempSensors.sensorName(i));
        if (tempSensors.isFound(i)) {
          label += " .." + tempSensors.getAddrSuffix(i);
        } else {
          label += " [---]";
        }
        html += "<input type='button' value='" + label + "' onclick=\"location.href='/calibrate?cal=start&idx=" + String(i) + "';\"><br>";
      }
    }
    p.answer(html);
    return;
  }
  
  // ========== Полная отрисовка страницы ==========
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("Calibrate", deviceName);
  GP.TITLE("Calibrate Sensors");
  
  // Блок датчиков на шине — всё внутри одного label с id=busBlock
  GP.BLOCK_BEGIN(GP_THIN, "", "Bus: " + String(tempSensors.getDeviceCount()) + " sensors");
  {
    String busHtml;
    for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
      const uint8_t* addr = tempSensors.getAllAddr(i);
      if (addr) {
        String shortAddr = String(addr[6], HEX) + String(addr[7], HEX);
        if (i > 0) busHtml += "<br>";
        busHtml += "#" + String(i) + " .." + shortAddr + " " + TemperatureSensors::formatTemp(tempSensors.getRawTemp(i)) + "C";
      }
    }
    GP.LABEL(busHtml, "busBlock");
  }
  GP.BLOCK_END();
  
  // Блок маппинга — всё внутри одного label с id=mapBlock
  GP.BLOCK_BEGIN(GP_THIN, "", "Mapping");
  {
    String mapHtml;
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (i > 0) mapHtml += "<br>";
      mapHtml += String(tempSensors.sensorName(i)) + ": ";
      if (tempSensors.isFound(i)) {
        mapHtml += "<font color='#00ff00'>" + TemperatureSensors::formatTemp(tempSensors.getTemp(i)) + "C</font>";
      } else {
        mapHtml += "<font color='#ff0000'>---</font>";
      }
    }
    GP.LABEL(mapHtml, "mapBlock");
  }
  GP.BLOCK_END();
  
  // Блок калибровки — всё внутри одного label с id=calBlock
  GP.BLOCK_BEGIN(GP_THIN, "", tempSensors.isCalibrating() ? 
    "Calibration: " + String(tempSensors.sensorName(tempSensors.getCalibrateIndex())) : 
    "Start Calibration");
  {
    String calHtml;
    if (tempSensors.isCalibrating()) {
      int idx = tempSensors.getCalibrateIndex();
      calHtml += "Heat until +" + String(CALIBRATE_THRESHOLD, 1) + "C  Timeout:60s";
      for (uint8_t i = 0; i < tempSensors.getAllAddrCount(); i++) {
        float delta = tempSensors.getCalibrateDelta(i);
        calHtml += "<br>#" + String(i) + ": " + TemperatureSensors::formatTemp(tempSensors.getRawTemp(i)) + "C";
        if (delta >= CALIBRATE_THRESHOLD) {
          calHtml += " <font color='#00ff00'>DONE!</font>";
        } else if (delta > 0) {
          calHtml += " <font color='#ffff00'>+" + String(delta, 1) + "C</font>";
        }
      }
      calHtml += "<br><input type='button' value='Cancel' onclick=\"location.href='/calibrate?cal=cancel';\">";
    } else {
      if (calibrateFeedbackTime > 0 && millis() - calibrateFeedbackTime < 5000) {
        calHtml += (calibrateLastResult ? "<font color='#00ff00'>OK</font>" : "<font color='#ff0000'>FAIL</font>") + String("<br>");
      }
      calHtml += "1. Click a sensor<br>2. Heat +" + String(CALIBRATE_THRESHOLD, 1) + "C<br>3. Wait";
      for (int i = 0; i < NUM_SENSORS; i++) {
        String label = String(tempSensors.sensorName(i));
        if (tempSensors.isFound(i)) {
          label += " .." + tempSensors.getAddrSuffix(i);
        } else {
          label += " [---]";
        }
        calHtml += "<br><input type='button' value='" + label + "' onclick=\"location.href='/calibrate?cal=start&idx=" + String(i) + "';\">";
      }
    }
    GP.LABEL(calHtml, "calBlock");
  }
  GP.BLOCK_END();
  
  GP.BUTTON_LINK("/", "Back");
  // Частичное обновление каждые 3 сек (без перезагрузки всей страницы)
  GP.UPDATE("busBlock,mapBlock,calBlock", 3000);
  GP.BUILD_END();
}

// Коллбэк по окончании калибровки
void onCalibrateDone(int sensorIndex, bool success) {
  calibrateLastResult = success;
  calibrateFeedbackTime = millis();
  if (success) {
    Serial.printf("[CALIBRATE] Sensor %s calibrated successfully!\n", 
      TemperatureSensors::sensorName(sensorIndex));
  }
}

// ==================== SMTP ====================
void trySendEmail(const String &subject, const String &body) {
  if (strlen(config.data.smtpEmail) == 0 || strlen(config.data.smtpRecipient) == 0) {
    Serial.println("[SMTP] Email not configured, skipping");
    return;
  }
  
  Serial.printf("[SMTP] Sending email to %s...\n", config.data.smtpRecipient);
  
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
  message.addRecipient(F("User"), config.data.smtpRecipient);
  message.text.content = body.c_str();
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;
  
  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.printf("[SMTP] Send error: %d %s\n",
      smtp.statusCode(), smtp.errorReason().c_str());
  } else {
    Serial.println("[SMTP] Email sent successfully");
  }
}

void sendDailyReport() {
  String body = String(deviceName) + " - Daily Report\n";
  body += "================================\n\n";
  body += "Date: " + String(valDate.encodeDMY()) + "\n";
  body += "Time: " + String(valTime.encode()) + "\n\n";
  
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

// ==================== Prometheus /metrics ====================
void handleMetrics(GyverPortal& p) {
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

  p.server.send(200, "text/plain; charset=utf-8", body);
}

// ==================== JSON API (Home Assistant) ====================
void handleApiJson(GyverPortal& p) {
  String json = "{";
  json += "\"device\":\"" + String(deviceName) + "\",";
  json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"wifi\":\"" + String(wifiConnected ? "connected" : "disconnected") + "\",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"time_valid\":" + String(timeValid ? "true" : "false") + ",";

  // Температуры
  json += "\"temperatures\":{";
  json += "\"cold\":" + String(tempSensors.getTempHVS(), 1) + ",";
  json += "\"hot\":" + String(tempSensors.getTempGVS(), 1) + ",";
  json += "\"supply\":" + String(tempSensors.getTempSupply(), 1) + ",";
  json += "\"return\":" + String(tempSensors.getTempReturn(), 1);
  json += "},";

  // Счётчики воды
  json += "\"meters\":{";
  json += "\"hot_m3\":" + String(config.data.meterHotM3, 3) + ",";
  json += "\"cold_m3\":" + String(config.data.meterColdM3, 3);
  json += "},";

  // Калибровка
  json += "\"calibrating\":" + String(tempSensors.isCalibrating() ? "true" : "false");

  json += "}\n";

  p.server.send(200, "application/json", json);
}

// Вызывается из TelnetSerial при команде "confirm"
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


