# Wemos_Mini.ino — Главный цикл

## Назначение

Главный файл прошивки: `setup()`, `loop()`, WebSocket-обработчики, HTTP-маршруты, SMTP.

## setup()

Порядок инициализации:

1. Serial (115200), LED (BOOTING)
2. Формирование deviceName по MAC: `SmartWaterMeter-XXXXXX`
3. ConfigStore::begin()
4. TemperatureSensors::begin()
5. Проверка всех датчиков → LED_SENSOR_ERR если не все найдены
6. MeterCounter::begin() для ГВС и ХВС
7. NTPClient::begin()
8. connectToWiFi() / startAPMode()
9. ArduinoOTA, Telnet, FailsafeOTA, LittleFS
10. WebSocket + HTTP настройка, server.begin()
11. Power-On email (если настроен SMTP)

## loop()

Основные задачи:

| Задача | Период | Описание |
|--------|--------|----------|
| LED tick | каждый цикл | Обновление LED |
| ArduinoOTA | каждый цикл | OTA |
| Telnet.handle | каждый цикл | Обслуживание клиентов |
| FailsafeOTA.handle | каждый цикл | Проверка таймаута |
| WiFiupd | каждый цикл | WiFi/AP поддержание |
| updateLocalTime | каждый цикл | NTP время |
| LED mode | каждый цикл | Выбор режима по состоянию |
| DS18B20 фаза 1 | 2000ms (1000ms калибровка) | startConversion |
| DS18B20 фаза 2 | через ≥850ms | readTemperatures + broadcast |
| rescanBusLight | каждые 30с | Обновление списка шины |
| MeterCounter::flush | каждый цикл | Сбор импульсов |
| EEPROM save | каждые 5 мин | Сохранение счётчиков |
| Email | по расписанию | Отчёт |

## WebSocket-обработчик (`handleWsMessage`)

| Тип | Описание |
|-----|----------|
| `getFullState` | Полное состояние клиенту |
| `saveConfig` | Сохранение + перезагрузка |
| `restart` | Перезагрузка |
| `confirmOta` | Подтверждение OTA |
| `testEmail` | Тестовый email |
| `startCalibration` | Запуск калибровки |
| `cancelCalibration` | Отмена калибровки |

## HTTP-маршруты

| Маршрут | Описание |
|---------|----------|
| `/` (static) | SPA из LittleFS |
| `/api.json` | JSON: температуры, счётчики, статус |
| `/metrics` | Prometheus-метрики |

## WiFi: реконнект

- Попытки каждые 30 секунд
- После 3 неудач → AP-режим
- В AP-режиме попытка реконнекта каждые 10 минут
- При успехе → выход из AP

## SMTP

Отправка email через `ESP_Mail_Client`. Используется для:
- Power-On уведомления
- Ежедневных/еженедельных/ежемесячных отчётов
- Тестовых писем из веб-интерфейса