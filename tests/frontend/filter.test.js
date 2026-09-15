// Защита осмотического фильтра: настройки и карточка на Dashboard.
//
// Цена ошибки здесь выше обычной: слипшиеся пороги заставят реле щёлкать
// на каждой десятой градуса, а невключённая карточка скроет от глаз то,
// что защита ослепла из-за отвалившегося датчика.

const { suite, test, equal } = require('../assert');
const { createHarness, baseConfig } = require('./harness');

const h = createHarness();

function load(cfgOverrides) {
  h.setState({ time_valid: false, config: baseConfig(cfgOverrides) });
  h.exec('setSettingsDirty(false); settingsLoaded = false; updateSettings();');
}

function collect(fields) {
  for (const [id, value] of Object.entries(fields || {})) {
    const el = h.el(id);
    if (typeof value === 'boolean') el.checked = value;
    else el.value = value;
  }
  return h.exec('(function(){ var e = []; var c = collectConfig(e); return {cfg: c, errors: e}; })()');
}

suite('настройки фильтра — round-trip', () => {
  test('значения с устройства попадают в форму', () => {
    load({ filterEnabled: true, filterTempOnC: 38.5, filterTempOffC: 31 });
    equal(h.el('cfgFilterEnabled').checked, true);
    equal(h.el('cfgFilterOn').value, 38.5);
    equal(h.el('cfgFilterOff').value, 31);
  });

  test('и возвращаются обратно без потерь', () => {
    load({ filterEnabled: true, filterTempOnC: 38.5, filterTempOffC: 31 });
    const r = collect();
    equal(r.errors.length, 0);
    equal(r.cfg.filterEnabled, true);
    equal(r.cfg.filterTempOnC, 38.5);
    equal(r.cfg.filterTempOffC, 31);
  });

  test('галочки каналов уведомления', () => {
    load({ filterNotifyMqtt: true, filterNotifyEmail: false, filterMetricsEnabled: true });
    equal(h.el('cfgFilterNotifyMqtt').checked, true);
    equal(h.el('cfgFilterNotifyEmail').checked, false);
    const r = collect();
    equal(r.cfg.filterNotifyMqtt, true);
    equal(r.cfg.filterNotifyEmail, false);
    equal(r.cfg.filterMetricsEnabled, true);
  });

  test('полярность реле сохраняется', () => {
    load({ filterRelayActiveLow: true });
    equal(h.el('cfgFilterActiveLow').checked, true);
    equal(collect().cfg.filterRelayActiveLow, true);
  });

  test('нога реле показывается подсказкой', () => {
    load({ filterRelayPin: 5 });
    equal(h.el('filterPinHint').textContent, 'GPIO5');
  });
});

suite('пороги фильтра', () => {
  test('запятая работает так же, как точка', () => {
    load({ filterEnabled: true });
    const r = collect({ cfgFilterOn: '38,5', cfgFilterOff: '30,5' });
    equal(r.errors.length, 0);
    equal(r.cfg.filterTempOnC, 38.5);
    equal(r.cfg.filterTempOffC, 30.5);
  });

  test('нижний порог выше верхнего — отказ', () => {
    load({ filterEnabled: true });
    const r = collect({ cfgFilterEnabled: true, cfgFilterOn: '35', cfgFilterOff: '40' });
    equal(r.errors.length, 1, 'ровно одна ошибка');
    equal(r.errors[0].indexOf('Reopen at') === 0, true);
  });

  test('равные пороги — тоже отказ: гистерезиса нет', () => {
    load({ filterEnabled: true });
    const r = collect({ cfgFilterEnabled: true, cfgFilterOn: '35', cfgFilterOff: '35' });
    equal(r.errors.length, 1);
  });

  test('при выключенной защите пороги не проверяются', () => {
    load({});
    const r = collect({ cfgFilterEnabled: false, cfgFilterOn: '35', cfgFilterOff: '40' });
    equal(r.errors.length, 0);
  });

  test('порог вне разумного диапазона отвергается', () => {
    load({ filterEnabled: true });
    equal(collect({ cfgFilterOn: '200' }).errors.length, 1);
    load({ filterEnabled: true });
    equal(collect({ cfgFilterOn: '5' }).errors.length, 1);
  });

  test('мусор в пороге не превращается в число молча', () => {
    load({ filterEnabled: true });
    const r = collect({ cfgFilterOn: 'горячо' });
    equal(r.errors.length, 1);
  });
});

