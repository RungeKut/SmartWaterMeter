# SmartWaterMeter — Сборка и прошивка

## Требования

- PlatformIO (`pip install platformio` или расширение для VS Code)
- USB-кабель (Wemos D1 mini)
- Для OTA: устройство должно быть в одной WiFi-сети с компьютером

## Первая прошивка (USB)

```bash
# Сборка
pio run

# Заливка прошивки
pio run --target upload --upload-port COM3

# Заливка файловой системы (LittleFS) с SPA фронтендом
pio run --target uploadfs --upload-port COM3
```

> **Важно:** `pio run --target upload` **не затирает** EEPROM (настройки WiFi, SMTP, калибровку). EEPROM лежит по адресу `0x7FB000`, прошивка пишется в `0x00000..0x100000` — они не пересекаются.
>
> Если настройки сбросились после прошивки — скорее всего сработал FailsafeOTA: не была подтверждена новая прошивка в течение 5 минут, и ESP8266 откатилась на старый слот. После USB-прошивки обязательно подтвердите прошивку через веб-интерфейс (кнопка Confirm на OTA-баннере).

## OTA-обновление (после первой прошивки)

```bash
# Собрать и залить через WiFi
pio run --target upload --upload-port 192.168.x.x

# Файловую систему тоже можно по WiFi
pio run --target uploadfs --upload-port 192.168.x.x
```

## Первый запуск

1. Подключитесь к точке доступа `SmartWaterMeter-XXXXXX` (открытая)
2. Откройте `http://192.168.0.1`
3. Перейдите в **Settings**, укажите SSID и пароль WiFi
4. Нажмите **Save & Reboot**
5. После перезагрузки устройство подключится к вашей сети
6. Настройте SMTP, откалибруйте датчики

## Зависимости (lib_deps)

| Библиотека | Версия | Назначение |
|-----------|--------|-----------|
| `paulstoffregen/OneWire` | 2.3.8 | Протокол 1-Wire |
| `milesburton/DallasTemperature` | 4.0.6 | Работа с DS18B20 |
| `NTPClient` | 3.2.1 | Синхронизация времени |
| `me-no-dev/ESPAsyncTCP` | 2.0.0 | Асинхронный TCP |
| `me-no-dev/ESPAsyncWebServer` | 3.6.0 | Веб-сервер + WebSocket |
| `bblanchon/ArduinoJson` | 7.4.3 | JSON для WebSocket |

## Известные warnings

При сборке может появляться:
```
elf2bin.py:54: SyntaxWarning: invalid escape sequence '\s'
```
Это warning от инструментария ESP8266, не влияет на сборку.