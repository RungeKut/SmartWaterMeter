/******************************************************************
 * MqttClient.h - MQTT + автодискавери Home Assistant
 *
 * ЗАЧЕМ, ЕСЛИ УЖЕ ЕСТЬ /api.json
 *
 * REST требует, чтобы Home Assistant опрашивал устройство и чтобы
 * сущности были описаны руками в YAML. MQTT переворачивает схему:
 * устройство само публикует данные в брокер и само себя описывает —
 * сущности появляются в HA без единой строки конфигурации.
 *
 * REST и /metrics при этом остаются: они не мешают, работают без
 * брокера и удобны для отладки и Prometheus.
 *
 * ПОЧЕМУ AsyncMqttClient, А НЕ PubSubClient
 *
 * PubSubClient::connect() блокирует loop() до таймаута сокета. Ровно
 * на этом обжёгся NTPClient (см. pitfalls): недоступный сервер — и
 * устройство тормозит постоянно. AsyncMqttClient работает поверх
 * ESPAsyncTCP, который в проекте уже есть, и не блокирует ничего.
 *
 * СТРУКТУРА ТЕМ
 *
 *   smartwatermeter/<MAC3>/state     все показания одним JSON
 *   smartwatermeter/<MAC3>/status    online / offline (LWT)
 *   smartwatermeter/<MAC3>/cmd/+     команды от Home Assistant
 *   homeassistant/<тип>/swm<MAC3>/<объект>/config   автодискавери
 *
 * Базовая тема и уникальные идентификаторы строятся от MAC, а НЕ от
 * имени устройства: имя пользователь может поменять, и тогда HA
 * потерял бы связь с уже созданными сущностями. Понятное имя уходит
 * в поле device.name автодискавери.
 ******************************************************************/

#ifndef MqttClient_h
#define MqttClient_h

#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "Log.h"
#include "ConfigStore.h"

#define MQTT_RECONNECT_MS      30000   // пауза между попытками подключения
#define MQTT_DEFAULT_INTERVAL  5       // период публикации состояния, с
#define MQTT_DISCOVERY_PREFIX  "homeassistant"

// Публикация автодискавери разнесена по циклам loop(): 17 сообщений
// подряд по ~400 байт легко исчерпали бы буферы TCP на 20 КБ кучи.
#define MQTT_DISCOVERY_STEP_MS 120

class MqttClient {
public:
  // Что устройство отдаёт наружу. Заполняется в Wemos_Mini.ino,
  // чтобы модуль не зависел от остального проекта.
  struct Payload {
    float tempCold, tempHot, tempSupply, tempReturn;
    bool  tempColdOk, tempHotOk, tempSupplyOk, tempReturnOk;
    float meterHotM3, meterColdM3;
    bool  reedHotClosed, reedColdClosed;
    int32_t rssi;
    uint32_t uptimeSec;
    uint32_t freeHeap;
    uint16_t debounceClosedMs, debounceOpenMs;

    // Защита осмотического фильтра
    bool  filterEnabled, filterClosed, filterSensorLost;
    uint32_t filterTrips;
    float filterTempOnC, filterTempOffC;
  };

  typedef void (*CommandCallback)(const String &cmd, const String &value);

private:
  AsyncMqttClient _mqtt;
  ConfigStore *_config;
  CommandCallback _cb;

  char _id[16];          // swm93C195
  char _base[40];        // smartwatermeter/93C195
  char _friendly[32];    // имя для карточки устройства в HA
  char _swVersion[24];

  bool _enabled;
  bool _connected;
  uint32_t _lastAttempt;
  uint32_t _lastPublish;
  int _discoveryStep;        // -1 = не идёт, иначе индекс сущности
  uint32_t _lastDiscoveryMs;

  uint32_t _publishCount;
  uint32_t _reconnects;

  static MqttClient *_instance;

  uint32_t intervalMs() const {
    uint16_t s = _config ? _config->data.mqttIntervalSec : 0;
    if (s == 0) s = MQTT_DEFAULT_INTERVAL;
    return (uint32_t)s * 1000UL;
  }

