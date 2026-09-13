# SmartWaterMeter

Умный контроллер учёта воды и тепла на базе ESP8266 (Wemos D1 mini).

Асинхронный веб-интерфейс на ESPAsyncWebServer + WebSocket, SPA-фронтенд из LittleFS, JSON API.

**Возможности:**
- 4 датчика температуры DS18B20 (ХВС, ГВС, подача, обратка) — асинхронный цикл конверсии
- 2 импульсных счётчика воды (ГВС и ХВС) с прерываниями по CHANGE и антидребезгом
- SPA веб-интерфейс (Material Design) через WebSocket — Dashboard + Settings + Calibrate
- SMTP-отправка показаний по расписанию (email) + тестовая отправка
- JSON API для Home Assistant / Prometheus
- Telnet-доступ к логам прошивки по WiFi
- Настраиваемое имя устройства (hostname, mDNS, SSID точки доступа)
- MQTT с автодискавери Home Assistant (сущности появляются без YAML)
- OTA-обновление по WiFi с подтверждением прошивки
- Калибровка датчиков температуры через веб-интерфейс (нагрев >5°C)
- Карта шины OneWire: просмотр всех датчиков на шине с адресами и температурами

---

## Требуемое оборудование

| Компонент | Назначение |
|-----------|------------|
| Wemos D1 mini (ESP8266) | Контроллер |
| DS18B20 × 4 | Датчики температуры (ХВС, ГВС, подача, обратка) |
| Резистор 4.7 кОм | Подтяжка шины OneWire |
| Герконовые счётчики воды × 2 | Импульсные счётчики ГВС/ХВС |
| Блок питания 5V | Питание Wemos D1 mini |
| USB-кабель Micro-USB | Для первой прошивки и отладки |

## Схема подключения

```
Wemos D1 mini
┌──────────────────────┐
│ D3 (GPIO0) ── DS18B20 (DATA) ── 4.7кΩ ── +3.3V
│                      │
│ D5 (GPIO14) ── Геркон ГВС (к GND)
│ D6 (GPIO12) ── Геркон ХВС (к GND)
│ 5V ── БП 5V
│ GND ── GND
└──────────────────────┘
```

**Примечания:**
- DS18B20 подключаются параллельно (все DATA на D3). Каждый датчик имеет уникальный 64-битный адрес.
- Герконы подключаются между пином и GND. Внутренняя подтяжка INPUT_PULLUP включена программно.
- Если счётчик воды имеет встроенный геркон (обычно 2 провода), подключайте один провод к пину, второй к GND.

## Быстрый старт

### 1. Установка PlatformIO

```bash
pip install platformio
pio --version
```

### 2. Настройка secrets.h

Скопируйте `src/secrets.h.example` в `src/secrets.h` и отредактируйте:

```bash
cp src/secrets.h.example src/secrets.h
```

В secrets.h нужно указать:
- **AP_SSID_PREFIX** — префикс имени точки доступа
- **PIN_METER_HOT / PIN_METER_COLD** — пины герконов (D5=14, D6=12)
- **ONE_WIRE_BUS** — пин датчиков DS18B20 (D3=0)
- **SENSOR_ADDR[4]** — адреса датчиков DS18B20 (можно оставить заглушки и откалибровать через веб)
- **LITERS_PER_PULSE** — цена импульса счётчика **в литрах** (обычно 1.0)

### 3. Сборка и прошивка

```bash
# Первая прошивка (прошивка + файловая система)
pio run --target upload --upload-port COM3
pio run --target uploadfs --upload-port COM3
```

### 4. Первоначальная настройка WiFi

1. Подключитесь к точке доступа `SmartWaterMeter-XXXXXX` (пароля нет)
2. Откройте http://192.168.0.1 в браузере
3. Перейдите на вкладку **Settings**, введите SSID и пароль вашей WiFi
4. Нажмите **Save & Reboot**

