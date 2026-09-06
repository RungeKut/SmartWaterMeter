/******************************************************************
 * TemperatureSensors.h - Работа с 4 датчиками DS18B20
 * 
 * Маппинг датчиков по адресам:
 *   [0] = ХВС, [1] = ГВС, [2] = Обратка, [3] = Подача
 * 
 * Адреса датчиков: сначала из EEPROM (после калибровки),
 * иначе из secrets.h (SENSOR_ADDR).
 * 
 * Режим калибровки: пользователь греет датчик, программа
 * отслеживает рост температуры и запоминает адрес.
 ******************************************************************/

#ifndef TemperatureSensors_h
#define TemperatureSensors_h

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define NUM_SENSORS 4
#define CALIBRATE_THRESHOLD 5.0   // порог нагрева для калибровки (градусов)
#define CALIBRATE_TIMEOUT 60000   // таймаут калибровки (мс)

class TemperatureSensors;

// Коллбэк для оповещения о завершении калибровки
typedef void (*CalibrateCallback)(int sensorIndex, bool success);

class TemperatureSensors {
private:
  OneWire _oneWire;
  DallasTemperature _sensors;
  DeviceAddress _expectedAddrs[NUM_SENSORS];
  float _temperatures[NUM_SENSORS];
  bool _found[NUM_SENSORS];
  uint8_t _deviceCount;
  
  // Калибровка
  bool _calibrating;
  int _calibrateIndex;           // какой датчик калибруем (0..3)
  float _calibrateBaseTemp[NUM_SENSORS];  // базовые температуры всех датчиков
  DeviceAddress _allAddrs[16];            // адреса всех обнаруженных датчиков
  uint8_t _allAddrsCount;
  uint32_t _calibrateStartTime;
  CalibrateCallback _calibrateCb;
  
