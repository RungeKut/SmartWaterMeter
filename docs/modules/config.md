# ConfigStore.h — Конфигурация и EEPROM

## Назначение

Хранение конфигурации WiFi, SMTP, счётчиков, адресов DS18B20 и расписания отчётов в EEPROM.

## Структура `ConfigData`

| Поле | Тип | Описание |
|------|-----|----------|
| `magic` | `uint16_t` | `0x5A4B` — проверка валидности |
| `wifiSSID` | `char[32]` | SSID |
| `wifiPass` | `char[64]` | Пароль |
| `smtpHost` | `char[32]` | SMTP-сервер |
| `smtpPort` | `uint16_t` | Порт SMTP |
| `smtpEmail` | `char[48]` | Email отправителя |
| `smtpPass` | `char[48]` | Пароль |
| `smtpRecipient` | `char[48]` | Email получателя |
| `meterHotM3` | `float` | Показания ГВС (м³) |
| `meterColdM3` | `float` | Показания ХВС (м³) |
| `litersPerPulseHot` | `float` | Л/импульс (ГВС) |
| `litersPerPulseCold` | `float` | Л/импульс (ХВС) |
| `sensorAddrs[4][8]` | `uint8_t[4][8]` | Адреса DS18B20 |
| `sensorAddrsValid` | `bool` | Валидность адресов |
| `reportHour/Minute` | `uint8_t` | Время отчёта |
| `reportSchedule` | `uint8_t` | 0=daily, 1=weekly, 2=monthly |
| `reportDay` | `uint8_t` | День недели/месяца |

## Методы

| Метод | Описание |
|-------|----------|
| `begin()` | Инициализация EEPROM, загрузка |
| `load()` | Чтение из EEPROM, автоинициализация если данные невалидны |
| `save()` | Запись в EEPROM |
| `resetDefaults()` | Сброс к значениям по умолчанию |
| `setSensorAddr(index, addr)` | Сохранение адреса DS18B20 |
| `getSensorAddr(index)` | Получение адреса (EEPROM или secrets.h) |
| `saveMeters()` | Сохранение показаний счётчиков |