  // ---- автодискавери ----

  // Описание одной сущности. Держим в PROGMEM-подобных константах,
  // собирая JSON на лету: хранить 17 готовых payload в RAM нельзя.
  struct EntityDef {
    const char *component;   // sensor / binary_sensor / button / number
    const char *object;      // water_hot
    const char *name;        // Вода ГВС
    const char *valueKey;    // путь в state JSON, пусто для button
    const char *deviceClass;
    const char *stateClass;
    const char *unit;
    const char *category;    // diagnostic / config / nullptr
    const char *command;     // суффикс cmd-темы для button/number
    const char *extra;       // доп. поля JSON (диапазон для number)
    bool filterOnly;         // публиковать только когда фильтр показан в HA
  };

  static const EntityDef *entities(int &count) {
    static const EntityDef defs[] = {
      // --- счётчики воды ---
      // Единица строго "m³": для device_class water Home Assistant
      // принимает только m³, L, gal, ft³, CCF. Написание "m3" он
      // отвергает, и сущность не попадает в статистику и панель «Вода».
      { "sensor", "water_hot", "Вода ГВС", "meters.hot_m3",
        "water", "total_increasing", "m³", nullptr, nullptr, nullptr },
      { "sensor", "water_cold", "Вода ХВС", "meters.cold_m3",
        "water", "total_increasing", "m³", nullptr, nullptr, nullptr },

      // --- температуры ---
      { "sensor", "temp_cold", "Температура ХВС", "temperatures.cold",
        "temperature", "measurement", "°C", nullptr, nullptr, nullptr },
      { "sensor", "temp_hot", "Температура ГВС", "temperatures.hot",
        "temperature", "measurement", "°C", nullptr, nullptr, nullptr },
      { "sensor", "temp_supply", "Подача отопления", "temperatures.supply",
        "temperature", "measurement", "°C", nullptr, nullptr, nullptr },
      { "sensor", "temp_return", "Обратка отопления", "temperatures.return",
        "temperature", "measurement", "°C", nullptr, nullptr, nullptr },

      // --- герконы ---
      { "binary_sensor", "reed_hot", "Геркон ГВС", "reed.hot",
        nullptr, nullptr, nullptr, "diagnostic", nullptr, nullptr },
      { "binary_sensor", "reed_cold", "Геркон ХВС", "reed.cold",
        nullptr, nullptr, nullptr, "diagnostic", nullptr, nullptr },

      // --- диагностика ---
      { "sensor", "rssi", "WiFi RSSI", "sys.rssi",
        "signal_strength", "measurement", "dBm", "diagnostic", nullptr, nullptr },
      { "sensor", "uptime", "Аптайм", "sys.uptime",
        "duration", "measurement", "s", "diagnostic", nullptr, nullptr },
      { "sensor", "free_heap", "Свободная память", "sys.heap",
        "data_size", "measurement", "B", "diagnostic", nullptr, nullptr },

      // --- управление ---
      { "button", "restart", "Перезагрузить", nullptr,
        "restart", nullptr, nullptr, "config", "restart", nullptr },
      { "button", "confirm_ota", "Подтвердить прошивку", nullptr,
        nullptr, nullptr, nullptr, "config", "confirm_ota", nullptr },

      { "number", "debounce_closed", "Антидребезг: замыкание", "cfg.debounce_closed",
        nullptr, nullptr, "ms", "config", "debounce_closed",
        "\"min\":0,\"max\":5000,\"step\":1,\"mode\":\"box\"" },
      { "number", "debounce_open", "Антидребезг: размыкание", "cfg.debounce_open",
        nullptr, nullptr, "ms", "config", "debounce_open",
        "\"min\":0,\"max\":60000,\"step\":1,\"mode\":\"box\"", false },

      // --- защита осмотического фильтра ---
      // device_class problem: в HA "ON" у такой сущности подсвечивается
      // как неисправность, а это ровно наш случай — идёт подмес ГВС.
      { "binary_sensor", "filter_closed", "Фильтр перекрыт", "filter.closed",
        "problem", nullptr, nullptr, nullptr, nullptr, nullptr, true },
      { "binary_sensor", "filter_sensor_lost", "Датчик ХВС потерян", "filter.sensor_lost",
        "problem", nullptr, nullptr, "diagnostic", nullptr, nullptr, true },
      { "sensor", "filter_trips", "Срабатываний защиты", "filter.trips",
        nullptr, "total_increasing", nullptr, "diagnostic", nullptr, nullptr, true },
      { "number", "filter_temp_on", "Порог закрытия", "cfg.filter_on",
        "temperature", nullptr, "°C", "config", "filter_temp_on",
        "\"min\":20,\"max\":90,\"step\":0.5,\"mode\":\"box\"", true },
      { "number", "filter_temp_off", "Порог открытия", "cfg.filter_off",
        "temperature", nullptr, "°C", "config", "filter_temp_off",
        "\"min\":5,\"max\":85,\"step\":0.5,\"mode\":\"box\"", true }
    };
    count = sizeof(defs) / sizeof(defs[0]);
    return defs;
  }

