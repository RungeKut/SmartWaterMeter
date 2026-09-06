# SmartWaterMeter Olimp

Умный контроллер учёта воды и тепла на базе ESP8266 (Wemos D1 mini).

Асинхронный веб-интерфейс на ESPAsyncWebServer + WebSocket, SPA-фронтенд из LittleFS, JSON API.

**Возможности:**
- 4 датчика температуры DS18B20 (ХВС, ГВС, подача, обратка) — асинхронный цикл конверсии
- 2 импульсных счётчика воды (ГВС и ХВС) с прерываниями по CHANGE и антидребезгом
- SPA веб-интерфейс (Material Design) через WebSocket — Dashboard + Settings + Calibrate
- SMTP-отправка показаний по расписанию (email) + тестовая отправка
- JSON API для Home Assistant / Prometheus
- Telnet-доступ к логам по WiFi
- OTA-обновление с A/B слотами и автоматическим откатом
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

### 3. Сборка и прошивка

```bash
# Первая прошивка (прошивка + файловая система)
pio run --target upload --upload-port COM3
pio run --target uploadfs --upload-port COM3
```

### 4. Первоначальная настройка WiFi

1. Подключитесь к точке доступа `SmartWaterMeter-XXXXXX` (пароля нет)
2. Откройте http://192.168.4.1 в браузере
3. Перейдите на вкладку **Settings**, введите SSID и пароль вашей WiFi
4. Нажмите **Save & Reboot**

После перезагрузки устройство подключится к вашей сети. IP можно узнать через Telnet или в роутере.

## Веб-интерфейс

Открывайте в браузере http://<IP-устройства>/. SPA на Vanilla JS с Material Design.

### Dashboard
- Температуры: Cold, Hot, Supply, Return (обновление каждую секунду)
- Показания счётчиков воды (ГВС / ХВС в м³)
- Системная информация: WiFi, RSSI, IP, uptime, свободная память

### Settings
- **WiFi** — SSID и пароль
- **SMTP** — хост, порт (465), email отправителя, пароль приложения, получатель
- **Расписание отчётов** — время (HH:MM), ежедневно / еженедельно / ежемесячно
- **Счётчики** — текущие показания и литров на импульс
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
  "time_valid": true,
  "temperatures": {
    "cold": 22.5, "hot": 55.3,
    "supply": 60.1, "return": 45.2
  },
  "meters": {
    "hot_m3": 103.000, "cold_m3": 127.000
  },
  "calibrating": false
}
```

### Home Assistant (RESTful sensor)

```yaml
sensor:
  - platform: rest
    name: "Water Meter Hot"
    resource: http://192.168.88.89/api.json
    value_template: "{{ value_json.meters.hot_m3 }}"
    unit_of_measurement: "m3"
  - platform: rest
    name: "Water Temperature Hot"
    resource: http://192.168.88.89/api.json
    value_template: "{{ value_json.temperatures.hot }}"
    unit_of_measurement: "°C"
```

## OTA-обновление

Прошивка с A/B слотами (каждый по 1MB) — автоматический откат при сбое.

```bash
# OTA-прошивка
pio run --target upload --upload-port 192.168.x.x

# Предварительно раскомментировать в platformio.ini:
# upload_protocol = espota
# upload_port = 192.168.x.x
```

После OTA на веб-интерфейсе появляется предупреждение. Нажмите **Confirm** в течение 5 минут, иначе автоматический откат.

Подтверждение через Telnet:
```
telnet 192.168.88.89
> confirm
```

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
  platformio.ini              # Конфигурация сборки (ESPAsyncWebServer, LittleFS, A/B)
  upload_script.py            # Двойная заливка A/B слотов при USB-прошивке
  data/
    index.html                # SPA-фронтенд (Vanilla JS, WebSocket)
  src/
    Wemos_Mini.ino            # Главный скетч (setup, loop, WebSocket, HTTP)
    secrets.h                 # Пароли WiFi/SMTP, адреса датчиков (в .gitignore)
    secrets.h.example         # Пример конфигурации
    ConfigStore.h             # EEPROM: сохранение/загрузка конфига
    MeterCounter.h            # Импульсные счётчики (прерывания CHANGE + debounce)
    TemperatureSensors.h      # DS18B20: асинхронный цикл, калибровка, карта шины
    StatusLED.h               # Светодиодная индикация (WiFi, ошибки датчиков)
    TelnetSerial.h            # Логи по WiFi (TCP:23)
    FailsafeOTA.h             # Безопасное OTA с A/B слотами и откатом
  .gitignore
  LICENSE
  README.md
```

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
- После первого USB-клннекта `upload_script.py` автоматически заливает прошивку в оба слота (A/B)
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