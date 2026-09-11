#pragma once
#include <Arduino.h>
#include "Log.h"
#include "ConfigStore.h"

/*
 * Счётчик импульсов герконового водосчётчика.
 *
 * АРХИТЕКТУРА: захват в прерывании, разбор в loop()
 *
 *   ISR      — ничего не решает: кладёт (время, уровень) в кольцевой
 *              буфер и выходит. Ни один фронт не теряется, даже когда
 *              loop() занят чтением датчиков на ~100 мс.
 *   process() — автомат с гистерезисом по времени, работает в loop().
 *
 * ПОЧЕМУ НЕ ПРЕЖНЯЯ СХЕМА
 *
 * Старый ISR при срабатывании антидребезга делал return, НЕ записав
 * время и не обновив состояние. Из-за этого короткое замыкание теряло
 * свой размыкающий фронт, флаг _waitingForRelease залипал в true — и
 * все последующие импульсы молча пропадали, пока не случалось
 * замыкание длиннее окна антидребезга:
 *
 *   t=0      FALLING -> принят, pulse++, waiting=true
 *   t=5мс    RISING  -> отброшен (< 20мс), waiting остаётся true
 *   t=1200мс FALLING -> принят, но waiting==true -> НЕ СЧИТАЕТСЯ
 *   t=2400мс FALLING -> снова НЕ СЧИТАЕТСЯ ...
 *
 * ПРАВИЛА АВТОМАТА
 *
 *   - Замыкание подтверждается, если уровень LOW продержался
 *     >= debounceClosedMs (по умолчанию 5 мс)
 *   - Размыкание подтверждается, если HIGH продержался
 *     >= debounceOpenMs (по умолчанию 50 мс)
 *   - Импульс засчитывается в момент подтверждённого ЗАВЕРШЕНИЯ
 *     замкнутой фазы, а не в начале
 *
 * ПОЧЕМУ СЧИТАЕМ ПО ЗАВЕРШЕНИЮ ЗАМЫКАНИЯ
 *
 * Это снимает неоднозначность при пропадании питания. Если плата
 * выключилась с замкнутым герконом, прошлая прошивка этот импульс
 * засчитать не могла — учёт происходит только при размыкании. Значит,
 * после включения его можно смело засчитать, когда геркон разомкнётся:
 * ни двойного счёта, ни пропуска.
 *
 * ЗАЛИПШИЙ ГЕРКОН
 *
 * Счётчик может месяцами стоять с замкнутым герконом, если водой не
 * пользуются. Само по себе это безопасно — стабильное состояние.
 * Опасен микродребезг в залипшем положении (вибрация трубы, ползучая
 * струйка): его отсекает требование debounceOpenMs — без полноценного
 * размыкания новое замыкание не засчитывается.
 *
 * Возраст состояния считается в СЕКУНДАХ (stateAgeSec), а не через
 * micros()/millis(), которые переполняются через 71 минуту и 49 дней
 * соответственно. Месяцы простоя отображаются корректно.
 */

#define METER_EVENT_QUEUE           16
#define METER_CLOSED_MIN_MS_DEFAULT 5
#define METER_OPEN_MIN_MS_DEFAULT   50

class MeterCounter {
private:
  struct Event {
    uint32_t tUs;
    uint8_t  level;
  };

  // --- разделяемое с ISR ---
  volatile Event   _q[METER_EVENT_QUEUE];
  volatile uint8_t _qHead;      // пишет ISR
  volatile uint8_t _qTail;      // читает loop
  volatile uint16_t _qOverflow; // событий потеряно при переполнении

  uint8_t _pin;
  bool _isHot;
  ConfigStore *_store;

  // --- автомат, только loop() ---
  uint8_t  _rawLevel;      // последний известный сырой уровень
  bool     _stableClosed;  // подтверждённое состояние
  bool     _candActive;    // есть кандидат на смену состояния
  uint32_t _candSinceUs;   // когда кандидат появился

  // Возраст текущего состояния: секунды + опорная точка в millis
  uint32_t _stateAgeSec;
  uint32_t _ageTickMs;

  uint32_t _pendingPulses;

