/******************************************************************
 * FailsafeOTA.h - Безопасное OTA-обновление с автоматическим откатом
 *
 * Механизм:
 *   1. Пользователь инициирует OTA через WiFi (ArduinoOTA)
 *   2. OTA пишет прошивку в неактивный слот B (0x101000)
 *   3. После завершения вызывается updateFirmware — выставляется
 *      флаг "ожидание подтверждения" в RTC memory
 *   4. ESP8266 перезагружается
 *   5. При старте FailsafeOTA видит флаг и запускает таймер на 5 минут
 *   6. Если пользователь подтвердил (веб-интерфейс / Telnet) — флаг снимается
 *   7. Если подтверждения нет:
 *      a) 3 перезагрузки без подтверждения → принудительный откат
 *      б) 5 минут без подтверждения → принудительный откат
 *   8. Откат: eboot command переключает загрузку на старый слот + reboot
 *
 * Раскладка flash для 4m2m (eagle.flash.4m2m.ld):
 *   Слот A (app0): физический 0x000000..0x0FFFFF, образ по 0x10000
 *   Слот B (app1): физический 0x100000..0x1FFFFF, образ по 0x101000
 *   LittleFS:       0x200000..0x3FA000
 *   EEPROM:         0x3FB000
 *
 * Использование:
 *   1. FailsafeOTA::begin() в setup()
 *   2. FailsafeOTA::updateFirmware() в ArduinoOTA.onEnd()
 *   3. FailsafeOTA::confirm() при успешном старте
 *   4. FailsafeOTA::isPending() — проверка, ждём ли подтверждения
 *   5. FailsafeOTA::remainingSec() — сколько осталось секунд до отката
 *   6. FailsafeOTA::handle() в loop()
 ******************************************************************/

#ifndef FailsafeOTA_h
#define FailsafeOTA_h

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <eboot_command.h>

// Адреса слотов для eagle.flash.4m2m.ld
// Слот A: image starts at 0x10000 (after bootloader), maps to 0x40200000
// Слот B: image starts at 0x101000, maps to 0x40300000
#define SLOT_A_IMAGE_ADDR   0x10000   // app0 — начальный слот
#define SLOT_B_IMAGE_ADDR   0x101000  // app1 — OTA-слот
#define SLOT_SIZE           0x100000  // 1MB per slot
#define FLASH_MASK          0x3FFFFF  // mask for 4MB flash (22 bits)

// Флаги RTC memory (сохраняются при soft-reset)
#define RTC_MAGIC_FIRST_BOOT  0xDEAD0001  // первый старт после OTA
#define RTC_MAGIC_RETRY       0xDEAD0002  // повторный старт без подтверждения
#define RTC_MAGIC_NORMAL      0x00000000  // нормальный старт

class FailsafeOTA {
private:
  uint32_t _bootTime;
  uint32_t _confirmTimeout;     // мс до отката (5 минут)
  bool _pending;                // ждём подтверждения
  bool _confirmed;              // пользователь подтвердил
  uint8_t _currentSlot;         // 0 = app0, 1 = app1

  // Определяем текущий слот по физическому адресу в flash
  // Используем адрес любой функции — он указывает в IROM (0x402xxxxx)
  static uint8_t getCurrentSlot() {
    uint32_t addr = (uint32_t)(void*)&getCurrentSlot & FLASH_MASK;
    if (addr < SLOT_B_IMAGE_ADDR) return 0;  // app0
    return 1;                                 // app1
  }

  // Чтение/запись RTC memory (слот 0)
  static void writeRTC(uint32_t val) {
    ESP.rtcUserMemoryWrite(0, &val, sizeof(val));
  }

  static uint32_t readRTC() {
    uint32_t val = 0;
    ESP.rtcUserMemoryRead(0, &val, sizeof(val));
    return val;
  }

