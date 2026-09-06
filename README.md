# SmartWaterMeter Olimp

Умный контроллер учёта воды и тепла на базе ESP8266 (Wemos D1 mini).

**Возможности:**
- 4 датчика температуры DS18B20 (ХВС, ГВС, подача, обратка)
- 2 импульсных счётчика воды (ГВС и ХВС) с прерываниями
- Веб-интерфейс (GyverPortal) — настройка, калибровка, мониторинг
- SMTP-отправка показаний по расписанию (email)
- JSON API для Home Assistant / Prometheus
- Telnet-доступ к логам по WiFi
- OTA-обновление прошивки по WiFi с автоматическим откатом (A/B слоты)
- Калибровка датчиков температуры через веб-интерфейс

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

## Настройка окружения для разработки

### 1. Установка PlatformIO

```bash
# Установка Python (скачайте с https://www.python.org/downloads/ версия 3.10+)

# Установка PlatformIO
pip install platformio

# Проверка
pio --version
```

### 2. Клонирование репозитория

```bash
git clone <url-репозитория>
cd SmartWaterMeter
```

### 3. Настройка secrets.h

Скопируйте и отредактируйте файл конфигурации:

```bash
cp src/secrets.h.example src/secrets.h
# Отредактируйте src/secrets.h под свои датчики
```

В secrets.h нужно указать:
- **AP_SSID_PREFIX** — префикс имени точки доступа (добавляется 3 байта MAC)
- **PIN_METER_HOT / PIN_METER_COLD** — пины герконов
- **ONE_WIRE_BUS** — пин датчиков DS18B20
- **SENSOR_ADDR** — адреса ваших датчиков DS18B20 (узнать: см. раздел Отладка)

### 4. Сборка без интернета

Все необходимые библиотеки включены в репозиторий (lib/). Для сборки нужен только PlatformIO:

```bash
pio run
```

Для сборки с интернетом PlatformIO сам скачает зависимости из lib_deps.

### 5. Сборка и заливка

```bash
# Просто сборка
pio run

# Заливка через USB (первый раз)
pio run --target upload --upload-port COM3

# Заливка через WiFi (OTA)
pio run --target upload --upload-port 192.168.x.x
```

## Первоначальная настройка

### 1. Подключение к устройству

После первой загрузки устройство создаёт точку доступа:
- **SSID:** SmartWaterMeter-XXXXXX (где XXXXXX — 3 байта MAC)
- **Пароль:** пустой (открытая сеть)
- **IP:** 192.168.4.1

Подключитесь к этой сети и откройте в браузере http://192.168.4.1.

### 2. Настройка WiFi

1. Перейдите в **Settings**
2. Введите SSID и пароль вашей WiFi-сети
3. Нажмите **Save and Reboot**
4. После перезагрузки устройство подключится к вашей сети
5. Узнайте IP в роутере или через Telnet

### 3. Настройка SMTP (email-отчёты)

В разделе **Settings**:
- **SMTP Host:** например, smtp.yandex.ru
- **Port:** 465
- **Sender Email:** ваш email
- **SMTP Password:** пароль приложения (для Яндекса — пароль приложения)
- **Recipient Email:** email для получения показаний

### 4. Настройка расписания отчётов

- **Time (HH:MM):** время отправки
- **Repeat:** Daily / Weekly / Monthly
- **Day:** день недели (для Weekly) или число месяца (для Monthly)

### 5. Настройка счётчиков воды

- **Hot m3 / Cold m3:** текущие показания счётчиков
- **cube/pulse:** сколько кубометров на один импульс (обычно 0.001 для 1 литр/импульс)

## Веб-интерфейс

Устройство работает на HTTP (порт 80). Открывайте в браузере http://<IP-адрес>/.

### Главная страница (/)

- **Date and Time** — текущее время (NTP или uptime если NTP недоступен)
- **Water Supply** — показания ГВС и ХВС (м3) и температуры
- **Heating** — температуры подачи и обратки
- **Status** — статус WiFi, IP-адрес
- **Controls** — ссылки на Settings, Calibrate, Restart

Страница обновляется автоматически каждые 5 секунд.

### Настройки (/settings)

- **WiFi** — SSID и пароль
- **Email (SMTP)** — настройки почтового сервера
- **Report Schedule** — расписание отправки показаний
- **Meter Readings** — показания счётчиков и коэффициенты
- **Test Email** — отправка тестового письма
- **Save and Reboot** — сохранить и перезагрузить

### Калибровка датчиков (/calibrate)

Позволяет сопоставить физические датчики DS18B20 с логическими каналами (ХВС, ГВС, подача, обратка).

**Процесс калибровки:**
1. На странице отображаются все найденные датчики на шине OneWire
2. Нажмите на датчик, который хотите откалибровать (например, "ХВС")
3. Нагрейте этот датчик (например, зажав в пальцах)
4. Когда температура поднимется на +5C — калибровка завершится
5. Адрес датчика сохраняется в EEPROM

## JSON API и Prometheus

### JSON API (/api.json)

Для Home Assistant и других систем умного дома:

```bash
curl http://192.168.88.89/api.json
```

