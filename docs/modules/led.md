# StatusLED.h — LED-индикация

## Назначение

Управление встроенным LED (GPIO2, Wemos D4, активный LOW).

## Режимы

| Режим | Индикация | Описание |
|-------|-----------|----------|
| `LED_OFF` | Всегда выкл | — |
| `LED_BOOTING` | 3 вспышки 100ms | При старте |
| `LED_AP_MODE` | Мигание 200/200ms | Точка доступа |
| `LED_CONNECTED` | Вспышка 100ms / 10с | Нормальная работа |
| `LED_CALIBRATE` | Постоянно горит | Калибровка |
| `LED_WIFI_LOST` | Мигание 500/500ms | Потеря WiFi |
| `LED_SENSOR_ERR` | 3 × (150/150ms) + пауза 2с | Датчик не найден |

## Методы

| Метод | Описание |
|-------|----------|
| `begin()` | Инициализация, переход в BOOTING |
| `tick()` | Вызов в `loop()` |
| `setMode(mode)` | Установка режима |
| `getMode()` | Текущий режим |
| `setOff()` / `setBooting()` / `setAPMode()` / `setConnected()` / `setCalibrate()` / `setWiFiLost()` / `setSensorErr()` | Удобные методы |