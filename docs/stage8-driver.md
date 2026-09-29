# Стадия 8. Драйвер libfprint goodix5125 на openchicago

Ветка `openchicago` в `upstream/libfprint-mr648` (от `8cef588` — MR !648 как есть; исходная ветка
`goodix-5125-oem-image` не тронута). К USB-устройству в ходе работы не обращались; всё проверено
offline. Системных изменений нет (sudo не использовался, тесты пишут только во временные каталоги).

## Чекпоинты

- 2026-09-29: контекст прочитан; ветка создана; базовая сборка !648 (`-Ddrivers=goodix5125`) —
  0 предупреждений, `meson test` — 6 OK / 33 SKIP.
- openchicago дополнен (сначала в `openchicago/`, потом скопирован в драйвер): `oc_identify` (кадр
  препроцессится один раз, лучший score > 0), `oc_session_rebase` + `oc_session_get_image_base`.
  `oc_verify` = `oc_identify` с одним шаблоном. unit: api (+ разделы 6 identify, 7 rebase), state,
  corrupt-state — OK; dll/e2e — все 9 сценариев 0 расхождений с AlgoChicago.dll.
- `chicago/` драйвера заменён на `openchicago/src` + `openchicago.h` (удалены bir, late-rejection-compat,
  late-rejection-private.h, `common/goodix-crc.*` — использовались только старым chicago/).
- Драйвер переписан (алгоритмическая часть вынесена в `goodix5125-algo.{c,h}` без USB), транспорт —
  необязательный ACK, PSK — только по переменной, ожидание пальца — последовательность «probe».
- Первая сборка: 1496 предупреждений из openchicago под флагами libfprint (`-Wparentheses` в деревьях
  решений match.c, 7 `-Wlogical-op`, shadow, 4 неиспользуемые переменные). Исправлено в
  `openchicago/src`: скобки расставлены по fix-it clang (машинный код match.c при -O2 идентичен до/после),
  противоречивые конъюнкции (`t0 >= 390 && t0 < 370` и т.п., всегда ложные) → `FALSE /* never: … */`,
  shadow переименован, лишние переменные удалены (код preprocess/enrollment идентичен; match — сдвиг
  стека). После правок: openchicago unit OK, e2e — снова 9/9 сценариев без расхождений с DLL.
- ASan+UBSan: ошибок памяти нет; UBSan — 25 мест в openchicago (знаковое переполнение 64/32 бит и сдвиг
  отрицательных — эмуляция арифметики DLL, унаследовано от порта). Сделано определённым: сдвиги →
  умножение, переполняемые суммы/произведения — в беззнаковых с приведением обратно (wraparound как в
  DLL), `range` без вычитания при пустой выборке. Код изменился → перепроверено: openchicago unit OK,
  stage1 155/155 (A и B), e2e 9/9 — 0 расхождений с DLL; UBSan-прогон offline-теста — 0 сообщений.
- Итог: `ninja` — 0 предупреждений; `meson test` — 7 OK / 0 FAIL / 33 SKIP (с данными для offline-теста);
  та же тройка тестов драйвера под ASan+UBSan — OK.

## 1. openchicago в драйвере: вариант А

`drivers/goodix5125/chicago/` = копия `openchicago/src/*` + `include/openchicago.h`. Выбран вариант А:

- имена файлов совпадают с chicago/ из !648 (`goodix-chicago-*.c`), поэтому
  `git diff 8cef588 -- libfprint/drivers/goodix5125/chicago` — пофайловый патч к коду Berke Kabagöz:
  автор MR видит, что именно исправлено (стадии 1–7), а не «новую библиотеку»;
- upstream libfprint не принимает subproject'ы для драйверов (все вендорные куски — nbis, chicago — лежат
  в дереве и собираются в `libfprint-drivers`); subproject дал бы вторую систему сборки и версионирование
  ради 9 файлов;
- лицензия и SPDX не меняются (LGPL-2.1+), в каждом файле есть строка «Modified 2026 for openchicago».

Источник правды — `openchicago/`; копия в драйвере побайтно равна ему (`cp openchicago/src/*
openchicago/include/openchicago.h …/chicago/` после любых правок библиотеки).

## 2. Что изменено в драйвере