  // Снять сущность: пустая retained-нагрузка удаляет её из Home
  // Assistant. Нужно, когда пользователь выключил показ фильтра в HA —
  // иначе в карточке навсегда осталась бы висеть недоступная сущность.
  void retractDiscoveryEntity(const EntityDef &e) {
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + e.component + "/"
                 + _id + "/" + e.object + "/config";
    _mqtt.publish(topic.c_str(), 0, true, "");
  }

  void publishDiscoveryEntity(const EntityDef &e) {
    // Сокращённые ключи (stat_t вместо state_topic) HA понимает наравне
    // с полными, а на ESP экономят сотни байт на каждом сообщении.
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + e.component + "/"
                 + _id + "/" + e.object + "/config";

    String p = "{";
    p += "\"name\":\"" + String(e.name) + "\",";
    p += "\"uniq_id\":\"" + String(_id) + "_" + e.object + "\",";
    p += "\"avty_t\":\"" + String(_base) + "/status\",";

    if (e.valueKey) {
      p += "\"stat_t\":\"" + String(_base) + "/state\",";
      p += "\"val_tpl\":\"{{ value_json." + String(e.valueKey) + " }}\",";
    }
    if (e.command) {
      p += "\"cmd_t\":\"" + String(_base) + "/cmd/" + e.command + "\",";
    }
    if (e.deviceClass) p += "\"dev_cla\":\"" + String(e.deviceClass) + "\",";
    if (e.stateClass)  p += "\"stat_cla\":\"" + String(e.stateClass) + "\",";
    if (e.unit)        p += "\"unit_of_meas\":\"" + String(e.unit) + "\",";
    if (e.category)    p += "\"ent_cat\":\"" + String(e.category) + "\",";
    if (e.extra)       p += String(e.extra) + ",";

    // Общий блок устройства — по нему HA группирует все сущности
    // в одну карточку
    p += "\"dev\":{\"ids\":[\"" + String(_id) + "\"],";
    p += "\"name\":\"" + String(_friendly) + "\",";
    p += "\"mdl\":\"Wemos D1 mini\",\"mf\":\"DIY\",";
    p += "\"sw\":\"" + String(_swVersion) + "\"}}";

    // retain = true: HA восстановит сущности после перезапуска,
    // не дожидаясь следующей загрузки устройства
    _mqtt.publish(topic.c_str(), 0, true, p.c_str());
  }

