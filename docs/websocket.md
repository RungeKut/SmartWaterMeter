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
    "litersPerPulseCold": 1.0
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

### `sensors`

Периодическое обновление (каждые ~2 секунды, при калибровке ~1 секунда).

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
  "sensorMapping": [
    { "name": "Cold", "found": true, "temp": 22.5 }
  ],
  "busDevices": [
    { "index": 0, "address": "287C7C3C000000ED", "temp": 22.5 }
  ]
}
```

> **Примечание:** во время калибровки в каждый объект `busDevices[]` добавляются поля `baseTemp` и `delta`.

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

```json
{
  "type": "saveConfig",
  "config": {
    "wifiSSID": "MyWiFi",
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
    "litersPerPulseCold": 1.0
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

## Как работает WebSocket-соединение

1. Клиент подключается к `ws://<host>/ws`
2. При `onopen` клиент отправляет `getFullState`
3. Сервер отвечает `fullState` — вся текущая конфигурация
4. Каждые ~2 секунды сервер шлёт `sensors` — телеметрия, температуры, счётчики и карта шины
5. При разрыве — клиент автоматически переподключается каждые 3 секунды
6. При переподключении — снова запрашивает `fullState`