# SmartWaterMeter — Сводка по миграции архитектуры

## Что было сделано

### 1. Миграция с GyverPortal на ESPAsyncWebServer + WebSocket + LittleFS

**Было:**
- GyverPortal — синхронный веб-сервер с серверным рендерингом HTML
- Каждое обновление данных — полная перезагрузка страницы
- GyverPortal AJAX — тяжёлый, синхронный, не поддерживает real-time

**Стало:**
- ESPAsyncWebServer — асинхронный, не блокирует `loop()`
- WebSocket — реальном времени: данные приходят на страницу сразу
- LittleFS — фронтенд лежит в файловой системе (не в прошивке)
- ArduinoJson — для структурированного обмена данными

### 2. SPA-фронтенд (data/index.html)

Одна HTML-страница с тремя вкладками:

| Вкладка | Функции |
|---|---|
| **Dashboard** | Температуры (4 датчика), показания счётчиков (ГВС/ХВС), статус системы (WiFi, IP, uptime, heap, время) |
| **Settings** | WiFi, SMTP (email), расписание отчётов, начальные показания счётчиков, коэффициенты |
| **Calibrate** | Калибровка датчиков DS18B20: выбор датчика, нагрев, автоматическое определение |

- Material Design стили
- Dark Mode (через `prefers-color-scheme`)
- WebSocket auto-reconnect
- OTA-баннер с обратным отсчётом

### 3. Исправленные баги

#### TemperatureSensors.h
1. **Рекурсия**: `readTemperatures()` вызывал `requestTemperatures()`, который вызывал `readTemperatures()` → бесконечный цикл. Убрал вызов `readTemperatures()` из `requestTemperatures()`, теперь это блокирующая обёртка: `startConversion()` + `delay(750)` + `readTemperatures()`.
2. **rescanBus() в каждом цикле**: вызывался каждые 5 секунд в `loop()`, убивая шину OneWire. Теперь `rescanBus()` вызывается только при старте калибровки.
3. **Порядок в конструкторе**: не совпадал с порядком объявления полей → warning. Исправлен.

#### Wemos_Mini.ino (loop)
4. **Порядок чтения датчиков**: было `readTemperatures()` → `startConversion()` → `rescanBus()`. Правильно: `startConversion()` в одной итерации, `readTemperatures()` через 850мс в следующей. Теперь двухфазный цикл с флагом `sensorConvPending`.

## Сборка

- **RAM**: 52.1% (42680 / 81920 байт)
- **Flash**: 62.2% (649956 / 1044464 байт)
- Платформа: ESP8266 (Wemos D1 mini), 80MHz
- **pio run**: SUCCESS, 0 ошибок, 0 warning

## Файловая структура (изменённые файлы)

```
SmartWaterMeter/
  platformio.ini              — обновлён (ESPAsyncTCP, ESPAsyncWebServer, ArduinoJson)
  src/
    Wemos_Mini.ino            — переписан (ESPAsyncWebServer + WebSocket)
    TemperatureSensors.h      — исправлены баги
    ConfigStore.h             — без изменений
    MeterCounter.h            — без изменений
    StatusLED.h               — без изменений
    TelnetSerial.h            — без изменений
    FailsafeOTA.h             — без изменений
    secrets.h.example         — без изменений
  data/
    index.html                — НОВЫЙ (SPA фронтенд)
  CHAT_SUMMARY.md             — НОВЫЙ
```

## Зависимости (lib_deps)

| Библиотека | Версия |
|---|---|
| paulstoffregen/OneWire | 2.3.8 |
| milesburton/DallasTemperature | 4.0.6 |
| NTPClient | 3.2.1 |
| me-no-dev/ESPAsyncTCP | 2.0.0 |
| me-no-dev/ESPAsyncWebServer | 3.6.0 |
| bblanchon/ArduinoJson | 7.4.3 |

Старый GyverPortal удалён из сборки (в `lib_ignore`).

## Инструкция по прошивке

### Первая прошивка (USB)

```bash
# Сборка
pio run

# Заливка прошивки в оба слота (A/B)
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
2. Откройте `http://192.168.4.1`
3. В настройках укажите SSID и пароль WiFi
4. Настройте SMTP при необходимости
5. Откалибруйте датчики температуры