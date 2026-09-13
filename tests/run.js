// Прогон всех тестов фронтенда.
//
//   node tests/run.js
//
// Зависимостей нет — нужен только Node. Возвращает ненулевой код при
// падении, поэтому годится для CI и pre-commit.

const fs = require('fs');
const path = require('path');
const { report } = require('./assert');

const dirs = ['frontend'];
let loaded = 0;

for (const d of dirs) {
  const dir = path.join(__dirname, d);
  if (!fs.existsSync(dir)) continue;
  for (const f of fs.readdirSync(dir).filter((f) => f.endsWith('.test.js'))) {
    require(path.join(dir, f));
    loaded++;
  }
}

if (loaded === 0) {
  console.log('Тестов не найдено');
  process.exit(1);
}

console.log(`Загружено файлов тестов: ${loaded}`);
process.exit(report() ? 0 : 1);