  // --- диагностика ---
  uint32_t _bounces;       // отбракованный дребезг
  uint32_t _totalPulses;   // импульсов с момента загрузки
  uint32_t _lastClosedMs;
  uint32_t _minClosedMs;
  uint32_t _maxClosedMs;
  uint32_t _lastOpenMs;

  static MeterCounter *_instanceHot;
  static MeterCounter *_instanceCold;

  uint32_t closedMinUs() const {
    uint16_t ms = _store ? _store->data.debounceClosedMs : 0;
    if (ms == 0) ms = METER_CLOSED_MIN_MS_DEFAULT;
    return (uint32_t)ms * 1000UL;
  }

  uint32_t openMinUs() const {
    uint16_t ms = _store ? _store->data.debounceOpenMs : 0;
    if (ms == 0) ms = METER_OPEN_MIN_MS_DEFAULT;
    return (uint32_t)ms * 1000UL;
  }

  // Накопление возраста состояния. Разность millis() устойчива к
  // переполнению, а накопитель в секундах хватает на 136 лет.
  void tickAge() {
    uint32_t now = millis();
    uint32_t d = now - _ageTickMs;
    if (d >= 1000) {
      uint32_t s = d / 1000;
      _stateAgeSec += s;
      _ageTickMs += s * 1000UL;
    }
  }

  // Возраст состояния начинается не «сейчас», а в момент физического
  // фронта — то есть offsetMs назад (окно подтверждения).
  void resetAge(uint32_t offsetMs) {
    _stateAgeSec = offsetMs / 1000;
    _ageTickMs = millis() - (offsetMs % 1000);
  }

  // Новый сырой уровень. Кандидат стартует только при реальной смене
  // уровня, поэтому повторный вызов с тем же значением безвреден —
  // на этом держится самокоррекция в process().
  void feed(uint32_t tUs, uint8_t level) {
    if (level == _rawLevel) return;
    _rawLevel = level;

    bool levelClosed = (level == LOW);
    if (levelClosed == _stableClosed) {
      // Вернулись к подтверждённому уровню — это был дребезг
      if (_candActive) {
        _candActive = false;
        _bounces++;
      }
    } else {
      _candActive = true;
      _candSinceUs = tUs;
    }
  }

  void evaluate(uint32_t nowUs) {
    if (!_candActive) return;

    bool candClosed = !_stableClosed;
    uint32_t needUs = candClosed ? closedMinUs() : openMinUs();
    if ((uint32_t)(nowUs - _candSinceUs) < needUs) return;

    uint32_t needMs = needUs / 1000;
    uint32_t age = stateAgeMs();
    uint32_t phaseMs = (age > needMs) ? (age - needMs) : 0;

    _candActive = false;
    _stableClosed = candClosed;
    resetAge(needMs);

    if (candClosed) {
      // Подтверждено замыкание — импульс ещё не считаем
      _lastOpenMs = phaseMs;
    } else {
      // Замкнутая фаза завершилась — вот теперь импульс
      _lastClosedMs = phaseMs;
      if (phaseMs < _minClosedMs) _minClosedMs = phaseMs;
      if (phaseMs > _maxClosedMs) _maxClosedMs = phaseMs;
      _pendingPulses++;
      _totalPulses++;
    }
  }

public:
  MeterCounter()
    : _qHead(0), _qTail(0), _qOverflow(0),
      _pin(0), _isHot(true), _store(nullptr),
      _rawLevel(HIGH), _stableClosed(false), _candActive(false), _candSinceUs(0),
      _stateAgeSec(0), _ageTickMs(0), _pendingPulses(0),
      _bounces(0), _totalPulses(0),
      _lastClosedMs(0), _minClosedMs(UINT32_MAX), _maxClosedMs(0), _lastOpenMs(0) {}

  void begin(uint8_t pin, bool isHot, ConfigStore *store) {
    _pin = pin;
    _isHot = isHot;
    _store = store;

    pinMode(_pin, INPUT_PULLUP);

    // Принимаем текущее положение геркона как исходное состояние.
    // Если он замкнут (например, питание пропало посреди импульса) —
    // импульс будет засчитан при размыкании, ровно один раз.
    _rawLevel = digitalRead(_pin);
    _stableClosed = (_rawLevel == LOW);
    resetAge(0);

    if (_isHot) {
      _instanceHot = this;
      attachInterrupt(digitalPinToInterrupt(_pin), isrHot, CHANGE);
    } else {
      _instanceCold = this;
      attachInterrupt(digitalPinToInterrupt(_pin), isrCold, CHANGE);
    }

    Log.printf("[Meter] %s on pin %d: %s at boot, debounce %u/%u ms\n",
      _isHot ? "Hot" : "Cold", _pin,
      _stableClosed ? "CLOSED" : "open",
      (unsigned)(closedMinUs() / 1000), (unsigned)(openMinUs() / 1000));
  }

