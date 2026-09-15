/******************************************************************
 * Нативные тесты FilterGuard — защита осмотической мембраны.
 *
 * Запуск: pio test -e native
 *
 * Логика решает, перекрыть ли воду квартире, и ошибается молча в обе
 * стороны: не сработала — мембрана получила кипяток, залипла — воды
 * нет. Проверять это на живом стояке дорого и долго, здесь же
 * температура задаётся тестом.
 ******************************************************************/

#include <unity.h>
#include <Arduino.h>

// Определения для заглушек — до модулей проекта, как в test_meter
uint64_t fakeTimeUs = 0;
uint8_t fakePinLevel[24];
SerialStub Serial;

#include <EEPROM.h>
EEPROMStub EEPROM;

#include "secrets.h"
#include "ConfigStore.h"
#include "FilterGuard.h"

static ConfigStore cfg;
static FilterGuard guard;

static const uint8_t RELAY_PIN = 5;

// --- перехват событий ---
static FilterGuard::Event lastEvents[16];
static int eventCount = 0;

static void onEvent(FilterGuard::Event ev, float tempC) {
  (void)tempC;
  if (eventCount < 16) lastEvents[eventCount] = ev;
  eventCount++;
}

static bool sawEvent(FilterGuard::Event ev) {
  for (int i = 0; i < eventCount && i < 16; i++) if (lastEvents[i] == ev) return true;
  return false;
}

// --- вспомогательное ---

static void resetAll(bool enabled = true) {
  fakeTimeUs = 1000000;
  for (int i = 0; i < 24; i++) fakePinLevel[i] = HIGH;
  eventCount = 0;

  cfg.resetDefaults();
  cfg.data.filterEnabled = enabled;
  cfg.data.filterTempOnC = 35.0f;
  cfg.data.filterTempOffC = 30.0f;
  cfg.data.filterRelayActiveLow = false;

  guard = FilterGuard();
  guard.begin(RELAY_PIN, &cfg, onEvent);
}

// Подать температуру и прокрутить время, как делает loop() раз в секунду
static void feed(float tempC, bool ok = true, int seconds = 1) {
  for (int i = 0; i < seconds; i++) {
    advanceMillis(1000);
    guard.handle(tempC, ok);
  }
}

// Уровень на ноге реле
static uint8_t relay() { return fakePinLevel[RELAY_PIN]; }

// --- направление отказа ---

// Клапан нормально открытый: обесточено = вода идёт. После begin()
// реле обязано быть отпущено, чем бы ни была занята плата до этого.
void test_relay_idle_after_begin(void) {
  resetAll();
  TEST_ASSERT_EQUAL_UINT8(LOW, relay());     // active HIGH -> покой LOW
  TEST_ASSERT_FALSE(guard.isClosed());
}

void test_relay_idle_after_begin_active_low(void) {
  resetAll();
  cfg.data.filterRelayActiveLow = true;
  guard = FilterGuard();
  guard.begin(RELAY_PIN, &cfg, onEvent);
  TEST_ASSERT_EQUAL_UINT8(HIGH, relay());    // active LOW -> покой HIGH
  TEST_ASSERT_FALSE(guard.isClosed());
}

// --- гистерезис ---

void test_closes_above_upper_threshold(void) {
  resetAll();
  feed(20.0f);
  TEST_ASSERT_FALSE(guard.isClosed());

  feed(35.0f);                                // ровно порог — уже срабатывает
  TEST_ASSERT_TRUE(guard.isClosed());
  TEST_ASSERT_EQUAL_UINT8(HIGH, relay());
  TEST_ASSERT_TRUE(sawEvent(FilterGuard::EVENT_CLOSED));
  TEST_ASSERT_EQUAL_UINT32(1, guard.trips());
}

// Между порогами состояние не меняется — в этом весь смысл гистерезиса
void test_holds_between_thresholds(void) {
  resetAll();
  feed(40.0f);
  TEST_ASSERT_TRUE(guard.isClosed());

  feed(34.0f);  TEST_ASSERT_TRUE(guard.isClosed());
  feed(31.0f);  TEST_ASSERT_TRUE(guard.isClosed());
  feed(30.1f);  TEST_ASSERT_TRUE(guard.isClosed());
}

void test_opens_at_lower_threshold(void) {
  resetAll();
  feed(40.0f);
  TEST_ASSERT_TRUE(guard.isClosed());

  feed(30.0f);                                // ровно нижний порог
  TEST_ASSERT_FALSE(guard.isClosed());
  TEST_ASSERT_EQUAL_UINT8(LOW, relay());
  TEST_ASSERT_TRUE(sawEvent(FilterGuard::EVENT_OPENED));
}

