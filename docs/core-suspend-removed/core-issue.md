# Устройство, удалённое во время suspend, навсегда остаётся открытым (libfprint) + fprintd теряет сессию при неудачном Release

Этот файл — для отдельного issue/MR в libfprint (и заметки для fprintd). В три коммита MR !669
правки ядра **не** входят.

Файлы рядом, в этом каталоге:

| Файл | Что это |
|---|---|
| `test-suspend-removed.c` | репродьюсер на текущем ядре (upstream/master `6f9479c`): все 3 теста **проходят**, т.е. фиксируют ошибочное поведение |
| `libfprint-suspend-removed.patch` | предлагаемый патч ядра (fp-device.c, fpi-device.c), uncrustify 0.81.0 — PASS |
| `test-suspend-removed-fixed.c` | тесты ожидаемого поведения: на текущем ядре **падают**, с патчем — **проходят** |
| `build.sh` | сборка любого из двух тестов против `_build` libfprint: `build.sh <_build> test-suspend-removed[-fixed]` |

Проверено: с патчем ядра `meson test` целиком — Ok 10 / Fail 0 (34 skip — umockdev без
introspection), тесты драйвера goodix5125-device тоже зелёные. Патч из рабочего дерева убран.

## Как это проявляется (goodix5125, fprintd 1.94.5, `--no-timeout`, экран блокировки крутит PAM)

1. `PrepareForSleep(true)` → fprintd `fp_device_suspend()`; у драйвера (e3b4c12) нет `suspend` →
   `fpi_device_suspend()` вызывает `fpi_device_suspend_complete(NOT_SUPPORTED)`
   (fpi-device.c:1773), тот ставит `is_suspended = TRUE` (1958) и отменяет verify с причиной
   BUSY «Cannot run while suspended.» (1976–1980). В журнале: `Device reported an error during
   verify: Cannot run while suspended.`
2. pam_fprintd получает `verify-unknown-error` и сразу делает `Release` → `fp_device_close()`
   отвечает BUSY, потому что `is_suspended` (fp-device.c:925). В журнале: `ReleaseDevice failed:
   … still busy`.
3. **fprintd**: `dev_close_cb()` (device.c:1130–1152) сбрасывает сессию
   (`session_data_set_new (priv, NULL, NULL)`, строка 1141) **до** проверки ошибки. FpDevice
   остаётся открытым, но fprintd считает его свободным и больше никогда его не закроет; следующий
   `Claim` получает `ALREADY_OPEN` от `fp_device_open()`.
4. Гибернация. libfprint сам выставляет `power/persist=0` (fpi-device.c:1884–1885), поэтому после
   потери питания корневым хабом ядро не восстанавливает устройство, а переподключает его
   (`USB disconnect` / `new full-speed USB device`). У пользователя `persist=0` и
   `wakeup=disabled` подтверждены в sysfs.
5. GUsb → `FpContext` → `fpi_device_remove()`; задачи нет → сигнал `removed` сразу. Но
   `FpContext::device_removed_cb` (fp-context.c:149–166) для **открытого** устройства ждёт
   `notify::open`, т.е. закрытия, и только потом испускает `device-removed`, на который fprintd
   снимает объект с D-Bus (manager.c:414, 504).
6. `PrepareForSleep(false)` → `fp_device_resume()`: `is_removed` → сразу REMOVED (fp-device.c:1064),
   **`is_suspended` остаётся TRUE навсегда**. Любой `fp_device_close()` дальше → BUSY.
7. Итог: старое FpDevice открыто навсегда, `device-removed` не приходит, `Device/0` висит в
   fprintd рядом с новым `Device/1`; pam_fprintd выбирает устройство по числу отпечатков (при
   равенстве — первое), попадает в мёртвое и падает с `Authentication service cannot retrieve
   authentication info`. При остановке fprintd: `User destroyed open device!`.

Репродьюсер `test-suspend-removed.c` проходит шаги 1–2 и 4–7 на фейковом устройстве и печатает
ровно те же сообщения, что в журнале («Cannot run while suspended.», «The device is still busy with
another operation, please try again later.»).

## Что драйвер может и что не может

Сделано в драйвере (MR !669): verify/identify переживают suspend (ожидание касания прерывается,
действие остаётся активным), поэтому на шаге 1 ошибки нет, pam не делает Release в окне suspend —
шаги 2–3 больше не происходят; удаление устройства завершает действие с REMOVED, close
завершается всегда.

Чего драйвер сделать **не может**: если USB-устройство исчезло **до** `PrepareForSleep(false)` (а
при гибернации это вероятный порядок: события udev об удалении приходят сразу после разморозки,
logind шлёт `PrepareForSleep(false)` только после завершения systemd-sleep и его хуков), то:

- драйвер честно завершает припаркованный verify с REMOVED;
- клиент делает Release → `fp_device_close()` → BUSY (`is_suspended`), драйвер даже не вызывается;
- `fp_device_resume()` → REMOVED, `is_suspended` не сбрасывается;