suite('подсказка о гистерезисе', () => {
  test('показывает зазор', () => {
    load({ filterTempOnC: 35, filterTempOffC: 30 });
    equal(h.el('filterHystHint').textContent.indexOf('Hysteresis 5.0') >= 0, true);
  });

  test('предупреждает о слипшихся порогах', () => {
    load({});
    h.el('cfgFilterOn').value = '35';
    h.el('cfgFilterOff').value = '40';
    h.exec('updateHystHint();');
    equal(h.el('filterHystHint').innerHTML.indexOf('33.0') >= 0, true,
      'называет значение, к которому устройство приведёт порог');
  });
});

suite('плановые отчёты', () => {
  test('выключатель сохраняется', () => {
    load({ reportEnabled: false });
    equal(h.el('cfgReportEnabled').checked, false);
    equal(collect().cfg.reportEnabled, false);
  });

  // Прошивка до этой версии поля не отдавала. Считать его отсутствие
  // за «выключено» значило бы молча лишить человека отчётов.
  test('отсутствие поля в конфиге означает «включено»', () => {
    const cfg = baseConfig();
    delete cfg.reportEnabled;
    h.setState({ time_valid: false, config: cfg });
    h.exec('setSettingsDirty(false); settingsLoaded = false; updateSettings();');
    equal(h.el('cfgReportEnabled').checked, true);
  });
});

suite('карточка фильтра на Dashboard', () => {
  function withFilter(filter, temps) {
    h.setState({
      time_valid: false, config: baseConfig(),
      temperatures: temps || {}, filter: filter,
    });
    h.exec('updateFilterCard();');
  }

  test('при выключенной защите карточка скрыта', () => {
    withFilter({ enabled: false });
    equal(h.el('filterCard').style.display, 'none');
  });

  test('открытый клапан показан как норма', () => {
    withFilter({ enabled: true, closed: false, sensorLost: false,
                 trips: 0, closedSec: 0, tempOn: 35, tempOff: 30 }, { cold: 18.4 });
    equal(h.el('filterCard').style.display, '');
    equal(h.el('filterState').innerHTML.indexOf('ok') >= 0, true);
    equal(h.el('filterTempNow').textContent, '18.4 °C');
  });

  test('перекрытый клапан подсвечен как тревога', () => {
    withFilter({ enabled: true, closed: true, sensorLost: false,
                 trips: 3, closedSec: 4320, tempOn: 35, tempOff: 30 }, { cold: 47.2 });
    equal(h.el('filterState').innerHTML.indexOf('err') >= 0, true);
    equal(h.el('filterTrips').textContent, 3);
    equal(h.el('filterClosedFor').textContent, '1 h 12 min');
  });

  // Ослепшая защита опаснее выключенной: клапан открыт, а решать не по чему
  test('потерянный датчик важнее состояния клапана', () => {
    withFilter({ enabled: true, closed: false, sensorLost: true,
                 trips: 0, closedSec: 0, tempOn: 35, tempOff: 30 }, {});
    equal(h.el('filterState').innerHTML.indexOf('sensor lost') >= 0, true);
    equal(h.el('filterState').innerHTML.indexOf('err') >= 0, true);
  });

  test('нет данных о пике — прочерк, а не ноль', () => {
    withFilter({ enabled: true, closed: false, sensorLost: false,
                 trips: 0, closedSec: 0, tempOn: 35, tempOff: 30, maxTemp: null }, {});
    equal(h.el('filterMaxTemp').textContent, '--');
  });
});
