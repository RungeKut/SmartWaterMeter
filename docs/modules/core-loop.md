# Wemos_Mini.ino — Главный цикл

## Назначение

Главный файл прошивки: `setup()`, `loop()`, WebSocket-обработчики, HTTP-маршруты, SMTP.

## setup()

Порядок инициализации:

1. Serial (115200), LED (BOOTING)
1a. `Log.setSink(&telnet)` — логи начинают дублироваться в Telnet
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
| DS18B20 фаза 1 | 1000ms | startConversion (~2 мс, неблокирующий) |
| DS18B20 фаза 2 | через ≥850ms | readTemperatures + рассылка `sensors` через `wsBroadcastTelemetry()` |
| rescanBusLight | каждые 30с | Обновление списка шины |
| MeterCounter::process | каждый цикл | Разбор событий геркона |
| EEPROM счётчиков | через 30 с после импульса | `config.save()` |
| EEPROM save | каждые 5 мин | `config.save()` — счётчики и конфигурация одной записью |
| Email | по расписанию | Отчёт |

## Формирование JSON (helpers)

Два помощника собирают состояние, общие для `fullState` и периодического `sensors`:

| Функция | Что кладёт в документ |
|---------|----------------------|
| `fillSystemState(doc)` | device, uptime_sec, free_heap, wifi, wifi_rssi, ap_mode, ap_ssid, **ip**, время, ota_pending, ota_remaining |
| `fillSensorState(doc)` | temperatures, meters, calibrating, calibrate_index, sensorMapping, busDevices |

Раньше эти блоки были продублированы в двух местах, причём `sensors` содержал только температуры, счётчики и шину. Из-за этого uptime, heap, время и IP на Dashboard замирали до переподключения WebSocket.

### Рассылка телеметрии

Секундный кадр `sensors` уходит не напрямую, а через `wsBroadcastTelemetry()`: он пропускает кадр, если очередь клиента заполнена. Очередь `AsyncWebSocketClient` на ESP8266 — 8 сообщений, и библиотека по умолчанию **закрывает соединение** при переполнении; браузер переподключается и получает `fullState`, что на вкладке Settings выглядело как самопроизвольный сброс формы. Пропущенный кадр телеметрии дешевле обрыва — следующий придёт через секунду.

Ответы на команды (`saveConfigResult`, результаты калибровки) идут через `wsSendJson`/`wsBroadcastJson` и не теряются. Подробности — в [pitfalls.md, раздел 27](../pitfalls.md).

Ещё один помощник — `keepOrSet(dst, src, size)`: записывает значение только если оно непустое. Применяется к паролям (см. раздел «Пароли» ниже).

## Пароли

Сервер **не отдаёт** `wifiPass` и `smtpPass` клиенту. Значит, в SPA поля паролей всегда пустые, пока пользователь их не заполнит.

Поэтому пустое значение в `saveConfig` трактуется как «оставить как было». Иначе сохранение любой настройки (например, времени отчёта) стирало бы оба пароля, и после перезагрузки устройство уходило бы в AP-режим.

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

Сообщение без поля `type` и `saveConfig` без объекта `config` отбрасываются с записью в лог. Раньше первое давало разыменование `nullptr` в `strcmp()`, второе — запись пустых строк во все поля конфигурации.

## HTTP-маршруты

| Маршрут | Описание |
|---------|----------|
| `/` (static) | SPA из LittleFS |
| `/api.json` | JSON: температуры, счётчики, статус |
| `/metrics` | Prometheus-метрики (см. README, раздел Prometheus) |

Отсутствующий датчик в `/api.json` отдаётся как `null`, а в `/metrics` не экспортируется вовсе — см. [pitfalls.md §21](../pitfalls.md).
| `/confirm` | Подтверждение прошивки после OTA (GET из браузера) |

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