Файлы: `goodix5125.c` (цикл захвата, действия), новый `goodix5125-algo.{c,h}` (всё, что делается с
декодированным кадром; без USB — его же вызывает offline-тест), `goodix5125-usb*.c`, `goodix5125-proto.*`,
`README.md`, `libfprint/meson.build`, `tests/`.

- **Enroll** — `OC_ENROLL_ENGINE`, `nr_enroll_stages = 12` (`OC_ENGINE_STAGES`). Кадр → `oc_enroll_add`
  (или `oc_enroll_add_pair`, см. ниже). Засчитан → `fpi_device_enroll_progress(stage)`. Не засчитан →
  progress с retry: подсказка EA (участок уже снят, кадр остался в шаблоне) и coverage < 65 →
  `FP_DEVICE_RETRY_CENTER_FINGER` (с направлением WinBio 1..4 в тексте); quality < 25, отказ
  препроцессора, нет признаков, merge failure → `FP_DEVICE_RETRY_GENERAL`. Завершение — `complete`
  (12 засчитанных или 20 кадров в шаблоне).
- **choose_enroll_img**: при регистрации после первого кадра сразу снимается второй кадр того же касания
  (второй `GET_IMAGE`), пара идёт в `oc_enroll_add_pair`. Отключается `GOODIX5125_ENROLL_PAIR=0`.
  На железе не проверено (см. открытые вопросы).
- **Шаблон** — `FPI_PRINT_RAW`, `fpi-data` = GVariant `(uayayay)` (версия формата 1, sensor id =
  OTP[0:16], SHA-256 калибровки — справочно, blob templatePack openchicago) — формат !648, через
  `oc_print_data_new`/`oc_print_data_get_template`. Чужой sensor id при сравнении пропускается.
- **Verify / identify** — `oc_identify` (новое в API): кадр препроцессится один раз и сравнивается со всеми
  шаблонами галереи (иначе адаптивное состояние продвигалось бы N раз на одном кадре — ошибка, которой
  нет у DLL). Выбор — наибольший score > 0. Негодный кадр — до 3 захватов, затем
  `FPI_MATCH_ERROR` + `FP_DEVICE_RETRY_GENERAL` (как !648 / match_retry EA).
- **templateStudy**: если после совпадения шаблон выучен, новый `fpi-data` ставится на совпавший
  `FpPrint` (`g_object_set(print, "fpi-data", …)`, как в !648). Ограничения (по коду libfprint 1.94 и
  fprintd): в libfprint нет API/сигнала «print обновлён» — `fpi_device_verify_report` отдаёт только
  результат и отсканированный print; upstream-драйверы ставят `fpi-data` только при enroll и на
  print'ы отсканированного/хранимого на устройстве отпечатка, шаблоны после совпадения не обновляет
  никто; fprintd (по его исходникам; в репозитории их нет) загружает prints с диска перед каждым
  verify/identify и сохраняет только после enroll. Значит, обучение живёт
  до конца операции (или у вызывающего, который сам пересохранит print); через fprintd — теряется.
  Вариант на будущее — см. открытые вопросы.
- Состояние openchicago сохраняется после каждого действия и в `close()`.

## 3. Калибровка и состояние

- **Где хранить.** Ни один драйвер upstream libfprint не хранит файлы; fprintd работает от root с
  `StateDirectory=fprint` (`/var/lib/fprint`, prints — `/var/lib/fprint/<user>/…`), `ProtectHome` — поэтому
  только каталог внутри `/var/lib/fprint`. Выбран `/var/lib/fprint/goodix5125/` (как PSK и calibration в
  !648). Если процесс не может его создать/писать (examples от пользователя) — fallback
  `$XDG_STATE_HOME/libfprint/goodix5125` (`~/.local/state/…`). Переопределения: `GOODIX5125_STATE_DIR`,
  `GOODIX5125_STATE_FILE`, `GOODIX5125_PSK_FILE`.
- **Файл** `openchicago.state` = `"G5OC"` + u32 версия + sensor id (OTP[0:16]) + `oc_session_save_state`
  (OCST v1, CRC). Запись атомарная (`g_file_set_contents_full`, 0600, каталог 0700).
