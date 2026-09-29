# Стадия 10: локальная ветка для MR «goodix: Add support for 27c6:5125»

Цель: чистая серия коммитов поверх свежего upstream/master с драйвером 27c6:5125
(итог ветки `openchicago`, включая ac792f8), без следов разбора вендорской
библиотеки. Ничего не пушится, MR не создаётся.

## Состояние (2026-09-29)

Репозиторий: `upstream/libfprint-mr648`, ветка **`goodix5125-mr`**, без
upstream-трекинга (защита от случайного push).

- remote `upstream` = https://gitlab.freedesktop.org/libfprint/libfprint.git
  (fetch), база — `upstream/master` = `6f9479c` («synaptics: Add new PID 0x10b»);
- `origin` (форк Thomas97460, !648) разшаллоен (`git fetch --unshallow`);
- ветки `openchicago` и worktree `upstream/wt-openchicago` не трогались.

Коммиты (автор RuVl <ru.vlad.13@gmail.com>, в каждом
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`):

| # | коммит | содержимое | Co-authored-by |
|---|--------|-----------|----------------|
| 1 | `goodix5125: Add the openchicago ChicagoHS matcher` | `libfprint/drivers/goodix5125/chicago/*` (в сборку ещё не подключены) | Berke Kabagöz, Thomas97460 |
| 2 | `goodix5125: Add support for Goodix 27c6:5125` | драйвер, README, `meson.build` (drivers_info, helper openssl), `libfprint/meson.build`, `data/autosuspend.hwdb`, `libfprint/fprint-list-udev-hwdb.c`; включает ACK-правку (3e58f90), хранилище learned (7438e79) и пустую галерею identify (ac792f8) | Thomas97460, Berke Kabagöz |
| 3 | `tests: Add goodix5125 unit tests` | `tests/goodix5125{,-tls,-algo}.c`, `tests/meson.build` | Thomas97460 |

Хеши после последнего autosquash (чекпоинт 2): `52c3b93`, `0e1df20`,
`a48f742` (могут измениться при доработках — смотреть
`git log upstream/master..goodix5125-mr`).

Имена/почты соавторов: Thomas97460 <t.collet974@outlook.fr> — автор коммита
8cef588 (!648); Berke Kabagöz <berkekbgz@gmail.com> — из копирайтов в файлах
!648 (своих коммитов в истории форка у него нет).

## Что сделано

1. **Перенос кода.** Файлы драйвера и тестов взяты из `openchicago` (с ac792f8),
   общие файлы сборки правлены вручную поверх master (конфликтов не было:
   правка `foreach driver_test, args` в tests/meson.build уже есть в master).
   Общий goodix-код upstream не менялся — все протокольные правки внутри
   `goodix5125/`, поэтому отдельного коммита «goodix: …» нет.
2. **Вычистка.** Из комментариев, строк, имён и сообщений коммитов убраны:
   адреса/RVA/смещения функций библиотеки (`0x18…`, `+0x…`, `G+`, `ctx+`,
   `rbp+`, `T+`), имена AlgoChicago/AlgoMilan/.dll, Windows/WinBio/EngineAdapter,
   «oracle», сверки «bit for bit», ссылки на `tools/algo`, `docs/`, `notes/`,
   `win-driver/`, хеш и версия DLL в комментарии, метки EFIXES/MFIXES/SFIXES,
   упоминание генератора правил. Переименованы идентификаторы:
   `*_dll` → без суффикса, `efix_*` → `group_*`, `vendor_statistics` →
   `raw_statistics`, `params_48` → `probe_resolution`, `scenario_e2e` →
   `scenario_plain`; вендорские имена функций в комментариях (getFeature,
   templateStudy, identifyImage, enrolAddImage …) заменены описаниями.
   Смещения полей собственной структуры `GoodixChicagoMatchScoreRecord`
   заменены именами полей. Код не менялся: токены без комментариев и пробелов
   совпадают с `openchicago`, кроме переименований и скобок/пробелов от
   uncrustify.
3. **Лицензии.** Во всех файлах SPDX `LGPL-2.1-or-later`. Копирайты Berke
   Kabagöz и «Goodix 5125 driver contributors» сохранены как в !648; строки
   `Thomas97460` в файлах, где в !648 копирайта не было, убраны (чекпоинт 2,
   заголовки как в 8cef588); в наши новые и изменённые файлы —
   `RuVl <ru.vlad.13@gmail.com>` (у файлов chicago/ строка «Modified … for
   openchicago» под этим копирайтом). Thomas97460 остаётся в
   `Co-authored-by`.
4. **hwdb/udev.** `usb:v27C6p5125*` перенесён из «unsupported» в блок
   `# Supported by libfprint driver goodix5125` (перед goodixmoc — так его
   ставит генератор; тест `udev-hwdb` с проверкой содержимого проходит),
   строка удалена из `fprint-list-udev-hwdb.c`.
5. **uncrustify.** В системе нет; собран uncrustify 0.81.0 в scratchpad
   (`git clone -b uncrustify-0.81.0`, cmake). Проверка: на всех 139 .c/.h
   upstream master `--check` без замечаний, значит версия совместима с CI
   (Fedora rawhide). Наши файлы отформатированы `--replace`, повторный
   `--check` чистый.
6. **Сборка/тесты** (см. ниже) — без предупреждений, всё зелёное.
7. `tools/record_umockdev_5125.sh` — запись umockdev-теста (запускает
   пользователь).
8. `docs/mr-description.md` — текст описания MR (англ.).

## Проверка из п.3 задания

```sh
cd upstream/libfprint-mr648
git grep -nE '0x18[0-9a-f]{7}|AlgoChicago|AlgoMilan|\.dll|winpe|oracle|оракул' goodix5125-mr -- .
```

Результат: **пусто** (совпадений нет ни в наших, ни в чужих драйверах).
Расширенная проверка по диффу ветки (`Windows|DLL|tools/|FIXES|EngineAdapter|
templateStudy|getFeature|§|recovered|official|bit for bit|e2e`) даёт только
переменные `windows`/`valid_windows` (окна изображения). Строка версии
шаблона заменена на `"openchicago_v1.0"` (чекпоинт 2), проверка
`...|Milan_v` тоже пуста. Сообщения коммитов чистые.

## Сборка и тесты

```sh
cd upstream/libfprint-mr648
PKG_CONFIG_PATH=$PWD/../../deps/root/usr/lib/pkgconfig meson setup build-mr \
  -Ddrivers=all -Dintrospection=false -Ddoc=false -Dgtk-examples=false \
  -Dudev_rules=enabled -Dudev_hwdb=enabled
ninja -C build-mr                       # 200/200, 0 warnings
GOODIX5125_TEST_FRAMES=$PWD/../../tools/algo meson test -C build-mr
```

- `-Ddrivers=all` собирается без сужения списка.
- Чекпоинт 2: `build-mr` — 0 warnings, meson test Ok 10 / Fail 0 /
  Skipped 33 (в `goodix5125-tls` 2 теста, включая deterministic);
  uncrustify 0.81.0 `--check` по 41 .c/.h диффа — все PASS. `openchicago`
  (`wt-openchicago/build`, goodix5125): 0 warnings, Ok 7 / Fail 0.
- meson test: **Ok 10, Fail 0, Skipped 33**. Пропуски — все umockdev-тесты
  драйверов (сборка без introspection, umockdev не установлен) — так и
  должно быть при `-Dintrospection=false`.
- `goodix5125-algo` с кадрами: OK (A: 127 проб, свой 8, чужой 0; B engine
  16 касаний; B identify: 123 пробы, чужие кадры ни разу не совпали со своим отпечатком;
  C состояние; D learned-хранилище). Без `GOODIX5125_TEST_FRAMES` — **SKIP**
  (exit 77), проверено.
- Коммит 2 отдельно тоже собирается (временный worktree на 47e6997,
  `-Ddrivers=goodix5125,virtual_image`, ninja rc=0; worktree удалён).

## Тест goodix5125-algo в MR

Кадры `tools/algo/*_raw.bin` — наши сырые изображения пальца, в MR не входят;
без них тест корректно пропускается. Варианты своего набора (не делались,
нужно решение):

- **синтетические кадры**: ImageBase = гладкий фон, касание = фон минус
  синтетический папиллярный узор (синусоиды с переменной ориентацией/частотой
  + шум, 12 бит). Проверяет стабильность API/состояния/хранилища и
  детерминизм (driver vs openchicago), но не качество распознавания;
- **не-палец**: запись касаний боковой поверхностью пальца/костяшкой
  (как советует tests/README.md) — небольшой набор (≈15 кадров) можно
  опубликовать только с согласия пользователя;
- минимум без данных: сценарии C/D на синтетике (state save/restore/rebase,
  learned-хранилище) — не требуют настоящих отпечатков.

## Чекпоинт 2 (2026-09-29): решения пользователя

Обе ветки; в `openchicago` — новые коммиты, в `goodix5125-mr` — fixup +
`GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash upstream/master`
(сообщения коммитов 2 и 3 обновлены через `amend!`).

| задача | openchicago | goodix5125-mr |
|---|---|---|
| копирайт Thomas97460 убран там, где его не было в !648 (14 файлов) | — (там его и не было) | коммиты 2, 3 |
| версия шаблона (тег 0xa3) `"openchicago_v1.0"`; при чтении проверяется только размер 64, старые шаблоны читаются | `cfdef18` | коммит 1 |
| убран `GOODIX5125_FDT_SEQUENCE=mr648` (FDT-up, NOP-async, `fdt_up_base_from_reply` и его тест, раздел README) | `4270976` | коммиты 2, 3 |
| детерминизм в эмуляции + тест + README | `7713008` | коммиты 2, 3 |
| `tests/goodix5125/custom.py` | — (алгоритм другой, запись под MR) | коммит 3 |

Оставшиеся переменные окружения (не удалялись): `GOODIX5125_PROVISION_PSK`
(рабочая: запись PSK), `GOODIX5125_STATE_DIR`, `GOODIX5125_PSK_FILE`,
`GOODIX5125_STATE_FILE`, `GOODIX5125_LEARNED_DIR` (пути), только для
отладки/миграции — `GOODIX5125_ENROLL_PAIR=0` (выключить второй кадр касания)
и `GOODIX5125_CHICAGO_CALIBRATION_FILE` (разовый импорт калибровки !648).

### Эмуляция (`FP_DEVICE_EMULATION=1`)

`fpi_device_emulation_mode_enabled()` — как в synaptics/goodixmoc/egismoc
(там — фиксированный serial). umockdev при replay сравнивает каждый OUT-URB с
записью побайтно (`memcmp` в `umockdev-pcap.vala`), IN-данные подставляет из
записи; URB, отменённые по таймауту при записи, при replay так же ждут
таймаута драйвера. Поэтому всё, что шлёт хост, должно зависеть только от
ответов сенсора:

- **TLS** (`goodix5125-tls.c`): `goodix5125_tls_new (..., deterministic, ...)`.
  В детерминированном режиме свой `OSSL_LIB_CTX`, в нём встроенный провайдер
  (`OSSL_PROVIDER_add_builtin`) с EVP_RAND, который на каждый запрос отдаёт
  один и тот же шаблон `0x5a + i*0x3d` (не зависит от числа и порядка
  запросов OpenSSL), `RAND_set_DRBG_type` на него, провайдер `default` для
  шифров; `SSL_OP_NO_TICKET` (тикет содержит время сессии). Фиксированы
  ServerHello.random, session ID, явные nonce AES-GCM.
- **PSK**: нулевой ключ, файл не читается, запись в сенсор запрещена
  (несовпадение хеша — ошибка).
- **Состояние**: `g_dir_make_tmp ("libfprint-goodix5125-XXXXXX")` на open,
  рекурсивно удаляется на close/finalize; `GOODIX5125_*` игнорируются
  (включая ENROLL_PAIR — всегда по умолчанию). `/var/lib/fprint` не трогается.
- **Таймеры** не менялись: запись тоже идёт в режиме эмуляции
  (create-driver-test.py ставит `FP_DEVICE_EMULATION=1`), и сенсору нужны те же
  2 с FDT-down-пробы и опрос подъёма раз в 100 мс. При replay ожидание пальца
  (FDT-down без таймаута) завершается сразу, проба ждёт 2 с реального времени —
  примерно 2–3 с на касание, ~40–60 с на весь сценарий → в `drivers_tests`
  поставить `'timeout': 120`. Зависнуть бесконечно тест не может: meson
  убьёт его по таймауту, umockdev пишет «Replay may be stuck».
- **Юнит-тест** `/goodix5125/tls/deterministic`: обычный клиент против
  детерминированного сервера A (запоминаются все полёты), затем те же байты
  клиента скармливаются новому серверу B — B отвечает теми же байтами,
  завершает рукопожатие и расшифровывает записанную прикладную запись;
  недетерминированный сервер на тот же ClientHello отвечает иначе. OK.

Риски replay: (1) другая версия OpenSSL на CI может иначе сформировать
ServerHello/расширения — тогда запись придётся обновить, проверяется только
на CI; (2) алгоритм должен дать те же решения при регистрации — код целочисленный
и одинаковый, но расхождения компилятора/архитектуры (s390x в CI) не
проверены.

### Сценарий `tests/goodix5125/custom.py` (ветка MR)

1. `identify` с пустой галереей → нет совпадения, без касания;
2. регистрация (`LEFT_LITTLE`, 12 стадий, `max(progress) == 12`, повторы
   допускаются);
3. `verify` отпечатка после `serialize`/`deserialize` (как fprintd) →
   совпадение; `close`.

### Сборка с introspection

`meson setup -Dintrospection=true` сейчас падает: нет `g-ir-scanner`
(pkg-config `gobject-introspection-1.0` есть — из `libgirepository`, а
инструменты в пакете `gobject-introspection`). GIR `GLib/GObject/Gio/GUsb`
в `/usr/share/gir-1.0` есть, `python-gobject` есть (системный
`/usr/bin/python3`; в `.venv` модуля `gi` нет). Не хватает пакетов:
`gobject-introspection` (сборка), `umockdev` (`umockdev-record`,
`umockdev-run`), `wireshark-cli` (`tshark`).

### Запись (делает пользователь)

1. `sudo pacman -S --needed umockdev wireshark-cli gobject-introspection` —
   записать в `SYSTEM_CHANGES.local.md` (откат: `sudo pacman -Rs …`).
2. `tools/record_umockdev_5125.sh check`, затем `… build`
   (`upstream/libfprint-mr648/build-umockdev`, goodix5125 + introspection).
3. `tools/record_umockdev_5125.sh record` — палец, которым не входят в систему
   (например, левый мизинец): пустой identify без касания, 12 засчитанных
   касаний регистрации, одно касание verify. Меняет систему (записать):
   `modprobe usbmon`, `systemctl stop fprintd`, USB-reset порта сенсора
   (не команда MCU). В сенсор ничего не пишется.
4. `tools/record_umockdev_5125.sh replay` → `REPLAY OK`.
5. Агент: `'goodix5125': { 'timeout': 120 }` в `drivers_tests`,
   `device` + `custom.pcapng` — в коммит тестов (fixup), `meson test -C
   build-umockdev goodix5125`; проверить `device` на серийный номер.

## Итог записи umockdev (2026-09-29)

- Запись: левый мизинец, регистрация одним местом пальца. `REPLAY OK`,
  `meson test -C build-umockdev`: `goodix5125` OK за 14 с; в build-mr
  Ok 9 / Fail 0. `device`: только пути PCI/USB и серийный номер прошивки
  `00000000001A`, без DMI и данных хоста.
- Коммиты ветки: 52c3b93 matcher, 468588e driver, 83af60b tests
  (custom.py, device, custom.pcapng, drivers_tests с timeout 120).
- Найдено и исправлено по дороге:
  1. **Воспроизведение таймаутов.** umockdev отдаёт чтение, которое в
     записи закончилось таймаутом (URB discard, -ENOENT), сразу как пустое
     успешное. Драйвер принимал его за данные, и воспроизведение расходилось
     на первом drain. Теперь пустое чтение считается таймаутом
     (`goodix5125_usb_in_check_empty`). Исправление в driver-коммите; в ветке
     openchicago — f52078c.
  2. **Мусор в `device`.** Из-за `G_MESSAGES_DEBUG=all` отладка
     umockdev-record попадала первой строкой в `device`. Скрипт
     теперь задаёт домены и чистит файл.
  3. **Причина −4 при проверке.** Не ошибка кода: регистрация,
     разнесённая по пальцу, давала непересекающиеся участки. Кадры
     неудачной записи расшифрованы нулевым PSK и прогнаны офлайн:
     совпадение с драйвером полное, перекрёстная проверка 0/23 (датасет с
     большим пальцем — 7/15). Инструкция записи теперь требует
     регистрировать палец одним местом.
- Скрипт: шаг `try` — тот же сценарий на устройстве без записи.

## Что осталось / открытые вопросы

1. Прогон записи umockdev на CI (версия OpenSSL, s390x).
2. Файл `chicago-calibration.dat` и `GOODIX5125_CHICAGO_CALIBRATION_FILE`
   оставлены как в !648.
3. CI upstream (`test_unsupported_list`, scan-build, s390x) локально не
   прогонялся.
4. Описание MR — `docs/mr-description.md`.

## Как воспроизвести с нуля

```sh
cd upstream/libfprint-mr648
git remote add upstream https://gitlab.freedesktop.org/libfprint/libfprint.git
git fetch upstream master
git log --oneline upstream/master..goodix5125-mr
git grep -nIiE '0x18[0-9a-f]{7}|AlgoChicago|AlgoMilan|\.dll|winpe|oracle|Milan_v' goodix5125-mr -- .
# uncrustify 0.81.0 (нет в системе): собрать из исходников и
#   uncrustify -c scripts/uncrustify.cfg --check <файлы goodix5125>
# сборка/тесты — см. выше
```
