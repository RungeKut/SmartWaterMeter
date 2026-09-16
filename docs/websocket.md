# SmartWaterMeter — Поток данных WebSocket

## Формат сообщений

Все сообщения — JSON, кодировка UTF-8.

## Сообщения от сервера → клиент

### `fullState`

Отправляется при подключении клиента или по запросу `getFullState`.

```json
{
  "type": "fullState",
  "device": "SmartWaterMeter-93C195",
  "uptime_sec": 1234,
  "free_heap": 32000,
  "wifi": "connected",
  "wifi_rssi": -65,
  "ap_mode": false,
  "ap_ssid": "SmartWaterMeter-93C195",
  "ip": "192.168.88.87",
  "time_valid": true,
  "time_hour": 14,
  "time_min": 30,
  "time_sec": 5,
  "time_year": 2026,
  "time_mon": 9,
  "time_mday": 9,
  "temperatures": {
    "cold": 22.5,
    "hot": 45.1,
    "return": 38.2,
    "supply": 52.0
  },
  "meters": {
    "hot_m3": 123.456,
    "cold_m3": 789.012
  },
  "calibrating": false,
  "calibrate_index": -1,
  "config": {
    "deviceName": "",
    "defaultDeviceName": "SmartWaterMeter-93C195",
    "wifiSSID": "MyWiFi",
    "smtpHost": "smtp.yandex.ru",
    "smtpPort": 465,
    "smtpEmail": "...",
    "smtpRecipient": "...",
    "reportHour": 9,
    "reportMinute": 0,
    "reportSchedule": 0,
    "reportDay": 0,
    "meterHotM3": 123.456,
    "meterColdM3": 789.012,
    "litersPerPulseHot": 1.0,
    "litersPerPulseCold": 1.0,
    "debounceClosedMs": 0,
    "debounceOpenMs": 0
  },
  "sensorMapping": [
    { "name": "Cold", "found": true, "temp": 22.5 },
    { "name": "Hot", "found": true, "temp": 45.1 },
    { "name": "Return", "found": true, "temp": 38.2 },
    { "name": "Supply", "found": true, "temp": 52.0 }
  ],
  "busDevices": [
    { "index": 0, "address": "287C7C3C000000ED", "temp": 22.5 },
    { "index": 1, "address": "28822F3E00000050", "temp": 45.1 }
  ],
  "ota_pending": false,
  "ota_remaining": 0
}
```

> **Пароли не передаются.** В `config` нет `wifiPass` и `smtpPass` — устройство их не отдаёт.

`fullState` дополнительно содержит `meterDiag` — диагностику герконов (по объекту на счётчик):

```json
"meterDiag": [
  { "name": "Hot", "closed": false, "stateAgeSec": 1234, "pulses": 17,
    "bounces": 2, "lastClosedMs": 120, "minClosedMs": 98, "maxClosedMs": 140,
    "lastOpenMs": 3400, "overflow": 0 }
]
```

В периодическое сообщение `sensors` эти поля не кладутся — они меняются медленно, а секундная рассылка должна оставаться компактной.

### `sensors`

Периодическое обновление — раз в секунду.

Содержит **те же поля, что и `fullState`, кроме `config`**: помимо датчиков шлётся системная телеметрия, иначе uptime, heap, время и IP на Dashboard замирали бы до переподключения.

