# MeterCounter.h — Счётчики воды

## Назначение

Счётчик импульсов с геркона (водосчётчик). Использует прерывания по CHANGE с антидребезгом.

## Поля

| Поле | Тип | Описание |
|------|-----|----------|
| `_pulseCount` | `volatile uint32_t` | Накопленные импульсы (из прерывания) |
| `_waitingForRelease` | `volatile bool` | Флаг ожидания размыкания геркона |
| `_lastChangeMicros` | `volatile uint32_t` | Время последнего изменения (мкс) |
| `_debounceInterval` | `uint32_t` | Антидребезг (20000 = 20ms) |
| `_isHot` | `bool` | true = ГВС, false = ХВС |
| `_store` | `ConfigStore*` | Для обновления показаний |

## Методы

| Метод | Описание |
|-------|----------|
| `begin(pin, isHot, store)` | Инициализация пина, установка прерывания |
| `flush()` | Атомарный сбор импульсов, обновление значений в RAM. **Не пишет в EEPROM.** |
| `getPulseCount()` | Количество неподтверждённых импульсов |
| `resetPulseCount()` | Сброс счётчика |

## Алгоритм антидребезга

```
CHANGE:
  if (now - lastChange < 20ms) → ignore
  if FALLING && !waitingForRelease → pulse++, waitingForRelease = true
  if RISING → waitingForRelease = false
```

## Важно: EEPROM

`flush()` обновляет только RAM. Сохранение в EEPROM — раз в 5 минут в `loop()` через `ConfigStore::saveMeters()`. Это предотвращает износ EEPROM.