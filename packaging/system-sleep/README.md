# Хуки systemd-sleep для HONOR MagicBook 16 (Ryzen 5 5600H)

Не входят в пакет драйвера, ставятся вручную в `/usr/lib/systemd/system-sleep/`
(systemd v262 читает хуки только оттуда):

```sh
sudo install -m755 lid-s2h fprintd-after-hibernate /usr/lib/systemd/system-sleep/
```

Откат: удалить файлы. Журнал: `journalctl -t lid-s2h -t fprintd-after-hibernate`.

## lid-s2h — рабочий suspend-then-hibernate

На этом ноутбуке штатный `suspend-then-hibernate` не работает:

- будильник RTC не будит из s0i3: SMU-прошивка 64.45 (BIOS 2.12), пробуждение по таймеру
  появилось в 64.53, обновлений BIOS нет;
- пробуждение по низкому заряду (ACPI `_BTP`) systemd не распознаёт: ждёт DMI wake type
  «APM Timer», который при выходе из s2idle не выставляется.

EC будит систему через GPIO 18 при каждом изменении заряда (на батарее примерно раз в 30 мин,
на зарядке чаще). Хук после такого пробуждения при закрытой крышке снова усыпляет систему, не
размораживая сессию, а на батарее через `DELAY` (1 ч), при падении заряда на `DROP_PCT` (10%)
или при заряде ≤ `LOW_PCT` (10%) запускает гибернацию через `systemctl hibernate`. Открытие
крышки и кнопка питания дают обычное пробуждение. Параметры — в начале файла.

Требует `HandleLidSwitch=suspend-then-hibernate` в logind и **не** совместим с параметром ядра
`gpiolib_acpi.ignore_wake=AMDI0030:00@18`: с ним EC во сне ничего не сообщает, а расход во сне
вырастает до ~2 Вт.

## fprintd-after-hibernate — обход для сенсора после гибернации

После гибернации USB переподключается, и fprintd (1.94.5) может оставить старое устройство
открытым рядом с новым, а pam_fprintd выберет мёртвое. Причина в ядре libfprint и в fprintd,
разбор и патч — `docs/core-suspend-removed/`. Хук перезапускает fprintd после гибернации.
Не нужен, когда исправление попадёт в libfprint.