```json
{
  "type": "sensors",
  "device": "SmartWaterMeter-93C195",
  "uptime_sec": 1234,
  "free_heap": 32000,
  "wifi": "connected",
  "wifi_rssi": -65,
  "ap_mode": false,
  "ap_ssid": "SmartWaterMeter-93C195",
  "ip": "192.168.88.87",
  "time_valid": true,
  "time_hour": 14, "time_min": 30, "time_sec": 5,
  "time_year": 2026, "time_mon": 9, "time_mday": 11,
  "ota_pending": false,
  "ota_remaining": 0,
  "temperatures": {
    "cold": 22.5, "hot": 45.1, "return": 38.2, "supply": 52.0
  },
  "meters": {
    "hot_m3": 123.456, "cold_m3": 789.012
  },
  "calibrating": false,
  "calibrate_index": -1,
  "calibrate_remaining": 0,
  "sensorMapping": [
    { "name": "Cold", "found": true, "assigned": true,
      "address": "287C7C3C000000ED", "temp": 22.5 }
  ],
  "busDevices": [
    { "index": 0, "address": "287C7C3C000000ED", "temp": 22.5 }
  ]
}
```

> **Примечание:** во время калибровки в каждый объект `busDevices[]` добавляются поля `baseTemp` и `delta`.

> В `sensorMapping` поле `address` присутствует, если за каналом закреплён осмысленный адрес (`assigned: true`) — **в том числе когда датчик не отвечает**. По этому адресу пользователь и находит нужный датчик, чтобы подключить его.

### `calibrationStarted`

```json
{ "type": "calibrationStarted", "index": 0 }
```

### `calibrationResult`

```json
{ "type": "calibrationResult", "success": true, "index": 0 }
```

### `calibrationCancelled`

```json
{ "type": "calibrationCancelled" }
```

### `saveConfigResult`

```json
{ "type": "saveConfigResult", "success": true, "message": "Config saved, restarting..." }
```

### `testEmailResult`

```json
{ "type": "testEmailResult", "success": true, "message": "Test email sent" }
```

### `confirmOtaResult`

```json
{ "type": "confirmOtaResult", "success": true }
```

## Сообщения от клиента → сервер

### `getFullState`

```json
{ "type": "getFullState" }
```

### `saveConfig`

Пустые `wifiPass` / `smtpPass` означают «оставить сохранённый пароль», а не «стереть». Поля `config` обязательны: сообщение без объекта `config` отбрасывается с ответом `success: false`.

`debounceClosedMs` / `debounceOpenMs` — пороги антидребезга герконов в миллисекундах; `0` означает «взять значения по умолчанию» (5 и 50 мс). `deviceName` пустой — вернуться к имени по MAC.

```json
{
  "type": "saveConfig",
  "config": {
    "wifiSSID": "MyWiFi",
    "deviceName": "kotelnaya-schetchik",
    "wifiPass": "password",
    "smtpHost": "smtp.yandex.ru",
    "smtpPort": 465,
    "smtpEmail": "...",
    "smtpPass": "...",
    "smtpRecipient": "...",
    "reportHour": 9,
    "reportMinute": 0,
    "reportSchedule": 0,
    "reportDay": 0,
    "meterHotM3": 123.456,
    "meterColdM3": 789.012,
    "litersPerPulseHot": 1.0,
    "litersPerPulseCold": 1.0,
    "debounceClosedMs": 0,
    "debounceOpenMs": 0
  }
}
```

### `restart`

```json
{ "type": "restart" }
```

### `confirmOta`

```json
{ "type": "confirmOta" }
```

### `testEmail`

```json
{ "type": "testEmail" }
```

### `startCalibration`

```json
{ "type": "startCalibration", "index": 0 }
```

### `cancelCalibration`

```json
{ "type": "cancelCalibration" }
```

### `assignSensor`

Ручное назначение датчика с шины на логический канал — альтернатива нагреву.

```json
{ "type": "assignSensor", "index": 0, "busIndex": 5 }
```

Ответ — `assignResult`:

```json
{ "type": "assignResult", "success": true, "message": "Return assigned, Cold released" }
```

Если выбранный датчик уже закреплён за другим каналом, тот освобождается, и это указано в `message`.

### Снимок состояния: три сообщения вместо одного

По подключению клиента (и по запросу `getFullState`) устройство шлёт **три** сообщения подряд:

