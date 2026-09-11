/******************************************************************
 * TemperatureSensors.h - DS18B20 temperature sensors (4 channels)
 *
 * Mapping: [0]=Cold, [1]=Hot, [2]=Return, [3]=Supply
 * Addresses: EEPROM (after calibration) -> secrets.h fallback
 *
 * Usage:
 *   tempSensors.begin()              // init + scan, setWaitForConversion(false)
 *   tempSensors.startConversion()    // ~2ms, конверсия идёт на шине ~750ms
 *   ...>=850ms спустя...
 *   tempSensors.readTemperatures()   // один проход по шине -> кеш _busTemps[]
 *
 * Все геттеры (getTemp, getRawTemp, дельты калибровки) читают кеш и
 * на шину не обращаются.
 *
 * Calibration: heat a sensor, temperature rise >5C triggers save.
 ******************************************************************/

#ifndef TemperatureSensors_h
#define TemperatureSensors_h

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Log.h"

#define NUM_SENSORS 4
#define MAX_BUS_DEVICES 16
#define CALIBRATE_THRESHOLD 5.0
#define CALIBRATE_TIMEOUT 300000   // 5 минут на нагрев датчика

// Сколько подряд неудачных чтений терпим, прежде чем показать N/D.
// Одиночный сбой CRC на шине OneWire — обычное дело (длинные провода,
// наводки, слабая подтяжка). Гасить из-за него показание не нужно:
// держим последнее валидное значение. Но и скрывать реально отключённый
// датчик нельзя, поэтому терпение ограничено.
#define MAX_READ_FAILURES 3

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
  float _calibrateBaseTemp[MAX_BUS_DEVICES];
  DeviceAddress _allAddrs[MAX_BUS_DEVICES];
  float _busTemps[MAX_BUS_DEVICES];   // кеш: заполняется раз за цикл
  uint8_t _busFails[MAX_BUS_DEVICES]; // подряд неудачных чтений
  uint8_t _allAddrsCount;
  uint32_t _calibrateStartTime;
  bool _calibrateBaseReady;
  CalibrateCallback _calibrateCb;
  class ConfigStore* _config;

