# Драйвер Goodix 27c6:5125 (HONOR MagicBook 16) для Linux

Сенсор отпечатка на кнопке питания. Кристалл ST411 (как 5110/5117), прошивка
`GF_ST411SEC_APP_12508`, кадр 64×80, 12 бит. Цель — работа через libfprint/fprintd.

**История находок и изменений:** [Changelog.md](Changelog.md) (ссылается на
детальные разборы в `tools/algo/re/notes/`).

## Жёсткие правила

- **Никогда не стирать и не прошивать MCU** (команды `0xa4`, `0xf0`, `0xf2`, `0xf4`).
  Другой 5125 был окирпичен именно перепрошивкой. Эти методы вырезаны из
  `tools/goodix5125/goodix.py` — не возвращать.
- Не делать soft reset MCU без необходимости (бит 1 в payload `reset`).
- Любое действие, меняющее систему (sudo, установка пакетов) или состояние сенсора
  (запись PSK), записывать в `SYSTEM_CHANGES.local.md`: что, зачем, как откатить.
- Когда нужен палец пользователя или его команда — дать пошаговую инструкцию
  (блок «НУЖНО ВАШЕ УЧАСТИЕ»), пользователь запускает сам через `! ...`, и ждать.

## Состояние сенсора

- В сенсор записан white-box **нулевого PSK** (хеш `b5e0beeb…6c89`). TLS: 1.2,
  `PSK-AES128-GCM-SHA256`, хост — сервер.
- Запись PSK: payload должен быть кратен 4 байтам (иначе CRC во flash не пишется,
  статус 3). См. `tools/provision.py`.

## Протокол — особенности 5125

- Данные на interface 1 (EP `0x01`/`0x81`).
- MCU теряет ACK, если ответ идёт сразу следом → ACK необязателен
  (`tools/goodix5125/device.py`, в libfprint — правка в `goodix_receive_timeout_cb`).
- Конфиг MCU (`0x90`) — от Windows-драйвера, с подстановкой OTP-калибровки в регистры
  `0x220/0x236/0x238/0x23a` и контрольной суммой `0xA5A5 + Σ16 → дополнение`.
- FDT: ответ = 2 байта состояния, **байт 2 — маска касания** (`0x3f` палец, `0x00` нет),
  затем 6 значений LE16. Рабочая последовательность ожидания касания: после кадра
  `fdt_mode(0x0d)` → пробный `fdt_down` (2 с без ответа) → `query_mcu_state` → `fdt_down(0x0c)`.
- Кадр: TLS-запись `0xb0`, открытый текст 7693 байта = 8 заголовок + 7680 + 5 хвост.

## Структура

- `tools/` — Python-прототип: `probe.py` (только чтение), `provision.py`, `tls_check.py`,
  `capture.py`, `fdt_test.py`, `collect.py` (+ `collect_*.sh`), `wbgen/` (генератор
  white-box), `fwre/` (разбор прошивки), `tune/` (offline-оценка алгоритмов сравнения).
- `libfprint/` — форк goodix-fp-linux-dev/libfprint (ветка `buildpackage`), наш драйвер:
  `libfprint/drivers/goodixtls/goodix5125.{c,h}`.
- `deps/root/` — локально распакованные `libgusb`, `opencv` (в систему не ставились).
- `win-driver/` — файлы Windows-драйвера 1.1.125.20 (не коммитить).
- `dumps/` — логи, кадры, датасет `dumps/dataset/` (не коммитить).

## Команды

```sh
# Python-инструменты
cd tools && ../.venv/bin/python probe.py
../.venv/bin/python -m pytest -q wbgen fwre

# Сборка libfprint с драйвером
cd libfprint
PKG_CONFIG_PATH=$PWD/../deps/root/usr/lib/pkgconfig meson setup build \
  -Ddrivers=goodixtls5125 -Dintrospection=false -Ddoc=false \
  -Dgtk-examples=false -Dudev_rules=disabled -Dudev_hwdb=disabled
ninja -C build

# Тесты на железе (запускает пользователь)
tools/libfprint_test.sh          # снимок + регистрация + проверка
tools/libfprint_verify_test.sh   # 10 проверок по сохранённой регистрации

# Offline-оценка алгоритмов сравнения на датасете
cd tools/tune && ../../.venv/bin/python eval_all.py
```

Примеры libfprint запускать с `LD_LIBRARY_PATH=deps/root/usr/lib`.
Python `protocol.read(timeout=...)` — в секундах; SIGINT не прерывает ожидание в libusb,
для тестов без пальца задавать короткий таймаут.

## Открытые задачи

- Точность сравнения: sigfm на этом сенсоре не узнаёт свой палец в 76–88% попыток.
  Палец на боковой кнопке ложится под разными углами (до 90°+) и разными участками.
  Направление: склейка эталонов в «карту» пальца + SIFT/RANSAC. Мерить на `dumps/dataset`.
- Установка: PKGBUILD (provides/conflicts `libfprint`) + `fprintd` + PAM.
