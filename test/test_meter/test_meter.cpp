/******************************************************************
 * Нативные тесты MeterCounter — автомат подсчёта импульсов геркона.
 *
 * Запуск: pio test -e native
 *
 * Логика подсчёта самая тонкая в проекте, и проверять её на плате
 * дорого: нужен физический импульс с геркона. Здесь время и уровень
 * пина задаёт тест, поэтому короткий импульс, дребезг и залипание
 * воспроизводятся точно и мгновенно.
 ******************************************************************/

#include <unity.h>
#include <Arduino.h>

// Определения для заглушек. Объявляются ДО модулей проекта: те
// рассчитывают, что secrets.h уже подключён (в прошивке это делает
// Wemos_Mini.ino).
uint32_t fakeMicros = 0;
uint8_t fakePinLevel[24];
SerialStub Serial;

#include <EEPROM.h>
EEPROMStub EEPROM;

#include "secrets.h"
#include "ConfigStore.h"    // определяет глобальный Log через Log.h
#include "MeterCounter.h"

static ConfigStore cfg;
// Указатель, а не объект: в MeterCounter есть volatile-массив событий,
// из-за которого оператор присваивания удалён. Пересоздаём экземпляр
// заново перед каждым тестом.
static MeterCounter *meter = nullptr;

static const uint8_t PIN = 14;

// --- вспомогательное ---

static void resetAll(float litersPerPulse = 1.0f) {
  fakeMicros = 1000000;              // не с нуля, чтобы ловить ошибки с 0
  for (int i = 0; i < 24; i++) fakePinLevel[i] = HIGH;
  cfg.resetDefaults();
  cfg.data.litersPerPulseHot = litersPerPulse;
  cfg.data.debounceClosedMs = 5;
  cfg.data.debounceOpenMs = 50;
  delete meter;
  meter = new MeterCounter();
  meter->begin(PIN, true, &cfg);
}

// Сменить уровень пина и отдать событие в ISR, как это сделало бы железо
static void setPin(uint8_t level) {
  fakePinLevel[PIN] = level;
  meter->handleInterrupt();
}

// Прокрутить время, периодически вызывая process() — имитация loop()
static void runFor(uint32_t ms, uint32_t stepMs = 10) {
  for (uint32_t t = 0; t < ms; t += stepMs) {
    advanceMillis(stepMs);
    meter->process();
  }
}

// Один «нормальный» импульс: замыкание на closedMs, затем пауза openMs
static void pulse(uint32_t closedMs, uint32_t openMs) {
  setPin(LOW);
  runFor(closedMs);
  setPin(HIGH);
  runFor(openMs);
}

// --- тесты ---

void test_normal_pulse_counted(void) {
  resetAll();
  pulse(100, 200);
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());
}

void test_several_pulses(void) {
  resetAll();
  for (int i = 0; i < 5; i++) pulse(100, 200);
  TEST_ASSERT_EQUAL_UINT32(5, meter->totalPulses());
}

// Главный баг прежней реализации: после короткого импульса счётчик
// замолкал навсегда, потому что размыкающий фронт отбрасывался
// антидребезгом и флаг залипал.
void test_short_pulse_does_not_break_counting(void) {
  resetAll();
  pulse(8, 200);          // короткое, но выше порога 5 мс
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());

  for (int i = 0; i < 3; i++) pulse(100, 200);
  TEST_ASSERT_EQUAL_UINT32(4, meter->totalPulses());
}

// Замыкание короче порога не считается и не ломает автомат
void test_too_short_pulse_ignored(void) {
  resetAll();
  setPin(LOW);
  runFor(2, 1);           // 2 мс при пороге 5
  setPin(HIGH);
  runFor(200);
  TEST_ASSERT_EQUAL_UINT32(0, meter->totalPulses());

  pulse(100, 200);        // следующий нормальный импульс считается
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());
}

