/******************************************************************
 * ConfigStore.h - Сохранение/загрузка настроек и показаний в EEPROM
 ******************************************************************/

#ifndef ConfigStore_h
#define ConfigStore_h

#include <Arduino.h>
#include <EEPROM.h>
#include "Log.h"

// Магическое число для проверки валидности данных в EEPROM
#define EEPROM_MAGIC 0x5A4B

// Структура данных в EEPROM
struct ConfigData {
  uint16_t magic;          // Проверка валидности (должно быть EEPROM_MAGIC)
  
  // WiFi
  char wifiSSID[32];
  char wifiPass[64];
  
  // SMTP
  char smtpHost[32];
  uint16_t smtpPort;
  char smtpEmail[48];
  char smtpPass[48];
  char smtpRecipient[48];
  
  // Показания счётчиков (в м³)
  float meterHotM3;        // ГВС
  float meterColdM3;       // ХВС
  // m3 на импульс (настраивается в веб-интерфейсе, 1 m3 = 1000 L)
  float litersPerPulseHot;
  float litersPerPulseCold;
  
  // Адреса датчиков DS18B20 (сохраняются после калибровки)
  // [0]=ХВС, [1]=ГВС, [2]=Обратка, [3]=Подача
  uint8_t sensorAddrs[4][8];
  bool sensorAddrsValid;   // true если адреса записаны
  
  // Расписание отправки показаний
  uint8_t reportHour;       // 0-23
  uint8_t reportMinute;     // 0-59
  uint8_t reportSchedule;   // 0=daily, 1=weekly, 2=monthly
  uint8_t reportDay;        // 0=Sun..6=Sat (weekly), 1..31 (monthly)
  
  // Антидребезг герконов (мс). 0 = значения по умолчанию из MeterCounter.h
  uint16_t debounceClosedMs;  // минимальная длительность замыкания
  uint16_t debounceOpenMs;    // минимальная пауза перед новым замыканием

  // Резерв (размер структуры не менялся — старые настройки читаются как есть)
  uint8_t _reserved[8];

  // Имя устройства (hostname, SSID точки доступа, mDNS).
  // Пустое = сгенерировать из MAC: SmartWaterMeter-XXXXXX.
  //
  // Поле ДОБАВЛЕНО В КОНЕЦ структуры намеренно: смещения всех прежних
  // полей не изменились, поэтому существующие настройки в EEPROM
  // читаются как есть. В байтах за прежним размером лежит 0xFF —
  // санитайзер при загрузке превратит это в пустую строку.
  char deviceName[32];
};

class ConfigStore {
public:
  ConfigData data;
  
  void begin() {
    EEPROM.begin(sizeof(ConfigData));
    load();
  }
  
  void load() {
    EEPROM.get(0, data);
    if (data.magic != EEPROM_MAGIC) {
      Log.println(F("[EEPROM] Данные не найдены, инициализация по умолчанию"));
      resetDefaults();
      save();
    } else {
      Log.println(F("[EEPROM] Данные загружены"));
      // Пароль в лог не пишем — Telnet-консоль открыта без авторизации
      Log.printf("[EEPROM] SSID='%s', пароль задан: %s\n",
        data.wifiSSID, strlen(data.wifiPass) > 0 ? "да" : "нет");
    }
  }
  
  // Запись в EEPROM. Ядро ESP8266 само сравнивает буфер с текущим
  // содержимым (EEPROM.put -> memcmp) и commit() не трогает flash,
  // если данные не изменились — дополнительная защита от износа не нужна.
  void save() {
    data.magic = EEPROM_MAGIC;
    EEPROM.put(0, data);
    EEPROM.commit();
    Log.println(F("[EEPROM] Данные сохранены"));
  }
  
  void resetDefaults() {
    memset(&data, 0, sizeof(ConfigData));
    data.magic = EEPROM_MAGIC;
    
    // WiFi — пустые, будут запрошены через Captive Portal
    data.wifiSSID[0] = '\0';
    data.wifiPass[0] = '\0';
    
    // SMTP — значения по умолчанию
    strcpy(data.smtpHost, SMTP_HOST_DEFAULT);
    data.smtpPort = SMTP_PORT_DEFAULT;
    data.smtpEmail[0] = '\0';
    data.smtpPass[0] = '\0';
    data.smtpRecipient[0] = '\0';
    
    // Счётчики
    data.meterHotM3 = 0;
    data.meterColdM3 = 0;
    data.litersPerPulseHot = 1.0;
    data.litersPerPulseCold = 1.0;
    
    // Адреса датчиков — невалидны (будет использоваться SENSOR_ADDR из secrets.h)
    data.sensorAddrsValid = false;
    
    // Антидребезг герконов
    data.debounceClosedMs = 0;   // 0 = взять значения по умолчанию
    data.debounceOpenMs = 0;

    // Имя устройства — пустое, будет сгенерировано из MAC
    data.deviceName[0] = '\0';

    // Расписание отчёта по умолчанию: ежедневно в 09:00
    data.reportHour = 9;
    data.reportMinute = 0;
    data.reportSchedule = 0;  // daily
    data.reportDay = 0;
  }
  
  // Сохранить адрес датчика после калибровки
  void setSensorAddr(int index, const uint8_t* addr) {
    if (index >= 0 && index < 4) {
      memcpy(data.sensorAddrs[index], addr, 8);
      data.sensorAddrsValid = true;
      save();
    }
  }
  
  // Получить адрес датчика (из EEPROM если есть, иначе из secrets.h)
  const uint8_t* getSensorAddr(int index) {
    if (index >= 0 && index < 4) {
      if (data.sensorAddrsValid) {
        return data.sensorAddrs[index];
      }
      return SENSOR_ADDR[index];
    }
    return SENSOR_ADDR[0];
  }
  
};

#endif