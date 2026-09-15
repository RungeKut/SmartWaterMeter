#pragma once
#include <Arduino.h>
#include <math.h>
#include "Log.h"
#include "ConfigStore.h"

/*
 * Защита осмотического фильтра от подмеса ГВС в ХВС.
 *
 * ЗАЧЕМ
 *
 * В доме есть подмес горячей воды в холодную через соседние квартиры.
 * Осмотическая мембрана на кипяток не рассчитана и от него портится.
 * Модуль следит за температурой ХВС и электромагнитным клапаном
 * перекрывает подачу воды на фильтр, пока вода горячая.
 *
 * ГИСТЕРЕЗИС
 *
 *   ХВС >= filterTempOnC   -> реле включается, клапан ЗАКРЫТ
 *   ХВС <= filterTempOffC  -> реле отключается, клапан ОТКРЫТ
 *
 * Два разных порога обязательны: с одним реле дребезжало бы на каждой
 * десятой градуса около границы. Если настройки заданы так, что
 * гистерезис схлопывается (off >= on), модуль чинит их сам —
 * см. offThreshold().
 *
 * НАПРАВЛЕНИЕ ОТКАЗА: ОБЕСТОЧЕНО = ВОДА ИДЁТ
 *
 * Клапан нормально открытый, реле перекрывает воду под током. Значит:
 *
 *   - плата выключена или перезагружается -> фильтр работает
 *   - катушка под током только во время тревоги, а не постоянно
 *   - GPIO5 при старте болтается, но реле от этого не сработает
 *
 * Цена решения: пока платы нет, мембрана без защиты. Выбрано
 * сознательно — обратный вариант означал бы, что любой сбой питания
 * оставляет квартиру без фильтрованной воды, а катушку греет
 * круглосуточно.
 *
 * Поэтому в begin() уровень пина выставляется ДО pinMode(OUTPUT):
 * на ESP8266 запись в защёлку до переключения в выход убирает
 * короткий выброс в момент конфигурации ноги.
 *
 * ЕСЛИ ДАТЧИК ХВС ПРОПАЛ
 *
 * Состояние реле НЕ меняется, поднимается отдельное событие. Логика
 * такая: обрыв провода не должен лишать квартиру воды, а о самом
 * обрыве нужно узнать сразу. Риск обратной стороны осознан — подмес
 * при мёртвом датчике пройдёт на мембрану.
 *
 * СТАТИСТИКА
 *
 * Счётчики срабатываний, максимума температуры и суммарного времени
 * закрытия живут только в RAM и обнуляются при перезагрузке — как и
 * диагностика герконов. В EEPROM модуль не пишет ничего.
 */

// Нога реле. Задаётся здесь, а не в secrets.h: тот в .gitignore, и в
// свежем клоне определения бы не оказалось. Переопределить из
// secrets.h всё равно можно — он подключается раньше.
#ifndef PIN_FILTER_RELAY
#define PIN_FILTER_RELAY 5        // D1, GPIO5 — нет роли при загрузке
#endif

#define FILTER_TEMP_ON_DEFAULT   35.0f
#define FILTER_TEMP_OFF_DEFAULT  30.0f
#define FILTER_HYST_MIN_C         2.0f   // минимальный зазор порогов
#define FILTER_TEMP_MAX_C       100.0f

class FilterGuard {
public:
  enum Event {
    EVENT_CLOSED,        // защита сработала, вода на фильтр перекрыта
    EVENT_OPENED,        // температура упала, вода снова идёт
    EVENT_SENSOR_LOST,   // датчик ХВС перестал отвечать
    EVENT_SENSOR_BACK,   // датчик вернулся
  };

  typedef void (*EventCallback)(Event ev, float tempC);

  static const char* eventName(Event ev) {
    switch (ev) {
      case EVENT_CLOSED:      return "valve closed";
      case EVENT_OPENED:      return "valve opened";
      case EVENT_SENSOR_LOST: return "cold sensor lost";
      case EVENT_SENSOR_BACK: return "cold sensor back";
    }
    return "?";
  }

private:
  uint8_t _pin;
  ConfigStore *_store;
  EventCallback _cb;

  bool _closed;        // подтверждённое состояние клапана
  bool _sensorLost;

  // --- статистика для отчёта ---
  uint32_t _trips;
  float    _maxTempC;
  bool     _maxValid;
  uint32_t _closedSec;
  uint32_t _tickMs;

  uint8_t activeLevel() const {
    bool activeLow = _store ? _store->data.filterRelayActiveLow : false;
    return activeLow ? LOW : HIGH;
  }

  uint8_t inactiveLevel() const {
    return activeLevel() == HIGH ? LOW : HIGH;
  }