// Дребезг на замыкании не должен давать лишних импульсов
void test_bounce_on_closing(void) {
  resetAll();
  for (int i = 0; i < 6; i++) { setPin(LOW); advanceMicros(300); setPin(HIGH); advanceMicros(300); }
  setPin(LOW);
  runFor(100);
  setPin(HIGH);
  runFor(200);
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());
}

// Микродребезг в залипшем положении не должен накручивать импульсы
void test_chatter_while_stuck_closed(void) {
  resetAll();
  setPin(LOW);
  runFor(500);            // залип
  TEST_ASSERT_EQUAL_UINT32(0, meter->totalPulses());

  // короткие размыкания, каждое короче порога 50 мс
  for (int i = 0; i < 10; i++) {
    setPin(HIGH); runFor(10, 2);
    setPin(LOW);  runFor(100, 10);
  }
  TEST_ASSERT_EQUAL_UINT32(0, meter->totalPulses());
}

void test_stuck_closed_state_age_grows(void) {
  resetAll();
  setPin(LOW);
  runFor(5000, 100);
  TEST_ASSERT_TRUE(meter->isClosed());
  TEST_ASSERT_TRUE(meter->stateAgeSec() >= 4);
}

// Пропадание питания при замкнутом герконе: импульс должен быть
// засчитан ровно один раз — при размыкании.
void test_closed_at_boot_counts_once_on_release(void) {
  resetAll();
  fakePinLevel[PIN] = LOW;
  delete meter;
  meter = new MeterCounter();
  meter->begin(PIN, true, &cfg);  // стартуем с замкнутым герконом

  runFor(300);
  TEST_ASSERT_EQUAL_UINT32(0, meter->totalPulses());

  setPin(HIGH);
  runFor(200);
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());
}

// Тот самый баг: литры на импульс, показания в м³
void test_liters_to_cubic_meters(void) {
  resetAll(1.0f);                 // 1 литр на импульс
  cfg.data.meterHotM3 = 0.0f;
  for (int i = 0; i < 10; i++) pulse(100, 200);
  // 10 литров = 0.010 м³, а не 10 м³
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.010f, cfg.data.meterHotM3);
}

void test_liters_per_pulse_ten(void) {
  resetAll(10.0f);                // счётчик с ценой импульса 10 литров
  cfg.data.meterHotM3 = 0.0f;
  for (int i = 0; i < 10; i++) pulse(100, 200);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.100f, cfg.data.meterHotM3);
}

// Подряд идущие импульсы без достаточной паузы не должны удваиваться
void test_short_gap_not_counted_twice(void) {
  resetAll();
  setPin(LOW);  runFor(100);
  setPin(HIGH); runFor(20, 5);    // пауза короче порога 50 мс
  setPin(LOW);  runFor(100);
  setPin(HIGH); runFor(200);
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());
}

void test_diagnostics_closed_duration(void) {
  resetAll();
  pulse(120, 200);
  TEST_ASSERT_EQUAL_UINT32(1, meter->totalPulses());
  TEST_ASSERT_TRUE(meter->lastClosedMs() >= 100);
  TEST_ASSERT_TRUE(meter->lastClosedMs() <= 140);
}

// Unity требует эти символы, даже если они пустые
void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_normal_pulse_counted);
  RUN_TEST(test_several_pulses);
  RUN_TEST(test_short_pulse_does_not_break_counting);
  RUN_TEST(test_too_short_pulse_ignored);
  RUN_TEST(test_bounce_on_closing);
  RUN_TEST(test_chatter_while_stuck_closed);
  RUN_TEST(test_stuck_closed_state_age_grows);
  RUN_TEST(test_closed_at_boot_counts_once_on_release);
  RUN_TEST(test_liters_to_cubic_meters);
  RUN_TEST(test_liters_per_pulse_ten);
  RUN_TEST(test_short_gap_not_counted_twice);
  RUN_TEST(test_diagnostics_closed_duration);
  return UNITY_END();
}
