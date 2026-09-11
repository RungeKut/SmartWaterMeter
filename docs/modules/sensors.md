# TemperatureSensors.h — Датчики DS18B20

## Назначение

Управление 4 датчиками DS18B20 на шине 1-Wire. Асинхронная конверсия, калибровка, сканирование шины.

## Поля

| Поле | Тип | Описание |
|------|-----|----------|
| `_expectedAddrs[4]` | `DeviceAddress[4]` | Ожидаемые адреса (из EEPROM или secrets.h) |
| `_temperatures[4]` | `float[4]` | Последние прочитанные температуры |
| `_found[4]` | `bool[4]` | Флаг «датчик найден на шине» |
| `_allAddrs[16]` | `DeviceAddress[16]` | Все устройства на шине (для таблицы калибровки) |
| `_allAddrsCount` | `uint8_t` | Количество устройств на шине |
| `_deviceCount` | `uint8_t` | Количество датчиков (из DallasTemperature) |
| `_calibrating` | `bool` | Флаг активной калибровки |
| `_calibrateIndex` | `int` | Индекс калибруемого датчика (0-3) |
| `_calibrateBaseTemp[16]` | `float[16]` | Базовые температуры при старте калибровки |
| `_calibrateStartTime` | `uint32_t` | Время старта калибровки (ms) |
| `_calibrateBaseReady` | `bool` | Базовые температуры сняты |
| `_config` | `ConfigStore*` | Для сохранения адресов после калибровки |

## Методы

| Метод | Описание |
|-------|----------|
| `begin()` | Инициализация, сканирование, загрузка адресов, заполнение `_allAddrs[]` |
| `rescanBusLight()` | Лёгкое пересканирование — обновляет только `_allAddrs[]` через DallasTemperature API. Не трогает `_found[]`. Вызывается каждые 30с. **Не использует прямой OneWire** (это ломало DallasTemperature в первой версии). |
| `startConversion()` | Асинхронный запуск конверсии (~10ms, запускает ~750ms на шине) |
| `readTemperatures()` | Чтение результатов конверсии (через ≥850ms после start) |
| `requestTemperatures()` | Блокирующая обёртка: start + delay(750) + read |
| `getTemp(index)` | Температура по индексу (0-3) |
| `isFound(index)` | Датчик найден? |
| `getTempHVS()` / `getTempGVS()` / `getTempReturn()` / `getTempSupply()` | Удобные методы для каждого канала |
| `getDeviceCount()` | Количество датчиков на шине |
| `startCalibration(index, cb)` | Запуск калибровки |
| `cancelCalibration()` | Отмена калибровки |
| `isCalibrating()` | Активна ли калибровка |
| `getAllAddrCount()` | Количество устройств в `_allAddrs[]` |
| `getAllAddr(i)` | Адрес устройства i на шине |
| `getRawTemp(busIndex)` | Сырая температура по индексу шины |
| `getCalibrateBaseTemp(busIndex)` | Базовая температура при калибровке |
| `getCalibrateDelta(busIndex)` | Дельта температуры |
| `getAddrSuffix(index)` | Последние 2 байта адреса (hex) |
| `refreshAddrList()` | Синоним `rescanBusLight()` |
| `sensorName(index)` | Статика: имя канала ("Cold", "Hot"...) |
| `formatTemp(temp)` | Статика: форматирование температуры |

## Процесс загрузки адресов (`loadExpectedAddrs()`)

1. EEPROM: если для датчика i записан ненулевой адрес — используется он
2. Fallback: `SENSOR_ADDR[i]` из `secrets.h`
3. Если EEPROM-адрес не найден на шине — пробуется `secrets.h`
4. При успешном fallback — новые адреса сохраняются в EEPROM

## Сопоставление адресов с шиной

Поиск идёт по **всем** устройствам на шине — `_allAddrsCount`, а не по первым `NUM_SENSORS`.

Это важно: раньше циклы сопоставления были ограничены `i < NUM_SENSORS`, то есть просматривались только первые 4 устройства. Если на шине висит больше датчиков (у проекта их бывает 8), а нужный стоит, например, шестым — он не находился никогда, и канал показывал «Missing» при физически исправном датчике.

Ограничение действовало в двух местах: в `begin()` и в fallback-поиске внутри `loadExpectedAddrs()`.

## Калибровка: порядок операций

```
startCalibration(index):
  1. rescanBusLight()            <- СНАЧАЛА обновляем список шины
  2. проверка _allAddrsCount > 0
  3. _calibrateBaseReady = false
  4. _calibrateBaseTemp[0..MAX_BUS_DEVICES] = DEVICE_DISCONNECTED_C

readTemperatures() (первый вызов после старта):
  5. если !_calibrateBaseReady — снимаем базовые температуры,
     _calibrateBaseReady = true
```

**Почему порядок такой.** Раньше `loop()` вызывал `startCalibration()`, а рескан шины делал *после* него. Базовые температуры инициализировались для старого `_allAddrsCount`; если рескан находил больше устройств, для новых индексов `_calibrateBaseTemp[]` оставался неинициализированным — в таблице Bus Sensors появлялись мусорные дельты вида `+536870900.0°` и `-1.5e+23°`.

Флаг `_calibrateBaseReady` заменил проверку `_calibrateBaseTemp[0] == DEVICE_DISCONNECTED_C`: если устройство с индексом 0 отключено, оно всегда возвращает `-127`, и базовые температуры переснимались бы на каждом цикле — дельта никогда не достигала бы порога.

## Асинхронный цикл

```
Фаза 1 (каждые 2000 мс, 1000 мс при калибровке):
  startConversion() → requestTemperatures() → ~750ms на шине

Фаза 2 (через ≥850 мс после Фазы 1):
  readTemperatures() → getTempC() для каждого found датчика
  → если калибровка: checkCalibration()
  → [каждые 30 с]: rescanBusLight()
```

**Важно:** Двухфазный подход гарантирует, что `loop()` не блокируется на 750ms, и WiFi/WebServer остаются отзывчивыми.