  // Ссылка на ConfigStore для сохранения адресов
  class ConfigStore* _config;
  
public:
  TemperatureSensors(uint8_t oneWirePin, class ConfigStore* config = nullptr)
    : _oneWire(oneWirePin), _sensors(&_oneWire), _deviceCount(0),
      _calibrating(false), _calibrateIndex(-1), _calibrateCb(nullptr),
      _config(config), _allAddrsCount(0) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      _temperatures[i] = DEVICE_DISCONNECTED_C;
      _found[i] = false;
    }
  }
  
  void begin() {
    _sensors.begin();
    _deviceCount = _sensors.getDeviceCount();
    Serial.printf("[DS18B20] Found sensors: %d\n", _deviceCount);
    
    // Загружаем ожидаемые адреса
    loadExpectedAddrs();
    
    // Маппим обнаруженные датчики
    for (uint8_t i = 0; i < _deviceCount && i < NUM_SENSORS; i++) {
      DeviceAddress addr;
      if (_sensors.getAddress(addr, i)) {
        printAddress(addr);
        int mapped = findMapping(addr);
        if (mapped >= 0) {
          Serial.printf(" -> Index %d (%s)\n", mapped, sensorName(mapped));
          _found[mapped] = true;
        } else {
          Serial.println(" -> unknown sensor");
        }
      }
    }
    
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (!_found[i]) {
        Serial.printf("[DS18B20] WARNING: sensor %s not found!\n", sensorName(i));
      }
    }
  }
  
  // Быстрое пересканирование шины OneWire через прямой поиск (5-10ms вместо 100-200ms)
  void rescanBus() {
    _allAddrsCount = 0;
    _deviceCount = 0;
    
    // Сбрасываем _found для всех ожидаемых датчиков
    for (int i = 0; i < NUM_SENSORS; i++) {
      _found[i] = false;
    }
    
    // Прямой поиск на шине OneWire (быстрее чем DallasTemperature::begin)
    _oneWire.reset_search();
    DeviceAddress addr;
    while (_oneWire.search(addr) && _allAddrsCount < 16) {
      // Проверка CRC
      if (OneWire::crc8(addr, 7) == addr[7]) {
        memcpy(_allAddrs[_allAddrsCount], addr, 8);
        // Проверяем, не является ли этот адрес одним из ожидаемых
        int mapped = findMapping(addr);
        if (mapped >= 0) {
          _found[mapped] = true;
        }
        _allAddrsCount++;
      }
    }
    _deviceCount = _allAddrsCount;
    
    // Передаём найденные адреса в DallasTemperature для последующих запросов температур
    _sensors.begin();
    
    if (_deviceCount > 0) {
      Serial.printf("[DS18B20] Rescan: %d devices on bus\n", _deviceCount);
    }
  }
  
  // Отправить команду на измерение (асинхронно, не блокирует)
  void startConversion() {
    _sensors.requestTemperatures();
  }
  
  // Прочитать результаты предыдущего измерения (вызывать через ~750мс после startConversion)
  void readTemperatures() {
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (_found[i]) {
        _temperatures[i] = _sensors.getTempC(_expectedAddrs[i]);
      } else {
        _temperatures[i] = DEVICE_DISCONNECTED_C;
      }
    }
    
    // Если идёт калибровка
    if (_calibrating) {
      // Записываем базовые температуры при первом чтении после старта
      if (_calibrateBaseTemp[0] == DEVICE_DISCONNECTED_C) {
        for (uint8_t i = 0; i < _allAddrsCount; i++) {
          _calibrateBaseTemp[i] = _sensors.getTempC(_allAddrs[i]);
        }
      }
      checkCalibration();
    }
  }
  
  // Для обратной совместимости: отправляет запрос И читает (может вернуть старые данные)
  void requestTemperatures() {
    startConversion();
    readTemperatures();
  }
  
  float getTemp(int index) {
    if (index >= 0 && index < NUM_SENSORS) {
      return _temperatures[index];
    }
    return DEVICE_DISCONNECTED_C;
  }
  
  float getTempHVS()   { return _temperatures[0]; }  // ХВС
  float getTempGVS()   { return _temperatures[1]; }  // ГВС
  float getTempReturn(){ return _temperatures[2]; }  // Обратка
  float getTempSupply(){ return _temperatures[3]; }  // Подача
  
  bool isFound(int index) {
    return index >= 0 && index < NUM_SENSORS && _found[index];
  }
  
  uint8_t getDeviceCount() { return _deviceCount; }
  
  bool isCalibrating() { return _calibrating; }
  int getCalibrateIndex() { return _calibrateIndex; }
  
  // Начать калибровку указанного датчика (0=ХВС, 1=ГВС, 2=Обратка, 3=Подача)
  void startCalibration(int sensorIndex, CalibrateCallback cb = nullptr) {
    if (sensorIndex < 0 || sensorIndex >= NUM_SENSORS) return;
    if (_calibrating) return;
    
    // Получаем все адреса на шине
    _allAddrsCount = 0;
    for (uint8_t i = 0; i < _deviceCount && i < 16; i++) {
      if (_sensors.getAddress(_allAddrs[_allAddrsCount], i)) {
        _allAddrsCount++;
      }
    }
    
    if (_allAddrsCount == 0) {
      Serial.println("[CALIBRATE] No sensors found on bus!");
      if (cb) cb(sensorIndex, false);
      return;
    }
    
    _calibrating = true;
    _calibrateIndex = sensorIndex;
    _calibrateCb = cb;
    _calibrateStartTime = millis();
    
    // Базовые температуры будут записаны при первом readTemperatures() в loop()
    // Инициализируем как DISCONNECTED, чтобы не было ложных срабатываний
    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      _calibrateBaseTemp[i] = DEVICE_DISCONNECTED_C;
    }
    
    Serial.printf("[CALIBRATE] Started for %s. Threshold: %.1f C, Timeout: %d sec\n", 
      sensorName(sensorIndex), CALIBRATE_THRESHOLD, CALIBRATE_TIMEOUT / 1000);
  }
  
  // Отменить калибровку
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
  
  
  
  // Получить адреса всех датчиков на шине (для отображения)
  uint8_t getAllAddrCount() { return _allAddrsCount; }
  const uint8_t* getAllAddr(uint8_t i) { return i < _allAddrsCount ? _allAddrs[i] : nullptr; }
  
  // Последние 2 байта адреса датчика по индексу маппинга (0..3)
  String getAddrSuffix(int sensorIndex) {
    if (sensorIndex < 0 || sensorIndex >= NUM_SENSORS || !_found[sensorIndex]) return "---";
    String s;
    for (int i = 6; i < 8; i++) {
      if (_expectedAddrs[sensorIndex][i] < 16) s += "0";
      s += String(_expectedAddrs[sensorIndex][i], HEX);
    }
    return s;
  }
  
  // Обновить список всех датчиков на шине (теперь просто делает rescan)
  void refreshAddrList() {
    rescanBus();
  }
  
  // Форматировать температуру: "N/D" если нет данных, иначе "XX.X"
  static String formatTemp(float temp) {
    if (isnan(temp) || temp < -50.0f) return "N/D";
    return String(temp, 1);
  }
  
  // Получить "сырую" температуру датчика на шине по индексу (0.._allAddrsCount-1)
  float getRawTemp(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    return _sensors.getTempC(_allAddrs[busIndex]);
  }
  
  // Базовая температура для калибровки (по индексу на шине)
  float getCalibrateBaseTemp(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    return _calibrateBaseTemp[busIndex];
  }
  
  // Дельта нагрева для калибровки (по индексу на шине)
  float getCalibrateDelta(uint8_t busIndex) {
    if (busIndex >= _allAddrsCount) return DEVICE_DISCONNECTED_C;
    float current = getRawTemp(busIndex);
    float base = _calibrateBaseTemp[busIndex];
    if (current == DEVICE_DISCONNECTED_C || base == DEVICE_DISCONNECTED_C) return 0;
    return current - base;
  }
  
