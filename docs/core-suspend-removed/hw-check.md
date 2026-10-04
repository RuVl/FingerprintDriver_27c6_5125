# Проверка на железе: suspend и отключение USB во время verify

Проверяется драйвер из ветки `goodix5125-mr` (коммит `0a38fb7`), собранный в
`upstream/libfprint-mr648/_build`, **без установки в систему**: системный fprintd
останавливается, вместо него вручную запускается `/usr/lib/fprintd` с
`LD_LIBRARY_PATH` на собранную библиотеку. Пакет `libfprint-goodix5125-git` не трогается.

Безопасность: в сенсор ничего не пишется. Не задавайте `GOODIX5125_PROVISION_PSK` и
другие переменные `GOODIX5125_*`; `sudo env` ниже передаёт только перечисленные переменные.
Отключение USB-порта (тест B) не трогает прошивку; если сенсор не вернётся, его вернёт
перезагрузка.

Команды ниже работают и в fish, и в bash. Понадобятся три терминала:
**Т1** — fprintd, **Т2** — `fprintd-verify`, **Т3** — управление.

## 0. Подготовка

```sh
cd /data/source/repos/C/FingerprintDriver_27c6_5125/upstream/libfprint-mr648
git log --oneline -1                 # 0a38fb7 tests: Add goodix5125 unit tests
meson compile -C _build              # библиотека актуальна
cat /sys/bus/usb/devices/1-4/idProduct   # 5125 — сенсор на 1-4
fprintd-list ruvl                    # снимок: одно устройство, ваши пальцы
```

Тесты A–C лучше делать с **разблокированным** экраном: заблокированный экран DMS
сам занимает сенсор (PAM), и `fprintd-verify` получит «already claimed».

## 1. Запуск fprintd с новой библиотекой

Т3:

```sh
sudo systemctl mask --runtime fprintd.service   # D-Bus-активация не поднимет системный fprintd
sudo systemctl stop fprintd.service
```

`mask --runtime` действует до перезагрузки или `unmask` (см. п. 6). Запишите это в
`SYSTEM_CHANGES.local.md` как временное изменение.

Т1:

```sh
cd /data/source/repos/C/FingerprintDriver_27c6_5125/upstream/libfprint-mr648
sudo env G_MESSAGES_DEBUG=all FP_DEBUG_TRANSFER=1 LD_LIBRARY_PATH=$PWD/_build/libfprint /usr/lib/fprintd --no-timeout 2>&1 | tee ~/fprintd-hwcheck.log
```

Т3 — убедиться, что загружена собранная библиотека, а не системная:

```sh
pgrep -x fprintd | xargs -I{} sudo grep -m1 libfprint-2 /proc/{}/maps
```

Ожидается путь `…/libfprint-mr648/_build/libfprint/libfprint-2.so.2.0.0`. Если там
`/usr/lib/libfprint-2.so…` — дальше не продолжать (п. 6, вернуть сервис).

## 2. Тест A: обычный сон (s2idle) во время verify — главный

1. Т2: `fprintd-verify ruvl` — палец **не** прикладывать. В логе Т1:
   `Device reported finger status change: … -> FP_FINGER_STATUS_NEEDED`.
2. Т3: `systemctl suspend`. Разбудить клавишей клавиатуры или открытием крышки
   (не кнопкой питания — это сенсор).
3. Т2: `fprintd-verify` должен **продолжать ждать** (не завершиться ошибкой).
   Приложить палец → `Verify result: verify-match (done)`.
4. `fprintd-list ruvl` — одно устройство.

В логе Т1 при успехе (порядок важен):

```
libfprint-goodix5125-DEBUG: capture cycle stopped for system suspend
libfprint-device-DEBUG: Device reported suspend completion (error: none)
...сон...
libfprint-device-DEBUG: Device reported resume completion (error: none)
```

и после пробуждения снова USB-передачи (`FP_DEBUG_TRANSFER=1`: строки `Transfer … submitted` /
`completed` на endpoint `0x1`/`0x81`), последней остаётся висеть одна `submitted … 0x81` —
снова ожидание касания. Признаки, что что-то не так:
`Cannot run while suspended`, `Device reported an error during verify`,
`ReleaseDevice failed`, `still busy`.

Варианты (по желанию, после основного):

- **A2 — заснуть кнопкой питания** при запущенном `fprintd-verify` (палец ложится на
  сенсор): ожидается, что до сна снимок сделается, но **результат не будет выдан**
  (`fprintd-verify` продолжает ждать; в логе `capture cycle stopped for system suspend`
  после захвата). После пробуждения драйвер ждёт, пока палец уберут, и только потом новое
  касание. Если отпечаток при этом совпал до сна, разблокировки до/после сна быть не должно.
- **A3 — экран блокировки**: закрыть `fprintd-verify` (Ctrl+C), заблокировать экран,
  `systemctl suspend` (или закрыть крышку), разбудить, разблокировать **пальцем**.
  В логе — тот же `capture cycle stopped …`, без `Cannot run while suspended`.

## 3. Тест B: настоящее отключение USB во время verify (как переподключение после гибернации)

Порт 4 корневого хаба отключается программно, ядро видит `USB disconnect`, затем после
включения — новое устройство. Это ближе всего к тому, что происходит при гибернации.

1. Т2: `fprintd-verify ruvl` (палец не прикладывать).
2. Т3:
   ```sh
   echo 1 | sudo tee /sys/bus/usb/devices/1-0:1.0/usb1-port4/disable
   sudo dmesg | tail -3        # usb 1-4: USB disconnect …
   ```
