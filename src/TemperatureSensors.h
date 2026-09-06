/******************************************************************
 * TemperatureSensors.h - DS18B20 temperature sensors (4 channels)
 *
 * Mapping: [0]=Cold, [1]=Hot, [2]=Return, [3]=Supply
 * Addresses: EEPROM (after calibration) -> secrets.h fallback
 *
 * Usage:
 *   tempSensors.begin()              // init + scan
 *   tempSensors.startConversion()    // async, ~750ms
 *   ...later...
 *   tempSensors.readTemperatures()   // read results
 *
 * Calibration: heat a sensor, temperature rise >5C triggers save.
 ******************************************************************/

#ifndef TemperatureSensors_h
#define TemperatureSensors_h

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define NUM_SENSORS 4
#define CALIBRATE_THRESHOLD 5.0
#define CALIBRATE_TIMEOUT 60000

typedef void (*CalibrateCallback)(int sensorIndex, bool success);

class TemperatureSensors {
private:
  OneWire _oneWire;
  DallasTemperature _sensors;
  DeviceAddress _expectedAddrs[NUM_SENSORS];
  float _temperatures[NUM_SENSORS];
  bool _found[NUM_SENSORS];
  uint8_t _deviceCount;

  bool _calibrating;
  int _calibrateIndex;
  float _calibrateBaseTemp[NUM_SENSORS];
  DeviceAddress _allAddrs[16];
  uint8_t _allAddrsCount;
  uint32_t _calibrateStartTime;
  CalibrateCallback _calibrateCb;
  class ConfigStore* _config;

public:
  TemperatureSensors(uint8_t oneWirePin, class ConfigStore* config = nullptr)
    : _oneWire(oneWirePin), _sensors(&_oneWire), _deviceCount(0),
      _calibrating(false), _calibrateIndex(-1),
      _allAddrsCount(0), _calibrateStartTime(0),
      _calibrateCb(nullptr), _config(config) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      _temperatures[i] = DEVICE_DISCONNECTED_C;
      _found[i] = false;
    }
  }

  void begin() {
    _sensors.begin();
    _deviceCount = _sensors.getDeviceCount();
    Serial.printf("[DS18B20] Found sensors: %d\n", _deviceCount);

    // Print all sensor addresses on the bus
    for (uint8_t i = 0; i < _deviceCount && i < 16; i++) {
      DeviceAddress addr;
      if (_sensors.getAddress(addr, i)) {
        Serial.printf("[DS18B20] Bus[%d]: ", i);
        for (int j = 0; j < 8; j++) {
          Serial.printf("%02X", addr[j]);
        }
        Serial.println();
      }
    }

    loadExpectedAddrs();

    for (uint8_t i = 0; i < _deviceCount && i < NUM_SENSORS; i++) {
      DeviceAddress addr;
      if (_sensors.getAddress(addr, i)) {
        int mapped = findMapping(addr);
        if (mapped >= 0) {
          Serial.printf("[DS18B20] Sensor %d -> %s\n", i, sensorName(mapped));
          _found[mapped] = true;
        }
      }
    }

    for (int i = 0; i < NUM_SENSORS; i++) {
      if (!_found[i]) {
        Serial.printf("[DS18B20] WARNING: %s not found!\n", sensorName(i));
      }
    }
  }

  // Fast bus rescan (~5-10ms, does NOT re-init DallasTemperature)
  void rescanBus() {
    _allAddrsCount = 0;
    _deviceCount = 0;
    for (int i = 0; i < NUM_SENSORS; i++) _found[i] = false;

    _oneWire.reset_search();
    DeviceAddress addr;
    while (_oneWire.search(addr) && _allAddrsCount < 16) {
      if (OneWire::crc8(addr, 7) == addr[7]) {
        memcpy(_allAddrs[_allAddrsCount], addr, 8);
        int mapped = findMapping(addr);
        if (mapped >= 0) _found[mapped] = true;
        _allAddrsCount++;
      }
    }
    _deviceCount = _allAddrsCount;
  }

  // Start async temperature conversion (~750ms on 12-bit resolution)
  void startConversion() {
    _sensors.requestTemperatures();
  }

  // Read temperatures from previous async conversion
  void readTemperatures() {
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (_found[i]) {
        _temperatures[i] = _sensors.getTempC(_expectedAddrs[i]);
      } else {
        _temperatures[i] = DEVICE_DISCONNECTED_C;
      }
    }

    if (_calibrating) {
      if (_calibrateBaseTemp[0] == DEVICE_DISCONNECTED_C) {
        for (uint8_t i = 0; i < _allAddrsCount; i++) {
          _calibrateBaseTemp[i] = _sensors.getTempC(_allAddrs[i]);
        }
      }
      checkCalibration();
    }
  }

  // Blocking: conversion + 750ms wait + read
  void requestTemperatures() {
    _sensors.requestTemperatures();
    delay(750);
    readTemperatures();
  }

  float getTemp(int index) {
    if (index >= 0 && index < NUM_SENSORS) return _temperatures[index];
    return DEVICE_DISCONNECTED_C;
  }

  float getTempHVS()   { return _temperatures[0]; }
  float getTempGVS()   { return _temperatures[1]; }
  float getTempReturn(){ return _temperatures[2]; }
  float getTempSupply(){ return _temperatures[3]; }

  bool isFound(int index) {
    return index >= 0 && index < NUM_SENSORS && _found[index];
  }

  uint8_t getDeviceCount() { return _deviceCount; }
  bool isCalibrating() { return _calibrating; }
  int getCalibrateIndex() { return _calibrateIndex; }

  void startCalibration(int sensorIndex, CalibrateCallback cb = nullptr) {
    if (sensorIndex < 0 || sensorIndex >= NUM_SENSORS) return;
    if (_calibrating) return;

    _allAddrsCount = 0;
    for (uint8_t i = 0; i < _deviceCount && i < 16; i++) {
      if (_sensors.getAddress(_allAddrs[_allAddrsCount], i)) {
        _allAddrsCount++;
      }
    }

    if (_allAddrsCount == 0) {
      Serial.println("[CALIBRATE] No sensors on bus!");
      if (cb) cb(sensorIndex, false);
      return;
    }

    _calibrating = true;
    _calibrateIndex = sensorIndex;
    _calibrateCb = cb;
    _calibrateStartTime = millis();
    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      _calibrateBaseTemp[i] = DEVICE_DISCONNECTED_C;
    }

    Serial.printf("[CALIBRATE] Started for %s. Heat by >%.1f C within %ds\n",
      sensorName(sensorIndex), CALIBRATE_THRESHOLD, CALIBRATE_TIMEOUT / 1000);
  }

  void cancelCalibration() {
    if (!_calibrating) return;
    _calibrating = false;
    Serial.println("[CALIBRATE] Cancelled");
    if (_calibrateCb) _calibrateCb(_calibrateIndex, false);
    _calibrateCb = nullptr;
    _calibrateIndex = -1;
  }

  static const char* sensorName(int index) {
    switch (index) {
      case 0: return "Cold";
      case 1: return "Hot";
      case 2: return "Return";
      case 3: return "Supply";
      default: return "?";
    }
  }

  uint8_t getAllAddrCount() { return _allAddrsCount; }
  const uint8_t* getAllAddr(uint8_t i) { return i < _allAddrsCount ? _allAddrs[i] : nullptr; }

  String getAddrSuffix(int sensorIndex) {
    if (sensorIndex < 0 || sensorIndex >= NUM_SENSORS || !_found[sensorIndex]) return "---";
    String s;
    for (int i = 6; i < 8; i++) {
      if (_expectedAddrs[sensorIndex][i] < 16) s += "0";
      s += String(_expectedAddrs[sensorIndex][i], HEX);
    }
    return s;
  }

  void refreshAddrList() { rescanBus(); }

  static String formatTemp(float temp) {
    if (isnan(temp) || temp < -50.0f) return "N/D";
    return String(temp, 1);
  }

  float getRawTemp(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    return _sensors.getTempC(_allAddrs[busIndex]);
  }

  float getCalibrateBaseTemp(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    return _calibrateBaseTemp[busIndex];
  }

  float getCalibrateDelta(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    float current = getRawTemp(busIndex);
    float base = _calibrateBaseTemp[busIndex];
    if (current == DEVICE_DISCONNECTED_C || base == DEVICE_DISCONNECTED_C) return 0;
    return current - base;
  }

