#pragma once
#include <Arduino.h>
#include <ConfigStore.h>

/*
 * Счётчик импульсов с геркона (водосчётчик).
 *
 * Использует прерывания по CHANGE с антидребезгом:
 *   - Любой CHANGE игнорируется, если с последнего прошло < 20ms
 *   - После FALLING (замыкание) засчитывается один импульс
 *   - После RISING (размыкание) разрешается следующий FALLING
 *   - Дополнительный флаг _waitingForRelease предотвращает множественный
 *     подсчёт при дребезге на замыкании
 *
 * Поток from loop(): flush() атомарно забирает накопленные импульсы
 * и сохраняет в EEPROM через ConfigStore.
 */

class MeterCounter {
private:
  uint8_t _pin;
  volatile uint32_t _pulseCount;
  volatile bool _waitingForRelease;
  volatile uint32_t _lastChangeMicros;
  uint32_t _debounceInterval;          // 20000 = 20ms
  ConfigStore *_store;
  bool _isHot;

  static MeterCounter* _instanceHot;
  static MeterCounter* _instanceCold;

public:
  MeterCounter() : _pin(0), _pulseCount(0), _waitingForRelease(false),
                   _lastChangeMicros(0), _debounceInterval(20000),
                   _store(nullptr), _isHot(true) {}

  void begin(uint8_t pin, bool isHot, ConfigStore *store) {
    _pin = pin;
    _isHot = isHot;
    _store = store;
    _pulseCount = 0;
    _waitingForRelease = false;

    pinMode(_pin, INPUT_PULLUP);

    if (_isHot) {
      _instanceHot = this;
      attachInterrupt(digitalPinToInterrupt(_pin), isrHot, CHANGE);
    } else {
      _instanceCold = this;
      attachInterrupt(digitalPinToInterrupt(_pin), isrCold, CHANGE);
    }

    // Восстановление после потери питания:
    // Если пин уже LOW (геркон замкнут) — ждём RISING
    if (digitalRead(_pin) == LOW) {
      _waitingForRelease = true;
      Serial.printf("[Meter] %s: pin LOW at boot, waiting for release\n",
        _isHot ? "Hot" : "Cold");
    }

    Serial.printf("[Meter] %s on pin %d (CHANGE, debounce=%ums)\n",
      _isHot ? "Hot" : "Cold", _pin, _debounceInterval / 1000);
  }

  static void IRAM_ATTR isrHot() {
    if (_instanceHot) _instanceHot->handleInterrupt();
  }

  static void IRAM_ATTR isrCold() {
    if (_instanceCold) _instanceCold->handleInterrupt();
  }

  void IRAM_ATTR handleInterrupt() {
    uint32_t now = micros();

    // Общий антидребезг: игнорируем любой CHANGE быстрее debounceInterval
    if (now - _lastChangeMicros < _debounceInterval) {
      return;
    }
    _lastChangeMicros = now;

    bool pinState = digitalRead(_pin);
    if (pinState == LOW) {
      // FALLING: геркон замкнулся
      if (!_waitingForRelease) {
        _pulseCount++;
        _waitingForRelease = true;
      }
    } else {
      // RISING: геркон разомкнулся — разрешаем следующий FALLING
      _waitingForRelease = false;
    }
  }

  // Вызывается из loop() — атомарно забирает накопленные импульсы
  // и обновляет значения в RAM (без записи в EEPROM).
  // Возвращает true, если были новые импульсы.
  bool flush() {
    noInterrupts();
    uint32_t count = _pulseCount;
    _pulseCount = 0;
    interrupts();

    if (count > 0 && _store) {
      float cubePerPulse = _isHot ? _store->data.litersPerPulseHot : _store->data.litersPerPulseCold;
      float totalM3 = count * cubePerPulse;

      if (_isHot) {
        _store->data.meterHotM3 += totalM3;
      } else {
        _store->data.meterColdM3 += totalM3;
      }

      Serial.printf("[Meter] %s: +%u pulses (%.3f m3, total %.3f)\n",
        _isHot ? "Hot" : "Cold", count, totalM3,
        _isHot ? _store->data.meterHotM3 : _store->data.meterColdM3);

      return true;
    }
    return false;
  }

  uint32_t getPulseCount() {
    noInterrupts();
    uint32_t c = _pulseCount;
    interrupts();
    return c;
  }

  void resetPulseCount() {
    noInterrupts();
    _pulseCount = 0;
    interrupts();
  }
};

MeterCounter* MeterCounter::_instanceHot = nullptr;
MeterCounter* MeterCounter::_instanceCold = nullptr;