После перезагрузки устройство подключится к вашей сети. IP можно узнать через Telnet или в роутере.

## Веб-интерфейс

Открывайте в браузере http://<IP-устройства>/. SPA на Vanilla JS с Material Design.

### Dashboard
- Температуры: Cold, Hot, Supply, Return (обновление раз в секунду)
- Показания счётчиков воды (ГВС / ХВС в м³)
- Системная информация: WiFi, RSSI, IP, uptime, свободная память (обновляется вместе с датчиками)

### Settings
- **Device name** — имя устройства: DHCP-hostname (видно на роутере), mDNS-имя (`http://<имя>.local`) и SSID точки доступа. Латиница, цифры, дефис; пустое поле — имя по MAC
- **WiFi** — SSID и пароль
- **SMTP** — хост, порт (465), email отправителя, пароль приложения, получатель
- **Расписание отчётов** — время (HH:MM), ежедневно / еженедельно / ежемесячно
- **Счётчики** — текущие показания (м³) и цена импульса в литрах
- **Test Email** — отправка тестового письма
- **Save & Reboot / Restart**

### Calibrate
- **Bus Sensors** — таблица всех датчиков DS18B20 на шине OneWire: номер, адрес (hex), температура, дельта (при калибровке). Обновляется каждую секунду.
- **Calibration** — сопоставление логических каналов (Cold, Hot, Return, Supply) с физическими датчиками. Выберите канал, нагрейте датчик >5°C — адрес сохранится в EEPROM.

## JSON API

```
GET http://<ip>/api.json
```

```json
{
  "device": "SmartWaterMeter-93C195",
  "uptime_sec": 12345,
  "free_heap": 21704,
  "wifi": "connected",
  "wifi_rssi": -65,
  "ip": "192.168.88.89",
  "time_valid": true,
  "temperatures": {
    "cold": 22.5, "hot": 55.3,
    "supply": 60.1, "return": null
  },
  "meters": {
    "hot_m3": 103.000, "cold_m3": 127.000
  },
  "calibrating": false
}
```

> Отсутствующий датчик отдаётся как `null`, а не как `-127`. `DEVICE_DISCONNECTED_C` — это признак «нет данных», а не измерение: иначе Home Assistant показывал бы -127 °C, а Prometheus клал бы эту точку в графики.

### Home Assistant

Один запрос к устройству на все значения — интеграция `rest` умеет отдавать несколько сенсоров из одного ответа. Вариант «по сенсору на запрос» создавал бы шесть HTTP-запросов к ESP каждый интервал.

```yaml
# configuration.yaml
rest:
  - resource: http://192.168.88.87/api.json
    scan_interval: 30
    sensor:
      - name: "Вода ГВС"
        unique_id: swm_water_hot
        value_template: "{{ value_json.meters.hot_m3 }}"
        unit_of_measurement: "m³"
        device_class: water
        state_class: total_increasing

      - name: "Вода ХВС"
        unique_id: swm_water_cold
        value_template: "{{ value_json.meters.cold_m3 }}"
        unit_of_measurement: "m³"
        device_class: water
        state_class: total_increasing

      - name: "Температура ХВС"
        unique_id: swm_temp_cold
        value_template: "{{ value_json.temperatures.cold }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
        availability: "{{ value_json.temperatures.cold is not none }}"

      - name: "Температура ГВС"
        unique_id: swm_temp_hot
        value_template: "{{ value_json.temperatures.hot }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
        availability: "{{ value_json.temperatures.hot is not none }}"

      - name: "Подача отопления"
        unique_id: swm_temp_supply
        value_template: "{{ value_json.temperatures.supply }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
        availability: "{{ value_json.temperatures.supply is not none }}"

      - name: "Обратка отопления"
        unique_id: swm_temp_return
        value_template: "{{ value_json.temperatures.return }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
        availability: "{{ value_json.temperatures.return is not none }}"
```

Что здесь важно:

