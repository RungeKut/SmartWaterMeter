/******************************************************************
 * TelnetSerial.h - Дублирование Serial в Telnet (TCP:23)
 * 
 * Использует os_install_putc1() из SDK ESP8266 для перехвата
 * ВСЕГО вывода — Serial.print, Serial.printf, printf — и
 * дублирования его во все подключённые Telnet-клиенты.
 * 
 * Использование:
 *   1. TelnetSerial.begin() в setup() после WiFi
 *   2. TelnetSerial.handle() в loop()
 *   3. Подключение: telnet <ip-адрес> (или PuTTY, порт 23)
 *   4. Команды: help, status, reset, heap, uptime
 * 
 * Безопасность: простое текстовое соединение, без пароля.
 * Только для доверенной сети!
 ******************************************************************/

#ifndef TelnetSerial_h
#define TelnetSerial_h

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

// Хук на putc1 из SDK ESP8266 — перехватывает каждый выводимый символ
extern "C" void os_install_putc1(void (*putc1)(char c));
static void (*_telnetOriginalPutc)(char c) = nullptr;

// Внешняя функция для подтверждения прошивки (реализована в Wemos_Mini.ino)
extern void telnetConfirmFirmware();

class TelnetSerial {
private:
  WiFiServer _server;
  WiFiClient _clients[4];  // до 4 одновременных клиентов
  uint32_t _lastCheck;
  
  // Буфер строки — накапливаем символы до \n, чтобы отправлять целиком
  char _lineBuf[512];
  uint16_t _linePos;
  
  static TelnetSerial* _instance;
  
  static void putcHook(char c) {
    // Оригинальный вывод в UART (Serial)
    if (_telnetOriginalPutc) _telnetOriginalPutc(c);
    
    // Дублирование в Telnet
    if (_instance) {
      _instance->broadcastChar(c);
    }
  }
  
  void broadcastChar(char c) {
    // Накапливаем в буфер
    if (_linePos < sizeof(_lineBuf) - 1) {
      _lineBuf[_linePos++] = c;
    }
    
    // По \n или переполнению — отправляем
    if (c == '\n' || _linePos >= sizeof(_lineBuf) - 1) {
      _lineBuf[_linePos] = '\0';
      for (int i = 0; i < 4; i++) {
        if (_clients[i] && _clients[i].connected()) {
          _clients[i].print(_lineBuf);
        }
      }
      _linePos = 0;
    }
  }
  
public:
  TelnetSerial() : _server(23), _lastCheck(0), _linePos(0) {}
  
  void begin() {
    _instance = this;
    
    // Устанавливаем хук на putc1 — os_install_putc1 НЕ возвращает предыдущий,
    // поэтому сохраняем текущий через отдельную переменную
    os_install_putc1(putcHook);
    
    _server.begin();
    _server.setNoDelay(true);
    Serial.println("[Telnet] TCP:23 ready (all output duplicated)");
  }
  
  void handle() {
    // Принимаем новых клиентов
    if (_server.hasClient()) {
      WiFiClient client = _server.accept();
      
      bool accepted = false;
      for (int i = 0; i < 4; i++) {
        if (!_clients[i] || !_clients[i].connected()) {
          if (_clients[i]) _clients[i].stop();
          _clients[i] = client;
          _clients[i].println("\r\n=== " __DATE__ " " __TIME__ " ===");
          _clients[i].println("SmartWaterMeter Telnet Console");
          _clients[i].println("Type 'help' for commands\r\n");
          accepted = true;
          Serial.printf("[Telnet] Client %d connected\n", i);
          break;
        }
      }
      
      if (!accepted) {
        client.println("Too many connections");
        client.stop();
      }
    }
    
    // Обслуживаем клиентов (читаем команды)
    for (int i = 0; i < 4; i++) {
      if (_clients[i] && _clients[i].connected()) {
        if (_clients[i].available()) {
          String cmd = _clients[i].readStringUntil('\n');
          cmd.trim();
          handleCommand(i, cmd);
        }
      } else if (_clients[i]) {
        _clients[i].stop();
      }
    }
    
    // Раз в 30 секунд чистим мёртвые соединения
    uint32_t now = millis();
    if (now - _lastCheck > 30000) {
      _lastCheck = now;
      for (int i = 0; i < 4; i++) {
        if (_clients[i] && !_clients[i].connected()) {
          _clients[i].stop();
        }
      }
    }
  }
  
private:
  void handleCommand(int clientIdx, const String &cmd) {
    WiFiClient &c = _clients[clientIdx];
    
    if (cmd == "help") {
      c.println("Available commands:");
      c.println("  help    - this message");
      c.println("  status  - show system status");
      c.println("  reset   - restart the device");
      c.println("  heap    - show free heap");
      c.println("  uptime  - show uptime");
    } else if (cmd == "status") {
      c.printf("WiFi: %s\r\n", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
      c.printf("IP: %s\r\n", WiFi.localIP().toString().c_str());
      c.printf("AP: %s\r\n", WiFi.softAPIP().toString().c_str());
      c.printf("Uptime: %lu seconds\r\n", millis() / 1000);
      c.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
    } else if (cmd == "reset" || cmd == "restart") {
      c.println("Restarting...");
      delay(100);
      ESP.restart();
    } else if (cmd == "heap") {
      c.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
    } else if (cmd == "uptime") {
      c.printf("Uptime: %lu seconds\r\n", millis() / 1000);
    } else if (cmd == "confirm") {
      telnetConfirmFirmware();
      c.println("Firmware confirmed!");
    } else if (cmd.length() > 0) {
      c.printf("Unknown: '%s'. Type 'help'\r\n", cmd.c_str());
    }
  }
};

// Статический указатель на экземпляр (определение)
TelnetSerial* TelnetSerial::_instance = nullptr;

#endif