// Колебания около верхнего порога не должны дёргать реле
void test_no_chatter_around_upper_threshold(void) {
  resetAll();
  feed(34.9f);
  TEST_ASSERT_EQUAL_UINT32(0, guard.trips());

  for (int i = 0; i < 10; i++) { feed(35.1f); feed(34.5f); }
  // Сработало ровно один раз: обратно открывает только 30 градусов
  TEST_ASSERT_EQUAL_UINT32(1, guard.trips());
  TEST_ASSERT_TRUE(guard.isClosed());
}

void test_full_cycle_counts_trips(void) {
  resetAll();
  for (int i = 0; i < 3; i++) {
    feed(40.0f);
    feed(25.0f);
  }
  TEST_ASSERT_EQUAL_UINT32(3, guard.trips());
  TEST_ASSERT_FALSE(guard.isClosed());
}

// --- пропавший датчик ---

// Выбрано: состояние держим, но сообщаем. Обрыв провода не должен
// лишать квартиру воды.
void test_sensor_lost_holds_open_state(void) {
  resetAll();
  feed(20.0f);
  TEST_ASSERT_FALSE(guard.isClosed());

  feed(-127.0f, false, 5);
  TEST_ASSERT_FALSE(guard.isClosed());
  TEST_ASSERT_TRUE(guard.isSensorLost());
  TEST_ASSERT_TRUE(sawEvent(FilterGuard::EVENT_SENSOR_LOST));
}

void test_sensor_lost_holds_closed_state(void) {
  resetAll();
  feed(40.0f);
  TEST_ASSERT_TRUE(guard.isClosed());

  feed(-127.0f, false, 5);
  TEST_ASSERT_TRUE(guard.isClosed());        // так и держим закрытым
  TEST_ASSERT_EQUAL_UINT8(HIGH, relay());
}

// Событие поднимается один раз на обрыв, а не каждую секунду
void test_sensor_lost_alerts_once(void) {
  resetAll();
  feed(20.0f);
  eventCount = 0;

  feed(-127.0f, false, 30);
  TEST_ASSERT_EQUAL_INT(1, eventCount);
}

void test_sensor_back_alerts_and_resumes(void) {
  resetAll();
  feed(20.0f);
  feed(-127.0f, false, 3);
  TEST_ASSERT_TRUE(guard.isSensorLost());

  feed(40.0f);
  TEST_ASSERT_FALSE(guard.isSensorLost());
  TEST_ASSERT_TRUE(sawEvent(FilterGuard::EVENT_SENSOR_BACK));
  TEST_ASSERT_TRUE(guard.isClosed());        // и сразу отработал по факту
}

// Пока датчик молчит, максимум температуры не обновляется мусором
void test_sensor_lost_does_not_poison_max(void) {
  resetAll();
  feed(28.0f);
  feed(-127.0f, false, 5);
  TEST_ASSERT_TRUE(guard.hasMaxTemp());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 28.0f, guard.maxTempC());
}

// --- выключенная защита ---

void test_disabled_never_closes(void) {
  resetAll(false);
  feed(80.0f, true, 10);
  TEST_ASSERT_FALSE(guard.isClosed());
  TEST_ASSERT_EQUAL_UINT8(LOW, relay());
  TEST_ASSERT_EQUAL_UINT32(0, guard.trips());
}

// Выключение защиты при закрытом клапане обязано открыть воду
void test_disabling_releases_valve(void) {
  resetAll();
  feed(40.0f);
  TEST_ASSERT_TRUE(guard.isClosed());

  cfg.data.filterEnabled = false;
  feed(40.0f);
  TEST_ASSERT_FALSE(guard.isClosed());
  TEST_ASSERT_EQUAL_UINT8(LOW, relay());
}

// --- защита от кривых настроек ---

// Схлопнутый гистерезис заставил бы реле дребезжать на каждой десятой
void test_collapsed_hysteresis_is_repaired(void) {
  resetAll();
  cfg.data.filterTempOffC = 40.0f;           // выше верхнего порога
  TEST_ASSERT_TRUE(guard.offThreshold() < guard.onThreshold());

  feed(40.0f);
  TEST_ASSERT_TRUE(guard.isClosed());
  feed(34.0f);
  TEST_ASSERT_TRUE(guard.isClosed());        // не открылось сразу же
}

void test_garbage_thresholds_fall_back_to_defaults(void) {
  resetAll();
  cfg.data.filterTempOnC = NAN;
  cfg.data.filterTempOffC = NAN;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, FILTER_TEMP_ON_DEFAULT, guard.onThreshold());
  TEST_ASSERT_TRUE(guard.offThreshold() < guard.onThreshold());

  cfg.data.filterTempOnC = 0.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, FILTER_TEMP_ON_DEFAULT, guard.onThreshold());

  cfg.data.filterTempOnC = 5000.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, FILTER_TEMP_ON_DEFAULT, guard.onThreshold());
}

