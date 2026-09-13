// Расчёт следующего отчёта и round-trip настроек расписания.
//
// Оба набора выросли из реальных багов:
//   - месячное расписание сбрасывалось при загрузке страницы
//   - полночь нельзя было выставить (parseInt('0') || 9 -> 9)

const { suite, test, equal, isNull } = require('../assert');
const { createHarness, baseConfig } = require('./harness');

const h = createHarness();

// --- вспомогательное ---

function nextFor(opts) {
  h.el('cfgReportTime').value = opts.time;
  if (opts.day !== undefined) {
    // список дней формирует сам скрипт
    h.exec('setSchedMode(' + opts.mode + ', ' + opts.day + ');');
  } else {
    h.exec('setSchedMode(' + opts.mode + ');');
  }
  h.el('cfgReportTime').value = opts.time;

  const state = opts.noTime
    ? { time_valid: false, config: baseConfig() }
    : {
        time_valid: true,
        time_year: opts.now[0], time_mon: opts.now[1], time_mday: opts.now[2],
        time_hour: opts.now[3], time_min: opts.now[4], time_sec: opts.now[5] || 0,
        config: baseConfig(),
      };
  h.setState(state);
  return h.exec('nextReportDate()');
}

function fmt(d) {
  if (!d) return null;
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

// --- когда уйдёт следующий отчёт ---

suite('nextReportDate — ежедневно', () => {
  test('время ещё впереди — сегодня', () => {
    equal(fmt(nextFor({ mode: 0, time: '09:00', now: [2026, 9, 13, 8, 0] })), '2026-09-13 09:00');
  });
  test('время прошло — завтра', () => {
    equal(fmt(nextFor({ mode: 0, time: '09:00', now: [2026, 9, 13, 10, 0] })), '2026-09-14 09:00');
  });
  test('переход через конец месяца', () => {
    equal(fmt(nextFor({ mode: 0, time: '09:00', now: [2026, 9, 30, 10, 0] })), '2026-10-01 09:00');
  });
  test('переход через конец года', () => {
    equal(fmt(nextFor({ mode: 0, time: '09:00', now: [2026, 12, 31, 10, 0] })), '2027-01-01 09:00');
  });
  test('полночь', () => {
    equal(fmt(nextFor({ mode: 0, time: '00:00', now: [2026, 9, 13, 10, 0] })), '2026-09-14 00:00');
  });
});

// 13.09.2026 — воскресенье (day = 0)
suite('nextReportDate — еженедельно', () => {
  test('сегодня нужный день, время впереди', () => {
    equal(fmt(nextFor({ mode: 1, day: 0, time: '09:00', now: [2026, 9, 13, 8, 0] })), '2026-09-13 09:00');
  });
  test('сегодня нужный день, время прошло — через неделю', () => {
    equal(fmt(nextFor({ mode: 1, day: 0, time: '09:00', now: [2026, 9, 13, 10, 0] })), '2026-09-20 09:00');
  });
  test('понедельник из воскресенья', () => {
    equal(fmt(nextFor({ mode: 1, day: 1, time: '09:00', now: [2026, 9, 13, 10, 0] })), '2026-09-14 09:00');
  });
  test('суббота из воскресенья', () => {
    equal(fmt(nextFor({ mode: 1, day: 6, time: '09:00', now: [2026, 9, 13, 10, 0] })), '2026-09-19 09:00');
  });
});

suite('nextReportDate — ежемесячно', () => {
  test('число ещё впереди', () => {
    equal(fmt(nextFor({ mode: 2, day: 20, time: '09:00', now: [2026, 9, 13, 10, 0] })), '2026-09-20 09:00');
  });
  test('число прошло — следующий месяц', () => {
    equal(fmt(nextFor({ mode: 2, day: 5, time: '09:00', now: [2026, 9, 13, 10, 0] })), '2026-10-05 09:00');
  });
  test('декабрь — январь следующего года', () => {
    equal(fmt(nextFor({ mode: 2, day: 5, time: '09:00', now: [2026, 12, 13, 10, 0] })), '2027-01-05 09:00');
  });
  test('28 февраля не високосного года', () => {
    equal(fmt(nextFor({ mode: 2, day: 28, time: '23:59', now: [2027, 2, 1, 0, 0] })), '2027-02-28 23:59');
  });
});

suite('nextReportDate — нет времени', () => {
  test('без синхронизации возвращает null', () => {
    isNull(nextFor({ mode: 0, time: '09:00', noTime: true }));
  });
});

// --- round-trip: загрузили настройки -> сохранили ---

function roundtrip(saved) {
  h.setState({ time_valid: false, config: baseConfig(saved) });
  h.exec('updateSettings();');
  return h.exec('getConfig()');
}

suite('round-trip настроек расписания', () => {
  test('ежемесячно, 15 число не теряется', () => {
    const got = roundtrip({ reportSchedule: 2, reportDay: 15, reportHour: 9, reportMinute: 30 });
    equal(got.reportSchedule, 2, 'режим');
    equal(got.reportDay, 15, 'день');
    equal(got.reportHour, 9, 'час');
    equal(got.reportMinute, 30, 'минута');
  });

  test('ежемесячно, 28 число', () => {
    const got = roundtrip({ reportSchedule: 2, reportDay: 28, reportHour: 23, reportMinute: 59 });
    equal(got.reportDay, 28);
    equal(got.reportHour, 23);
  });

  test('еженедельно, среда', () => {
    const got = roundtrip({ reportSchedule: 1, reportDay: 3, reportHour: 7, reportMinute: 5 });
    equal(got.reportSchedule, 1);
    equal(got.reportDay, 3);
  });

  test('полночь сохраняется как 0, а не подменяется на 9', () => {
    const got = roundtrip({ reportSchedule: 1, reportDay: 0, reportHour: 0, reportMinute: 0 });
    equal(got.reportHour, 0, 'час');
    equal(got.reportMinute, 0, 'минута');
    equal(got.reportDay, 0, 'день');
  });

  test('ежедневно — день принудительно 0', () => {
    const got = roundtrip({ reportSchedule: 0, reportDay: 17, reportHour: 12, reportMinute: 0 });
    equal(got.reportSchedule, 0);
    equal(got.reportDay, 0);
  });
});
