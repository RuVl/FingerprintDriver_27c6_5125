# Код-ревью всего проекта (с первого коммита)

- **Диапазон:** `36d1a69` (root) → `HEAD` (`4ebd3ae`), 17 коммитов, 147 отслеживаемых файлов.
- **Уровень:** high. Ревью только по чтению кода; тесты на железе и на датасете не запускались.
- **Итог:** 10 замечаний, из них 1 нарушает жёсткое правило из CLAUDE.md.

## Охват

**Просмотрено:** `tools/goodix5125/*`, `tools/*.py`, `tools/*.sh`, `openchicago/src/openchicago.c`, код runtime, calibration, template и unpack, `packaging/arch/PKGBUILD`, `udev/70-goodix-5125.rules`. Файл `tools/algo/winpe.c` только просмотрен.

**Не проверялось:**

- `libfprint/` не отслеживается в git, поэтому `goodix5125.{c,h}` не смотрели.
- Не читались `goodix-chicago-match.c`, остальные части `goodix-chicago-feature.c` и `goodix-chicago-preprocess.c`, `tools/tune/`, `tools/fwre/`, прочие C-файлы и Python-скрипты в `tools/algo`.
- Компиляция `openchicago/src` с `-Wall -Wextra` дала только предупреждения sign-compare.

## Сводка

| # | Серьёзность | Файл | Суть |
|---|---|---|---|
| 1 | Высокая (нарушение правила) | `tools/goodix5125/goodix.py:762` | `read_firmware()` (`0xf2`) и константы прошивки остались в коде |
| 2 | Высокая | `tools/capture.py:32` | Защита от опасных команд построена как denylist |
| 3 | Средняя | `tools/goodix5125/goodix.py:337` | `for i in length` (int) в `write_sensor_register()`, плюс `struct.error` в `decode_mcu_state` |
| 4 | Средняя | `tools/record_umockdev_5125.sh:142` | При сбое скрипт не возвращает права и не запускает fprintd; действия с sudo не записываются в `SYSTEM_CHANGES.local.md` |
| 5 | Средняя | `tools/record_umockdev_5125.sh:146` | `sed -i '/^P: /,$!d'` стирает весь файл, если нет строки `P: ` |
| 6 | Высокая | `openchicago/src/openchicago.c:240` | Нет проверки диапазонов в `feature_state_load()`, запись за границы |
| 7 | Высокая | `openchicago/src/goodix-chicago-enrollment.c:1967` | Нет проверки `relation_base` при распаковке шаблона |
| 8 | Низкая | `tools/algo/winpe.c:72` | `HeapReAlloc` не обнуляет память, нет проверки переполнения |
| 9 | Средняя | `udev/70-goodix-5125.rules:3` | `uaccess` без ограничения по `DEVTYPE` |
| 10 | Средняя | `packaging/arch/PKGBUILD:37` | Источник — локальный клон вне репозитория, `b2sums=SKIP` |

## Нарушение жёсткого правила

### 1. Команда `0xf2` осталась в коде

`tools/goodix5125/goodix.py:762`

CLAUDE.md: «Эти методы вырезаны из `tools/goodix5125/goodix.py` — не возвращать». Фактически:

- `Device.read_firmware()` (`0xf2`) присутствует;
- константы `COMMAND_MCU_ERASE_APP`, `COMMAND_WRITE_FIRMWARE`, `COMMAND_CHECK_FIRMWARE` остались (строки 26 и 39–41);
- `tools/goodix5125/__init__.py` утверждает, что команды удалены.

Любой скрипт, вызвавший `device.read_firmware()`, отправит `0xf2` в MCU. Блокирует это только denylist в `capture.py`; `probe.py`, `tls_check.py` и `provision.py` используют allowlist. Команды `0xa4`, `0xf0`, `0xf4` нигде не отправляются.

**Исправление:** удалить `read_firmware()` и три константы, привести `__init__.py` в соответствие.

### 2. Denylist вместо allowlist в `capture.py`

`tools/capture.py:32`

`BLOCKED_COMMANDS` защищает `capture`, `fdt_test` и `collect`. Пропускается всё, чего нет в списке: `0xf6`, `0x60`, `0x92`, `0xc4`. Проверяется только первый байт команды. Правка `fdt_test.py` или `collect.py`, вызывающая новый деструктивный метод, ничем не остановится.

**Исправление:** перевести на allowlist, как в остальных инструментах.

## Баги

### 3. `write_sensor_register()` и `decode_mcu_state`

`tools/goodix5125/goodix.py:337`, `:140`

- В ветке со списком регистров стоит `for i in length:`, где `length` — число. Любой вызов со списком адресов и значений даёт `TypeError: 'int' object is not iterable`. Нужно `range(length)`. Работает только ветка с одним регистром.
- В `decode_mcu_state` (строка 140) распаковывается `'<H'` из однобайтового среза `data[10:11]`. Вызов даёт `struct.error`.

### 4. `record_umockdev_5125.sh` не возвращает систему в исходное состояние

`tools/record_umockdev_5125.sh:142`

При `set -euo pipefail` неудачный запуск `create-driver-test.py` (документированный случай «verify did not match») прерывает скрипт до `chown` и до перезапуска fprintd. Каталог `tests/goodix5125` остаётся у root, остановленный `fprintd` не запускается. Скрипт также выполняет `sudo modprobe usbmon` и `systemctl stop fprintd`, но не записывает это в `SYSTEM_CHANGES.local.md`, как требует CLAUDE.md.

**Исправление:** `trap` для восстановления состояния и запись в `SYSTEM_CHANGES.local.md`.

### 5. `sed` стирает файл устройства

`tools/record_umockdev_5125.sh:146`

