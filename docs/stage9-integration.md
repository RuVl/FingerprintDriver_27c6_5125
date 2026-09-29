# Стадия 9. Интеграция с fprintd: хранилище выученных шаблонов и пакет для Arch

Ветка `openchicago` в `upstream/libfprint-mr648`. К USB-устройству не обращались, sudo не использовался,
системных изменений нет.

## Чекпоинты

- 2026-09-29: прочитаны stage8 (§2, §9), libfprint-integration §5, код драйвера. Базовый `goodix5125-algo`
  OK (A: 18 обновлений study). fprintd.service (v1.94.5, из исходников fprintd — fprintd не установлен):
  `ProtectSystem=strict`, `StateDirectory=fprint` (0700), `ProtectHome=true`, `PrivateTmp=true`,
  `ReadWritePaths=/sys/devices` → писать можно только в `/var/lib/fprint` (и приватный /tmp).
  `delete` драйвера fprintd вызывает только при `FP_DEVICE_FEATURE_STORAGE` (delete + list/clear) —
  для нашего драйвера (хранилище у хоста) удаление print не видно.
- Задача 1 сделана, коммит `7438e79` (ветка `openchicago`): хранилище выученных шаблонов
  `/var/lib/fprint/goodix5125/learned/<sha256>.tpl`. `ninja` — 0 предупреждений; `meson test` — 7 OK / 0 FAIL /
  33 SKIP; offline-тест под ASan+UBSan — OK, 0 сообщений. Сценарий D: сеанс 1 — 10 study, сеанс 2 (новый объект,
  print из байтов) — 69 проб через выученный шаблон, результаты = openchicago напрямую; порча/обрезка/чужой
  sensor-id → исходный шаблон + warning; истечение и лимит работают.
- Задача 2: `packaging/arch/PKGBUILD` (`libfprint-goodix5125-git`). Проверено `makepkg -f --nodeps` в scratchpad
  (`BUILDDIR/SRCDEST/PKGDEST` — вне репозитория; libgusb из `deps/root` через `PKG_CONFIG_PATH`, т.к. в системе
  его нет): сборка без ошибок, `check()` — 9 OK / 0 FAIL / 33 SKIP, пакет `1.94.100.r4.g7438e79-1` собран,
  не установлен.

## 1. Хранилище выученных шаблонов (templateStudy через fprintd)

Проблема (stage8 §2): fprintd перед каждым verify/identify читает print с диска и после совпадения его не
сохраняет; сигнала «print обновлён» в libfprint нет. Раньше драйвер менял `fpi-data` только в памяти.

- **Где.** `<каталог состояния>/learned/`, для fprintd — `/var/lib/fprint/goodix5125/learned/` (каталог 0700);
  переопределение `GOODIX5125_LEARNED_DIR`. Один файл на print: `<sha256>.tpl`.
- **Ключ** — SHA-256 `fpi-data` print'а в нормальной форме GVariant, т.е. ровно того, что хранит fprintd
  (`(uayayay)` внутри его файла). Ключ не зависит от выученных данных: драйвер больше **не** подменяет
  `fpi-data` совпавшего print'а (иначе у вызывающего, держащего print в памяти, ключ бы «уезжал»). Подмена в
  памяти осталась только как запасной путь, если запись в хранилище не удалась.
- **Файл** = `"G5LT"` + u32 версия формата (1) + sensor id (OTP[0:16]) + ключ + SHA-256 содержимого +
  содержимое (выученный `fpi-data`, `(uayayay)`: версия print, sensor id, хеш калибровки, blob).
  Запись — `g_file_set_contents_full(CONSISTENT, 0600)`: временный файл + fsync + rename.
- **Verify/identify.** Для каждого годного print (исходный шаблон этого сенсора) ищется запись; валидна →
  сравнение с выученным blob, иначе с исходным. После study запись переписывается под тем же ключом.
- **Сбои.** Нет файла — молча исходный. Не читается / чужой формат или версия / другой ключ / контрольная
  сумма / чужой sensor id / blob не разбирается → `fp_warn("ignoring the learned template …")`, исходный
  шаблон; ошибкой операция не завершается. Следующий study перезаписывает запись.
- **Рост.** Удаление print'а драйвер не видит: fprintd вызывает `fp_device_delete_print` только при
  `FP_DEVICE_FEATURE_STORAGE`, а libfprint ставит её лишь при `delete` + (`list` или `clear_storage`)
  (`fpi-device.c:159–166`, fprintd `device.c:2142, 2395`); объявить их — значит превратить драйвер в
  «хранилище на устройстве» с другой логикой fprintd (очистка перед первой регистрацией, проверки
  device-stored) — не делаем. Поэтому при каждой записи: удаляются записи без использования > **180 дней**
  (время использования = mtime, обновляется при каждом чтении), затем, если записей > **50**, — самые давно
  использованные; незавершённые временные файлы старше часа тоже удаляются, чужие файлы не трогаются.
  Обоснование: fprintd хранит ≤ 10 пальцев на пользователя, 50 — это 5 пользователей со всеми пальцами;
  выученный blob 57–112 КБ → предел ≈ 5,6 МБ. Устаревшие записи появляются только при удалении/перерегистрации;
  если палец не использовали полгода, запись удаляется и сравнение идёт с исходным шаблоном (без отказа).
