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
- Данные с датчиков обновляются каждые 2 секунды
- OTA-баннер с обратным отсчётом (FailsafeOTA — 5 мин на подтверждение)

### Сборка (актуальная)

| Параметр | Значение |
|---|---|
| RAM | 53.1% (43468 / 81920 байт) |
| Flash | 62.5% (652400 / 1044464 байт) |
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

---

## Последние изменения

### [12] fix: OTA-тест — исправлен host_ip, добавлено правило FW (10.09.2026)

**Проблема:**
- `platformio.ini` содержал устаревший `--host_ip=192.168.88.113` (предыдущий IP хоста для portproxy)
- OTA падал с `Listen Failed` на несуществующем адресе
- После исправления IP — ошибка `No response from device` из-за блокировки порта 33777 Windows Firewall

**Исправления:**
- `platformio.ini`: `--host_ip` изменён с `192.168.88.113` на актуальный `192.168.88.92`
- На хосте добавлено правило Windows Firewall: `PlatformIO OTA 33777` (TCP in, порт 33777)

**Результаты OTA-теста:**
- Сборка: SUCCESS (RAM 53.1%, Flash 62.5%)
- OTA-upload: SUCCESS (28 сек, 656560 байт, `Result: OK`)
- ESP перезагрузка: SUCCESS (18 мс, TTL=255)
- Подтверждение прошивки (`/confirm`): SUCCESS
- OTA-прошивка работает напрямую (хост и ESP в одной подсети `/24`)

### [11] fix: Переписан FailsafeOTA — корректная адресация слотов, eboot, интеграция с ArduinoOTA (11.09.2026)

**Проблемы старой реализации:**
- `getCurrentSlot()` — брал адрес переменной в RAM (`0x3FFFxxxx`), всегда возвращал слот 1
- `rollback()` — неверные адреса: при slot=0 писал `0x100000` (начало slot B, а не slot A)
- Нет интеграции с ArduinoOTA — `onEnd()` не выставлял флаг в RTC, откат никогда не срабатывал
- Счётчик bootloop (3 retry) избыточен — достаточно одного retry для защиты

**Исправления в `FailsafeOTA.h`:**
- `getCurrentSlot()` использует адрес функции (IROM `0x402xxxxx`), маскирует `0x3FFFFF`, сравнивает с `SLOT_B_IMAGE_ADDR=0x101000`
- Корректные константы: `SLOT_A_IMAGE_ADDR=0x10000`, `SLOT_B_IMAGE_ADDR=0x101000`
- `rollback()` пишет eboot command на противоположный слот с правильным адресом
- Добавлен метод `updateFirmware()` — вызывается из `ArduinoOTA.onEnd()`, выставляет `RTC_MAGIC_FIRST_BOOT`
- Упрощена логика: два состояния (FIRST_BOOT → RETRY → rollback) вместо счётчика до 3

**Доработки в `Wemos_Mini.ino`:**
- `ArduinoOTA.onEnd()` вызывает `failsafe.updateFirmware()`
- Добавлен HTTP-эндпоинт `/confirm` для подтверждения прошивки из браузера

**Изменения в `platformio.ini`:**
- Раскомментированы `upload_protocol = espota` и `upload_port = 192.168.88.87`
- Добавлены `upload_flags` для совместимости с пробросом через netsh (раскомментировать при необходимости)

**Статус тестирования:**
- Сборка: SUCCESS (RAM 53.1%, Flash 62.5%)
- USB-прошивка: SUCCESS (залита через COM3)
- **OTA: SUCCESS** (см. [12] — протестирован: upload 28 сек, подтверждён через `/confirm`)
- Условие: хост и ESP в одной подсети; на хосте открыт порт 33777 (TCP in) в Windows Firewall

### [10] fix: интервал датчиков 5000 → 2000 мс
- Dashboard и Calibrate теперь получают данные с одинаковой частотой (~2 сек)
- 2000 мс безопасно: конверсия ~750 мс, ~1250 мс на WiFi/WebSocket

---

## Состояние оборудования

**Устройство:** `SmartWaterMeter-93C195` (MAC: e8:9f:6d:93:c1:95)
**IP:** `192.168.88.87`
**Ревизия платы:** Wemos D1 mini, ESP8266, 4MB Flash
**Последняя прошивка:** OTA (через WiFi), 10.09.2026
**Датчики DS18B20:** подключены, показывают ~28°C (комнатная температура)
**Счётчики:** горячая — 2.000 m³, холодная — 0.000 m³
**WiFi:** connected, RSSI -78..-79 dBm

## Инструкция по прошивке

### USB

```bash
pio run --target upload --upload-port COM3
pio run --target uploadfs --upload-port COM3   # если менялся index.html
```

### OTA (через WiFi)

```bash
pio run --target upload
```

> **Важно:** Хост и ESP должны быть в одной сети. На хосте должен быть открыт TCP-порт 33777 (входящие) в Windows Firewall. Если OTA не работает:
> 1. Проверьте `upload_flags --host_ip` в `platformio.ini` — должен быть актуальный IP хоста
> 2. Проверьте правило FW: `netsh advfirewall firewall show rule name="PlatformIO OTA 33777"`
> 3. Если правило отсутствует: `netsh advfirewall firewall add rule name="PlatformIO OTA 33777" dir=in action=allow protocol=TCP localport=33777`

### Первый запуск после USB

1. Плата загрузится с новым FailsafeOTA
2. Если **не было OTA** — баннер подтверждения НЕ появляется, всё работает сразу
3. Если был OTA — открой `http://192.168.88.87/confirm` в браузере или нажми Confirm на баннере