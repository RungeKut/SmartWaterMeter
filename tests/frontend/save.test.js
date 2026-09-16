// Сквозной путь сохранения настроек: подключение -> правка -> Save.
//
// Проверок на это не было, и цена оказалась высокой: после разбиения
// снимка на три сообщения кнопка Save перестала что-либо отправлять, а
// внешне это выглядело как «устройство не сохраняет». Тесты здесь
// повторяют ровно те действия, которые делает человек в браузере.

const { suite, test, equal } = require('../assert');
const { createHarness, baseConfig } = require('./harness');

const h = createHarness();

// Полный цикл: устройство прислало снимок, человек что-то поменял,
// нажал Save. Возвращает то, что реально ушло в сокет.
function edit(changes, cfgOverrides) {
  h.connect(cfgOverrides);
  h.installWs();
  h.sent.length = 0;
  for (const [id, value] of Object.entries(changes)) {
    const el = h.el(id);
    if (typeof value === 'boolean') el.checked = value;
    else el.value = value;
  }
  h.exec('markSettingsDirty();');
  h.exec('saveConfig();');
  return h.lastSent();
}

suite('Save отправляет сообщение на устройство', () => {
  test('без правок кнопка всё равно шлёт saveConfig', () => {
    h.connect();
    h.installWs();
    h.sent.length = 0;
    h.exec('saveConfig();');
    const m = h.lastSent();
    equal(m !== null, true, 'сообщение должно уйти');
    equal(m.type, 'saveConfig');
  });

  test('без подключения ничего не шлёт, но и не молчит', () => {
    h.connect();
    h.installWs(3);            // CLOSED
    h.sent.length = 0;
    h.exec('saveConfig();');
    equal(h.sent.length, 0, 'отправлять некуда');
    equal(h.el('toast').textContent, 'Not connected', 'но пользователю сказали');
  });
});

suite('изменённые значения доезжают до устройства', () => {
  test('время отчёта', () => {
    const m = edit({ cfgReportTime: '07:45' });
    equal(m.config.reportHour, 7);
    equal(m.config.reportMinute, 45);
  });

  test('полночь не подменяется девяткой', () => {
    const m = edit({ cfgReportTime: '00:00' });
    equal(m.config.reportHour, 0);
    equal(m.config.reportMinute, 0);
  });

  test('выключатель плановых отчётов', () => {
    const m = edit({ cfgReportEnabled: false }, { reportEnabled: true });
    equal(m.config.reportEnabled, false);
  });

  test('галочки уведомлений фильтра', () => {
    const m = edit({
      cfgFilterNotifyMqtt: true,
      cfgFilterNotifyEmail: false,
      cfgFilterMetrics: true,
    }, { filterNotifyMqtt: false, filterNotifyEmail: false, filterMetricsEnabled: false });
    equal(m.config.filterNotifyMqtt, true);
    equal(m.config.filterNotifyEmail, false);
    equal(m.config.filterMetricsEnabled, true);
  });

  test('включение защиты фильтра вместе с порогами', () => {
    const m = edit({ cfgFilterEnabled: true, cfgFilterOn: '38,5', cfgFilterOff: '31' });
    equal(m.config.filterEnabled, true);
    equal(m.config.filterTempOnC, 38.5);
    equal(m.config.filterTempOffC, 31);
  });

  test('показания счётчика', () => {
    const m = edit({ cfgMeterHot: '200,5' });
    equal(m.config.meterHotM3, 200.5);
  });

  test('адрес брокера и порт', () => {
    const m = edit({ cfgMqttEnabled: true, cfgMqttHost: 'homeassistant.local', cfgMqttPort: '1883' });
    equal(m.config.mqttEnabled, true);
    equal(m.config.mqttHost, 'homeassistant.local');
    equal(m.config.mqttPort, 1883);
  });

  test('антидребезг герконов', () => {
    const m = edit({ cfgDbClosed: '7', cfgDbOpen: '60' });
    equal(m.config.debounceClosedMs, 7);
    equal(m.config.debounceOpenMs, 60);
  });
});

suite('ничего не теряется по дороге', () => {
  // Устройство перезаписывает ВСЕ поля: то, чего нет в сообщении,
  // уедет в EEPROM значением по умолчанию. Один пропущенный ключ
  // здесь стирает SSID или показания счётчика.
  const REQUIRED = [
    'deviceName', 'wifiSSID', 'wifiPass',
    'smtpHost', 'smtpPort', 'smtpEmail', 'smtpPass', 'smtpRecipient',
    'reportEnabled', 'reportHour', 'reportMinute', 'reportSchedule', 'reportDay',
    'mqttEnabled', 'mqttHost', 'mqttPort', 'mqttUser', 'mqttPass', 'mqttIntervalSec',
    'meterHotM3', 'meterColdM3', 'litersPerPulseHot', 'litersPerPulseCold',
    'debounceClosedMs', 'debounceOpenMs',
    'filterEnabled', 'filterTempOnC', 'filterTempOffC', 'filterRelayActiveLow',
    'filterNotifyMqtt', 'filterNotifyEmail', 'filterMetricsEnabled',
  ];

  test('в сообщении есть все поля, которые ждёт прошивка', () => {
    const m = edit({});
    const missing = REQUIRED.filter((k) => !(k in m.config));
    equal(missing.join(', '), '', 'отсутствуют в saveConfig');
  });

  test('SSID из снимка доезжает обратно, а не пустой строкой', () => {
    const m = edit({}, { wifiSSID: 'HomeNet' });
    equal(m.config.wifiSSID, 'HomeNet');
  });

  test('показания счётчиков не обнуляются при правке чего-то другого', () => {
    const m = edit({ cfgReportTime: '10:00' }, { meterHotM3: 106.51, meterColdM3: 152.86 });
    equal(m.config.meterHotM3, 106.51);
    equal(m.config.meterColdM3, 152.86);
  });
});

suite('снимок из трёх сообщений заполняет форму целиком', () => {
  // Регрессия: fullState делает state = msg и затирает config, а
  // configState приходит следом. Если порядок или слияние сломаны,
  // форма останется пустой — и Save отправит на устройство пустоту.
  test('после connect поля заполнены значениями устройства', () => {
    h.connect({
      wifiSSID: 'HomeNet', mqttHost: 'broker.local',
      reportHour: 7, reportMinute: 45, meterHotM3: 106.51,
    });
    equal(h.el('cfgSSID').value, 'HomeNet');
    equal(h.el('cfgMqttHost').value, 'broker.local');
    equal(h.el('cfgReportTime').value, '07:45');
    equal(h.el('cfgMeterHot').value, 106.51);
  });

  test('повторный снимок (keep-alive) не опустошает форму', () => {
    h.connect({ wifiSSID: 'HomeNet' });
    h.connect({ wifiSSID: 'HomeNet' });   // как будто прошли 30 секунд
    equal(h.el('cfgSSID').value, 'HomeNet');
  });

  test('и Save после этого шлёт непустой конфиг', () => {
    h.connect({ wifiSSID: 'HomeNet', mqttHost: 'broker.local' });
    h.installWs();
    h.sent.length = 0;
    h.exec('saveConfig();');
    const m = h.lastSent();
    equal(m.config.wifiSSID, 'HomeNet');
    equal(m.config.mqttHost, 'broker.local');
  });
});
