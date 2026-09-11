# Log.h — Единая точка вывода логов

## Назначение

Дублирование логов прошивки в два места сразу: USB-Serial и Telnet.

## Зачем понадобился

Раньше `TelnetSerial` ставил хук `os_install_putc1()` и документация утверждала, что в Telnet попадает «весь вывод». На деле это не так:

- `os_install_putc1()` перехватывает только вывод **SDK** (`os_printf` — сообщения WiFi-стека)
- `Serial.print` / `Serial.printf` идут через `HardwareSerial::write` → напрямую в UART, **минуя `putc1`**

То есть строки вида `[DS18B20] Found sensors: 4` в Telnet не попадали никогда. Вдобавок хук не сохранял предыдущий обработчик (`_telnetOriginalPutc` оставался `nullptr`), поэтому вывод SDK пропадал и из USB-Serial.

## Как устроено сейчас

`Logger` — наследник `Print`. Каждый байт уходит в `Serial` и в зарегистрированный приёмник:

```cpp
class LogSink {
public:
  virtual void logWrite(const uint8_t *buf, size_t size) = 0;
};

Logger Log;   // глобальный объект
```

`TelnetSerial` реализует `LogSink` и регистрируется в `setup()`:

```cpp
Log.setSink(&telnet);
telnet.begin();
```

## Использование

```cpp
Log.printf("[DS18B20] Found sensors: %d\n", count);
Log.println("[Setup] Ready");
```

Весь проект пишет логи через `Log`, а не через `Serial`. Исключение — `Serial.begin(115200)` в `setup()`: это инициализация порта, а не вывод.

## Что осталось только на USB-Serial

Низкоуровневый вывод SDK (`scandone`, `state: 0 -> 2` и подобные сообщения WiFi-стека). Хук `putc1` намеренно не ставится: писать в TCP-сокет из контекста SDK небезопасно.

## Почему определение объекта в заголовке

Проект собирается как одна единица трансляции — все модули header-only и включаются из `Wemos_Mini.ino`. Поэтому `Logger Log;` в заголовке не даёт конфликта символов. Так же оформлены `MeterCounter::_instanceHot` и другие статики проекта.

Если когда-нибудь появится второй `.cpp`, определение нужно будет вынести: `extern Logger Log;` в заголовке, объект — в одном `.cpp`.