3. Ожидается: `fprintd-verify` **сразу** завершается с `verify-disconnected` (не висит).
   В логе Т1: `Device reported an error during verify: This device has been removed from the
   system.`; после Release устройство снимается с D-Bus:
   `fprintd-list ruvl` → `No devices available` (или пустой список).
4. Т3:
   ```sh
   echo 0 | sudo tee /sys/bus/usb/devices/1-0:1.0/usb1-port4/disable
   sudo dmesg | tail -3        # new full-speed USB device … idProduct=5125
   ```
   Если через 5 с нового устройства нет — повторить `echo 0`, иначе перезагрузка.
5. `fprintd-list ruvl` — **ровно одно** устройство с вашими пальцами; `fprintd-verify ruvl`
   работает (первый verify после подключения инициализирует сенсор — палец в этот момент
   не держать).

## 4. Тест C: `authorized` 0/1 во время verify

Важно: `authorized=0` только снимает конфигурацию с устройства, сам USB-device (и
`/dev/bus/usb/…`) остаётся, поэтому libusb/fprintd **не видят удаления** — это проверка
«передачи падают, операция завершается ошибкой, а не висит», а не проверка удаления.

1. Т2: `fprintd-verify ruvl`.
2. Т3: `echo 0 | sudo tee /sys/bus/usb/devices/1-4/authorized`
3. Ожидается (не проверено без железа): `fprintd-verify` завершается с ошибкой
   (`verify-unknown-error` или `verify-disconnected`), не висит; устройство остаётся в
   `fprintd-list`.
4. Т3: `echo 1 | sudo tee /sys/bus/usb/devices/1-4/authorized`
5. `fprintd-verify ruvl` снова работает (драйвер заново инициализирует сенсор). Если нет —
   приложить лог; вернуть сенсор можно тестом B (disable 1/0) или перезагрузкой.

## 5. Тест D (по желанию): гибернация с verify

Драйвер больше не роняет verify при уходе в сон, но если при пробуждении удаление USB придёт
**раньше**, чем `PrepareForSleep(false)` (вероятный порядок), старое устройство всё равно
застрянет открытым — это ошибка ядра libfprint (см. `core-issue.md`), драйвер её обойти не
может. Тест нужен, чтобы увидеть фактический порядок событий.

1. Т2: `fprintd-verify ruvl`; Т3: `systemctl hibernate`; включить ноутбук.
2. Собрать: лог Т1, `fprintd-list ruvl`,
   `busctl tree net.reactivated.Fprint`.
3. В логе найти порядок строк `Device reported resume completion` / `Preparing devices for
   resume` и `This device has been removed from the system`; и есть ли
   `ReleaseDevice failed` / `still busy`.

Ожидание: с этим драйвером без патча ядра после гибернации может по-прежнему остаться два
устройства (`Device/0` мёртвое). С патчем ядра — одно.

Проверка с патчем ядра (собирается отдельно, в рабочий клон не попадает):

```sh
cd /data/source/repos/C/FingerprintDriver_27c6_5125/upstream/libfprint-mr648
git worktree add ~/lf-core goodix5125-mr
cd ~/lf-core
git apply /data/source/repos/C/FingerprintDriver_27c6_5125/docs/core-suspend-removed/libfprint-suspend-removed.patch
meson setup _build -Ddrivers=all -Dinstalled-tests=false -Dintrospection=false -Ddoc=false -Dgtk-examples=false -Dudev_rules=disabled -Dudev_hwdb=disabled
meson compile -C _build
```

Затем п. 1 с `LD_LIBRARY_PATH=$HOME/lf-core/_build/libfprint`. Убрать потом:
`git -C …/libfprint-mr648 worktree remove ~/lf-core`.

## 6. Возврат к системному fprintd

Т1: Ctrl+C. Т3:

```sh
sudo systemctl unmask --runtime fprintd.service
sudo systemctl start fprintd.service
systemctl status fprintd --no-pager | head -5
fprintd-list ruvl          # одно устройство
```

## Что прислать

- `~/fprintd-hwcheck.log` (лог Т1 за все тесты);
- вывод `fprintd-verify` и `fprintd-list ruvl` после каждого теста;
- для B/C/D — `sudo dmesg | tail -20` после шага с USB.

## Результаты (2026-10-04, ветка `0a38fb7`, библиотека из `_build`)

- **A, s2idle во время verify:** пройден. При засыпании `capture cycle stopped for system suspend`,
  `suspend completion (error: none)`; после пробуждения `resume completion (error: none)`, проверка
  подъёма пальца и новое ожидание касания; verify не прерван, приложенный палец дал `verify-match`.
  Ни `Cannot run while suspended`, ни `still busy`.
- **B, отключение порта во время ожидания касания:** `fprintd-verify` сразу завершился, старое устройство
  снято с D-Bus, после включения порта — ровно одно новое устройство, verify на нём работает.
  На железе libusb сообщает о пропаже (`G_USB_DEVICE_ERROR_NO_DEVICE`) раньше, чем libfprint отмечает
  удаление, поэтому fprintd показал `verify-unknown-error`. После этого драйвер переводит `NO_DEVICE` в
  `FP_DEVICE_ERROR_REMOVED` (fprintd: `verify-disconnected`), тест `unplug-before-removal`.
- **Отключение во время close:** close завершился ошибкой USB, fprintd снял устройство и подхватил новое.
- **D, гибернация:** не проверялась; ожидается, что без патча ядра старое устройство остаётся (`core-issue.md`).
