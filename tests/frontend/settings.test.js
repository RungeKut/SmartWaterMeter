// Страница настроек: разбор чисел и защита незаконченного ввода.
//
// Оба бага проявлялись одинаково незаметно. Показания счётчика с
// запятой молча превращались в 0 и уезжали в EEPROM, а keep-alive раз
// в 30 секунд откатывал форму к сохранённому — на ввод всех настроек
// было ровно полминуты.

const { suite, test, equal, isNull } = require('../assert');
const { createHarness, baseConfig } = require('./harness');

const h = createHarness();
const dec = (raw) => h.exec('parseDecimal(' + JSON.stringify(raw) + ')');

suite('parseDecimal — разделитель дробной части', () => {
  test('точка', () => equal(dec('123.456'), 123.456));
  test('запятая', () => equal(dec('123,456'), 123.456));
  test('целое', () => equal(dec('42'), 42));
  test('ноль', () => equal(dec('0'), 0));
  test('точка в конце — так выглядит поле во время набора', () =>
    equal(dec('123.'), 123));
  test('запятая в конце', () => equal(dec('123,'), 123));
  test('без целой части', () => equal(dec('.5'), 0.5));
  test('пробелы по краям', () => equal(dec('  12.5  '), 12.5));
  test('неразрывный пробел как разделитель разрядов', () =>
    equal(dec('1\u00a0234,5'), 1234.5));
});

suite('parseDecimal — что числом не является', () => {
  test('пустая строка', () => isNull(dec('')));
  test('только пробелы', () => isNull(dec('   ')));
  test('две запятые', () => isNull(dec('1,2,3')));
  test('точка и запятая вперемешку', () => isNull(dec('1.2,3')));
  test('буквы', () => isNull(dec('12abc')));
  test('только минус', () => isNull(dec('-')));
  test('undefined', () => isNull(dec(undefined)));
});

// --- сбор настроек из формы ---

function collect(fields) {
  for (const [id, value] of Object.entries(fields)) h.el(id).value = value;
  const errors = h.exec('(function(){ var e = []; var c = collectConfig(e); return {cfg: c, errors: e}; })()');
  return errors;
}

function resetForm() {
  h.setState({ time_valid: false, config: baseConfig() });
  h.exec('setSettingsDirty(false); settingsLoaded = false; updateSettings();');
}

suite('показания счётчика в форме', () => {
  test('запятая не превращается в ноль', () => {
    resetForm();
    const r = collect({ cfgMeterHot: '123,456', cfgMeterCold: '78,9' });
    equal(r.errors.length, 0, 'ошибок быть не должно');
    equal(r.cfg.meterHotM3, 123.456);
    equal(r.cfg.meterColdM3, 78.9);
  });

  test('точка работает по-прежнему', () => {
    resetForm();
    const r = collect({ cfgMeterHot: '123.456' });
    equal(r.errors.length, 0);
    equal(r.cfg.meterHotM3, 123.456);
  });

  test('мусор в показаниях — отказ сохранять, а не тихий ноль', () => {
    resetForm();
    const r = collect({ cfgMeterHot: '12 34 abc' });
    equal(r.errors.length, 1, 'ровно одна ошибка');
    equal(r.errors[0].indexOf('Hot m3') === 0, true, 'ошибка называет поле');
  });

  test('пустое поле показаний — это ноль, а не ошибка', () => {
    resetForm();
    const r = collect({ cfgMeterHot: '' });
    equal(r.errors.length, 0);
    equal(r.cfg.meterHotM3, 0);
  });

  test('отрицательные показания отвергаются', () => {
    resetForm();
    const r = collect({ cfgMeterHot: '-5' });
    equal(r.errors.length, 1);
  });

  test('цена импульса с запятой', () => {
    resetForm();
    const r = collect({ cfgLPH: '0,5', cfgLPC: '10' });
    equal(r.errors.length, 0);
    equal(r.cfg.litersPerPulseHot, 0.5);
    equal(r.cfg.litersPerPulseCold, 10);
  });

  test('нулевая цена импульса отвергается — счётчик бы замолчал', () => {
    resetForm();
    const r = collect({ cfgLPH: '0' });
    equal(r.errors.length, 1);
  });
});

