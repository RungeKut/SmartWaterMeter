// Мини-раннер без зависимостей: нужен только Node.
//
// Специально не тянем jest/mocha — проект встраиваемый, и требовать
// npm install ради полутора десятков проверок несоразмерно.

let current = null;
const results = [];

function suite(name, fn) {
  current = { name, tests: [] };
  results.push(current);
  fn();
  current = null;
}

function test(name, fn) {
  if (!current) throw new Error('test() вне suite()');
  try {
    fn();
    current.tests.push({ name, ok: true });
  } catch (e) {
    current.tests.push({ name, ok: false, error: e.message });
  }
}

function equal(got, expected, what) {
  if (got !== expected) {
    throw new Error(
      (what ? what + ': ' : '') +
      'получено ' + JSON.stringify(got) + ', ожидалось ' + JSON.stringify(expected));
  }
}

function isNull(got, what) {
  if (got !== null) {
    throw new Error((what ? what + ': ' : '') + 'ожидался null, получено ' + JSON.stringify(got));
  }
}

function report() {
  let pass = 0, fail = 0;
  for (const s of results) {
    console.log('\n' + s.name);
    for (const t of s.tests) {
      if (t.ok) { pass++; console.log('  OK    ' + t.name); }
      else { fail++; console.log('  FAIL  ' + t.name + '\n          ' + t.error); }
    }
  }
  console.log('\n' + '='.repeat(50));
  console.log(`пройдено ${pass}, провалено ${fail}`);
  return fail === 0;
}

module.exports = { suite, test, equal, isNull, report };