- **`device_class: water` + `state_class: total_increasing`** — без них счётчики не попадут в панель «Вода» в Home Assistant. Именно эта пара делает из числа полноценный учётный счётчик с историей потребления.
- **`availability`** — отсутствующий датчик приходит как `null`, и сенсор корректно переходит в состояние «недоступен» вместо того, чтобы показывать мусор.
- **`unique_id`** — без него сенсор нельзя переименовать или привязать к устройству через интерфейс.
- **`scan_interval: 30`** — устройство обновляет данные раз в секунду, но опрашивать его чаще, чем раз в 15-30 секунд, незачем: на каждый запрос тратится память ESP.

### MQTT и автодискавери Home Assistant

Самый удобный способ подключить устройство к HA: сущности появляются автоматически, YAML не нужен.

1. Поставьте брокер — в HA это надстройка **Mosquitto broker** (Settings → Add-ons)
2. В веб-интерфейсе устройства: Settings → **MQTT / Home Assistant** — адрес брокера, порт, логин, пароль
3. Сохраните. После перезагрузки устройство появится в Settings → Devices со всеми сущностями

Публикуется 15 сущностей под одной карточкой: показания счётчиков (с `device_class: water` — попадают в панель «Вода»), четыре температуры, состояние герконов, диагностика и управление — кнопки перезагрузки и подтверждения прошивки, числовые поля порогов антидребезга.

Показания уходят в HA сразу при импульсе счётчика, остальное — раз в 5 секунд (настраивается).

Раздел `rest:` из `configuration.yaml` после этого можно удалить. Но `/api.json` и `/metrics` продолжают работать — они не зависят от брокера и удобны для отладки.

Подробности, структура тем и что именно проверено — в [docs/modules/mqtt.md](docs/modules/mqtt.md).

### Prometheus

```yaml
# prometheus.yml
scrape_configs:
  - job_name: smartwatermeter
    scrape_interval: 30s
    static_configs:
      - targets: ['192.168.88.87']
```

Экспортируемые метрики:

| Метрика | Тип | Описание |
|---------|-----|----------|
| `smartwatermeter_temperature_celsius{sensor}` | gauge | Температуры. Отсутствующий датчик **не экспортируется** |
| `smartwatermeter_water_m3_total{type}` | counter | Показания счётчиков, м³ |
| `smartwatermeter_sensor_present{sensor}` | gauge | 1 = датчик читается |
| `smartwatermeter_reed_closed{type}` | gauge | 1 = геркон сейчас замкнут |
| `smartwatermeter_uptime_seconds` | gauge | Аптайм |
| `smartwatermeter_free_heap_bytes` | gauge | Свободная память |
| `smartwatermeter_wifi_rssi_dbm` | gauge | Уровень сигнала (только в режиме клиента) |
| `smartwatermeter_calibrating` | gauge | 1 = идёт калибровка |

Метки `sensor` — `cold`, `hot`, `supply`, `return`; метки `type` — `hot`, `cold`.

Примеры запросов:

```promql
# расход горячей воды за сутки, м³
increase(smartwatermeter_water_m3_total{type="hot"}[24h])

# датчик пропал с шины дольше 5 минут
min_over_time(smartwatermeter_sensor_present[5m]) == 0

# дельта отопления
smartwatermeter_temperature_celsius{sensor="supply"}
  - on() smartwatermeter_temperature_celsius{sensor="return"}
```

Отсутствующий датчик не экспортируется вовсе — это штатный для Prometheus способ сказать «данных нет». Экспорт `-127` положил бы в график настоящую точку и испортил бы средние и алерты.

## OTA-обновление

Прошивка по WiFi через ArduinoOTA.

```bash
# OTA-прошивка (адрес задан в platformio.ini)
pio run --target upload

# Или явно
pio run --target upload --upload-port 192.168.x.x
```

После OTA в веб-интерфейсе появляется баннер с обратным отсчётом. Нажмите **Confirm** в течение 5 минут — иначе устройство один раз перезагрузится.

