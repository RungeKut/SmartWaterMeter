/******************************************************************
 * Нативные тесты WsRxBuffer — сборка входящего WebSocket-сообщения.
 *
 * Запуск: pio test -e native
 *
 * Дефект, ради которого это написано, был предельно тихим: сообщение
 * длиннее одного TCP-сегмента молча выбрасывалось, и настройки
 * переставали сохраняться. Ни ошибки, ни записи в лог. Проявился он
 * только когда конфиг подрос и перевалил за ~536 байт.
 ******************************************************************/

#include <unity.h>
#include <Arduino.h>

uint64_t fakeTimeUs = 0;
uint8_t fakePinLevel[24];
SerialStub Serial;

#include "WsRxBuffer.h"

static WsRxBuffer rx;

static const uint32_t C1 = 1;
static const uint32_t C2 = 2;

static bool feedStr(uint32_t client, uint8_t opcode, bool final,
                    uint32_t index, uint32_t frameLen, const char *chunk) {
  return rx.feed(client, opcode, final, index, (uint32_t)strlen(chunk),
                 frameLen, (const uint8_t *)chunk);
}

static void resetAll() { rx = WsRxBuffer(); }

// --- обычный случай ---

void test_single_chunk_message(void) {
  resetAll();
  const char *m = "{\"type\":\"restart\"}";
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, strlen(m), m));
  TEST_ASSERT_EQUAL_STRING(m, rx.message());
  TEST_ASSERT_EQUAL_UINT32(strlen(m), rx.length());
}

// Тот самый случай: сообщение приехало двумя кусками одного кадра.
// Раньше оно выбрасывалось целиком.
void test_message_split_across_two_chunks(void) {
  resetAll();
  const char *a = "{\"type\":\"saveConfig\",";
  const char *b = "\"config\":{\"reportMinute\":17}}";
  uint32_t total = strlen(a) + strlen(b);

  TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, total, a));
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, strlen(a), total, b));
  TEST_ASSERT_EQUAL_STRING("{\"type\":\"saveConfig\",\"config\":{\"reportMinute\":17}}",
                           rx.message());
}

void test_message_split_across_many_chunks(void) {
  resetAll();
  const char *parts[5] = { "12345", "67890", "abcde", "fghij", "klmno" };
  uint32_t total = 25, off = 0;
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_TEXT, true, off, total, parts[i]));
    off += 5;
  }
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, off, total, parts[4]));
  TEST_ASSERT_EQUAL_STRING("1234567890abcdefghijklmno", rx.message());
}

// Длинное сообщение разбито на КАДРЫ: продолжения идут с opcode 0
void test_continuation_frames(void) {
  resetAll();
  TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_TEXT, false, 0, 5, "hello"));
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_CONT, true, 0, 6, " world"));
  TEST_ASSERT_EQUAL_STRING("hello world", rx.message());
}

// --- граничные и недобрые случаи ---

// Продолжение без начала дописывать некуда
void test_continuation_without_start_is_dropped(void) {
  resetAll();
  TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_CONT, true, 10, 20, "хвост"));
  TEST_ASSERT_EQUAL_UINT32(0, rx.length());
  TEST_ASSERT_EQUAL_UINT32(1, rx.drops());
}

// Кусок от другого клиента не должен попасть в чужое сообщение
void test_other_client_chunk_does_not_corrupt(void) {
  resetAll();
  const char *a = "{\"from\":\"first\",";
  const char *b = "\"ok\":true}";
  uint32_t total = strlen(a) + strlen(b);

  TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, total, a));
  TEST_ASSERT_FALSE(feedStr(C2, WSRX_OPCODE_CONT, true, 5, 10, "ЧУЖОЕ"));
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, strlen(a), total, b));
  TEST_ASSERT_EQUAL_STRING("{\"from\":\"first\",\"ok\":true}", rx.message());
  TEST_ASSERT_EQUAL_UINT32(1, rx.drops());
}

// Новое сообщение от того же клиента начинает набор заново
void test_new_message_resets_previous(void) {
  resetAll();
  TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, 100, "недописанное"));
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, 6, "второе"));
  TEST_ASSERT_EQUAL_STRING("второе", rx.message());
}

// Переполнение: обрезанный JSON хуже отказа — он разобрался бы с
// ошибкой и увёл бы диагностику не туда
void test_oversize_message_is_refused_not_truncated(void) {
  resetAll();
  char big[600];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  uint32_t total = WSRX_MAX_BYTES * 2;
  bool done = false;
  for (int i = 0; i < 5; i++) {
    done = feedStr(C1, i == 0 ? WSRX_OPCODE_TEXT : WSRX_OPCODE_CONT,
                   false, i * 599, total, big);
    if (done) break;
  }
  TEST_ASSERT_FALSE(done);
  TEST_ASSERT_TRUE(rx.drops() > 0);
  TEST_ASSERT_EQUAL_UINT32(0, rx.length());
}

// Сообщение ровно в размер буфера минус терминатор ещё проходит
void test_message_at_capacity_still_fits(void) {
  resetAll();
  static char big[WSRX_MAX_BYTES];
  uint32_t n = WSRX_MAX_BYTES - 1;
  memset(big, 'y', n);
  big[n] = '\0';
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, n, big));
  TEST_ASSERT_EQUAL_UINT32(n, rx.length());
}

// Пустой кадр не должен ломать сборку
void test_empty_frame(void) {
  resetAll();
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, 0, ""));
  TEST_ASSERT_EQUAL_UINT32(0, rx.length());
}

// После собранного сообщения следующее начинается с чистого листа
void test_two_messages_in_a_row(void) {
  resetAll();
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, 6, "первое"));
  TEST_ASSERT_EQUAL_STRING("первое", rx.message());
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, 6, "второе"));
  TEST_ASSERT_EQUAL_STRING("второе", rx.message());
  TEST_ASSERT_EQUAL_UINT32(0, rx.drops());
}

// Реальный размер saveConfig: около 700 байт, два сегмента по 536
void test_realistic_save_config_size(void) {
  resetAll();
  static char part1[537], part2[200];
  memset(part1, 'a', 536); part1[536] = '\0';
  memset(part2, 'b', 165); part2[165] = '\0';
  uint32_t total = 536 + 165;

  TEST_ASSERT_FALSE(feedStr(C1, WSRX_OPCODE_TEXT, true, 0, total, part1));
  TEST_ASSERT_TRUE(feedStr(C1, WSRX_OPCODE_TEXT, true, 536, total, part2));
  TEST_ASSERT_EQUAL_UINT32(total, rx.length());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_single_chunk_message);
  RUN_TEST(test_message_split_across_two_chunks);
  RUN_TEST(test_message_split_across_many_chunks);
  RUN_TEST(test_continuation_frames);
  RUN_TEST(test_continuation_without_start_is_dropped);
  RUN_TEST(test_other_client_chunk_does_not_corrupt);
  RUN_TEST(test_new_message_resets_previous);
  RUN_TEST(test_oversize_message_is_refused_not_truncated);
  RUN_TEST(test_message_at_capacity_still_fits);
  RUN_TEST(test_empty_frame);
  RUN_TEST(test_two_messages_in_a_row);
  RUN_TEST(test_realistic_save_config_size);
  return UNITY_END();
}