// --- полярность реле ---

void test_active_low_relay_inverts_levels(void) {
  resetAll();
  cfg.data.filterRelayActiveLow = true;
  guard = FilterGuard();
  guard.begin(RELAY_PIN, &cfg, onEvent);

  feed(40.0f);
  TEST_ASSERT_TRUE(guard.isClosed());
  TEST_ASSERT_EQUAL_UINT8(LOW, relay());     // под током = LOW

  feed(25.0f);
  TEST_ASSERT_FALSE(guard.isClosed());
  TEST_ASSERT_EQUAL_UINT8(HIGH, relay());
}

// --- статистика для отчёта ---

void test_stats_track_max_and_closed_time(void) {
  resetAll();
  feed(20.0f);
  feed(47.2f);                               // пик
  TEST_ASSERT_TRUE(guard.isClosed());
  feed(40.0f, true, 60);                     // минута с закрытым клапаном
  feed(25.0f);

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 47.2f, guard.maxTempC());
  TEST_ASSERT_EQUAL_UINT32(1, guard.trips());
  TEST_ASSERT_TRUE(guard.closedSec() >= 58);
  TEST_ASSERT_TRUE(guard.closedSec() <= 64);
}

// Пока клапан открыт, время закрытия не капает
void test_closed_time_does_not_run_while_open(void) {
  resetAll();
  feed(20.0f, true, 120);
  TEST_ASSERT_EQUAL_UINT32(0, guard.closedSec());
}

void test_reset_stats_starts_new_period(void) {
  resetAll();
  feed(40.0f, true, 30);
  TEST_ASSERT_TRUE(guard.trips() > 0);

  guard.resetStats();
  TEST_ASSERT_EQUAL_UINT32(0, guard.trips());
  TEST_ASSERT_EQUAL_UINT32(0, guard.closedSec());
  TEST_ASSERT_FALSE(guard.hasMaxTemp());
  TEST_ASSERT_TRUE(guard.isClosed());        // само состояние не трогаем
}

// --- миграция настроек ---

// Нетронутая flash отдаёт 0xFF: bool читается как true, float как NaN.
// Ровно на этом проект уже обжигался с MQTT.
void test_virgin_eeprom_gets_safe_filter_defaults(void) {
  memset(EEPROM.buf, 0xFF, sizeof(EEPROM.buf));
  ConfigStore fresh;
  fresh.resetDefaults();
  fresh.data.magic = EEPROM_MAGIC;
  // Имитируем старую прошивку: блок фильтра ещё не инициализирован
  memset(&fresh.data.filterEnabled, 0xFF,
         sizeof(ConfigData) - offsetof(ConfigData, filterEnabled));
  fresh.sanitizeFilterFields();

  TEST_ASSERT_FALSE(fresh.data.filterEnabled);        // не включаем сами
  TEST_ASSERT_FLOAT_WITHIN(0.01f, FILTER_TEMP_ON_DEFAULT, fresh.data.filterTempOnC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, FILTER_TEMP_OFF_DEFAULT, fresh.data.filterTempOffC);
  // Плановые отчёты остаются включёнными: обновление не должно их
  // молча отключить тем, кто ими уже пользуется
  TEST_ASSERT_TRUE(fresh.data.reportEnabled);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_relay_idle_after_begin);
  RUN_TEST(test_relay_idle_after_begin_active_low);
  RUN_TEST(test_closes_above_upper_threshold);
  RUN_TEST(test_holds_between_thresholds);
  RUN_TEST(test_opens_at_lower_threshold);
  RUN_TEST(test_no_chatter_around_upper_threshold);
  RUN_TEST(test_full_cycle_counts_trips);
  RUN_TEST(test_sensor_lost_holds_open_state);
  RUN_TEST(test_sensor_lost_holds_closed_state);
  RUN_TEST(test_sensor_lost_alerts_once);
  RUN_TEST(test_sensor_back_alerts_and_resumes);
  RUN_TEST(test_sensor_lost_does_not_poison_max);
  RUN_TEST(test_disabled_never_closes);
  RUN_TEST(test_disabling_releases_valve);
  RUN_TEST(test_collapsed_hysteresis_is_repaired);
  RUN_TEST(test_garbage_thresholds_fall_back_to_defaults);
  RUN_TEST(test_active_low_relay_inverts_levels);
  RUN_TEST(test_stats_track_max_and_closed_time);
  RUN_TEST(test_closed_time_does_not_run_while_open);
  RUN_TEST(test_reset_stats_starts_new_period);
  RUN_TEST(test_virgin_eeprom_gets_safe_filter_defaults);
  return UNITY_END();
}