| Тип | Что несёт |
|-----|-----------|
| `fullState` | система и датчики — то же, что и в секундном `sensors` |
| `configState` | объект `config` с настройками |
| `diagState` | массив `meterDiag` с диагностикой герконов |

Раньше это был один JSON примерно на 2.2 КБ, и его отправка роняла плату: одновременно требовались пул ArduinoJson, строка сериализации и буфер отправки — несколько килобайт крупными непрерывными кусками, которых на измельчённой куче не находилось. Подробности — в [pitfalls.md §29](pitfalls.md).

Клиент собирает состояние из кусков и к порядку не чувствителен: `configState`, пришедший раньше `fullState`, применяется нормально. Секундный `sensors` сливается через `Object.assign` и `config` не несёт, поэтому настройки не затирает.

### `filterEvent`

Приходит в момент переключения защиты фильтра, не дожидаясь очередной секундной рассылки: состояние клапана на Dashboard должно смениться сразу.

```json
{ "type": "filterEvent", "event": "valve closed",
  "closed": true, "sensorLost": false, "temp": 47.2 }
```

Поле `event` — одно из `valve closed`, `valve opened`, `cold sensor lost`, `cold sensor back`.

Текущее состояние фильтра приходит и в составе `sensors`/`fullState`:

```json
"filter": { "enabled": true, "closed": false, "sensorLost": false,
            "trips": 3, "closedSec": 4320, "tempOn": 35.0,
            "tempOff": 30.0, "maxTemp": 47.2 }
```

`maxTemp` равен `null`, пока за период не было ни одного достоверного измерения — отсутствие данных и ноль это разные вещи.

## Приём: сообщение может приехать кусками

ESPAsyncWebServer отдаёт данные так, как они пришли по TCP. Сегмент на ESP8266 — около 536 байт, поэтому сообщение длиннее приезжает несколькими вызовами `WS_EVT_DATA`, а очень длинное — ещё и несколькими кадрами с `opcode = CONTINUATION`.

Собирает их `src/WsRxBuffer.h`: фиксированный буфер на 1536 байт, привязка набора к идентификатору клиента, отказ (а не обрезка) при переполнении. Счётчик отброшенных кусков виден в `/metrics` как `ws_rx_drops_total`; ноль — норма, рост означает, что команды с веб-интерфейса до платы не доезжают.

> Прежний обработчик принимал сообщение, только если оно уложилось в один вызов. `saveConfig` вырос до ~700 байт — и настройки молча перестали сохраняться. Разбор в [pitfalls.md §30](pitfalls.md).

Перезагрузка после `saveConfig` отложена на 500 мс: ответ `saveConfigResult` ставится в очередь, и `ESP.restart()` в том же проходе `loop()` рвал соединение раньше, чем ответ уходил.

## Как работает WebSocket-соединение

1. Клиент подключается к `ws://<host>/ws`
2. При `onopen` клиент отправляет `getFullState`
3. Сервер отвечает `fullState` — вся текущая конфигурация
4. Раз в секунду сервер шлёт `sensors` — телеметрия, температуры, счётчики и карта шины
5. При разрыве — клиент автоматически переподключается каждые 3 секунды
6. При переподключении — снова запрашивает `fullState`

### Очередь клиента и разрывы

Очередь `AsyncWebSocketClient` на ESP8266 — 8 сообщений, и библиотека по умолчанию **закрывает соединение** при переполнении. Секундная телеметрия забивает её за считанные секунды, стоит плате уйти в долгую операцию или линку просесть; браузер переподключается и получает `fullState` — на странице настроек это выглядело как самопроизвольный сброс формы.

Поэтому телеметрия отправляется через `wsBroadcastTelemetry()`: при забитой очереди кадр просто не отправляется, следующий придёт через секунду. Плюс на `WS_EVT_CONNECT` выставляется `setCloseClientOnQueueFull(false)`. Ответы на команды (`saveConfigResult`, результаты калибровки) идут через `wsSendJson`/`wsBroadcastJson` и не теряются.