private:
  void loadExpectedAddrs() {
    for (int i = 0; i < NUM_SENSORS; i++) {
      // Сначала пробуем адрес из EEPROM (если он не нулевой)
      bool useEeprom = false;
      if (_config) {
        useEeprom = true;
        for (int j = 0; j < 8; j++) {
          if (_config->data.sensorAddrs[i][j] != 0) break;
          if (j == 7) useEeprom = false;  // все 8 байт нулевые
        }
      }
      if (useEeprom) {
        memcpy(_expectedAddrs[i], _config->data.sensorAddrs[i], 8);
      } else {
        memcpy(_expectedAddrs[i], SENSOR_ADDR[i], 8);
      }
    }
    Serial.println("[DS18B20] Addresses loaded (EEPROM where available, secrets.h otherwise)");
  }
  
  void checkCalibration() {
    uint32_t elapsed = millis() - _calibrateStartTime;
    Serial.printf("[CALIBRATE] check: elapsed=%lu addrs=%d base0=%.1f\n",
      elapsed, _allAddrsCount, _calibrateBaseTemp[0]);
    
    // Проверка таймаута
    if (elapsed > CALIBRATE_TIMEOUT) {
      Serial.println("[CALIBRATE] Timeout! No sensor heated.");
      _calibrating = false;
      if (_calibrateCb) _calibrateCb(_calibrateIndex, false);
      _calibrateCb = nullptr;
      _calibrateIndex = -1;
      return;
    }
    
    // Проверяем каждый датчик на шине
    for (uint8_t i = 0; i < _allAddrsCount; i++) {
      float currentTemp = _sensors.getTempC(_allAddrs[i]);
      float base = _calibrateBaseTemp[i];
      float delta = (base == DEVICE_DISCONNECTED_C) ? 0 : currentTemp - base;
      Serial.printf("[CALIBRATE]  sensor %d: current=%.1f base=%.1f delta=%.1f\n", i, currentTemp, base, delta);
      
      if (delta >= CALIBRATE_THRESHOLD) {
        // Нашли нагретый датчик!
        Serial.printf("[CALIBRATE] Detected! Sensor %d (", i);
        printAddress(_allAddrs[i]);
        Serial.printf(") heated by %.1f C -> assigned to %s\n", delta, sensorName(_calibrateIndex));
        
        // Сохраняем адрес
        memcpy(_expectedAddrs[_calibrateIndex], _allAddrs[i], 8);
        _found[_calibrateIndex] = true;
        
        if (_config) {
          _config->setSensorAddr(_calibrateIndex, _allAddrs[i]);
        }
        
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
      if (memcmp(addr, _expectedAddrs[i], 8) == 0) {
        return i;
      }
    }
    return -1;
  }
  
  void printAddress(DeviceAddress addr) {
    for (uint8_t i = 0; i < 8; i++) {
      if (addr[i] < 16) Serial.print("0");
      Serial.print(addr[i], HEX);
    }
  }
};

#endif