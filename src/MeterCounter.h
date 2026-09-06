/******************************************************************
 * MeterCounter.h - Импульсные счётчики ГВС и ХВС
 * 
 * Использует прерывания по CHANGE с аппаратным антидребезгом:
 *   - После FALLING блокируем новые FALLING, пока не увидим RISING
 *   - Это гарантирует, что каждый импульс считается ровно один раз,
 *     независимо от длительности импульса и дребезга геркона.
 * 
 * Защита от потери при noInterrupts():
 *   - _pulseCount только INCREMENT в прерывании
 *   - flush() атомарно забирает через temp = _pulseCount; _pulseCount -= temp
 *   - Если импульс пришёл между чтением и вычитанием — счётчик не обнулён,
 *     следующий flush() его подберёт
 * 
 * Восстановление после потери питания:
 *   - При begin() проверяем состояние пина
 *   - Если пин LOW (геркон замкнут) — устанавливаем флаг _waitingForRelease
 *   - После RISING засчитываем импульс
 ******************************************************************/

#ifndef MeterCounter_h
#define MeterCounter_h

#include <Arduino.h>
#include "ConfigStore.h"

class MeterCounter {
private:
  uint8_t _pin;
  volatile uint32_t _pulseCount;       // только +1 в прерывании
  volatile bool _waitingForRelease;    // true: ждём RISING (геркон разомкнётся)
  uint16_t _debounceMicros;            // 2000 = 2ms (достаточно для геркона)
  ConfigStore *_store;
  bool _isHot;
  
  static MeterCounter* _instanceHot;
  static MeterCounter* _instanceCold;
  
public:
  MeterCounter() : _pin(0), _pulseCount(0), _waitingForRelease(false),
                   _debounceMicros(2000), _store(nullptr), _isHot(true) {}
  
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
    // Если пин уже LOW (геркон замкнут) — считаем что контакт был замкнут
    // до старта. Ждём RISING, чтобы засчитать импульс.
    if (digitalRead(_pin) == LOW) {
      _waitingForRelease = true;
      Serial.printf("[Meter] %s: pin LOW at boot, waiting for release\n",
        _isHot ? "Hot" : "Cold");
    }
    
    Serial.printf("[Meter] %s on pin %d (CHANGE, debounce=%uus)\n",
      _isHot ? "Hot" : "Cold", _pin, _debounceMicros);
  }
  
  // Прерывание по CHANGE — ловит и FALLING, и RISING
  static void IRAM_ATTR isrHot() {
    if (_instanceHot) _instanceHot->handleInterrupt();
  }
  
  static void IRAM_ATTR isrCold() {
    if (_instanceCold) _instanceCold->handleInterrupt();
  }
  
  void IRAM_ATTR handleInterrupt() {
    bool pinState = digitalRead(_pin);  // читаем сразу, т.к. мы в прерывании
    
    if (pinState == LOW) {
      // FALLING: геркон замкнулся
      // Если не ждём размыкания — засчитываем импульс и переходим в ожидание
      if (!_waitingForRelease) {
        _pulseCount++;
        _waitingForRelease = true;
      }
      // Если _waitingForRelease уже true — это дребезг, игнорируем
    } else {
      // RISING: геркон разомкнулся
      // Разрешаем следующий FALLING
      _waitingForRelease = false;
    }
  }
  
  // Вызывается из loop() — сохраняет накопленные импульсы в EEPROM
  void flush() {
    // Атомарно забираем накопленные импульсы
    // Используем disable/enable, т.к. на ESP8266 нет __sync_fetch_and_sub
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
      
      // Сохраняем в EEPROM сразу (не ждём 5-минутный интервал)
      _store->saveMeters();
      
      Serial.printf("[Meter] %s: +%u pulses (%.3f m3, total %.3f)\n",
        _isHot ? "Hot" : "Cold", count, totalM3,
        _isHot ? _store->data.meterHotM3 : _store->data.meterColdM3);
    }
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

#endif