- **open()**: файл читается и разбирается (`oc_session_new_from_state`); битый → ошибка open с текстом
  «remove it to calibrate from scratch». **Активация** (первое действие после open; снимается свежий
  ImageBase между двумя FDT-замерами без пальца, как в !648): сохранённое состояние этого сенсора →
  `oc_session_rebase(new ImageBase)`; чужой sensor id → ошибка, файл не трогается; файла нет →
  разовый импорт калибровки `[sensor id][payload]` (`chicago-calibration.dat` из !648 или Windows
  `goodix_calib.dat`, `GOODIX5125_CHICAGO_CALIBRATION_FILE`), иначе первичная калибровка из ImageBase
  (`oc_session_new`, = `preprocess_init_calidata` + `preprocessor_init`). Состояние сразу сохраняется.
- **Rebase имеет смысл вне DLL.** ImageBase — физический фон сенсора (b = фон, вычитается из каждого
  кадра, notes/80 §2.5); он меняется между включениями и с температурой, старый фон из файла портил бы
  все кадры. Windows тоже берёт свежий фон при каждом старте и по NeedUpdateImageBase вызывает
  `preprocessor_init` с новым фоном, сохраняя kr/framenum; прочие адаптивные глобалы DLL
  `preprocessor_init`/`exit` не сбрасывает (установлено на стадиях 1–7, комментарий в
  `goodix-chicago-preprocess.c`). Поэтому `oc_session_rebase` меняет только ImageBase и плоскость b в
  calibration, всё адаптивное состояние сохраняется. Проверено: rebase свежего сеанса = новый сеанс на
  том же фоне (побайтно по состоянию), rebase туда-обратно = исходное состояние, save/restore после
  rebase — идентичны (openchicago test_api §7; offline-тест драйвера, сценарий C).
- Отличие от Windows: сохраняется *всё* адаптивное состояние (включая то, что DLL живёт только в
  памяти процесса), т.е. поведение = «сервис не перезапускался». Для точности это не хуже.

## 4. Протокол: наши находки против !648

| находка | в !648 | сделано |
|---|---|---|
| ACK необязателен (MCU теряет ACK, если ответ следует сразу) | строго «ACK, потом ответ»; иначе ошибка `ACK validation`; ACK-only команды ждут ACK 5 с и падают по таймауту | `goodix5125_proto_classify_rx` + цикл приёма (sync и async): читать до своего ответа, ACK — по желанию, чужие/устаревшие ACK и ответы пропускаются (до 16), для команд без ответа нет ACK за 1 с — успех. NOP: любой ответ не-ACK игнорируется. Unit-тест `/goodix5125/protocol/classify-rx` |
| FDT: байт 2 — маска касания | уже есть: `read_le16(reply+2) & 0x3f` = биты 0–5 байта 2, значения каналов с байта 4 | без изменений |
| Ожидание касания: `fdt_mode(0x0d)` → пробный `fdt_down` 2 с → `query_mcu_state` → `fdt_down(0x0c)`; подъём — опрос `fdt_mode(0x0d)` (FDT-up ненадёжен) | NOP → query → `fdt_down(0x0c)`; подъём — FDT-up `0x0a` с порогами из ответа FDT-down; пробы нет | по умолчанию наша последовательность («probe», опрос подъёма каждые 100 мс); последовательность !648 — `GOODIX5125_FDT_SEQUENCE=mr648` для сравнения на железе |
| Конфиг `0x90`: OTP в `0x220/0x236/0x238/0x23a`, контрольная сумма `0xA5A5+Σ16` → дополнение | то же + tcode (`0x5c`), `fdt_delta` (`0x82`), `fdt_offset` (`0x56`); DAC — FT-группа OTP[50..53] с откатом на MT OTP[46..49]; регистры ищутся по 4-байтной сетке секции | без изменений. Сверено на OTP нашего сенсора (`dumps/probe-*.json`): FT = MT = `bd c0 be be`, значения DAC совпадают с нашими; отличия от Windows-шаблона — tcode `0x110` и `0x82` = `0x1d80` (у нас было `0x1f80` из шаблона без подстановки) — это подстановка из OTP, как делает Windows во время работы |

Инициализация (drain, enable, reset, chip id 0x2504, OTP, PSK, конфиг, TLS, FDT 0x09, ImageBase) —
из !648 без изменений.

## 5. PSK

- Нулевой PSK не записывается: в PSK-файл кладётся 64 символа `0`; `goodix5125_pairing_wrap`+`hash` от
  нулевого ключа = хеш на сенсоре (`b5e0beeb…6c89`, docs/libfprint-integration.md §6) → запись не нужна.
