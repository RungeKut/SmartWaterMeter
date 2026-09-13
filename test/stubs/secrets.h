/******************************************************************
 * secrets.h — значения для нативных тестов
 *
 * Настоящий secrets.h в .gitignore, поэтому тесты используют свой,
 * с заведомо невалидными адресами датчиков (как и заглушки в
 * secrets.h.example).
 ******************************************************************/

#ifndef SECRETS_TEST_H
#define SECRETS_TEST_H

#include <stdint.h>

#define AP_SSID_PREFIX "SmartWaterMeter"
#define AP_PASS_DEFAULT ""
#define SMTP_HOST_DEFAULT "smtp.example.com"
#define SMTP_PORT_DEFAULT 465
#define PIN_METER_HOT 14
#define PIN_METER_COLD 12
#define ONE_WIRE_BUS 0
#define LITERS_PER_PULSE 1.0

static const uint8_t SENSOR_ADDR[4][8] = {
  {0x28, 0, 0, 0, 0, 0, 0, 0},
  {0x28, 0, 0, 0, 0, 0, 0, 0},
  {0x28, 0, 0, 0, 0, 0, 0, 0},
  {0x28, 0, 0, 0, 0, 0, 0, 0}
};

#endif
