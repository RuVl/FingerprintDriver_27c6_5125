# Отпечаток на экране блокировки: зависания и ошибки после сна (разбор 2026-10-14)

Источники: журнал 06–14.10, fprintd 1.94.5, pam_fprintd, libgusb 0.4.9, systemd-logind v262,
DankMaterialShell 1.6.3 (`Modules/Lock/Pam.qml`), quickshell 0.3.2, драйвер `goodix5125` (MR !669).

## Симптомы

| Что видно | Сколько за неделю | Причина |
|---|---|---|
| После выхода из гибернации отпечаток не работает до перезагрузки, все `Claim` → «Device was already claimed», остановка fprintd 90 с | 3 эпизода (10.10, 13.10, 14.10) | вечный вложенный цикл, §1 |
| «Fingerprint error» сразу после «Failed to match», в журнале `Finger contact changed during Goodix calibration` | 13 сессий | калибровка с пальцем на сенсоре, §2 |
| «Fingerprint error» после пробуждения, в fprintd мёртвое устройство рядом с новым | 5× `Error closing device after disconnect: busy` | переподключение сенсора, §3 |
| `Failed to install a sleep delay inhibitor: OperationInProgress` | 10 | безвредно, §3 |
| «Failed to match fingerprint» | 13 сессий на DMS 1.6.3 | несовпадения, не разбирались |

## 1. Вечный вложенный цикл (главное)

Цепочка, проверено по коду:

1. Драйвер инициализирует сенсор синхронно внутри vfunc `verify`/`identify` (`ensure_activated` →
   `initialize_sensor`, ~1,2 с на каждую пару Claim/VerifyStart: `close` сбрасывает инициализацию).
2. Синхронная передача libgusb (`g_usb_device_bulk_transfer`, через `fpi_usb_transfer_submit_sync`)
   ждёт, крутя `g_main_loop_run` на контексте по умолчанию, то есть на главном цикле fprintd. Внутри
   vfunc fprintd обрабатывает D-Bus.
3. DMS прерывает PAM-сессию через `abort()` = SIGKILL подпроцесса (quickshell `conversation.cpp`):
   на каждое снятие блокировки, PrepareForSleep, гашение мониторов. pam_fprintd не успевает сделать
   VerifyStop/Release.
4. fprintd на исчезновение клиента (`_fprint_device_client_vanished`, device.c:988) крутит
   `while (current_action != ACTION_NONE) g_main_context_iteration ()`. Если это случилось внутри
   вложенного цикла libgusb, действие не завершится никогда: его vfunc ниже по стеку.
5. Дальше fprintd отвечает на `GetDevices` (вложенный цикл крутится), отказывает в `Claim` (проверка в
   потоке `g-authorize-method`), а SIGTERM только завершает внешний цикл → SIGKILL через 90 с.

Триггер 14.10: после разблокировки отпечатком из кадров кнопки питания niri/DMS 9 раз подряд
«снимают» и «ставят» блокировку (так же 10.10 и 13.10), сессия B убита посреди инициализации.

**Исправлено в драйвере:** синхронные передачи ждут на собственном `GMainContext`
(`goodix5125-usb.c: submit_sync`), D-Bus внутри vfunc больше не обрабатывается.
**Страховка:** `packaging/fprintd/stop-timeout.conf` (`TimeoutStopSec=5s`).
**Сознательно не сделано:** асинхронная инициализация (FpiSsm). Главный поток fprintd по-прежнему
занят ~1,2 с на сессию, но D-Bus внутри vfunc больше не обрабатывается, а перевод инициализации
на машину состояний рискует сломать тайминги MCU (ACK, пауза после ответа).

## 2. Калибровка с пальцем на сенсоре

Инициализация снимает фон без пальца. DMS после несовпадения сразу начинает новую сессию, палец ещё
лежит → `G_IO_ERROR_BUSY` → fprintd `verify-unknown-error` → pam `PAM_AUTHINFO_UNAVAIL` (код 9) →
DMS считает это сбоем и ждёт 1,5 с, затем 35 с, затем по 60 с.

**Исправлено в драйвере:** это `FP_DEVICE_RETRY_REMOVE_FINGER`, сообщённый через
`fpi_device_verify_report`/`identify_report` после возврата vfunc (с паузой 300 мс). fprintd сам
перезапускает действие, pam остаётся в той же сессии и не тратит попытку. Для enroll — прежняя ошибка.

## 3. Переподключение сенсора и сон

- libfprint пишет `power/persist=0`, ядро переподключает сенсор после гибернации и иногда после
  s2idle (6 раз с 07.10). Открытое в этот момент устройство libfprint закрыть не может
  (`docs/core-suspend-removed/core-issue.md`). **Исправлено:** патч ядра накладывается в PKGBUILD.
- Страховка — `udev/72-goodix-5125-replug.rules` + `packaging/fprintd/goodix5125-replug.service`:
  после конца операции сна, только если в fprintd больше одного устройства. Безусловный перезапуск
  после пробуждения убивал бы первую проверку и с ней кадры кнопки питания (MCU отдаёт их один раз).
- Отказ в ингибиторе сна у fprintd, запущенного посреди операции сна, безвреден: fprintd просит его
  заново на каждый `PrepareForSleep(false)` (manager.c:362-386), к концу той же операции он есть.

## Что осталось вне проекта

- **DMS:** «мигание» блокировки после пробуждения (niri/DMS), SIGKILL pam-подпроцесса вместо SIGINT,
  verify при закрытой крышке. В master DMS есть `ba8c4e60a` (#3774, не стартовать отпечаток между
  запросом сна и PrepareForSleep) — обновиться, когда выйдет релиз после 1.6.3.
- **fprintd upstream:** `dev_close_cb` сбрасывает сессию до проверки ошибки close; вложенные циклы
  без ограничения (device.c:988, 1187); resume, пришедший до завершения suspend, теряется
  (fp-device.c:1057, ошибку fprintd игнорирует).
- **Несовпадения:** 13 «Failed to match» за 4 дня — отдельная тема (качество кадров, обучение шаблона).

## Проверка на железе

Воспроизведение вечного цикла без пальца: прервать `fprintd-verify` во время инициализации.

```sh
for d in 0.3 0.5 0.7 0.9 1.1; do
  fprintd-verify >/dev/null 2>&1 & p=$!; sleep $d; kill -9 $p; sleep 1.5
done
fprintd-list "$USER"     # отвечает
fprintd-verify           # «Place your finger», а не «already claimed»
```

На старом драйвере после одной из итераций `fprintd-verify` получает «Device was already claimed».
