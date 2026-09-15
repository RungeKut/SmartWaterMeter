/******************************************************************
 * ConfigStore.h - Сохранение/загрузка настроек и показаний в EEPROM
 ******************************************************************/

#ifndef ConfigStore_h
#define ConfigStore_h

#include <Arduino.h>
#include <EEPROM.h>
#include <math.h>
#include "Log.h"

// Пороги защиты фильтра по умолчанию. Объявлены здесь, а не в
// FilterGuard.h: ConfigStore нужен раньше и не должен зависеть от
// модуля, который сам подключает ConfigStore.
#ifndef FILTER_TEMP_ON_DEFAULT
#define FILTER_TEMP_ON_DEFAULT   35.0f
#define FILTER_TEMP_OFF_DEFAULT  30.0f
#endif

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

  // MQTT (Home Assistant autodiscovery).
  // Снова добавлено В КОНЕЦ структуры — смещения прежних полей не
  // меняются, существующие настройки читаются как есть.
  bool mqttEnabled;
  char mqttHost[48];
  uint16_t mqttPort;
  char mqttUser[32];
  char mqttPass[32];
  uint16_t mqttIntervalSec;   // период публикации состояния, 0 = по умолчанию

  // Защита осмотического фильтра от подмеса ГВС в ХВС.
  // И снова В КОНЕЦ структуры — смещения прежних полей не меняются,
  // существующие настройки читаются как есть. Признак нетронутой
  // flash здесь — NaN во filterTempOnC (0xFFFFFFFF как float).
  bool  filterEnabled;
  float filterTempOnC;         // верхний порог: реле включается, клапан закрыт
  float filterTempOffC;        // нижний порог: реле отключается
  bool  filterRelayActiveLow;  // управляющий уровень реле
  bool  filterNotifyMqtt;      // событие уходит в Home Assistant
  bool  filterNotifyEmail;     // событие уходит письмом
  bool  filterMetricsEnabled;  // метрики фильтра отдаются в /metrics

  // Общий выключатель плановых писем. SMTP при этом остаётся
  // настроенным: Test Email и алерты фильтра продолжают работать.
  bool  reportEnabled;
};

class ConfigStore {
public:
  ConfigData data;
  
  void begin() {
    EEPROM.begin(sizeof(ConfigData));
    load();
  }
  
  // Есть ли нулевой терминатор внутри поля.
  static bool isTerminated(const char *s, size_t size) {
    for (size_t i = 0; i < size; i++) if (s[i] == 0) return true;
    return false;
  }

  // ОБЯЗАТЕЛЬНО для любого поля, добавленного в конец структуры.
  //
  // При первой загрузке после расширения ConfigData новые поля читаются
  // из неинициализированной flash, где лежит 0xFF. Для bool это true,
  // для uint16_t — 65535, для строки — мусор без терминатора. Магическое
  // число при этом валидно (оно в начале структуры), поэтому сброса на
  // defaults не происходит и мусор уходит в работу как настройка.
  //
  // Ровно так и случилось: MQTT оказался «включён» с мусорным хостом и
  // портом 65535, и устройство принялось долбиться в несуществующий
  // брокер каждые 30 секунд.
  void sanitizeNewFields() {
    bool virgin = (data.mqttPort == 0xFFFF)
               || !isTerminated(data.mqttHost, sizeof(data.mqttHost))
               || !isTerminated(data.mqttUser, sizeof(data.mqttUser))
               || !isTerminated(data.mqttPass, sizeof(data.mqttPass));

    if (virgin) {
      Log.println(F("[EEPROM] Блок MQTT не инициализирован -> значения по умолчанию"));
      data.mqttEnabled = false;
      data.mqttHost[0] = '\0';
      data.mqttUser[0] = '\0';
      data.mqttPass[0] = '\0';
      data.mqttPort = 1883;
      data.mqttIntervalSec = 0;
      return;
    }

    // Поля валидны, но отдельные значения могли прийти из 0xFF
    if (data.mqttPort == 0) data.mqttPort = 1883;
    if (data.mqttIntervalSec == 0xFFFF) data.mqttIntervalSec = 0;
    data.mqttEnabled = (data.mqttEnabled != 0);
  }

  // Второй блок, добавленный в конец структуры, — защита фильтра.
  // Проверяется отдельно от MQTT: у пользователя, прошившегося на
  // промежуточной версии, блок MQTT уже инициализирован, а этот ещё нет.
  //
  // Признак нетронутой flash — NaN: 0xFFFFFFFF, прочитанные как float,
  // дают именно его. Сравнение через !(v > 0) ловит и NaN, и ноль.
  void sanitizeFilterFields() {
    if (isnan(data.filterTempOnC) || isnan(data.filterTempOffC)
        || !(data.filterTempOnC > 0.0f)) {
      Log.println(F("[EEPROM] Блок защиты фильтра не инициализирован -> по умолчанию"));
      data.filterEnabled = false;
      data.filterTempOnC = FILTER_TEMP_ON_DEFAULT;
      data.filterTempOffC = FILTER_TEMP_OFF_DEFAULT;
      data.filterRelayActiveLow = false;
      data.filterNotifyMqtt = true;
      data.filterNotifyEmail = true;
      data.filterMetricsEnabled = true;
      // true, а не false: у тех, кто уже пользуется плановыми
      // отчётами, обновление не должно их молча отключить.
      data.reportEnabled = true;
      return;
    }

    data.filterEnabled = (data.filterEnabled != 0);
    data.filterRelayActiveLow = (data.filterRelayActiveLow != 0);
    data.filterNotifyMqtt = (data.filterNotifyMqtt != 0);
    data.filterNotifyEmail = (data.filterNotifyEmail != 0);
    data.filterMetricsEnabled = (data.filterMetricsEnabled != 0);
    data.reportEnabled = (data.reportEnabled != 0);
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
    sanitizeNewFields();
    sanitizeFilterFields();
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

    // MQTT выключен по умолчанию
    data.mqttEnabled = false;
    data.mqttHost[0] = '\0';
    data.mqttPort = 1883;
    data.mqttUser[0] = '\0';
    data.mqttPass[0] = '\0';
    data.mqttIntervalSec = 0;

    // Защита фильтра выключена по умолчанию: без реле на ноге она
    // бессмысленна, а включать её за пользователя нельзя
    data.filterEnabled = false;
    data.filterTempOnC = FILTER_TEMP_ON_DEFAULT;
    data.filterTempOffC = FILTER_TEMP_OFF_DEFAULT;
    data.filterRelayActiveLow = false;
    data.filterNotifyMqtt = true;
    data.filterNotifyEmail = true;
    data.filterMetricsEnabled = true;

    // Расписание отчёта по умолчанию: ежедневно в 09:00, отправка включена
    data.reportEnabled = true;
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