Подтверждение через Telnet:
```
telnet 192.168.88.89
> confirm
```

Или через браузер: `http://192.168.88.89/confirm`

> **Важно:** A/B-слотов и автоматического отката на ESP8266 нет. Загрузчик `eboot` при перезагрузке копирует новый образ поверх старого — возвращаться некуда. Подтверждение служит признаком того, что прошивка дожила до `loop()`. Если прошивка оказалась нерабочей, помогает только перепрошивка по USB. Подробности — в [docs/modules/ota.md](docs/modules/ota.md).

## Отладка

### USB-Serial

```bash
pio device monitor --port COM3 --baud 115200
```

### Telnet (по WiFi)

```bash
telnet 192.168.88.89
```

Команды: `help`, `status`, `reset`, `heap`, `uptime`, `confirm`.

## Структура проекта

```
SmartWaterMeter/
  platformio.ini              # Конфигурация сборки (ESPAsyncWebServer, LittleFS)
  data/
    index.html                # SPA-фронтенд (Vanilla JS, WebSocket)
  src/
    Wemos_Mini.ino            # Главный скетч (setup, loop, WebSocket, HTTP)
    Log.h                     # Единый вывод логов: USB-Serial + Telnet
    secrets.h                 # Пароли WiFi/SMTP, адреса датчиков (в .gitignore)
    secrets.h.example         # Пример конфигурации
    ConfigStore.h             # EEPROM: сохранение/загрузка конфига
    MeterCounter.h            # Импульсные счётчики (прерывания CHANGE + debounce)
    TemperatureSensors.h      # DS18B20: асинхронный цикл, калибровка, карта шины
    StatusLED.h               # Светодиодная индикация (WiFi, ошибки датчиков)
    TelnetSerial.h            # Логи по WiFi (TCP:23) + консоль команд
    MqttClient.h              # MQTT + автодискавери Home Assistant
    FailsafeOTA.h             # Подтверждение прошивки после OTA
  test/                       # нативные тесты C++ (pio test -e native)
    stubs/                    # заглушки Arduino/EEPROM/secrets
    test_meter/               # автомат подсчёта импульсов
  tests/                      # тесты SPA на Node (node tests/run.js)
    frontend/
  .gitignore
  LICENSE
  README.md
```

## Тесты

Два набора, оба гоняются на компьютере без платы:

```bash
pio test -e native     # логика прошивки (C++, Unity)
node tests/run.js      # логика SPA (JavaScript, без зависимостей)
```

Покрыты места, где дефекты были тихими: пересчёт литров в м³, автомат подсчёта импульсов геркона, расписание отчётов. Подробности и требования — в [docs/testing.md](docs/testing.md).

## Сборка

```bash
# Просто сборка
pio run

# Прошивка через USB
pio run --target upload --upload-port COM3

# Заливка файловой системы (LittleFS)
pio run --target uploadfs --upload-port COM3

# OTA
pio run --target upload --upload-port 192.168.88.87

# Монитор
pio device monitor --port COM3 --baud 115200
```

### Важно

- После изменения `data/index.html` нужно перезаливать файловую систему: `pio run --target uploadfs`
- Используйте **Ctrl+F5** в браузере для сброса кэша после обновления LittleFS

## Зависимости

Управляются через `lib_deps` в platformio.ini (скачиваются автоматически):

- `paulstoffregen/OneWire` — шина OneWire
- `milesburton/DallasTemperature` — датчики DS18B20
- `me-no-dev/ESPAsyncTCP` — асинхронный TCP
- `me-no-dev/ESPAsyncWebServer` — асинхронный веб-сервер + WebSocket
- `bblanchon/ArduinoJson` — JSON
- `NTPClient` — NTP-время
- `ESP_Mail_Client` — SMTP-клиент

Libraries removed (old architecture): GyverPortal, Arduino_ESP32_OTA, ArduinoOTA.

## Лицензия

MIT