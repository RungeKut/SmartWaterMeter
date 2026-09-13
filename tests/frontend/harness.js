// Подставной DOM для прогона SPA на хосте.
//
// Скрипт из data/index.html загружается в изолированный контекст Node.
// Настоящего браузера нет, поэтому DOM эмулируется ровно настолько,
// насколько нужно проверяемой логике.
//
// ВАЖНО: state и schedMode в скрипте объявлены через let, то есть живут
// в лексической области модуля, а не в объекте контекста. Присваивать их
// снаружи через ctx.state НЕЛЬЗЯ — функции этого не увидят. Для этого
// есть exec(), выполняющий код внутри того же контекста.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const INDEX = path.join(__dirname, '..', '..', 'data', 'index.html');

function extractScript(file) {
  const html = fs.readFileSync(file || INDEX, 'utf8');
  const m = html.match(/<script>([\s\S]*?)<\/script>/);
  if (!m) throw new Error('В ' + (file || INDEX) + ' нет блока <script>');
  return { html, code: m[1] };
}

// Элемент-заглушка. Селекты ведут себя как настоящие: присваивание
// значения, которого нет среди option, игнорируется — именно на этом
// ловится баг со сбросом месячного расписания.
function makeElement(id, isSelect) {
  const el = {
    id,
    textContent: '',
    checked: false,
    style: {},
    dataset: {},
    classList: { toggle() {}, add() {}, remove() {} },
    addEventListener() {},
    querySelectorAll: () => [],
  };

  if (!isSelect) {
    el.value = '';
    el.innerHTML = '';
    return el;
  }

  let options = [];
  let value = '';
  Object.defineProperty(el, 'innerHTML', {
    get() { return this._html || ''; },
    set(v) {
      this._html = v;
      options = [...String(v).matchAll(/value="([^"]+)"/g)].map((m) => m[1]);
      value = options.length ? options[0] : '';
    },
  });
  Object.defineProperty(el, 'value', {
    get() { return value; },
    set(v) { if (options.includes(String(v))) value = String(v); },
  });
  Object.defineProperty(el, 'selectedIndex', {
    get() { return options.indexOf(value); },
  });
  return el;
}

const SELECT_IDS = new Set(['cfgReportDay', 'cfgReportSchedule']);

function createHarness(file) {
  const { html, code } = extractScript(file);
  const elements = {};

  const getElementById = (id) => {
    if (!elements[id]) elements[id] = makeElement(id, SELECT_IDS.has(id));
    return elements[id];
  };

  const ctx = {
    document: {
      getElementById,
      querySelectorAll: () => [],
      querySelector: () => null,
    },
    console,
    Date, JSON, Math, Object, String, Number,
    parseInt, parseFloat, isNaN,
    setInterval() {}, setTimeout() {}, clearTimeout() {},
    WebSocket: function () {},
    location: { host: 'test', protocol: 'http:' },
  };
  ctx.window = ctx;
  vm.createContext(ctx);
  vm.runInContext(code, ctx);

  return {
    ctx,
    html,
    el: getElementById,
    // Выполнить выражение внутри контекста скрипта (видит let-переменные)
    exec: (src) => vm.runInContext(src, ctx),
    // Подсунуть состояние, как будто пришёл fullState
    setState: (state) => vm.runInContext('state = ' + JSON.stringify(state) + ';', ctx),
  };
}

// Полный объект config, каким его отдаёт устройство в fullState
function baseConfig(overrides) {
  return Object.assign({
    deviceName: '',
    wifiSSID: 'net',
    smtpHost: 'smtp.example.com',
    smtpPort: 465,
    smtpEmail: 'from@example.com',
    smtpRecipient: 'to@example.com',
    reportHour: 9,
    reportMinute: 0,
    reportSchedule: 0,
    reportDay: 0,
    meterHotM3: 1,
    meterColdM3: 2,
    litersPerPulseHot: 1,
    litersPerPulseCold: 1,
    debounceClosedMs: 0,
    debounceOpenMs: 0,
    mqttEnabled: false,
    mqttHost: '',
    mqttPort: 1883,
    mqttUser: '',
    mqttIntervalSec: 0,
  }, overrides || {});
}

module.exports = { createHarness, baseConfig, extractScript, INDEX };
