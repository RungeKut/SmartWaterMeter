// Проверки целостности SPA: синтаксис и связь скрипта с разметкой.
//
// Ловит опечатки в id и сломанные строковые литералы до прошивки —
// в проекте это уже случалось при генерации кода скриптами.

const vm = require('vm');
const { suite, test, equal } = require('../assert');
const { extractScript } = require('./harness');

const { html, code } = extractScript();

suite('целостность SPA', () => {
  test('скрипт синтаксически корректен', () => {
    new vm.Script(code, { filename: 'index.html<script>' });
  });

  test('все getElementById разрешаются в разметке', () => {
    const ids = new Set();
    for (const m of code.matchAll(/getElementById\('([^']+)'\)/g)) ids.add(m[1]);
    const missing = [...ids].filter((id) => !html.includes('id="' + id + '"'));
    equal(missing.join(', '), '', 'отсутствуют в разметке');
  });

  test('нет обращений к удалённым элементам расписания', () => {
    const dead = ['cfgReportHour', 'cfgReportMin', 'cfgReportSchedule']
      .filter((id) => code.includes(id));
    equal(dead.join(', '), '', 'остались ссылки на старые поля');
  });

  test('страница помещается в LittleFS с запасом', () => {
    // Раздел 1 МБ, но держим SPA компактным: он читается в память
    // при каждой отдаче
    const kb = Math.round(html.length / 1024);
    if (kb > 120) throw new Error('SPA разросся до ' + kb + ' КБ');
  });
});
