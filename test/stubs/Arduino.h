/******************************************************************
 * Arduino.h — заглушка для нативных тестов на хосте
 *
 * Позволяет собрать модули проекта обычным компилятором и прогнать
 * логику без платы. Время и уровни пинов управляются тестом, поэтому
 * можно воспроизвести дребезг, короткие импульсы и залипший геркон
 * с точностью до микросекунды.
 ******************************************************************/

#ifndef ARDUINO_STUB_H
#define ARDUINO_STUB_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define LOW  0
#define HIGH 1
#define INPUT_PULLUP 2
#define OUTPUT 1
#define CHANGE 3
#define IRAM_ATTR
#ifndef PROGMEM
#define PROGMEM
#endif
#define F(x) (x)

// ---- управляемое тестом время ----
//
// База — 64-битный монотонный счётчик микросекунд, а micros() и millis()
// урезаются до 32 бит КАЖДЫЙ САМОСТОЯТЕЛЬНО, ровно как на железе:
// micros() переполняется через 71.6 минуты, millis() — через 49.7 суток.
//
// Раньше базой был 32-битный fakeMicros, а millis() считался как
// fakeMicros / 1000 — то есть переполнялся вместе с micros(), каждые
// 71.6 минуты. Проверить поведение на долгих интервалах было нельзя:
// возраст состояния скакал там, где на железе он растёт ровно.
extern uint64_t fakeTimeUs;
extern uint8_t  fakePinLevel[24];

inline uint32_t micros() { return (uint32_t)fakeTimeUs; }
inline uint32_t millis() { return (uint32_t)(fakeTimeUs / 1000ULL); }

// Сдвинуть время вперёд (используется тестами)
inline void advanceMicros(uint32_t us) { fakeTimeUs += us; }
inline void advanceMillis(uint32_t ms) { fakeTimeUs += (uint64_t)ms * 1000ULL; }

// ---- пины ----
inline void pinMode(uint8_t, uint8_t) {}
inline int digitalRead(uint8_t pin) { return pin < 24 ? fakePinLevel[pin] : HIGH; }
// Запись видна тестам через тот же fakePinLevel: иначе уровень на
// ноге реле проверить было бы нечем. Для входов это безвредно —
// туда тест пишет напрямую, а digitalWrite по ним не вызывается.
inline void digitalWrite(uint8_t pin, uint8_t level) {
  if (pin < 24) fakePinLevel[pin] = level;
}
inline uint8_t digitalPinToInterrupt(uint8_t pin) { return pin; }
inline void attachInterrupt(uint8_t, void (*)(), int) {}
inline void detachInterrupt(uint8_t) {}
inline void noInterrupts() {}
inline void interrupts() {}
inline void delay(uint32_t ms) { advanceMillis(ms); }
inline void yield() {}

// ---- минимальный Print, нужен Log.h ----
class Print {
public:
  virtual size_t write(uint8_t c) { (void)c; return 1; }
  virtual size_t write(const uint8_t *buf, size_t size) { (void)buf; return size; }

  size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t println() { return write((const uint8_t *)"\n", 1); }
  size_t println(const char *s) { print(s); return println(); }

  size_t printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return 0;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    return write((const uint8_t *)buf, (size_t)n);
  }
};

class SerialStub : public Print {
public:
  void begin(unsigned long) {}
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *, size_t size) override { return size; }
};

extern SerialStub Serial;

inline size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size) {
    size_t n = (len >= size) ? size - 1 : len;
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}

#endif