  void applyRelay() {
    digitalWrite(_pin, _closed ? activeLevel() : inactiveLevel());
  }

  void emit(Event ev, float tempC) {
    Log.printf("[Filter] %s at %.1f C\n", eventName(ev), tempC);
    if (_cb) _cb(ev, tempC);
  }

  // Суммарное время в закрытом состоянии. Разность millis() устойчива
  // к переполнению, накопитель в секундах хватает на 136 лет.
  void tickClosedTime() {
    uint32_t now = millis();
    uint32_t d = now - _tickMs;
    if (d >= 1000) {
      uint32_t s = d / 1000;
      if (_closed) _closedSec += s;
      _tickMs += s * 1000UL;
    }
  }

public:
  FilterGuard()
    : _pin(PIN_FILTER_RELAY), _store(nullptr), _cb(nullptr),
      _closed(false), _sensorLost(false),
      _trips(0), _maxTempC(0.0f), _maxValid(false),
      _closedSec(0), _tickMs(0) {}

  void begin(uint8_t pin, ConfigStore *store, EventCallback cb) {
    _pin = pin;
    _store = store;
    _cb = cb;
    _closed = false;
    _tickMs = millis();

    // Уровень до pinMode: иначе в момент переключения ноги в выход
    // проскакивает короткий импульс, и реле успевает щёлкнуть.
    digitalWrite(_pin, inactiveLevel());
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, inactiveLevel());

    Log.printf("[Filter] pin %d, %s, thresholds %.1f/%.1f C, guard %s\n",
      _pin, activeLevel() == LOW ? "active LOW" : "active HIGH",
      onThreshold(), offThreshold(),
      (_store && _store->data.filterEnabled) ? "enabled" : "disabled");
  }

  // Верхний порог. Мусор и выход за разумные пределы чиним значением
  // по умолчанию: сравнение через !(v > 0) заодно ловит NaN.
  float onThreshold() const {
    float v = _store ? _store->data.filterTempOnC : 0.0f;
    if (!(v > 0.0f) || v > FILTER_TEMP_MAX_C) v = FILTER_TEMP_ON_DEFAULT;
    return v;
  }

  // Нижний порог. Схлопнувшийся гистерезис (off >= on) заставил бы реле
  // дребезжать на каждой десятой градуса, поэтому зазор восстанавливаем.
  float offThreshold() const {
    float on = onThreshold();
    float v = _store ? _store->data.filterTempOffC : 0.0f;
    if (!(v > 0.0f) || v >= on) v = on - FILTER_HYST_MIN_C;
    return v;
  }

  // Вызывается после каждого чтения датчиков (раз в секунду).
  void handle(float tempC, bool tempOk) {
    tickClosedTime();
    if (!_store) return;

    // Защита выключена — клапан обязан быть открыт
    if (!_store->data.filterEnabled) {
      if (_closed) {
        _closed = false;
        applyRelay();
        emit(EVENT_OPENED, tempC);
      }
      _sensorLost = false;
      return;
    }

    // Датчик молчит: состояние держим, но сообщаем — один раз на обрыв
    if (!tempOk) {
      if (!_sensorLost) {
        _sensorLost = true;
        emit(EVENT_SENSOR_LOST, tempC);
      }
      return;
    }
    if (_sensorLost) {
      _sensorLost = false;
      emit(EVENT_SENSOR_BACK, tempC);
    }

    if (!_maxValid || tempC > _maxTempC) {
      _maxTempC = tempC;
      _maxValid = true;
    }

    if (!_closed && tempC >= onThreshold()) {
      _closed = true;
      _trips++;
      applyRelay();
      emit(EVENT_CLOSED, tempC);
    } else if (_closed && tempC <= offThreshold()) {
      _closed = false;
      applyRelay();
      emit(EVENT_OPENED, tempC);
    }
  }

  // --- состояние и статистика ---
  bool     isClosed()     const { return _closed; }
  bool     isSensorLost() const { return _sensorLost; }
  uint32_t trips()        const { return _trips; }
  bool     hasMaxTemp()   const { return _maxValid; }
  float    maxTempC()     const { return _maxTempC; }

  uint32_t closedSec() const {
    // Незавершённый текущий интервал тоже учитываем, иначе отчёт,
    // снятый при закрытом клапане, показывал бы меньше правды.
    if (!_closed) return _closedSec;
    return _closedSec + (millis() - _tickMs) / 1000;
  }

  // Вызывается после отправки планового отчёта: период начинается заново
  void resetStats() {
    _trips = 0;
    _maxValid = false;
    _maxTempC = 0.0f;
    _closedSec = 0;
    _tickMs = millis();
  }
};