  static void IRAM_ATTR isrHot() {
    if (_instanceHot) _instanceHot->handleInterrupt();
  }

  static void IRAM_ATTR isrCold() {
    if (_instanceCold) _instanceCold->handleInterrupt();
  }

  // ISR принципиально ничего не решает: только фиксирует факт фронта.
  // Любая логика здесь означала бы потерю событий при дребезге.
  void IRAM_ATTR handleInterrupt() {
    uint8_t next = (uint8_t)((_qHead + 1) % METER_EVENT_QUEUE);
    if (next == _qTail) {
      _qOverflow++;   // буфер полон; process() восстановится по уровню пина
      return;
    }
    _q[_qHead].tUs = micros();
    _q[_qHead].level = (uint8_t)digitalRead(_pin);
    _qHead = next;
  }

  // Вызывается из loop(). Возвращает true, если добавились импульсы.
  bool process() {
    tickAge();

    // 1. Разбираем очередь событий
    while (true) {
      Event e;
      bool have;
      noInterrupts();
      have = (_qTail != _qHead);
      if (have) {
        e.tUs = _q[_qTail].tUs;
        e.level = _q[_qTail].level;
        _qTail = (uint8_t)((_qTail + 1) % METER_EVENT_QUEUE);
      }
      interrupts();
      if (!have) break;
      feed(e.tUs, e.level);
    }

    // 2. Самокоррекция: сверяемся с реальным уровнем пина.
    // Если очередь переполнилась и последнее событие потерялось,
    // автомат всё равно догонит фактическое состояние.
    feed(micros(), (uint8_t)digitalRead(_pin));

    // 3. Созрел ли кандидат
    evaluate(micros());

    // 4. Переносим импульсы в показания
    if (_pendingPulses == 0 || !_store) return false;

    uint32_t count = _pendingPulses;
    _pendingPulses = 0;

    // Коэффициент задаётся в ЛИТРАХ на импульс (веб-интерфейс: "L per pulse"),
    // а показания храним в м³ — отсюда деление на 1000.
    float litersPerPulse = _isHot ? _store->data.litersPerPulseHot
                                  : _store->data.litersPerPulseCold;
    float totalM3 = (count * litersPerPulse) / 1000.0f;

    if (_isHot) _store->data.meterHotM3 += totalM3;
    else        _store->data.meterColdM3 += totalM3;

    Log.printf("[Meter] %s: +%u pulses (%.3f m3, total %.3f), closed %u ms\n",
      _isHot ? "Hot" : "Cold", count, totalM3,
      _isHot ? _store->data.meterHotM3 : _store->data.meterColdM3,
      _lastClosedMs);

    return true;
  }

  // --- диагностика для веб-интерфейса ---
  bool     isClosed()      const { return _stableClosed; }
  uint32_t stateAgeSec()   const { return _stateAgeSec; }
  uint32_t bounces()       const { return _bounces; }
  uint32_t totalPulses()   const { return _totalPulses; }
  uint32_t lastClosedMs()  const { return _lastClosedMs; }
  uint32_t maxClosedMs()   const { return _maxClosedMs; }
  uint32_t lastOpenMs()    const { return _lastOpenMs; }
  uint16_t queueOverflow() const { return _qOverflow; }

  uint32_t minClosedMs() const {
    return (_minClosedMs == UINT32_MAX) ? 0 : _minClosedMs;
  }

  uint32_t stateAgeMs() const {
    if (_stateAgeSec >= 4294000UL) return UINT32_MAX;   // насыщение (~49 суток)
    return _stateAgeSec * 1000UL + (millis() - _ageTickMs);
  }
};

MeterCounter* MeterCounter::_instanceHot = nullptr;
MeterCounter* MeterCounter::_instanceCold = nullptr;