Ответ:
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

**Пример для Home Assistant (RESTful sensor):**

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
    unit_of_measurement: "C"
```

### Prometheus (/metrics)

```bash
curl http://192.168.88.89/metrics
```

Ответ:
```
# HELP smartwatermeter_temperature Temperature sensors
# TYPE smartwatermeter_temperature gauge
smartwatermeter_temperature{sensor="cold"} 22.5
smartwatermeter_temperature{sensor="hot"} 55.3
smartwatermeter_temperature{sensor="supply"} 60.1
smartwatermeter_temperature{sensor="return"} 45.2
# HELP smartwatermeter_meter Water meter readings in m3
# TYPE smartwatermeter_meter gauge
smartwatermeter_meter{type="hot"} 103.000
smartwatermeter_meter{type="cold"} 127.000
# HELP smartwatermeter_uptime_seconds System uptime
# TYPE smartwatermeter_uptime_seconds counter
smartwatermeter_uptime_seconds 12345
# HELP smartwatermeter_free_heap_bytes Free heap memory
# TYPE smartwatermeter_free_heap_bytes gauge
smartwatermeter_free_heap_bytes 21704
# HELP smartwatermeter_wifi_rssi WiFi signal strength
# TYPE smartwatermeter_wifi_rssi gauge
smartwatermeter_wifi_rssi -65
# HELP smartwatermeter_calibrating Whether calibration is in progress
# TYPE smartwatermeter_calibrating gauge
smartwatermeter_calibrating 0
```

**Пример конфигурации Prometheus:**

```yaml
scrape_configs:
  - job_name: 'smartwatermeter'
    static_configs:
      - targets: ['192.168.88.89']
    metrics_path: /metrics
```

## Telnet-доступ

Подключитесь к устройству через Telnet для просмотра логов в реальном времени:

```bash
telnet 192.168.88.89
```

**Доступные команды:**
- help — список команд
- status — статус устройства (WiFi, IP, uptime, heap)
- reset — перезагрузка устройства
- heap — свободная память
- uptime — время работы
- confirm — подтверждение новой прошивки (после OTA)

**Примечание:** Все логи, которые выводятся в Serial (USB), автоматически дублируются в Telnet.

## OTA-обновление

### Как это работает

1. Прошивка загружается в неактивный слот флеш-памяти (A/B схема)
2. Устройство перезагружается в новый слот
3. На веб-интерфейсе появляется предупреждение:
   Firmware Update — "New firmware detected! Auto-rollback in 300 seconds"
4. Нажмите Confirm в течение 5 минут
5. Если не нажать — устройство автоматически вернётся к предыдущей версии

### Принудительный откат

Если прошивка вызывает циклическую перезагрузку:
- После 3 неудачных загрузок подряд происходит автоматический откат
- Можно также перепрошить через USB

### Подтверждение через Telnet

```
telnet 192.168.88.89
> confirm
Firmware confirmed!
```

## Отладка

### USB-Serial (всегда доступен)

Подключите USB-кабель и откройте монитор порта:

```bash
pio device monitor --port COM3 --baud 115200
```

### Telnet (по WiFi)

После подключения к WiFi можно смотреть логи удалённо:

```bash
telnet 192.168.88.89
```

### Определение адресов DS18B20

Для определения адресов датчиков на шине OneWire используйте пример из библиотеки DallasTemperature:

```cpp
#include <OneWire.h>
#include <DallasTemperature.h>

OneWire oneWire(D3);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(115200);
  sensors.begin();
  DeviceAddress addr;
  for (int i = 0; i < sensors.getDeviceCount(); i++) {
    sensors.getAddress(addr, i);
    for (int j = 0; j < 8; j++) {
      Serial.print("0x");
      if (addr[j] < 16) Serial.print("0");
      Serial.print(addr[j], HEX);
      if (j < 7) Serial.print(", ");
    }
    Serial.println();
  }
}

void loop() {}
```

Полученные адреса запишите в secrets.h в массив SENSOR_ADDR.

## Структура проекта

```
SmartWaterMeter/
  platformio.ini         # Конфигурация сборки
  upload_script.py       # Скрипт двойной заливки (A/B слоты)
  src/
    Wemos_Mini.ino       # Главный скетч (setup, loop, WiFi, веб-интерфейс)
    secrets.h            # Конфиденциальные настройки (пины, адреса)
    secrets.h.example    # Пример конфигурации
    ConfigStore.h        # EEPROM: сохранение/загрузка настроек
    MeterCounter.h       # Импульсные счётчики (прерывания, CHANGE)
    TemperatureSensors.h # DS18B20: чтение, калибровка
    StatusLED.h          # Светодиодная индикация
    TelnetSerial.h       # Логи по WiFi (TCP:23)
    FailsafeOTA.h        # Безопасное OTA с откатом
  lib/                   # Библиотеки для офлайн-сборки
    GyverPortal/
    OneWire/
    DallasTemperature/
    ESP_Mail_Client/
    NTPClient/
    ESP8266WiFi/
    ESP8266WebServer/
    ESP8266mDNS/
    ArduinoOTA/
  LICENSE
  README.md
```

