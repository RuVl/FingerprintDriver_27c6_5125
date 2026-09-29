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

Хеши после последнего amend: `e707269`, `47e6997`, `c506f0e` (могут
измениться при доработках — смотреть `git log upstream/master..goodix5125-mr`).

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
   Kabagöz и «Goodix 5125 driver contributors» сохранены; в файлы !648 без
   копирайта добавлен `Thomas97460 <t.collet974@outlook.fr>`; в наши новые и
   изменённые файлы — `RuVl <ru.vlad.13@gmail.com>` (у файлов chicago/ строка
   «Modified … for openchicago» теперь под этим копирайтом).
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
переменные `windows`/`valid_windows` (окна изображения) и строку
`"Milan_v_3.02.00.15"` — см. открытые вопросы. Сообщения коммитов чистые.

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

## umockdev (осталось)

`tools/record_umockdev_5125.sh`: `check` → `build` (introspection) →
`record` (sudo, create-driver-test.py `--test custom goodix5125`,
custom.py: enroll 12 касаний, verify, identify; нулевой PSK во временном
state-каталоге) → `replay`.

Нужно установить: `umockdev wireshark-cli gobject-introspection` (сейчас их
нет), python-gobject уже есть. Запись требует usbmon и остановки fprintd.

Ожидаемая проблема воспроизведения: TLS-рукопожатие недетерминировано
(ServerHello.random хоста из RAND_bytes), записанный Finished сенсора при
replay не сойдётся. Нужна доработка драйвера: при `FP_DEVICE_EMULATION=1`
детерминированный RAND для TLS (свой `OSSL_LIB_CTX` с тестовым DRBG либо
фиксированный server random через callback) — отдельный коммит, после записи.
Запись содержит OTP и кадры, зашифрованные нулевым PSK (то есть фактически
изображения) — публиковать только осознанно.

## Что осталось / открытые вопросы

1. Копирайт `Thomas97460` в файлах !648 без копирайта — подтвердить форму
   (или оставить только «Goodix 5125 driver contributors»).
2. `"Milan_v_3.02.00.15"` — строка версии в упакованном шаблоне (тег 0xa3),
   была уже в !648, при распаковке не проверяется. Оставить (совместимость
   формата) или заменить нейтральной строкой (сломает побайтовую
   совместимость шаблонов, но не их чтение)?
3. Имена переменных окружения `GOODIX5125_FDT_SEQUENCE=mr648` и файл
   `chicago-calibration.dat` оставлены как в !648.
4. umockdev-запись и детерминированный TLS в эмуляции (см. выше).
5. CI upstream (`test_unsupported_list`, scan-build, s390x) локально не
   прогонялся.
6. Описание MR — `docs/mr-description.md`.

## Как воспроизвести с нуля

```sh
cd upstream/libfprint-mr648
git remote add upstream https://gitlab.freedesktop.org/libfprint/libfprint.git
git fetch upstream master
git log --oneline upstream/master..goodix5125-mr
git grep -nE '0x18[0-9a-f]{7}|AlgoChicago|AlgoMilan|\.dll|winpe|oracle|оракул' goodix5125-mr -- .
# uncrustify 0.81.0 (нет в системе): собрать из исходников и
#   uncrustify -c scripts/uncrustify.cfg --check <файлы goodix5125>
# сборка/тесты — см. выше
```