- **sandbox fprintd** (unit v1.94.5): `ProtectSystem=strict` (всё только для чтения), `StateDirectory=fprint`
  → `/var/lib/fprint` на запись, `ProtectHome=true`, `PrivateTmp=true`, `ReadWritePaths=/sys/devices`.
  `/var/lib/fprint/goodix5125/{psk,openchicago.state,learned/}` — внутри, доступно. fprintd работает от root,
  `state_directory()` выберет `/var/lib/fprint/goodix5125` (mkdir + access W_OK проходят).
- **Тест** — `tests/goodix5125-algo.c`, сценарий D: engine-регистрация; сеанс 1 — fprintd-подобный verify
  всегда исходного print со study (своих кадров через один + половина чужих), лимиты 3 записи / 30 дней на
  подложенных фиктивных записях; сеанс 2 — новый `Goodix5125Algo`, load + activate с другим ImageBase, print
  заново из байтов: каждая проба (69) идёт через выученный blob и совпадает по score/решению/study с
  openchicago напрямую с тем же blob; файл после study = новый blob; 0600. Порча содержимого, обрезка до 3 байт,
  чужой sensor id → ровно 3 warning, результат = исходный шаблон. Истечение/лимит/временные файлы — проверены.

## 2. Пакет для Arch (`packaging/arch/PKGBUILD`)

- `pkgname=libfprint-goodix5125-git`, источник `git+file://$startdir/../../upstream/libfprint-mr648#branch=openchicago`
  (makepkg сам клонирует **закоммиченную** ветку; незакоммиченные правки в пакет не попадут; другой путь —
  `LIBFPRINT_OPENCHICAGO_REPO=/путь makepkg …`). `pkgver()` = `1.94.100.r<коммиты>.g<hash>`.
- Флаги как у extra/libfprint и AUR `libfprint-tod`/`libfprint-git`: `-Ddrivers=all` (goodix5125 — драйвер по
  умолчанию, т.е. пакет — полная замена штатного), `installed-tests=false`; плюс `introspection=false`, `doc=false`
  (fprintd их не использует, из зависимостей libfprint в системе только fprintd; экономит gobject-introspection
  и gtk-doc), `udev_rules=enabled`, `udev_hwdb=enabled`.
- `depends` = extra/libfprint 1.94.100-1 (`libgcc glib2 glibc libgudev libgusb openssl pixman`);
  `provides=(libfprint=1.94.100 libfprint-2.so)` — makepkg превращает soname в `libfprint-2.so=2-64`, которого
  требует fprintd; `conflicts=(libfprint)`.
- udev/hwdb: fprintd работает от root (`DeviceAllow=char-usb_device rw`), отдельные права на USB ему не нужны.
  `60-autosuspend-libfprint-2.hwdb` (есть `usb:v27C6p5125*` → `ID_AUTOSUSPEND=1`, `ID_PERSIST=0`) дублирует
  systemd-шный `60-autosuspend-fingerprint-reader.hwdb` (там 5125 уже есть с теми же свойствами) — отсюда
  предупреждение meson «installed by both systemd and libfprint», безвредно. `70-libfprint-2.rules` — SPI-устройства.
- Предупреждение makepkg «Package contains reference to $srcdir» — upstream-путь
  `FPI_EMULATION_HELPER_BUILDDIR` (эмуляция для тестов, `fpi-device.c:102`), не наше.
- Содержимое: `libfprint-2.so*`, заголовки, `.pc`, hwdb, rules, metainfo; второй пакет `-debug` ставить не нужно.

## 3. НУЖНО ВАШЕ УЧАСТИЕ: установка и включение через fprintd

Каждый пункт «Система» — готовая запись для `SYSTEM_CHANGES.local.md` (что / зачем / откат). Команды — из
корня проекта (`! …`). До начала: examples не запущены.

**Шаг 0. Страховка.** Открыть отдельный терминал с root-сессией (`sudo -i`) и не закрывать до конца шага 5.

**Шаг 1. Сборка и установка пакета.**
```sh
cd packaging/arch
makepkg -s                     # поставит недостающую зависимость libgusb (sudo pacman), соберёт, прогонит check()
sudo pacman -U libfprint-goodix5125-git-1.*-x86_64.pkg.tar.zst      # маска исключает -debug
sudo pacman -S --needed fprintd                                       # libfprint уже «предоставлен» пакетом
pacman -Qi libfprint-goodix5125-git | grep -E '^(Version|Provides)'
```
Если libfprint из extra уже стоял — pacman предложит его заменить (conflicts), ответить `y`.
- Система: установлены `libgusb` (зависимость), `libfprint-goodix5125-git` (вместо `libfprint`), `fprintd`.
  Зачем: драйвер 27c6:5125 для fprintd. Откат: `sudo pacman -S libfprint` (заменит наш пакет, ответить `y`);
  `sudo pacman -Rs fprintd` при ненадобности; `sudo pacman -Rs libgusb`, если больше никому не нужен.