  void onConnected() {
    _connected = true;
    _reconnects++;
    Log.printf("[MQTT] Connected to %s:%u\n",
      _config->data.mqttHost, _config->data.mqttPort);

    String statusTopic = String(_base) + "/status";
    _mqtt.publish(statusTopic.c_str(), 1, true, "online");

    String cmdTopic = String(_base) + "/cmd/+";
    _mqtt.subscribe(cmdTopic.c_str(), 1);

    _discoveryStep = 0;          // начинаем публиковать описания
    _lastDiscoveryMs = millis();
    _lastPublish = 0;            // и сразу состояние
  }

public:
  MqttClient()
    : _config(nullptr), _cb(nullptr), _enabled(false), _connected(false),
      _lastAttempt(0), _lastPublish(0), _discoveryStep(-1),
      _lastDiscoveryMs(0), _publishCount(0), _reconnects(0) {
    _id[0] = _base[0] = _friendly[0] = _swVersion[0] = '\0';
  }

  void begin(ConfigStore *config, const uint8_t *mac,
             const char *friendlyName, const char *swVersion,
             CommandCallback cb) {
    _config = config;
    _cb = cb;
    _instance = this;

    snprintf(_id, sizeof(_id), "swm%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(_base, sizeof(_base), "smartwatermeter/%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    strlcpy(_friendly, friendlyName, sizeof(_friendly));
    strlcpy(_swVersion, swVersion, sizeof(_swVersion));

    _mqtt.onConnect([](bool sessionPresent) {
      if (_instance) _instance->onConnected();
    });

    _mqtt.onDisconnect([](AsyncMqttClientDisconnectReason reason) {
      if (!_instance) return;
      _instance->_connected = false;
      _instance->_discoveryStep = -1;
      Log.printf("[MQTT] Disconnected, reason %d\n", (int)reason);
    });

    _mqtt.onMessage([](char *topic, char *payload,
                       AsyncMqttClientMessageProperties props,
                       size_t len, size_t index, size_t total) {
      if (!_instance) return;
      // payload не заканчивается нулём — берём ровно len байт
      String value;
      value.concat(payload, len);
      String t(topic);
      int slash = t.lastIndexOf('/');
      String cmd = (slash >= 0) ? t.substring(slash + 1) : t;
      Log.printf("[MQTT] Command '%s' = '%s'\n", cmd.c_str(), value.c_str());
      if (_instance->_cb) _instance->_cb(cmd, value);
    });

    applyConfig();
  }

  // Перечитать настройки. Вызывается при старте и после сохранения
  // конфигурации, чтобы не требовать перезагрузки.
  void applyConfig() {
    if (!_config) return;
    _enabled = _config->data.mqttEnabled && strlen(_config->data.mqttHost) > 0;

    if (!_enabled) {
      if (_connected) _mqtt.disconnect();
      Log.println("[MQTT] Disabled");
      return;
    }

    _mqtt.setServer(_config->data.mqttHost, _config->data.mqttPort);
    if (strlen(_config->data.mqttUser) > 0) {
      _mqtt.setCredentials(_config->data.mqttUser, _config->data.mqttPass);
    }
    _mqtt.setClientId(_id);

    // Last Will: если связь оборвётся, брокер сам объявит устройство
    // недоступным — HA узнает об этом сразу, а не по таймауту
    static String willTopic;
    willTopic = String(_base) + "/status";
    _mqtt.setWill(willTopic.c_str(), 1, true, "offline");

    _lastAttempt = 0;   // подключиться при ближайшем handle()
    Log.printf("[MQTT] Enabled, broker %s:%u\n",
      _config->data.mqttHost, _config->data.mqttPort);
  }

  void handle(const Payload &p) {
    if (!_enabled || WiFi.status() != WL_CONNECTED) return;

    uint32_t now = millis();

    if (!_connected) {
      if (_lastAttempt == 0 || now - _lastAttempt > MQTT_RECONNECT_MS) {
        _lastAttempt = now;
        Log.println("[MQTT] Connecting...");
        _mqtt.connect();      // не блокирует: ESPAsyncTCP
      }
      return;
    }

    // Описания сущностей публикуем по одному за проход
    if (_discoveryStep >= 0) {
      if (now - _lastDiscoveryMs >= MQTT_DISCOVERY_STEP_MS) {
        int count;
        const EntityDef *defs = entities(count);
        // Настройку берём из ConfigStore, а не из Payload: автодискавери
        // начинается сразу после connect, когда состояние ещё ни разу
        // не публиковалось и поле было бы не заполнено.
        bool showFilter = _config && _config->data.filterNotifyMqtt;
        if (defs[_discoveryStep].filterOnly && !showFilter) {
          retractDiscoveryEntity(defs[_discoveryStep]);
        } else {
          publishDiscoveryEntity(defs[_discoveryStep]);
        }
        _lastDiscoveryMs = now;
        _discoveryStep++;
        if (_discoveryStep >= count) {
          _discoveryStep = -1;
          Log.printf("[MQTT] Discovery published: %d entities\n", count);
        }
      }
      return;   // состояние подождёт, пока не разошлём описания
    }

    if (_lastPublish == 0 || now - _lastPublish >= intervalMs()) {
      publishState(p);
      _lastPublish = now;
    }
  }

  void publishState(const Payload &p) {
    if (!_connected) return;

    JsonDocument doc;

    JsonObject t = doc["temperatures"].to<JsonObject>();
    // Отсутствующий датчик уходит как null — HA покажет «неизвестно»,
    // а не -127 (см. pitfalls, раздел про sentinel-значения)
    if (p.tempColdOk)   t["cold"] = serialized(String(p.tempCold, 1));   else t["cold"] = nullptr;
    if (p.tempHotOk)    t["hot"] = serialized(String(p.tempHot, 1));     else t["hot"] = nullptr;
    if (p.tempSupplyOk) t["supply"] = serialized(String(p.tempSupply, 1)); else t["supply"] = nullptr;
    if (p.tempReturnOk) t["return"] = serialized(String(p.tempReturn, 1)); else t["return"] = nullptr;

    JsonObject m = doc["meters"].to<JsonObject>();
    m["hot_m3"] = serialized(String(p.meterHotM3, 3));
    m["cold_m3"] = serialized(String(p.meterColdM3, 3));

    JsonObject r = doc["reed"].to<JsonObject>();
    r["hot"] = p.reedHotClosed ? "ON" : "OFF";
    r["cold"] = p.reedColdClosed ? "ON" : "OFF";

    JsonObject s = doc["sys"].to<JsonObject>();
    s["rssi"] = p.rssi;
    s["uptime"] = p.uptimeSec;
    s["heap"] = p.freeHeap;

    JsonObject f = doc["filter"].to<JsonObject>();
    f["closed"] = p.filterClosed ? "ON" : "OFF";
    f["sensor_lost"] = p.filterSensorLost ? "ON" : "OFF";
    f["trips"] = p.filterTrips;

    JsonObject c = doc["cfg"].to<JsonObject>();
    c["debounce_closed"] = p.debounceClosedMs;
    c["debounce_open"] = p.debounceOpenMs;
    c["filter_on"] = serialized(String(p.filterTempOnC, 1));
    c["filter_off"] = serialized(String(p.filterTempOffC, 1));

    String out;
    serializeJson(doc, out);

    String topic = String(_base) + "/state";
    _mqtt.publish(topic.c_str(), 0, false, out.c_str());
    _publishCount++;
  }

  // Снять описания сущностей с брокера. Пустой retained-payload —
  // штатный способ убрать сущность из Home Assistant.
  void clearDiscovery() {
    if (!_connected) return;
    int count;
    const EntityDef *defs = entities(count);
    for (int i = 0; i < count; i++) {
      String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + defs[i].component
                   + "/" + _id + "/" + defs[i].object + "/config";
      _mqtt.publish(topic.c_str(), 0, true, "");
    }
    Log.println("[MQTT] Discovery cleared");
  }

  bool isEnabled() const { return _enabled; }
  bool isConnected() const { return _connected; }
  uint32_t publishCount() const { return _publishCount; }
  uint32_t reconnects() const { return _reconnects; }
  const char *baseTopic() const { return _base; }
};

MqttClient *MqttClient::_instance = nullptr;

#endif
