/******************************************************************
 * TelnetSerial.h - Логи прошивки по WiFi (TCP:23) + консоль команд
 *
 * Регистрируется как приёмник Log (см. Log.h): всё, что прошивка
 * пишет через Log.print/printf/println, дублируется во все
 * подключённые Telnet-клиенты (до 4 одновременно).
 *
 * Низкоуровневый вывод SDK (сообщения WiFi-стека) в Telnet НЕ
 * попадает — он остаётся на USB-Serial. Писать в TCP-сокет из
 * контекста SDK небезопасно.
 *
 * Использование:
 *   1. TelnetSerial.begin() в setup() после WiFi
 *   2. Log.setSink(&telnet) в setup()
 *   3. TelnetSerial.handle() в loop()
 *   4. Подключение: telnet <ip-адрес> (или PuTTY, порт 23)
 *   5. Команды: help, status, reset, heap, uptime, confirm
 *
 * Безопасность: простое текстовое соединение, без пароля.
 * Только для доверенной сети!
 ******************************************************************/

#ifndef TelnetSerial_h
#define TelnetSerial_h

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include "Log.h"

// Внешняя функция для подтверждения прошивки (реализована в Wemos_Mini.ino)
extern void telnetConfirmFirmware();

#define TELNET_MAX_CLIENTS 4

class TelnetSerial : public LogSink {
private:
  WiFiServer _server;
  WiFiClient _clients[TELNET_MAX_CLIENTS];
  uint32_t _lastCheck;

  // Буфер строки — накапливаем символы до \n, чтобы отправлять целиком
  char _lineBuf[512];
  uint16_t _linePos;

  void flushLine() {
    _lineBuf[_linePos] = '\0';
    for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
      if (_clients[i] && _clients[i].connected()) {
        _clients[i].print(_lineBuf);
      }
    }
    _linePos = 0;
  }

public:
  TelnetSerial() : _server(23), _lastCheck(0), _linePos(0) {}

  // Приёмник Log: накапливаем строку и отправляем по \n
  void logWrite(const uint8_t *buf, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      char c = (char)buf[i];
      if (_linePos < sizeof(_lineBuf) - 1) {
        _lineBuf[_linePos++] = c;
      }
      if (c == '\n' || _linePos >= sizeof(_lineBuf) - 1) {
        flushLine();
      }
    }
  }

  void begin() {
    _server.begin();
    _server.setNoDelay(true);
    Log.println("[Telnet] TCP:23 ready (firmware log mirrored)");
  }

  void handle() {
    // Принимаем новых клиентов
    if (_server.hasClient()) {
      WiFiClient client = _server.accept();

      bool accepted = false;
      for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
        if (!_clients[i] || !_clients[i].connected()) {
          if (_clients[i]) _clients[i].stop();
          _clients[i] = client;
          _clients[i].println("\r\n=== " __DATE__ " " __TIME__ " ===");
          _clients[i].println("SmartWaterMeter Telnet Console");
          _clients[i].println("Type 'help' for commands\r\n");
          accepted = true;
          Log.printf("[Telnet] Client %d connected\n", i);
          break;
        }
      }

      if (!accepted) {
        client.println("Too many connections");
        client.stop();
      }
    }

    // Обслуживаем клиентов (читаем команды)
    for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
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
      for (int i = 0; i < TELNET_MAX_CLIENTS; i++) {
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
      c.println("  confirm - confirm new firmware after OTA");
    } else if (cmd == "status") {
      bool sta = (WiFi.status() == WL_CONNECTED);
      c.printf("WiFi: %s\r\n", sta ? "connected" : "disconnected");
      if (sta) {
        c.printf("IP: %s\r\n", WiFi.localIP().toString().c_str());
      }
      if (WiFi.getMode() & WIFI_AP) {
        c.printf("AP: %s\r\n", WiFi.softAPIP().toString().c_str());
      }
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

#endif