- В !648 по умолчанию запись **была возможна**: файла нет и сенсор пуст → случайный ключ + запись; файл
  есть, сенсор пуст → запись. Теперь любой путь к записи (`provision_psk`, команда `0xe0`) требует
  `GOODIX5125_PROVISION_PSK=random`; без неё — ошибка с объяснением (файла нет / ключ не совпадает /
  сенсор пуст). С переменной — поведение !648 (нет файла → случайный ключ, сохраняется до записи).

## 6. Сборка и тесты

```sh
cd upstream/libfprint-mr648
PKG_CONFIG_PATH=$PWD/../../deps/root/usr/lib/pkgconfig meson setup build -Ddrivers=goodix5125 \
  -Dintrospection=false -Ddoc=false -Dgtk-examples=false -Dudev_rules=disabled -Dudev_hwdb=disabled
ninja -C build                                                       # 0 предупреждений
GOODIX5125_TEST_FRAMES=$PWD/../../tools/algo meson test -C build     # 7 OK, 33 SKIP (другие драйверы)
```

- `goodix5125` (unit, + classify-rx), `goodix5125-tls`, `goodix5125-algo` — OK; `fpi-*`, metainfo — OK.
  Без `GOODIX5125_TEST_FRAMES` тест `goodix5125-algo` возвращает 77 (SKIP) — данные не коммитятся.
- openchicago: `meson test -C openchicago/build` — unit 3/3, dll/e2e 9/9 без расхождений с DLL.
- ASan+UBSan (`-Db_sanitize=address,undefined`, отдельный build-каталог): goodix5125, goodix5125-tls,
  goodix5125-algo — OK, 0 сообщений санитайзеров (после правок UB, см. чекпоинты).

## 7. Offline-тест драйверной логики (`tests/goodix5125-algo.c`)

Реалистично и сделано: вся работа с кадром вынесена в `goodix5125-algo.c`; USB-часть (FDT, TLS) offline
не проверить (umockdev-записи нет — нужен захват на железе). Кадр идёт путём драйвера: 12-битная упаковка
как с сенсора → `goodix5125_image_decode_transposed` (сверено с `oc_frame_transpose`) →
`goodix5125_algo_*`; параллельно те же кадры — прямо через openchicago (тот путь, что e2e сверяет с DLL).
Сравниваются все результаты, выученные шаблоны и файл состояния.

- A. Сценарий e2e n12 (plain-регистрация 12 естественных касаний, остальные — verify со study):
  127 годных проб (e2e: 127), совпадений 15 + 8 = 23 (e2e: 23 шага study), чужие 0, 18 обновлений
  шаблона; всё идентично прямому пути, сохранённое состояние побайтно равно.
- B. Регистрация драйвера (ENGINE): 16 касаний, 4 retry (подсказки → CENTER_FINGER); пары — 15 пар;
  identify по {шаблон чужого, свой}: 123 пробы, индексы/score/study идентичны `oc_identify`, кадры чужого
  ни разу не совпали со своим шаблоном.
- C. Состояние: save → новый «open» → load → activate с другим ImageBase = `oc_session_rebase` прямого
  сеанса (файл побайтно равен), далее 31 кадр идентичен; чужой sensor id и испорченный файл отвергаются.

## 8. Тест на железе (для пользователя; ничего из этого не запускалось)

НУЖНО ВАШЕ УЧАСТИЕ. Команды запускать из корня проекта (`! …`).

1. Сборка (если ещё нет): `cd upstream/libfprint-mr648 && ninja -C build` (setup — раздел 6).
2. PSK-файл из нулей (для запуска examples от своего пользователя; fprintd/root — `/var/lib/fprint/goodix5125/psk`):
   ```sh
   mkdir -p -m 700 ~/.local/state/libfprint/goodix5125
   (umask 077; printf '%064d\n' 0 > ~/.local/state/libfprint/goodix5125/psk)
   stat -c '%a %U' ~/.local/state/libfprint/goodix5125/psk     # ожидается: 600 <вы>
   ```
   `GOODIX5125_PROVISION_PSK` **не задавать**.
3. Доступ к USB без root: у пользователя должны быть права на `/dev/bus/usb/…` устройства 27c6:5125
   (как в прошлых тестах). fprintd на время теста остановить, если он держит устройство.