suite('целочисленные поля', () => {
  test('порт разбирается', () => {
    resetForm();
    const r = collect({ cfgMqttPort: '1884' });
    equal(r.errors.length, 0);
    equal(r.cfg.mqttPort, 1884);
  });

  test('пустой порт — значение по умолчанию', () => {
    resetForm();
    const r = collect({ cfgMqttPort: '' });
    equal(r.errors.length, 0);
    equal(r.cfg.mqttPort, 1883);
  });

  test('порт вне диапазона отвергается', () => {
    resetForm();
    const r = collect({ cfgMqttPort: '70000' });
    equal(r.errors.length, 1);
  });

  test('антидребезг округляется до целых миллисекунд', () => {
    resetForm();
    const r = collect({ cfgDbClosed: '7,6' });
    equal(r.errors.length, 0);
    equal(r.cfg.debounceClosedMs, 8);
  });
});

// --- защита незаконченного ввода ---

suite('fullState не затирает правки', () => {
  test('без правок форма заполняется с устройства', () => {
    resetForm();
    equal(h.el('cfgMqttHost').value, '');
    h.setState({ time_valid: false, config: baseConfig({ mqttHost: '10.0.0.5' }) });
    h.exec('updateSettings();');
    equal(h.el('cfgMqttHost').value, '10.0.0.5');
  });

  test('после правки keep-alive не откатывает поле', () => {
    resetForm();
    h.el('cfgMqttHost').value = '192.168.88.10';
    h.exec('markSettingsDirty();');

    // Прилетел fullState — раньше он перезаписывал всё подряд
    h.setState({ time_valid: false, config: baseConfig({ mqttHost: 'old-broker' }) });
    h.exec('updateSettings();');

    equal(h.el('cfgMqttHost').value, '192.168.88.10', 'введённое значение');
  });

  test('показания счётчика тоже не откатываются', () => {
    resetForm();
    h.el('cfgMeterHot').value = '123,4';
    h.exec('markSettingsDirty();');
    h.setState({ time_valid: false, config: baseConfig({ meterHotM3: 1 }) });
    h.exec('updateSettings();');
    equal(h.el('cfgMeterHot').value, '123,4');
  });

  test('явный отказ от правок возвращает значения устройства', () => {
    resetForm();
    h.el('cfgMqttHost').value = 'typed';
    h.exec('markSettingsDirty();');
    h.setState({ time_valid: false, config: baseConfig({ mqttHost: 'from-device' }) });
    h.exec('discardSettingsEdits();');
    equal(h.el('cfgMqttHost').value, 'from-device');
  });

  test('успешное сохранение снимает защиту', () => {
    resetForm();
    h.el('cfgMqttHost').value = 'typed';
    h.exec('markSettingsDirty();');
    h.exec('handleMessage({type: "saveConfigResult", success: true});');

    h.setState({ time_valid: false, config: baseConfig({ mqttHost: 'from-device' }) });
    h.exec('updateSettings();');
    equal(h.el('cfgMqttHost').value, 'from-device');
  });

  // Пока устройство не прислало config, поля пусты. Сохранить такую
  // форму значило бы стереть SSID и всё остальное, поэтому первое
  // заполнение выполняется даже поверх набранного.
  test('первое заполнение проходит даже при включённой защите', () => {
    h.exec('state = null; setSettingsDirty(false); settingsLoaded = false;');
    h.el('cfgMqttHost').value = 'typed-too-early';
    h.exec('markSettingsDirty();');

    h.setState({ time_valid: false, config: baseConfig({ mqttHost: 'from-device' }) });
    h.exec('updateSettings();');
    equal(h.el('cfgMqttHost').value, 'from-device', 'первый fullState заполняет форму');

    // ...а вот следующий уже не трогает: защита осталась включённой
    h.el('cfgMqttHost').value = 'typed-later';
    h.setState({ time_valid: false, config: baseConfig({ mqttHost: 'from-device' }) });
    h.exec('updateSettings();');
    equal(h.el('cfgMqttHost').value, 'typed-later', 'второй fullState не затирает');
  });

  test('баннер о несохранённых правках показывается и прячется', () => {
    resetForm();
    equal(h.el('settingsDirty').style.display, 'none');
    h.exec('markSettingsDirty();');
    equal(h.el('settingsDirty').style.display, '');
    h.exec('discardSettingsEdits();');
    equal(h.el('settingsDirty').style.display, 'none');
  });
});
