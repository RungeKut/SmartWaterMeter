/******************************************************************
 * EEPROM.h — заглушка для нативных тестов
 *
 * Хранит данные в обычном массиве. Поведение ядра ESP8266
 * воспроизведено в части, важной для логики: put() сравнивает
 * содержимое и помечает изменение, commit() без изменений ничего
 * не делает.
 ******************************************************************/

#ifndef EEPROM_STUB_H
#define EEPROM_STUB_H

#include <stdint.h>
#include <string.h>

class EEPROMStub {
public:
  uint8_t buf[4096];
  bool dirty = false;
  uint32_t commits = 0;      // сколько раз реально «писали во flash»

  EEPROMStub() { memset(buf, 0xFF, sizeof(buf)); }

  void begin(size_t) {}
  void end() {}

  template <typename T> T &get(int addr, T &t) {
    memcpy(&t, buf + addr, sizeof(T));
    return t;
  }

  template <typename T> const T &put(int addr, const T &t) {
    if (memcmp(buf + addr, &t, sizeof(T)) != 0) {
      memcpy(buf + addr, &t, sizeof(T));
      dirty = true;
    }
    return t;
  }

  bool commit() {
    if (!dirty) return true;    // как в ядре: без изменений flash не трогаем
    dirty = false;
    commits++;
    return true;
  }
};

extern EEPROMStub EEPROM;

#endif
