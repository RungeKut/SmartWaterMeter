/******************************************************************
 * StatusLED.h - LED indicator for device status on built-in LED
 * 
 * Wemos Mini: LED on GPIO2 (D4), LOW = ON, HIGH = OFF
 * 
 * Modes:
 *   OFF         - always off
 *   BOOTING     - 3 quick flashes, then off
 *   AP_MODE     - blink 200/200ms
 *   CONNECTED   - short flash 100ms every 10 seconds
 *   CALIBRATE   - solid on
 *   WIFI_LOST   - blink 500/500ms
 *   SENSOR_ERR  - 3 flashes (150/150ms), pause 2000ms, repeat
 ******************************************************************/

#ifndef StatusLED_h
#define StatusLED_h

#include <Arduino.h>

#define LED_PIN 2  // GPIO2, D4 on Wemos Mini (LOW = ON)

enum LEDMode {
  LED_OFF = 0,
  LED_BOOTING,
  LED_AP_MODE,
  LED_CONNECTED,
  LED_CALIBRATE,
  LED_WIFI_LOST,
  LED_SENSOR_ERR
};

class StatusLED {
private:
  LEDMode _mode;
  uint32_t _lastChange;
  bool _ledOn;
  bool _initialized;

  // For BOOTING and SENSOR_ERR: burst of N flashes then pause
  uint8_t _burstCount;   // how many ON transitions done in current burst
  uint32_t _burstStart;  // when burst/pause started

  void ledOn() {
    digitalWrite(LED_PIN, LOW);
    _ledOn = true;
  }

  void ledOff() {
    digitalWrite(LED_PIN, HIGH);
    _ledOn = false;
  }

public:
  StatusLED() : _mode(LED_OFF), _lastChange(0), _ledOn(false),
                _initialized(false), _burstCount(0), _burstStart(0) {}

  void begin() {
    pinMode(LED_PIN, OUTPUT);
    ledOff();
    _initialized = true;
    setMode(LED_BOOTING);
  }

  void setMode(LEDMode newMode) {
    if (newMode == _mode) return;
    _mode = newMode;
    _lastChange = 0;
    _burstCount = 0;
    _burstStart = 0;
    ledOff();

    if (_mode == LED_CALIBRATE) {
      ledOn();
    }
  }

  LEDMode getMode() { return _mode; }

  void tick() {
    if (!_initialized) return;
    uint32_t now = millis();

    switch (_mode) {

      case LED_OFF:
      case LED_CALIBRATE:
        // Already set in setMode()
        break;

      case LED_CONNECTED:
        // Flash 100ms every 10000ms
        if (_ledOn && now - _lastChange >= 100) {
          ledOff();
          _lastChange = now;
        } else if (!_ledOn && now - _lastChange >= 10000) {
          ledOn();
          _lastChange = now;
        }
        break;

      case LED_AP_MODE:
        // 200ms on, 200ms off
        if (now - _lastChange >= 200) {
          _lastChange = now;
          if (_ledOn) ledOff(); else ledOn();
        }
        break;

      case LED_WIFI_LOST:
        // 500ms on, 500ms off
        if (now - _lastChange >= 500) {
          _lastChange = now;
          if (_ledOn) ledOff(); else ledOn();
        }
        break;

      case LED_BOOTING:
        // 3 flashes (100ms on, 100ms off), then off
        if (_burstCount < 3) {
          if (!_ledOn && now - _lastChange >= 100) {
            ledOn();
            _lastChange = now;
          } else if (_ledOn && now - _lastChange >= 100) {
            ledOff();
            _lastChange = now;
            _burstCount++;
          }
        }
        // After 3 flashes, stay off (mode stays BOOTING but does nothing)
        break;

      case LED_SENSOR_ERR:
        // 3 flashes (150ms on, 150ms off), pause 2000ms, repeat
        if (_burstCount < 3) {
          if (!_ledOn && now - _lastChange >= 150) {
            ledOn();
            _lastChange = now;
          } else if (_ledOn && now - _lastChange >= 150) {
            ledOff();
            _lastChange = now;
            _burstCount++;
          }
        } else {
          // Pause 2000ms after burst
          if (_burstStart == 0) {
            _burstStart = now;
            ledOff();
          }
          if (now - _burstStart >= 2000) {
            _burstCount = 0;
            _burstStart = 0;
            _lastChange = now;
          }
        }
        break;
    }
  }

  // Convenience methods
  void setOff()        { setMode(LED_OFF); }
  void setBooting()    { setMode(LED_BOOTING); }
  void setAPMode()     { setMode(LED_AP_MODE); }
  void setConnected()  { setMode(LED_CONNECTED); }
  void setCalibrate()  { setMode(LED_CALIBRATE); }
  void setWiFiLost()   { setMode(LED_WIFI_LOST); }
  void setSensorErr()  { setMode(LED_SENSOR_ERR); }
};

#endif