  // Откат — переключение на ДРУГОЙ (противоположный) слот
  void rollback() {
    Serial.println("[Failsafe] ROLLBACK: switching to other slot");

    // Сбрасываем RTC
    writeRTC(RTC_MAGIC_NORMAL);

    // Определяем целевой слот (противоположный текущему)
    uint32_t targetAddr;
    if (_currentSlot == 0) {
      // Сейчас в app0 → переключаемся на app1 (0x101000)
      targetAddr = SLOT_B_IMAGE_ADDR;
      Serial.printf("[Failsafe] Slot A -> Slot B (0x%05X)\n", targetAddr);
    } else {
      // Сейчас в app1 → переключаемся на app0 (0x10000)
      targetAddr = SLOT_A_IMAGE_ADDR;
      Serial.printf("[Failsafe] Slot B -> Slot A (0x%05X)\n", targetAddr);
    }

    // Устанавливаем eboot command для принудительной загрузки
    eboot_command ebootCmd;
    ebootCmd.magic = EBOOT_MAGIC;
    ebootCmd.action = ACTION_LOAD_APP;
    ebootCmd.args[0] = targetAddr;
    eboot_command_write(&ebootCmd);

    Serial.println("[Failsafe] eboot command written, restarting...");
    delay(100);
    ESP.restart();
  }

public:
  FailsafeOTA() : _bootTime(0), _confirmTimeout(300000),
                  _pending(false), _confirmed(false), _currentSlot(0) {}

  void begin() {
    _bootTime = millis();
    _currentSlot = getCurrentSlot();
    uint32_t rtcVal = readRTC();

    Serial.printf("[Failsafe] Slot %d, RTC=0x%08X\n", _currentSlot, rtcVal);

    if (rtcVal == RTC_MAGIC_FIRST_BOOT) {
      // Первый запуск после OTA
      _pending = true;
      _confirmed = false;
      Serial.printf("[Failsafe] New firmware detected in slot %d\n", _currentSlot);
      Serial.printf("[Failsafe] Confirm within %u seconds or rollback!\n",
        _confirmTimeout / 1000);

      // Переключаем на режим retry — при следующей перезагрузке
      // без подтверждения будем считать это повторной попыткой
      writeRTC(RTC_MAGIC_RETRY);

    } else if (rtcVal == RTC_MAGIC_RETRY) {
      // Повторная загрузка без подтверждения
      // Если это уже не первый retry — откатываемся
      // (защита от bootloop: если устройство циклически ребутится,
      //  RTC сохраняет значение, и мы откатимся)
      Serial.println("[Failsafe] Boot without confirmation — rolling back!");
      rollback();
      return;

    } else {
      // Нормальный запуск (RTC_MAGIC_NORMAL или любое другое значение)
      _pending = false;
      _confirmed = true;
      writeRTC(RTC_MAGIC_NORMAL);
    }
  }

  // Вызывается из loop() — проверяет таймаут
  void handle() {
    if (!_pending || _confirmed) return;

    if (millis() - _bootTime > _confirmTimeout) {
      Serial.println("[Failsafe] Confirmation timeout! Rolling back...");
      rollback();
    }
  }

  // ВЫЗЫВАТЬ ПОСЛЕ УСПЕШНОГО OTA (в ArduinoOTA.onEnd)
  // Устанавливает флаг, что после перезагрузки нужно ждать подтверждения
  void updateFirmware() {
    writeRTC(RTC_MAGIC_FIRST_BOOT);
    Serial.println("[Failsafe] OTA done — flag set, will wait for confirmation on next boot");
  }

  // Подтверждение новой прошивки (вызывается из веб-интерфейса или Telnet)
  void confirm() {
    if (_pending) {
      _confirmed = true;
      _pending = false;
      writeRTC(RTC_MAGIC_NORMAL);
      Serial.println("[Failsafe] Firmware confirmed!");
    }
  }

  bool isPending() { return _pending && !_confirmed; }

  uint32_t remainingSec() {
    if (!_pending || _confirmed) return 0;
    uint32_t elapsed = (millis() - _bootTime) / 1000;
    uint32_t total = _confirmTimeout / 1000;
    return (elapsed < total) ? (total - elapsed) : 0;
  }

  uint8_t getSlot() { return _currentSlot; }

  // Принудительный откат на другой слот (можно вызвать вручную)
  void forceRollback() {
    Serial.println("[Failsafe] Force rollback requested");
    rollback();
  }
};

#endif