4. Регистрация (12 касаний, между касаниями поднимать палец):
   ```sh
   cd upstream/libfprint-mr648/build/examples
   G_MESSAGES_DEBUG=all LD_LIBRARY_PATH=../../../../deps/root/usr/lib ./enroll 2>&1 | tee ~/enroll.log
   ```
   Ожидается: «Enroll stage n of 12» при засчитанном касании; «Retry …» с текстом — касание не засчитано
   (подсказка «area is enrolled already…» — сдвинуть палец в указанную сторону). В логе:
   `state …/openchicago.state, PSK …/psk, finger wait probe`, `persisted PSK matches`,
   `enroll frame: accepted q=… c=…`. Print сохраняется в `test-storage.variant` в текущем каталоге.
5. Проверка (запускать 5–10 раз, своим и чужим пальцем):
   ```sh
   G_MESSAGES_DEBUG=all LD_LIBRARY_PATH=../../../../deps/root/usr/lib ./verify 2>&1 | tee -a ~/verify.log
   ```
   Ожидается «MATCH!» для своего пальца, «NO MATCH!» для чужого; в логе `match: score … -> match`.
   После первого запуска появится `~/.local/state/libfprint/goodix5125/openchicago.state` (~235 КБ).
6. Если что-то не так:
   - палец не обнаруживается (висит на «Place finger») → Ctrl-C, повторить шаг 4 с
     `GOODIX5125_FDT_SEQUENCE=mr648`; прислать оба лога;
   - после касания долго нет результата (ждёт подъёма пальца) → поднять палец; если висит — то же
     `GOODIX5125_FDT_SEQUENCE=mr648`;
   - ошибки на втором кадре касания при регистрации → повторить с `GOODIX5125_ENROLL_PAIR=0`;
   - «PSK file … missing» / «does not match» / «must be a private regular file» → проверить шаг 2
     (путь из строки `state …, PSK …` в логе), **не** ставить `GOODIX5125_PROVISION_PSK`;
   - «Remove the finger before Goodix calibration» → убрать палец и повторить;
   - «… is not a valid state file» → `rm ~/.local/state/libfprint/goodix5125/openchicago.state`;
   - «No reply to Goodix command 0x..» / таймауты → прислать лог (G_MESSAGES_DEBUG=all);
   - сенсор перестал отвечать → переподключение не требуется, достаточно перезапуска примера; MCU
     драйвер не стирает и не прошивает (команд 0xa4/0xf0/0xf2/0xf4 в коде нет).

## 9. Открытые вопросы

- Железо: последовательность «probe» в асинхронном драйвере, второй кадр касания (choose_enroll_img),
  необязательный ACK в async-пути, точность ENGINE-регистрации на живых касаниях — только после теста.
- templateStudy через fprintd теряется (раздел 2). Возможное решение в драйвере: хранить выученный blob
  в state-каталоге под ключом SHA-256 исходного `fpi-data` и подставлять при verify; или предложить в
  libfprint/fprintd API «print updated». Сделано на стадии 9 (хранилище `learned/`, docs/stage9-integration.md §1).
- backup-путь ENGINE после вливания в группу (docs/stage-lib.md «Остаток») и путь замены при 50
  подшаблонах не сверены с DLL.
- Upstream-блокеры !648 остаются: одна единица железа, нет umockdev-записи (её можно снять этим
  драйвером после успешного теста).

## Коммиты (ветка `openchicago`)

- `3e58f90` goodix5125: make the ACK optional and skip stale messages (транспорт + unit-тест; собирается
  и проходит тесты отдельно).
- `e44b7c9` goodix5125: use openchicago for enrolment and matching (chicago/ → openchicago, драйвер,
  состояние, PSK, FDT, offline-тест, README).

## 10. Тест на железе (2026-09-29, HONOR MagicBook 16, examples от пользователя, PSK-файл из нулей)

- Регистрация: 12/12 стадий за 19 кадров (5 подсказок «сдвинуть палец», 1 class-reject), q 71–100, c 77–100;
  PSK из файла совпал с хешем сенсора (записи не было), TLS, FDT-ожидание, пара кадров — работают.
- Проверка: свой палец 4/4 MATCH (score 48, 67, 83, 54; подшаблоны 0/4/10/3), другой палец 1/1 NO MATCH (score 0).
- Логи: ~/enroll.log, ~/verify.log (не коммитятся).