public:
  TemperatureSensors(uint8_t oneWirePin, class ConfigStore* config = nullptr)
    : _oneWire(oneWirePin), _sensors(&_oneWire), _deviceCount(0),
      _calibrating(false), _calibrateIndex(-1),
      _allAddrsCount(0), _calibrateStartTime(0), _calibrateBaseReady(false),
      _calibrateCb(nullptr), _config(config) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      _temperatures[i] = DEVICE_DISCONNECTED_C;
      _found[i] = false;
    }
    for (int i = 0; i < MAX_BUS_DEVICES; i++) {
      _calibrateBaseTemp[i] = DEVICE_DISCONNECTED_C;
      _busTemps[i] = DEVICE_DISCONNECTED_C;
      _busFails[i] = 0;
    }
  }

  void begin() {
    _sensors.begin();

    // КРИТИЧНО для неблокирующего цикла.
    // По умолчанию waitForConversion = true, и requestTemperatures()
    // крутится в yield()-цикле до конца конверсии (~750 мс при 12 битах).
    // Тогда двухфазная схема теряет смысл: loop() всё равно стоит.
    // С false запуск конверсии стоит ~2 мс, а результат забирает
    // readTemperatures() через >=850 мс.
    _sensors.setWaitForConversion(false);
    _deviceCount = _sensors.getDeviceCount();
    Log.printf("[DS18B20] Found sensors: %d\n", _deviceCount);

    // Fill _allAddrs for calibration bus table
    _allAddrsCount = 0;
    for (uint8_t i = 0; i < _deviceCount && _allAddrsCount < MAX_BUS_DEVICES; i++) {
      DeviceAddress addr;
      if (_sensors.getAddress(addr, i)) {
        memcpy(_allAddrs[_allAddrsCount], addr, 8);
        _allAddrsCount++;
        Log.printf("[DS18B20] Bus[%d]: ", i);
        for (int j = 0; j < 8; j++) {
          Log.printf("%02X", addr[j]);
        }
        Log.println();
      }
    }

    loadExpectedAddrs();

    // Перебираем ВСЕ устройства на шине: искомый датчик может стоять
    // на любой позиции, а не только среди первых NUM_SENSORS.
    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      int mapped = findMapping(_allAddrs[i]);
      if (mapped >= 0) {
        Log.printf("[DS18B20] Bus[%d] -> %s\n", i, sensorName(mapped));
        _found[mapped] = true;
      }
    }

    for (int i = 0; i < NUM_SENSORS; i++) {
      if (!_found[i]) {
        Log.printf("[DS18B20] WARNING: %s not found!\n", sensorName(i));
      }
    }
  }

  // Lightweight bus rescan — only refreshes _allAddrs[] for calibration bus table,
  // does NOT touch _found[] or _expectedAddrs.
  // Uses DallasTemperature::getAddress() instead of raw OneWire to avoid
  // desynchronizing the DallasTemperature internal state (which caused all
  // sensors to return DEVICE_DISCONNECTED_C after rescan).
  void rescanBusLight() {
    // Состав шины меняется — старые значения кеша больше не соответствуют
    // индексам, сбрасываем до следующего чтения
    for (uint8_t i = 0; i < MAX_BUS_DEVICES; i++) {
      _busTemps[i] = DEVICE_DISCONNECTED_C;
      _busFails[i] = 0;
    }

    _allAddrsCount = 0;
    uint8_t count = _sensors.getDeviceCount();

    for (uint8_t i = 0; i < count && _allAddrsCount < MAX_BUS_DEVICES; i++) {
      DeviceAddress addr;
      if (_sensors.getAddress(addr, i)) {
        memcpy(_allAddrs[_allAddrsCount], addr, 8);
        _allAddrsCount++;
      }
    }
    _deviceCount = _allAddrsCount;

    Log.printf("[DS18B20] light rescan: %d devices on bus\n", _allAddrsCount);
  }

  // Start async temperature conversion (~750ms on 12-bit resolution)
  void startConversion() {
    _sensors.requestTemperatures();
  }

  // Read temperatures from previous async conversion
  // Читает шину РОВНО ОДИН раз за цикл и раскладывает результат по кешу.
  // Раньше каждое обращение к getRawTemp() лезло на шину заново, и при
  // 8 датчиках одна рассылка стоила ~90 мс блокировки (а во время
  // калибровки вдвое больше — дельты читали те же значения повторно).
  void readTemperatures() {
    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      float t = _sensors.getTempC(_allAddrs[i]);

      // Одна повторная попытка: сбой CRC на шине обычно разовый
      if (t == DEVICE_DISCONNECTED_C) {
        t = _sensors.getTempC(_allAddrs[i]);
      }

      if (t != DEVICE_DISCONNECTED_C) {
        _busTemps[i] = t;
        _busFails[i] = 0;
      } else if (_busFails[i] < MAX_READ_FAILURES) {
        // Держим последнее валидное значение — не мигаем N/D из-за помехи
        _busFails[i]++;
        if (_busFails[i] == MAX_READ_FAILURES) {
          Log.printf("[DS18B20] Bus[%d]: %d read failures in a row -> N/D\n",
            i, _busFails[i]);
          _busTemps[i] = DEVICE_DISCONNECTED_C;
        }
      } else {
        _busTemps[i] = DEVICE_DISCONNECTED_C;
      }
    }

    for (int i = 0; i < NUM_SENSORS; i++) {
      _temperatures[i] = DEVICE_DISCONNECTED_C;
      if (!_found[i]) continue;

      int busIdx = findBusIndex(_expectedAddrs[i]);
      if (busIdx >= 0) {
        _temperatures[i] = _busTemps[busIdx];      // из кеша, без обращения к шине
      } else {
        _temperatures[i] = _sensors.getTempC(_expectedAddrs[i]);
      }
    }

    if (_calibrating) {
      // Базовые температуры снимаем один раз — на первом чтении после
      // старта калибровки. Явный флаг вместо проверки _calibrateBaseTemp[0]:
      // если устройство 0 отключено, оно навсегда возвращает -127 и
      // базовые температуры переснимались бы каждый цикл.
      if (!_calibrateBaseReady) {
        for (uint8_t i = 0; i < _allAddrsCount; i++) {
          _calibrateBaseTemp[i] = _busTemps[i];
        }
        _calibrateBaseReady = true;
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

    // Актуализируем список шины ДО снятия базовых температур.
    // Порядок важен: если рескан произойдёт позже, _allAddrsCount
    // вырастет, а базовые температуры для новых индексов останутся
    // неинициализированными — в таблице появлялись мусорные дельты.
    rescanBusLight();

    if (_allAddrsCount == 0) {
      Log.println("[CALIBRATE] No sensors on bus!");
      if (cb) cb(sensorIndex, false);
      return;
    }

    _calibrating = true;
    _calibrateIndex = sensorIndex;
    _calibrateCb = cb;
    _calibrateStartTime = millis();
    _calibrateBaseReady = false;
    // Инициализируем ВЕСЬ массив, а не только _allAddrsCount элементов
    for (uint8_t i = 0; i < MAX_BUS_DEVICES; i++) {
      _calibrateBaseTemp[i] = DEVICE_DISCONNECTED_C;
    }

    Log.printf("[CALIBRATE] Started for %s. Heat by >%.1f C within %ds\n",
      sensorName(sensorIndex), CALIBRATE_THRESHOLD, CALIBRATE_TIMEOUT / 1000);
  }

  // Сколько секунд осталось до таймаута калибровки
  uint32_t getCalibrateRemainingSec() {
    if (!_calibrating) return 0;
    uint32_t elapsed = millis() - _calibrateStartTime;
    if (elapsed >= CALIBRATE_TIMEOUT) return 0;
    return (CALIBRATE_TIMEOUT - elapsed) / 1000;
  }

  // Ручное назначение датчика с шины на логический канал — альтернатива
  // нагреву. Нужна, когда датчик недоступен физически или нагреть его
  // на 5 градусов затруднительно.
  //
  // displacedChannel (если передан) получает индекс канала, с которого
  // снято дублирующее назначение, либо -1. Дубликаты недопустимы:
  // findMapping() вернул бы для одного адреса только младший канал, и
  // второй после перезагрузки молча стал бы Missing.
  bool assignSensor(int sensorIndex, uint8_t busIndex, int *displacedChannel = nullptr) {
    if (displacedChannel) *displacedChannel = -1;
    if (sensorIndex < 0 || sensorIndex >= NUM_SENSORS) return false;
    if (busIndex >= _allAddrsCount) return false;

    // Снимаем этот адрес с другого канала, если он там уже стоит
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (i == sensorIndex) continue;
      if (memcmp(_expectedAddrs[i], _allAddrs[busIndex], 8) == 0) {
        memset(_expectedAddrs[i], 0, 8);
        _found[i] = false;
        _temperatures[i] = DEVICE_DISCONNECTED_C;
        if (_config) {
          uint8_t zero[8] = {0};
          _config->setSensorAddr(i, zero);
        }
        if (displacedChannel) *displacedChannel = i;
        Log.printf("[CALIBRATE] %s released — same sensor moved to %s\n",
          sensorName(i), sensorName(sensorIndex));
      }
    }

    memcpy(_expectedAddrs[sensorIndex], _allAddrs[busIndex], 8);
    _found[sensorIndex] = true;
    _temperatures[sensorIndex] = _busTemps[busIndex];
    if (_config) _config->setSensorAddr(sensorIndex, _allAddrs[busIndex]);

    Log.printf("[CALIBRATE] Bus[%d] assigned to %s manually\n",
      busIndex, sensorName(sensorIndex));

    // Если для этого канала шла калибровка — завершаем её успехом
    if (_calibrating && _calibrateIndex == sensorIndex) {
      _calibrating = false;
      _calibrateBaseReady = false;
      CalibrateCallback cb = _calibrateCb;
      _calibrateCb = nullptr;
      _calibrateIndex = -1;
      if (cb) cb(sensorIndex, true);
    }
    return true;
  }

  void cancelCalibration() {
    if (!_calibrating) return;
    _calibrating = false;
    _calibrateBaseReady = false;
    Log.println("[CALIBRATE] Cancelled");
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

  void refreshAddrList() { rescanBusLight(); }

  static String formatTemp(float temp) {
    if (isnan(temp) || temp < -50.0f) return "N/D";
    return String(temp, 1);
  }

  float getRawTemp(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    return _busTemps[busIndex];   // кеш, заполняется в readTemperatures()
  }

  float getCalibrateBaseTemp(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    return _calibrateBaseTemp[busIndex];
  }

  float getCalibrateDelta(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount || !_calibrateBaseReady) return 0;
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
        Log.printf("[DS18B20] [%d] EEPROM: ", i);
      } else {
        memcpy(_expectedAddrs[i], SENSOR_ADDR[i], 8);
        Log.printf("[DS18B20] [%d] secrets: ", i);
      }
      for (int j = 0; j < 8; j++) {
        Log.printf("%02X", _expectedAddrs[i][j]);
      }
      Log.println();
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
        Log.printf("[DS18B20] [%d] EEPROM addr not found, trying secrets.h...\n", i);
        memcpy(_expectedAddrs[i], SENSOR_ADDR[i], 8);
        // Remap with new address
        for (uint8_t di = 0; di < _allAddrsCount; di++) {
          if (memcmp(_allAddrs[di], _expectedAddrs[i], 8) == 0) {
            _found[i] = true;
            Log.printf("[DS18B20] [%d] Found with secrets.h address!\n", i);
            break;
          }
        }
        if (!_found[i]) {
          Log.printf("[DS18B20] [%d] Still not found with secrets.h either\n", i);
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
      Log.println("[DS18B20] EEPROM addresses updated from secrets.h");
    }
  }

  void checkCalibration() {
    uint32_t elapsed = millis() - _calibrateStartTime;

    if (elapsed > CALIBRATE_TIMEOUT) {
      Log.println("[CALIBRATE] Timeout!");
      _calibrating = false;
      if (_calibrateCb) _calibrateCb(_calibrateIndex, false);
      _calibrateCb = nullptr;
      _calibrateIndex = -1;
      return;
    }

    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      float currentTemp = _busTemps[i];
      float base = _calibrateBaseTemp[i];
      if (base == DEVICE_DISCONNECTED_C || currentTemp == DEVICE_DISCONNECTED_C) continue;
      float delta = currentTemp - base;

      if (delta >= CALIBRATE_THRESHOLD) {
        Log.printf("[CALIBRATE] Sensor %d heated by %.1f C -> %s\n",
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

  int findBusIndex(const uint8_t* addr) {
    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      if (memcmp(addr, _allAddrs[i], 8) == 0) return i;
    }
    return -1;
  }

  int findMapping(DeviceAddress addr) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (memcmp(addr, _expectedAddrs[i], 8) == 0) return i;
    }
    return -1;
  }
};

#endif