и устройство опять застревает открытым. Сбросить `is_suspended` драйвер не может ничем
(`fpi_device_resume_complete()` требует `suspend_resume_task`, которого для удалённого устройства не
создаётся). Тест драйвера `/goodix5125/device/remove-while-suspended` это показывает: close там
возвращает BUSY на текущем ядре и REMOVED (устройство закрыто) с патчем ядра.

Тот же класс проблем касается всех драйверов, которые держат действие через suspend (synaptics,
fpcmoc) и всех, у которых suspend не поддержан (ошибка «Cannot run while suspended» массово
встречается в отчётах пользователей Framework/Fedora после сна).

## Предлагаемый патч ядра (`libfprint-suspend-removed.patch`)

1. `fp_device_close()`: удалённое устройство можно закрыть и в состоянии suspend
   (`priv->is_suspended && !priv->is_removed`). Возобновлять его всё равно никто не будет, а
   драйвер обязан уметь закрываться без устройства.
2. `fp_device_resume()`: для удалённого устройства сбрасывать `is_suspended` перед возвратом REMOVED.
3. `fpi_device_remove()`: если устройство в suspend и есть текущее действие, отменить его с
   причиной REMOVED: у «припаркованного» действия нет USB-передач, которые могли бы упасть сами,
   и без этого драйвер без собственной обработки удаления (synaptics, fpcmoc) висит до отмены
   клиентом. (Для goodix5125 это не нужно: драйвер ловит `notify::removed` сам; пункт стоит
   обсудить с мейнтейнерами — у synaptics `cancel` шлёт USB-команду, она просто упадёт.)

## Текст issue для libfprint (англ.)

> **A device removed while suspended can never be closed, so it is never removed from FpContext**
>
> When a USB reader is re-enumerated across hibernation (libfprint itself writes
> `power/persist=0`, so this is the expected path after the root hub loses power) and the device is
> open at that time, it gets stuck:
>
> 1. `fp_device_suspend()` sets `is_suspended` (in `fpi_device_suspend_complete()`), with or
>    without a running action.
> 2. GUsb reports the removal, `fpi_device_remove()` sets `is_removed` and emits `removed`.
>    `FpContext` keeps the device until it is closed (`device_removed_cb` waits for `notify::open`).
> 3. `fp_device_close()` fails with `FP_DEVICE_ERROR_BUSY` because `is_suspended` is set.
> 4. `fp_device_resume()` returns `FP_DEVICE_ERROR_REMOVED` for a removed device and leaves
>    `is_suspended` set, so every later close fails with BUSY too.
>
> The device stays open forever; `FpContext::device-removed` is never emitted, and fprintd keeps
> exporting the dead device next to the re-enumerated one (pam_fprintd may pick the dead one). On
> exit: `User destroyed open device! Not cleaning up properly!`.
>
> The removal is usually seen before `PrepareForSleep(false)`: udev events are processed as soon
> as user space is thawed, while logind signals the resume only after systemd-sleep (and its
> hooks) has finished. A driver cannot work around it: it is not called for close or resume
> of a removed suspended device, and `is_suspended` can only be cleared through
> `fpi_device_resume_complete()`.
>
> Additionally, a driver that keeps an action running over suspend (synaptics, fpcmoc) has no
> transfer in flight while suspended, so the removal does not fail the action; it only ends when
> the client cancels it.
>
> Proposed fix (patch attached, with tests based on `test-device-fake`): allow closing a removed
> device even if suspended, clear `is_suspended` when resuming a removed device, and cancel a
> suspended action with `FP_DEVICE_ERROR_REMOVED` on removal.

Тесты для MR в ядро: два теста из `test-suspend-removed-fixed.c` переносятся в
`tests/test-fpi-device.c` почти без изменений (там уже есть `FPI_TYPE_DEVICE_FAKE`, нужно лишь
заменить ручную подмену vfunc на `auto_reset_device_class ()`).

## Заметка для fprintd (англ., к issue в fprintd)

> **Release after a failed close leaves the FpDevice open and unusable**
>
> In `dev_close_cb()` (src/device.c, 1.94.5) the session is cleared with
> `session_data_set_new (priv, NULL, NULL)` before the result of `fp_device_close_finish()` is
> checked. If the close fails — libfprint refuses to close a device between `fp_device_suspend()`
> and `fp_device_resume()` with `FP_DEVICE_ERROR_BUSY` — the FpDevice stays open, but fprintd
> considers it released: nobody closes it any more and every following `Claim` fails because
> `fp_device_open()` returns `FP_DEVICE_ERROR_ALREADY_OPEN`. This is easy to hit with a lock screen
> that keeps a PAM session running: `PrepareForSleep(true)` cancels the verify with "Cannot run
> while suspended.", pam_fprintd releases the device at once, the release fails.
>
> Suggested handling: when the close fails and `fp_device_is_open (dev)` is still TRUE, remember
> that a close is pending and retry it after `fp_device_resume()` completed (and in
> `device_removed_cb`), instead of dropping it; alternatively, before calling
> `fp_device_open()` in Claim, close an open device that has no session.

Патч для fprintd не готовился и не проверялся.