`sed -i '/^P: /,$!d'` удаляет все строки, если строки `P: ` в выводе `umockdev-record` нет (например, запись оборвана). Файл устройства молча становится пустым; сырой pcapng сохраняется, но результат запуска теряется.

**Исправление:** проверять наличие `P: ` через `grep -q` до `sed`.

## Безопасность и надёжность `openchicago`

### 6. Нет проверки диапазонов в `feature_state_load()`

`openchicago/src/openchicago.c:240`

Поля `history_count`, `stable_count`, `last_level` и поля консенсуса копируются из blob как есть; `oc_session_new_from_state()` им доверяет, хотя CRC-32 не является аутентификацией. Blob с `history_count = 0xffffffff` (-1) и пересчитанным CRC проходит загрузку. Следующий вызов getFeature (`goodix-chicago-feature.c`, около строк 2175 и 2340) выполняет `memcpy(state->history[history_count], ...)` с отрицательным индексом или индексом больше 3: запись за границы. В `load_state` препроцессора те же пропуски для `history_count`, `repeat_count` и подобных полей.

**Исправление:** проверять диапазоны всех индексных и счётных полей при загрузке состояния.

### 7. `relation_base` не проверяется при распаковке

`openchicago/src/goodix-chicago-enrollment.c:1967`

`goodix_chicago_enrollment_unpack()` принимает любое `relation_base`. Шаблон с большим значением и валидным CRC-32 приводит `synthesize_capacity_relations` к чтению за границами в `g_array_index(relations, relation_base + older/newer)` (строки 1964–1976). `drop_last` (строка 74) вызывает `g_array_set_size(relations, relation_base)`, что может выделить гигабайты. Для сравнения: `relation_between_subtemplates` проверку делает.

**Исправление:** проверять `relation_base` против фактического размера массива при распаковке.

## Инфраструктура

### 8. Эталонный загрузчик DLL

`tools/algo/winpe.c:72`, `:95`

- `k_HeapReAlloc` игнорирует `HEAP_ZERO_MEMORY`: расширенная часть блока содержит остатки malloc, и эталонные результаты могут меняться между запусками. Это подрывает сравнения «бит-в-бит с `AlgoChicago.dll`».
- `k_HeapAlloc` считает `size + 16` без проверки переполнения.
- `TlsAlloc` может выдать индекс больше 255, а `TlsSetValue` молча его отбрасывает.

### 9. Правило udev слишком широкое

`udev/70-goodix-5125.rules:3`

Правило срабатывает на `SUBSYSTEM=="usb"` без `DEVTYPE` и выдаёт `uaccess`. Любой процесс залогиненного пользователя получает сырой USB-доступ к сенсору, включая PSK и команды MCU, которые проект запрещает. Тег ставится и на узел интерфейса, а не только на узел устройства.

**Исправление:** добавить `ENV{DEVTYPE}=="usb_device"`; после перехода на fprintd убрать или перенести правило.

### 10. `PKGBUILD` не воспроизводим

`packaging/arch/PKGBUILD:37`

Источник — локальный клон `$startdir/../../upstream/libfprint-mr648` (ветка `openchicago`). Каталог `upstream/` в `.gitignore`, `b2sums=SKIP`, `pkgver` записан вручную (`1.94.100.r7.gac792f8`) и корректируется только функцией `pkgver()`. На чистом чекауте `makepkg -si` падает, а коммит ветки `openchicago` нигде не зафиксирован.

**Исправление:** брать источник по URL с фиксированным коммитом и указать контрольную сумму.

## Рекомендуемый порядок работ

1. Пункты 1 и 2 (правило про MCU).
2. Пункты 6 и 7 (проверка входных данных `openchicago`).
3. Пункты 3, 4, 5 (баги в инструментах).
4. Пункты 9, 10, 8.

## Статус исправлений (2026-09-29)

Каждое замечание перед правкой проверено по коду.

| # | Статус | Что сделано |
|---|---|---|
| 1 | исправлено | `read_firmware()` и константы `0xa4`/`0xf0`/`0xf2`/`0xf4` удалены из `goodix.py`, `__init__.py` приведён в соответствие |
| 2 | исправлено | `capture.py`: allowlist (19 команд, которые шлют capture/fdt_test/collect), пакеты с другими флагами блокируются |
| 3 | исправлено | `range(length)`; `decode_mcu_state` читает `data[10:12]` |
| 4 | исправлено | `trap` возвращает права на `tests/goodix5125` и запускает fprintd при любом выходе; `modprobe usbmon` и остановка fprintd дописываются в `SYSTEM_CHANGES.local.md` |
| 5 | исправлено | `sed` выполняется, только если в файле есть строка `P: ` |
| 6 | исправлено | проверки диапазонов при загрузке: FEAT (`history_count` 0..3 и др.), PREP (`framenum`, `multiplier_count`, `history_count`, `residue_count` — делители `n + 1`); регрессионные тесты в `openchicago/tests/test_state.c` и в `tests/goodix5125.c` ветки MR |
| 7 | исправлено | при распаковке шаблона проверяется, что `relation_base + [0, i)` не выходит за `relation_count` |
| 8 | исправлено | `HeapReAlloc` обнуляет прирост при `HEAP_ZERO_MEMORY`, проверка переполнения размера, `TlsAlloc`/`FlsAlloc` возвращают `TLS_OUT_OF_INDEXES`; вывод `algo_eval4` на DLL побайтно совпал с прежним |
| 9 | исправлено | правило только для узла устройства (`ENV{DEVTYPE}=="usb_device"`) |
| 10 | исправлено | источник PKGBUILD — публичная ветка форка на GitLab (см. коммит PKGBUILD) |

Правки 6 и 7 перенесены в обе ветки libfprint (`goodix5125-mr`, `openchicago`).