**Шаг 2. PSK для fprintd** (драйвер от root читает `/var/lib/fprint/goodix5125/psk`: обычный файл, не
ссылка, владелец = root, без прав group/other, 64 hex-цифры; нулевой ключ = хеш сенсора, в сенсор ничего
не пишется). `GOODIX5125_PROVISION_PSK` нигде не задавать.
```sh
sudo install -d -m 700 -o root -g root /var/lib/fprint/goodix5125
printf '%064d\n' 0 | sudo install -m 600 -o root -g root /dev/stdin /var/lib/fprint/goodix5125/psk
sudo stat -c '%a %U %s' /var/lib/fprint/goodix5125/psk          # ожидается: 600 root 65
```
Необязательно — перенести накопленную адаптацию из тестов examples (тот же сенсор):
`sudo install -m 600 -o root -g root ~/.local/state/libfprint/goodix5125/openchicago.state /var/lib/fprint/goodix5125/`
- Система: каталог `/var/lib/fprint/goodix5125/` (psk; драйвер сам добавит `openchicago.state`, `learned/`).
  Зачем: PSK для TLS с сенсором, состояние openchicago, выученные шаблоны. Откат: `sudo rm -r /var/lib/fprint/goodix5125`
  (после `fprintd-delete`, см. шаг 5).

**Шаг 3. Регистрация и проверка.**
```sh
sudo systemctl restart fprintd       # или stop: fprintd активируется по D-Bus заново
fprintd-list $USER                   # устройство «Goodix … 5125», отпечатков нет
fprintd-enroll -f right-thumb        # палец по вкусу; касаться кнопки, поднимать палец между касаниями
fprintd-verify -f right-thumb        # повторить 20–30 раз своим пальцем, 5–10 раз чужими
journalctl -b -u fprintd | grep -E 'goodix5125|learned' | tail -20
```
Ожидается: enroll — 12 стадий, `enroll-completed`; verify — `verify-match` своим, `verify-no-match` чужими. Считайте
долю совпадений; `sudo ls -l /var/lib/fprint/goodix5125/learned/` — после первых совпадений появится `<sha256>.tpl`
(0600). Логи с отладкой: `sudo systemctl stop fprintd; sudo G_MESSAGES_DEBUG=all /usr/lib/fprintd -t` в одном
терминале, `fprintd-verify` в другом.
- Система: отпечаток в `/var/lib/fprint/$USER/`. Откат: `fprintd-delete $USER`.

**Шаг 4. PAM — сначала только sudo.** В `/etc/pam.d/sudo` **первой** строкой после `#%PAM-1.0`:
```
auth      sufficient  pam_fprintd.so max-tries=3 timeout=15
```
(`sudoedit /etc/pam.d/sudo` или редактор из root-сессии шага 0). Проверка в **новом** терминале:
`sudo -k; sudo true` — просит палец; Ctrl-C или неудача → запрос пароля (sufficient: провал не блокирует пароль).
- Система: строка в `/etc/pam.d/sudo`. Зачем: sudo по отпечатку. Откат: удалить строку (из root-сессии).

**Шаг 5. Дальше — по желанию и только после нескольких дней sudo без сбоев**, та же строка первой `auth`-строкой в:
`/etc/pam.d/polkit-1` (окна запроса пароля GUI), затем `/etc/pam.d/system-local-login` (вход с консоли и
экранов входа, которые его включают; у GDM свой `gdm-fingerprint`). После каждой правки — проверка в новом
сеансе, root-сессия открыта; если вход сломан — вернуть файл из root-сессии. Откат — удалить строку.

**Полный откат** (обратный порядок): удалить строки PAM → `fprintd-delete $USER` →
`sudo rm -r /var/lib/fprint/goodix5125` → `sudo pacman -S libfprint` → при ненадобности `sudo pacman -Rs fprintd`.

## 4. Открытые вопросы

- fprintd с драйвером не запускался (нет fprintd и доступа к железу): поведение D-Bus-активации, таймауты
  fprintd (enroll/verify) против ожидания пальца «probe», тексты retry в `fprintd-enroll` — проверить на шаге 3.
- Точность на живых касаниях через fprintd (FRR/FAR) и выигрыш от хранилища — по результатам 20–30 попыток.
- Отказ от подмены `fpi-data` в памяти после study: для вызывающих, которые сами пересохраняют print, ключ
  теперь стабилен, выученное берётся из хранилища — поведение отличается от !648.
- Правило udev для USB-доступа пользователю (examples без root) пакет не ставит — fprintd он не нужен.
