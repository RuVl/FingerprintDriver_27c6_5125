# Интеграция драйвера 27c6:5125 с libfprint: как принято в сообществе

Дата исследования: 2026-09-28. Upstream смотрел по клону
`https://gitlab.freedesktop.org/libfprint/libfprint.git` (HEAD `6f9479c`, 2026-09-02)
и `https://gitlab.freedesktop.org/libfprint/fprintd.git` (HEAD `b10251e`, 2026-07-24).
Пути `libfprint/...` ниже — от корня upstream-репозитория.

## 0. Главное открытие

**Для 27c6:5125 уже есть открытый MR в upstream с открытым (LGPL) портом вендорского
алгоритма Chicago:** [libfprint!648](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/648)
«goodix5125: add experimental 27c6:5125 ChicagoHS driver» (автор Thomas97460, открыт
2026-08-28, статус opened, комментариев мейнтейнеров на момент проверки нет; ссылается на
issue [#497](https://gitlab.freedesktop.org/libfprint/libfprint/-/work_items/497) «Honor MagicBook 16», 2022).
Исходная ветка: `https://gitlab.freedesktop.org/Thomas97460/libfprint`, ветка `goodix-5125-oem-image`.

- Драйвер `goodix5125` — `FpDevice` (не `FpImageDevice`), host matching; enroll 12 образцов
  (`nr_enroll_stages = GOODIX_CHICAGO_ENGINE_REQUIRED_SAMPLES`), verify/identify,
  шаблон в `FpPrint` как `FPI_PRINT_RAW` + `fpi-data` = GVariant `(uayayay)`
  (версия, sensor id из OTP, дайджест калибровки, упакованный Chicago-шаблон с CRC);
  после успешного совпадения — `templateStudy` (адаптивное обновление шаблона в памяти).
- Алгоритм — каталог `drivers/goodix5125/chicago/` (~600 КБ C: preprocess, feature, match,
  enrollment, template, late-rejection), взят из
  [berkekbgz/libfprint-goodix-spi](https://github.com/berkekbgz/libfprint-goodix-spi)
  (LGPL-2.1-or-later, rev `010a665f`), где он реверснут с официального драйвера
  и «validated byte for byte with official driver» (для SPI-сенсора GDIX51C0, Huawei MateBook 16s).
- Транспорт: interface 1, EP 0x01/0x81, TLS 1.2 PSK (хост — сервер, OpenSSL memory BIO),
  собственный white-box/пэйринг (запись PSK `0xe0`, проверка хэша `0xbb020003`), PSK в
  `/var/lib/fprint/goodix5125/psk` (0600), не перезаписывает чужой пэйринг без
  `GOODIX5125_PROVISION_PSK=random`; калибровка ImageBase на каждой холодной активации.
  «No proprietary driver, firmware, or binary blob is included or loaded.»
- Тесты: unit-тесты `tests/goodix5125.c`, `tests/goodix5125-tls.c` (синтетика), **umockdev-записи нет**
  (README: «Only one OEM-integrated unit has been tested»). Удаляет 5125 из списка unsupported.

Вывод: наш PE-загрузчик для AlgoChicago/AlgoMilan — ровно тот «велосипед», который сообщество
уже заменило открытым портом. См. §5.

## 1. Устройство драйверов в upstream libfprint

**Два базовых класса** (`libfprint/fpi-device.h`, `libfprint/fpi-image-device.h`):

- `FpImageDevice` (`FpImageDeviceClass`: `img_open/img_close/activate/deactivate/change_state`,
  `img_width/height`, `bz3_threshold`). Драйвер только отдаёт картинку
  (`fpi_image_device_image_captured`), ядро само делает минуции NBIS и сравнение
  bozorth3 (`fpi-image-device.c`: `fpi_print_add_from_image`, `fpi_print_bz3_match`),
  тип отпечатка `FPI_PRINT_NBIS`. Своего сравнения не подключить — в upstream только NBIS.
  SIGFM в upstream **нет**: [!418 «add sigfm algorithm implementation»](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/418)
  открыт с 2022-11, не влит; живёт только в форке goodix-fp-linux-dev.
- `FpDevice` напрямую — все MoC и «свои» драйверы: `goodixmoc`, `synaptics`, `elanmoc`, `fpcmoc`,
  `egismoc`, `egis_etu905`, `focaltech_moc`, `mafpmoc`, `realtek`, `upekts`. Драйвер
  реализует колбэки `FpDeviceClass`: `probe/open/close/enroll/verify/identify/capture/list/delete/clear_storage/cancel/suspend/resume`;
  `fpi_device_class_auto_initialize_features()` выводит `FP_DEVICE_FEATURE_*` из
  заданных колбэков (`identify` ⇒ IDENTIFY|VERIFY; `list/delete` ⇒ STORAGE_*).
  Флаг `FP_DEVICE_FEATURE_DUPLICATES_CHECK` ставится вручную (goodixmoc).

**Как MoC/host-matching драйвер делает enroll/verify/identify:**

- Enroll: на каждый образец `fpi_device_enroll_progress(dev, completed_stages, print|NULL, error)`;
  для плохого образца — `error = fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL | _TOO_SHORT | _CENTER_FINGER | _REMOVE_FINGER | _TOO_FAST)`
  (`fp-device.h`), стадия не засчитывается; в конце `fpi_device_enroll_complete(dev, print, NULL)`.
  Число стадий — `dev_class->nr_enroll_stages` (или `fpi_device_set_nr_enroll_stages()` в рантайме).
  Пример: `drivers/goodixmoc/goodix.c` ~стр. 700–766.
- Шаблон: `fpi_print_set_type(print, FPI_PRINT_RAW)` + `g_object_set(print, "fpi-data", GVariant, NULL)`;
  для MoC ещё `fpi_print_set_device_stored(print, TRUE)` (goodixmoc хранит `(y@ay@ay)` =
  finger/tid/uid, сам шаблон на чипе). Если хранилища на чипе нет — `device_stored` не ставится,
  а в `fpi-data` кладётся весь шаблон (так делают `upekts` — `drivers/upekts.c:1122`, и MR !648).
  `fp_print_serialize()` (`fp-print.c:655`) сериализует GVariant целиком; fprintd пишет его в
  `/var/lib/fprint/<user>/<driver>/<device-id>/<finger>` (`fprintd/src/file_storage.c:42,139`).
  Размер 30–45 КБ проблем не создаёт.
- Verify: `fpi_device_get_verify_data(dev, &print)` → `g_object_get(print, "fpi-data", …)`
  → `fpi_device_verify_report(dev, FPI_MATCH_SUCCESS|FAIL|ERROR, new_scan, error)` →
  `fpi_device_verify_complete(dev, NULL)`. Retry-ошибки отдаются через `*_report`, не `*_complete`
  (`fpi-device.c:1418–1545`, там же предупреждения, если порядок нарушен).
- Identify: `fpi_device_get_identify_data(dev, &gallery)` (GPtrArray отпечатков пользователя от fprintd)
  → `fpi_device_identify_report(dev, match|NULL, new_scan, error)` → `fpi_device_identify_complete`.
  fprintd предпочитает identify, если есть `FP_DEVICE_FEATURE_IDENTIFY` (`fprintd/src/device.c:1730`).
- Обновление шаблона после совпадения (адаптивное обучение) в API не предусмотрено: fprintd
  не пересохраняет отпечаток после verify. berkekbgz для `templateStudy` патчит fprintd;
  `FP_DEVICE_FEATURE_UPDATE_PRINT` — это другое (дозапись при enroll с существующим print,
  `fp-device.c:1173`; ставится у всех `FpImageDevice`).

**Регистрация драйвера (meson):**

- `meson.build`: словарь `drivers_info` (`'goodixmoc': {}`, `'uru4000': {'helper': ['openssl']}` и т.д.);
  `default_drivers` = все не-`optional`; опция `-Ddrivers=default|all|a,b`.
- `libfprint/meson.build`: `driver_sources = {'имя': files(...)}`.
- `id_table` (`FpIdEntry {.vid,.pid,.driver_data}`) в драйвере; из него при сборке генерируются
  `fprint-list-udev-hwdb` → `data/autosuspend.hwdb` (цель `meson compile sync-udev-hwdb`,
  тест `tests/test-generated-hwdb.sh` падает, если файл не перегенерирован),
  `fprint-list-udev-rules` (70-libfprint-2.rules), `fprint-list-metainfo` (AppStream
  `org.freedesktop.libfprint.metainfo.xml`), `fprint-list-supported-devices`.
- Тесты: `tests/meson.build` → `drivers_tests = {...}`; каталог `tests/<driver>/` с
  `device` (umockdev-описание), `custom.pcapng`/`capture.pcapng` (USB-запись) и `custom.py`
  (сценарий enroll/verify/identify для не-image) или `capture.png`. Создаётся
  `sudo tests/create-driver-test.py [--test custom] <driver>` (`tests/README.md`). Для
  не-image драйверов тест — `custom.py`; для TLS-драйвера запись проходит только если
  handshake детерминирован в эмуляции (нужен фиксированный RNG/ключи при `FP_DEVICE_EMULATION=1`;
  так делает, например, `elan` для таймаутов). Формального правила «без umockdev не мержим»
  в HACKING.md нет, но все драйверы последних лет пришли с тестом (список `drivers_tests`).

## 2. Как попасть в список поддерживаемых

- Процесс: MR на `https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests`
  (`HACKING.md`, раздел Contributing). Лицензия — LGPL-2.1-or-later (`COPYING`, SPDX в файлах).
  Мейнтейнеры (`MAINTAINERS`): Marco Trevisan (3v1n0), Benjamin Berg.
  Требования к драйверам, если их пишут сами мейнтейнеры: «3 stand-alone devices… specifications
  of the protocol»; внешние MR принимаются, стиль — `scripts/uncrustify.sh`, CI (`.gitlab-ci.yml`).
- **Позиция по закрытому коду** (`HACKING.md`, «License clarification»): «no code to integrate
  proprietary drivers will be accepted in libfprint upstream… we would not know what the
  proprietary driver does». Реверс ради свободного драйвера поощряется. ⇒ загрузка
  AlgoChicago.dll/AlgoMilan.dll в upstream невозможна в принципе; открытый порт алгоритма (как в !648) — допустим.
- Supported devices: страница `https://fprint.freedesktop.org/supported-devices.html`,
  генерируется из `id_table` всех драйверов утилитой `libfprint/fprint-list-supported-devices.c`
  (Markdown-таблица, которую `mkd2html` превращает в страницу; отражает development-версию —
  попадание = драйвер влит в master).
- Unsupported devices: вики `https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices`
  → скрипт `scripts/sync-unsupported-devices.py` → `allowlist_id_table` в
  `libfprint/fprint-list-udev-hwdb.c` (между маркерами GENERATED IDS) → `data/autosuspend.hwdb`
  (включает autosuspend для неподдерживаемых). CI-джоб `test_unsupported_list`.
  **27c6:5125 есть** в вики (`| 27c6:5125 | HONOR HYM-WXX MagicBook 16 |`), в upstream
  `fprint-list-udev-hwdb.c:163` и `data/autosuspend.hwdb:521`; 5110 и 5117 — тоже (5110 ссылается на
  issue [#376](https://gitlab.freedesktop.org/libfprint/libfprint/-/work_items/376)).
  Локально в `libfprint/` (форк): `libfprint/fprint-list-udev-hwdb.c:99` (whitelist) и
  `data/autosuspend.hwdb:352`; при добавлении драйвера ID надо убрать из allowlist (как делает !648).
  Отдельная вики-страница `Devices/27c6:5125` отсутствует (есть для 5110).

## 3. libfprint-TOD (Touch OEM Drivers)

- Что это: «light fork of libfprint to expose internal Drivers API in order to create drivers as
  shared libraries» (`README.tod.md`). Репо: `https://gitlab.freedesktop.org/3v1n0/libfprint`, ветка
  `tod` (HEAD `17b3bfd`, «libfprint-TOD v1.95.1+tod1», 2026-03-10; последний тег `v1.95.2+tod1`),
  есть `tod-devel`. Пример модуля: `https://gitlab.freedesktop.org/3v1n0/libfprint-tod-example-driver`.
- Механизм (`libfprint/tod/tod-shared-loader.c`, `libfprint/tod/meson.build`): каталог модулей
  `$prefix/$libdir/libfprint-2/tod-1/` (`TOD_DRIVERS_DIR`, переопределяется `FP_TOD_DRIVERS_DIR`);
  каждый `.so` открывается `g_module_open(..., G_MODULE_BIND_LAZY|G_MODULE_BIND_LOCAL)`, из него берётся
  `fpi_tod_shared_driver_get_type()` → GType класса `FpDevice`/`FpImageDevice`. Модуль собирается против
  pkg-config `libfprint-2-tod-1`; экспортируемый driver-API фиксирован version-script
  `libfprint/tod/libfprint-tod.ver.in` (`LIBFPRINT_TOD_1.0.0`: `fpi_device_*`, `fpi_print_*`, `fpi_ssm_*`,
  `fpi_usb_transfer_*`, 12 символов `fpi_image_device_*`, SDCP). API-версия `tod1`; при смене ABI —
  новый каталог `tod-N`. Есть «костыли» под конкретные бинарники (`tod-goodix-wrapper.c` для `goodix-tod`).
- Ограничения: модуль грузится **внутрь процесса fprintd** — наследует его sandbox (см. §4);
  TOD-ветка отстаёт от upstream; дистрибутив должен ставить `libfprint-tod` вместо `libfprint`;
  модуль не может заменить алгоритм у `FpImageDevice` (сравнение — NBIS в ядре), только свой `FpDevice`.
- Реальные модули (все — закрытые `.so` от OEM через Canonical/Launchpad `~oem-solutions-engineers`):
  `libfprint-2-tod1-goodix` (Goodix 53xc MoC, Dell/Lenovo), `libfprint-2-tod1-broadcom` (Dell ControlVault 3),
  `libfprint-2-tod1-elan`, `libfprint-2-tod1-goodix-v2`; открытый — `synaTudor` (см. §4).
- Arch/AUR (проверено через AUR RPC 2026-09-28): `libfprint-tod` 1.95.2+tod1 (provides `libfprint`,
  `libfprint-2.so`; conflicts `libfprint`); модули `libfprint-2-tod1-goodix` 0.0.9 (out-of-date),
  `-goodix-v2`, `-broadcom`, `-broadcom-cv3plus`, `-elan`, `-xps9300-bin`, `-synatudor-git`.
  В `extra` сейчас `libfprint 1.94.100`, `fprintd 1.94.5`.
- Для нас: TOD — легальный способ поставлять **закрытый** модуль отдельно от upstream, но (а) с открытым
  портом алгоритма (§0) закрытая часть не нужна вовсе; (б) PE-загрузчик внутри fprintd упрётся в
  `MemoryDenyWriteExecute` (§4). TOD имеет смысл только как упаковка «драйвер без пересборки libfprint»,
  ценой зависимости от форка `libfprint-tod`.

## 4. Прецеденты: Windows-DLL / закрытые алгоритмы в Linux

- **synaTudor** (`https://github.com/Popax21/synaTudor`, LGPL-2.1, 149★, push 2025-06) — ближайший
  аналог нашего `winpe.c`: грузит x86-64 Windows-драйвер Synaptics Tudor (`libtudor/src/pe/*`,
  `libtudor/src/winapi/*` — заглушки WinAPI/bcrypt/WDF), драйвер скачивается и распаковывается
  `innoextract` при сборке (не распространяется). Архитектура: TOD-модуль (`libfprint-tod/`) в fprintd
  ↔ D-Bus-сервис `tudor-host-launcher` (отдельный systemd unit) запускает процесс `tudor-host`,
  который сам себя сэндбоксит (`tudor-host/src/sandbox.c`: `unshare(CLONE_NEWUSER)`, размонтирование `/`,
  seccomp-allowlist через libseccomp). Загрузчик делает анонимный `mmap(RW)` + `mprotect(...PROT_EXEC)`
  (`libtudor/src/loader.c:61,124`) — именно поэтому его нельзя запускать в fprintd: README
  launcher'а: «fprintd is sandboxed in such a way that the host's own sandbox fails to properly
  initialize». AUR: `libfprint-2-tod1-synatudor-git`.
- **taviso/loadlibrary** (`https://github.com/taviso/loadlibrary`, GPL-2.0, не архивирован, push 2025-04):
  PE-загрузчик из ndiswrapper для фаззинга; сборка требует 32-битных `glibc-devel.i686`/`gcc-multilib`,
  «we need the 32bit libraries to use the 32bit dll» — **только x86 (PE32)**, x86-64 DLL не грузит;
  GPL-2.0 несовместима с LGPL-линковкой в libfprint. Для нас непригоден.
- **Wine/winelib**: тянет весь Wine-рантайм в процесс демона; для драйверов отпечатков в сообществе не
  используется (прецедентов в libfprint/fprintd не нашёл).
- **python-validity + open-fprintd** (`https://github.com/uunicorn/python-validity`, MIT;
  `https://github.com/uunicorn/open-fprintd`, GPL-2.0): Synaptics/Validity 138a:*. Закрытая часть —
  прошивка-расширение (`xpfwext`), которую `bin/validity-sensors-firmware` выкачивает из
  Lenovo-инсталлятора и распаковывает `innoextract`, затем заливает в сенсор; сравнение — на чипе.
  libfprint не используется вообще: open-fprintd — замена fprintd с D-Bus API, бэкенд — отдельный Python-сервис.
  Т.е. закрытое — только «данные для устройства», в хост-процесс чужой код не грузится.
- **goodix-fp-linux-dev**: `goodix-fp-dump` (`https://github.com/goodix-fp-linux-dev/goodix-fp-dump`, MIT,
  push 2023-05; `run_5110.py` и др., «very unstable») и форк libfprint
  (`https://github.com/goodix-fp-linux-dev/libfprint`, push 2023-07; ветки `goodixtls`, `sigfm`,
  `buildpackage`, `0x00002a/libfprint-sigfm`, `wip/mpi3d/libfprint-sigfm`). Драйверы `goodixtls511`
  (5110) и `goodix5xx`/`goodixtls` — `FpImageDevice` + SIGFM. Upstream-статус: драйвера в upstream нет,
  SIGFM — [!418](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/418) открыт с 2022.
  Работ по вендорскому алгоритму у них нет. Проект фактически заморожен (2023). В AUR: `libfprint-goodixtls-git`,
  `libfprint-goodixtls511-git`, `libfprint-goodixtls-55x4` и т.п. — форки целиком (conflicts `libfprint`).
- **berkekbgz/libfprint-goodix-spi** (§0): вендорский Chicago-матчер реверснут в открытый C, а не
  загружается из DLL; out-of-tree «shadow» сборка libfprint+fprintd в `/opt` через `install.sh`
  (пинованы libfprint v1.94.10 / fprintd v1.94.5). Это и есть «как принято» для закрытого алгоритма.

**systemd-hardening fprintd** (upstream `fprintd/data/fprintd.service.in`, проверено):
`SystemCallFilter=@system-service`, `MemoryDenyWriteExecute=true`, `ProtectSystem=strict`,
`ProtectHome=true`, `PrivateTmp=true`, `NoNewPrivileges=true`, `RestrictAddressFamilies=AF_UNIX AF_LOCAL AF_NETLINK`,
`DeviceAllow=char-usb_device rw` (+spi/hidraw), `StateDirectory=fprint`, `ReadWritePaths=/sys/devices`.

- `MemoryDenyWriteExecute` (man systemd.exec, systemd 261): запрещены `mmap` с PROT_EXEC|PROT_WRITE,
  **любой** `mprotect`/`pkey_mprotect` с PROT_EXEC и `shmat(SHM_EXEC)`; реализуется seccomp или
  `prctl(PR_SET_MDWE)`. ⇒ текущий `winpe.c` (RWX-mmap по 0x180000000) в fprintd упадёт с EPERM,
  как и схема synaTudor (RW→mprotect RX).
- `arch_prctl` входит в `@default` ⊂ `@system-service` (`systemd-analyze syscall-filter @default`) —
  `ARCH_SET_GS` для TEB разрешён.
- Обход **без ослабления** unit'а: так же, как ld.so грузит .so — `mmap(PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED_NOREPLACE, fd, off)`
  исполняемых секций прямо из файла; это разрешено обоими механизмами. Условия: (1) смещение секции в
  файле ≡ VA по модулю страницы — у PE `FileAlignment=0x200`, поэтому DLL нужно один раз
  (при установке/первом запуске) переложить в «image-файл» с раскладкой по `SectionAlignment`
  (например в `/var/lib/fprint/...` или `/usr/lib/...`); (2) грузить по предпочтительной базе
  (0x180000000), чтобы не применять base relocations к `.text`; (3) импорты патчатся в IAT
  (`.rdata`, RW без X); (4) ms_abi-переходники — скомпилированный код в нашем .so, без генерации
  трамплинов в рантайме. Путь через `memfd_create`/`/dev/shm` тоже технически проходит (man прямо
  называет это обходом), но это именно обход защиты — не рекомендуется. Файл образа с
  `ProtectSystem=strict` можно писать только в `StateDirectory` (`/var/lib/fprint`); если он на
  разделе с `noexec` — mmap PROT_EXEC не пройдёт.

## 5. Рекомендация

1. **Не тащить DLL в продукт.** Upstream закрытый код не примет (HACKING.md), в fprintd RWX запрещён,
   а для 5125 уже есть открытый LGPL-порт Chicago-матчера (berkekbgz → MR !648). Первым делом —
   прогнать `drivers/goodix5125/chicago/` из !648 на `dumps/dataset` (офлайн, как `tools/tune`) и сравнить
   FRR/FAR с SIGFM и с нашим `winpe`+AlgoChicago/AlgoMilan. Наш PE-загрузчик оставить только как
   **оракул** для проверки паритета (dev-инструмент, не пакет).
2. **Архитектура: `FpDevice` с собственными enroll/verify/identify** (как !648 / goodixmoc без
   `device_stored`): `FPI_PRINT_RAW`, шаблон в `fpi-data` GVariant с версией и sensor-id,
   `nr_enroll_stages` = требование матчера, `fpi_device_enroll_progress` + `FP_DEVICE_RETRY_*`, identify
   по галерее от fprintd. `FpImageDevice` не подходит — в upstream сравнение только NBIS.
3. **Не делать второй параллельный драйвер 5125.** Правильный путь — присоединиться к !648:
   протестировать на своём MagicBook 16, отписать в MR/issue #497, предлагать патчи (наши находки: FDT
   байт 2 — маска касания, потеря ACK, OTP-подстановка в конфиг). Внимание: !648 при чужом/нашем PSK
   откажется работать без `GOODIX5125_PROVISION_PSK=random` (перезапись PSK командой 0xe0 — не
   прошивка/стирание, но меняет состояние сенсора ⇒ записать в `SYSTEM_CHANGES.local.md`, согласовать с
   пользователем). Upstream-блокеры !648: одна единица железа (HACKING просит 3 устройства), нет umockdev-теста.
4. **Если всё же нужен закрытый алгоритм** (порт хуже DLL): вынести в отдельный процесс-хелпер по схеме
   synaTudor (TOD-модуль или наш драйвер ↔ D-Bus/сокет ↔ сэндбокс-процесс с DLL, DLL скачивается/извлекается
   пользователем, не распространяется), либо внутри fprintd — file-backed RX-маппинг предразложенного
   образа (§4). В upstream это в любом случае не попадёт; упаковка — AUR `libfprint-tod` + модуль.
5. **Упаковка для Arch сейчас:** PKGBUILD форка/ветки libfprint с `provides=('libfprint' 'libfprint-2.so')`,
   `conflicts=('libfprint')` (так делают `libfprint-tod`, `libfprint-goodixtls-git`), штатный `fprintd`
   из `extra`, PAM через `pam_fprintd.so` (`/etc/pam.d/`). Базироваться лучше на upstream master + патч
   !648, а не на заброшенном форке goodix-fp-linux-dev (2023, SIGFM).
6. **Списки:** в «supported» 5125 попадёт только с влитием драйвера в upstream master
   (страница генерируется из `id_table`). До того — можно дополнить вики Unsupported-Devices
   (колонка статуса для 27c6:5125 пуста) ссылкой на !648 и создать страницу `Devices/27c6:5125`.

## 6. Проверено позже: наш нулевой PSK совместим с !648 без записи в сенсор

`goodix5125_pairing_wrap`+`_hash` из !648 для PSK = 32 нулевых байта дают
`b5e0beeb94c84eb99b883abd5c251073c56b91035c562a91a46c7f3349c36c89` — ровно хеш на нашем сенсоре
(проверено сборкой их `goodix5125-pairing.c`, scratchpad `psk_zero.c`). Обёртка детерминирована.
Поэтому достаточно положить в PSK-файл (`/var/lib/fprint/goodix5125/psk` или `GOODIX5125_PSK_FILE`)
64 символа `0` (hex), права 0600, владелец — пользователь сервиса: драйвер найдёт совпадение хеша и
**не будет писать PSK** (`GOODIX5125_PROVISION_PSK=random` НЕ нужен).
