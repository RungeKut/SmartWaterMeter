/******************************************************************
 * FailsafeOTA.h - Безопасное OTA-обновление с автоматическим откатом
 * 
 * Механизм:
 *   1. После OTA прошивка загружается в неактивный слот
 *   2. ESP8266 перезагружается в новый слот
 *   3. При старте устанавливается флаг "ожидание подтверждения"
 *   4. Запускается таймер на 5 минут
 *   5. Если пользователь подтвердил прошивку (через веб-интерфейс
 *      или Telnet) — флаг снимается, всё ОК
 *   6. Если подтверждения нет 5 минут — ESP8266 переключается
 *      обратно на старый слот и перезагружается
 * 
 * Использование:
 *   1. FailsafeOTA::begin() в setup()
 *   2. FailsafeOTA::confirm() при успешном старте
 *   3. FailsafeOTA::isPending() — проверка, ждём ли подтверждения
 *   4. FailsafeOTA::remainingSec() — сколько осталось секунд до отката
 *   5. FailsafeOTA::handle() в loop()
 ******************************************************************/

#ifndef FailsafeOTA_h
#define FailsafeOTA_h

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <eboot_command.h>  // для eboot_command_write

class FailsafeOTA {
private:
  uint32_t _bootTime;         // millis() когда стартанули
  uint32_t _confirmTimeout;   // миллисекунды до отката (5 минут)
  bool _pending;              // true: ждём подтверждения
  bool _confirmed;            // true: пользователь подтвердил
  uint8_t _currentSlot;       // 0 или 1 — текущий слот
  
public:
  FailsafeOTA() : _bootTime(0), _confirmTimeout(300000),
                  _pending(false), _confirmed(false), _currentSlot(0) {}
  
  void begin() {
    _bootTime = millis();
    
    // Определяем текущий слот (0 = app0 @ 0x10000, 1 = app1 @ 0x101000)
    _currentSlot = getCurrentSlot();
    
    // Проверяем, есть ли флаг "ожидание подтверждения" в RTC memory
    // Используем слот 0 RTC memory (4 байта, сохраняются при soft-reset)
    uint32_t bootCount = readBootCount();
    
    if (bootCount == 0xDEAD) {
      // Первый запуск после OTA — ждём подтверждения
      _pending = true;
      _confirmed = false;
      Serial.printf("[Failsafe] New firmware detected in slot %d\n", _currentSlot);
      Serial.printf("[Failsafe] Confirm within %u seconds or rollback!\n",
        _confirmTimeout / 1000);
      
      // Увеличиваем счётчик, чтобы при следующей перезагрузке
      // без подтверждения мы знали что это повторная попытка
      writeBootCount(1);
    } else if (bootCount > 0 && bootCount < 10) {
      // Повторная загрузка без подтверждения — увеличиваем счётчик
      writeBootCount(bootCount + 1);
      
      if (bootCount >= 3) {
        // 3 перезагрузки без подтверждения — откатываемся
        Serial.println("[Failsafe] Too many boots without confirmation! Rolling back...");
        rollback();
        return;
      }
      
      _pending = true;
      _confirmed = false;
      Serial.printf("[Failsafe] Boot #%u without confirmation\n", bootCount);
    } else {
      // Нормальный запуск (не после OTA)
      _pending = false;
      _confirmed = true;
      writeBootCount(0);
    }
    
    Serial.printf("[Failsafe] Slot %d, pending=%d\n", _currentSlot, _pending);
  }
  
  // Вызывается из loop() — проверяет таймаут
  void handle() {
    if (!_pending || _confirmed) return;
    
    if (millis() - _bootTime > _confirmTimeout) {
      Serial.println("[Failsafe] Confirmation timeout! Rolling back...");
      rollback();
    }
  }
  
  // Подтверждение новой прошивки (вызывается из веб-интерфейса или Telnet)
  void confirm() {
    if (_pending) {
      _confirmed = true;
      _pending = false;
      writeBootCount(0);
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
  
private:
  // Определяем текущий слот по адресу запуска
  // Используем адрес любой функции в flash
  uint8_t getCurrentSlot() {
    // Адрес текущей функции в flash-памяти
    // app0: 0x00000..0x100000, app1: 0x100000..0x200000
    uint32_t addr = (uint32_t)(void*)&_bootTime & 0x3FFFFF;
    if (addr < 0x100000) return 0;
    return 1;
  }
  
  // Чтение/запись RTC memory (сохраняется при soft-reset)
  static void writeBootCount(uint32_t val) {
    ESP.rtcUserMemoryWrite(0, &val, sizeof(val));
  }
  
  static uint32_t readBootCount() {
    uint32_t val = 0;
    ESP.rtcUserMemoryRead(0, &val, sizeof(val));
    return val;
  }
  
  // Откат к предыдущему слоту
  void rollback() {
    Serial.println("[Failsafe] ROLLBACK: switching to other slot");
    
    // Сбрасываем счётчик загрузок в RTC
    writeBootCount(0);
    
    // ESP8266: переключаем слот через eboot command
    // Для 4m2m layout: app0=0x00000, app1=0x100000
    uint32_t targetAddr = 0x100000;  // по умолчанию app1
    
    // Если мы в app1 (0x100000+), переключаемся на app0 (0x10000)
    if (_currentSlot == 1) {
      targetAddr = 0x10000;
    }
    
    Serial.printf("[Failsafe] Rebooting to slot at 0x%05X\n", targetAddr);
    
    // Устанавливаем eboot command для принудительной загрузки
    // с указанного адреса
    eboot_command ebootCmd;
    ebootCmd.magic = EBOOT_MAGIC;
    ebootCmd.action = ACTION_LOAD_APP;
    ebootCmd.args[0] = targetAddr;
    ebootCmd.crc32 = 0;  // eboot_command_write рассчитает CRC
    eboot_command_write(&ebootCmd);
    
    delay(100);
    ESP.restart();
  }
};

#endif