private:
  void loadExpectedAddrs() {
    bool eepromChanged = false;
    for (int i = 0; i < NUM_SENSORS; i++) {
      bool useEeprom = false;
      if (_config) {
        useEeprom = true;
        for (int j = 0; j < 8; j++) {
          if (_config->data.sensorAddrs[i][j] != 0) break;
          if (j == 7) useEeprom = false;
        }
      }
      if (useEeprom) {
        memcpy(_expectedAddrs[i], _config->data.sensorAddrs[i], 8);
        Serial.printf("[DS18B20] [%d] EEPROM: ", i);
      } else {
        memcpy(_expectedAddrs[i], SENSOR_ADDR[i], 8);
        Serial.printf("[DS18B20] [%d] secrets: ", i);
      }
      for (int j = 0; j < 8; j++) {
        Serial.printf("%02X", _expectedAddrs[i][j]);
      }
      Serial.println();
    }

    // After initial mapping, re-check each EEPROM address individually:
    // if EEPROM address was used but sensor not found, try secrets.h fallback
    for (int i = 0; i < NUM_SENSORS; i++) {
      bool isFromEeprom = false;
      if (_config) {
        isFromEeprom = true;
        for (int j = 0; j < 8; j++) {
          if (_config->data.sensorAddrs[i][j] != 0) break;
          if (j == 7) isFromEeprom = false;
        }
      }
      if (isFromEeprom && !_found[i]) {
        // EEPROM address didn't match — try secrets.h
        Serial.printf("[DS18B20] [%d] EEPROM addr not found, trying secrets.h...\n", i);
        memcpy(_expectedAddrs[i], SENSOR_ADDR[i], 8);
        // Remap with new address
        for (uint8_t di = 0; di < _deviceCount && di < NUM_SENSORS; di++) {
          DeviceAddress addr;
          if (_sensors.getAddress(addr, di)) {
            if (memcmp(addr, _expectedAddrs[i], 8) == 0) {
              _found[i] = true;
              Serial.printf("[DS18B20] [%d] Found with secrets.h address!\n", i);
              break;
            }
          }
        }
        if (!_found[i]) {
          Serial.printf("[DS18B20] [%d] Still not found with secrets.h either\n", i);
        }
        eepromChanged = true;
      }
    }

    if (eepromChanged && _config) {
      // Save updated addresses back to EEPROM
      for (int i = 0; i < NUM_SENSORS; i++) {
        memcpy(_config->data.sensorAddrs[i], _expectedAddrs[i], 8);
      }
      _config->save();
      Serial.println("[DS18B20] EEPROM addresses updated from secrets.h");
    }
  }

  void checkCalibration() {
    uint32_t elapsed = millis() - _calibrateStartTime;

    if (elapsed > CALIBRATE_TIMEOUT) {
      Serial.println("[CALIBRATE] Timeout!");
      _calibrating = false;
      if (_calibrateCb) _calibrateCb(_calibrateIndex, false);
      _calibrateCb = nullptr;
      _calibrateIndex = -1;
      return;
    }

    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      float currentTemp = _sensors.getTempC(_allAddrs[i]);
      float base = _calibrateBaseTemp[i];
      float delta = (base == DEVICE_DISCONNECTED_C) ? 0 : currentTemp - base;

      if (delta >= CALIBRATE_THRESHOLD) {
        Serial.printf("[CALIBRATE] Sensor %d heated by %.1f C -> %s\n",
          i, delta, sensorName(_calibrateIndex));

        memcpy(_expectedAddrs[_calibrateIndex], _allAddrs[i], 8);
        _found[_calibrateIndex] = true;

        if (_config) _config->setSensorAddr(_calibrateIndex, _allAddrs[i]);

        _calibrating = false;
        if (_calibrateCb) _calibrateCb(_calibrateIndex, true);
        _calibrateCb = nullptr;
        _calibrateIndex = -1;
        return;
      }
    }
  }

  int findMapping(DeviceAddress addr) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (memcmp(addr, _expectedAddrs[i], 8) == 0) return i;
    }
    return -1;
  }
};

#endif