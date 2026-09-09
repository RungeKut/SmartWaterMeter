# SmartWaterMeter — Сводка по состоянию проекта

## Текущая архитектура

### Веб-сервер
- **ESPAsyncWebServer** + **WebSocket** — асинхронный, не блокирует `loop()`
- **LittleFS** — SPA-фронтенд лежит в файловой системе (не в прошивке)
- **ArduinoJson 7.4.3** — структурированный обмен данными через WebSocket

### SPA-фронтенд (data/index.html)

Одна HTML-страница с тремя вкладками:

| Вкладка | Функции |
|---|---|
| **Dashboard** | Температуры (4 датчика), показания счётчиков (ГВС/ХВС), статус системы (WiFi, IP, uptime, heap, время) |
| **Settings** | WiFi, SMTP (email), расписание отчётов, начальные показания счётчиков, коэффициенты |
| **Calibrate** | Калибровка датчиков DS18B20: выбор датчика, нагрев, автоматическое определение |

- Material Design стили + Dark Mode
- WebSocket auto-reconnect
- Данные с датчиков обновляются каждые 5 секунд
- OTA-баннер с обратным отсчётом (FailsafeOTA — 5 мин на подтверждение)

### Сборка (актуальная)

| Параметр | Значение |
|---|---|
| RAM | 52.5% (43032 / 81920 байт) |
| Flash | 62.4% (651484 / 1044464 байт) |
| Платформа | ESP8266 (Wemos D1 mini), 80MHz |
| Статус | `pio run` — SUCCESS, 0 ошибок, 0 warning |

### Зависимости (lib_deps)

| Библиотека | Версия |
|---|---|
| paulstoffregen/OneWire | 2.3.8 |
| milesburton/DallasTemperature | 4.0.6 |
| NTPClient | 3.2.1 |
| me-no-dev/ESPAsyncTCP | 2.0.0 |
| me-no-dev/ESPAsyncWebServer | 3.6.0 |
| bblanchon/ArduinoJson | 7.4.3 |

GyverPortal удалён из сборки (в `lib_ignore`).

---

## История изменений

### [1] Миграция с GyverPortal на ESPAsyncWebServer + WebSocket + LittleFS
- Полная замена веб-стека: синхронный GyverPortal → асинхронный ESPAsyncWebServer
- SPA-фронтенд на Material Design вместо серверного HTML
- WebSocket для real-time обновлений
- `loop()` переписан под асинхронный цикл DS18B20 (двухфазный: startConversion → 850ms → readTemperatures)

### [2] Исправление рекурсии в TemperatureSensors.h
- `readTemperatures()` вызывал `requestTemperatures()`, который вызывал `readTemperatures()` → бесконечный цикл
- Убран вызов `readTemperatures()` из `requestTemperatures()`, теперь это блокирующая обёртка

### [3] Исправление порождающего watchdog-reset бага (09.09.2026)
**Симптомы**: плата зависала при старте — LED горел постоянно, мусор на COM, ни WiFi, ни AP, циклический watchdog reset.

**Корневая причина** — два коммита (f44f264 + bc54809) внесли:
1. **`sensorInterval` 5000 → 1000 мс** — каждый 1 секунду запускалась 750ms конверсия DS18B20. При 12-bit точности это не оставляло времени loop() на обслуживание WiFi/WebServer.
2. **`rescanBus()` + `startConversion()` в фазе чтения** — каждые 30 секунд после `readTemperatures()` вызывался `rescanBus()` (дёргает `_oneWire.reset_search()`), а затем немедленно `startConversion()` + `lastSensorConv = now`. Это срывало асинхронный протокол OneWire: следующий `readTemperatures()` через 850ms попадал на незавершённую конвертацию → зависание в `DallasTemperature::getTempC()` → **ESP8266 watchdog reset** → циклическая перезагрузка.

**Также**: `flush()` в MeterCounter вызывал `saveMeters()` (запись в EEPROM) при **каждом** импульсе — износ EEPROM.

**Исправление** (commit `c932ac9`):
- `sensorInterval` восстановлен до 5000 мс (1000 мс при калибровке)
- Убран `rescanBus()` из `loop()` — теперь только при старте калибровки
- Убран повторный `startConversion()` после рескана
- `MeterCounter::flush()` больше не пишет в EEPROM — возвращает `bool`, сохранение отложено на 5-минутный цикл
- `EEPROM.saveMeters()` и `save()` вызываются вместе раз в 5 минут
- IP точки доступа: `192.168.0.1/24`

---

## Инструкция по прошивке

### Первая прошивка (USB)

```bash
# Сборка
pio run

# Заливка прошивки
pio run --target upload --upload-port COM3

# Заливка файловой системы (LittleFS) с SPA фронтендом
pio run --target uploadfs --upload-port COM3
```

### OTA-обновление

```bash
# Собрать и залить через WiFi
pio run --target upload --upload-port 192.168.x.x

# Файловую систему тоже можно по WiFi
pio run --target uploadfs --upload-port 192.168.x.x
```

### Первый запуск

1. Подключитесь к точке доступа `SmartWaterMeter-XXXXXX` (открытая)
2. Откройте `http://192.168.0.1`
3. В настройках укажите SSID и пароль WiFi
4. Настройте SMTP при необходимости
5. Откалибруйте датчики температуры