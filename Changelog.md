# Changelog — реверс алгоритма Goodix и воспроизведение конвейера

Воспроизведение проприетарного алгоритма сравнения Goodix (`win-driver/AlgoChicago.dll`,
`AlgoMilan.dll`) на Linux через нативный загрузчик Win64-DLL `tools/algo/winpe.c` (без wine).
Подробности, адреса и дизасм-доказательства — `tools/algo/re/notes/`
(индекс и эталонный конвейер — [00-overview](tools/algo/re/notes/00-overview.md)).

## Ключевые находки

1. **Штатный алгоритм для 27c6:5125 — AlgoChicago.dll + `ppp_param_init(12)`**: chip id 0x2504 →
   sensorType 0xc → LoadAlgorithm → AlgoChicago. AlgoMilan + профиль 10 тоже работает (64×80) и на
   датасете лучше. → [30](tools/algo/re/notes/30-engine-flow.md)
2. **Препроцессинг — обязательная отдельная стадия**: каждый сырой u16-кадр проходит вендорский
   `preprocessor` до enroll/identify; калибровка — `preprocess_init_calidata` + `preprocessor_init` по
   фон-кадру. → [10](tools/algo/re/notes/10-preprocess.md)
3. **Геометрия кадра**: `gimg+0x08` = ширина 80, `+0x0a` = высота 64; наш буфер 80×64 транспонируется
   в 64 строки × 80; `cal+0x24` = ROW 64, `+0x28` = COL 80. → [20](tools/algo/re/notes/20-template-identify.md), [10](tools/algo/re/notes/10-preprocess.md)
4. **`preprocessor(&src, &purpose, cbuf, &dst, qcov, 0, 0)`**: purpose 1/0, cbuf ≥ 0x4c98 байт на кадр,
   qcov = {coverage, quality}, dst+0x28 = quality, +0x29 = coverage. → [10](tools/algo/re/notes/10-preprocess.md)
5. **Вендорские вызовы**: `enrolStart()` = `enrolStartEx(&50)`; `enrolAddImage(sess, &dst, cbuf, raw16, 0, qcov)`;
   `identifyImage(&dst, cbuf, &holder, 1, &idx, &score, cq, 0, 1, raw16, 0)`, совпадение ⇔
   `rc==0 && idx>=0 && score>0`. → [30](tools/algo/re/notes/30-engine-flow.md)
6. **Хранение**: `templateGetPackedSize/templatePack` до `enrolFinish` (он разрушает шаблон);
   `templateUnPack(blob, len, NULL, &holder)` — 3-й аргумент строго NULL. `0x18000f810` — деструктор
   шаблона, не study. → [20](tools/algo/re/notes/20-template-identify.md)
7. **Проверка — `identifyImage`**; `identifytemplate` — только CheckForDuplicate при регистрации;
   `InitIdentifyImage` для 5125 не нужен. → [30](tools/algo/re/notes/30-engine-flow.md)
8. **Решение матчера Milan**: score — код исхода (101/104/111 совпадение, −100/−101/−102/−104/−701);
   внутренний порог score > 207 + сдвиг от flag (205..211) и таблицы по числу пар минуций. У Chicago
   score > 0 — величина сходства. → [20](tools/algo/re/notes/20-template-identify.md)
9. **Датасет** — сырые 12-битные кадры (`(N,4,5120)`, фон = 0, контакт = 2). → [05](tools/algo/re/notes/05-dataset.md)

## Текущий результат

`cd tools/algo && ALGO=milan ./algo_eval4 15` / `ALGO=chicago ./algo_eval4 15` (галерея — 15 natural,
одна попытка, свежая копия галереи на каждую пробу; калибровка — один фон-кадр, kr нейтральный):

| DLL / профиль | свой natural | свой varied (до 90°) | чужой (FAR) |
|---|---|---|---|
| AlgoMilan / 10 | 10/14 (71%) | 5/24 (21%) | 0/83 (0%) |
| AlgoChicago / 12 (штатный) | 8/14 (57%) | 1/25 (4%) | 0/85 (0%) |
| (для сравнения) sigfm | 12–24% | — | — |

Не помогают: калибровка фоном каждого касания, templateStudy, размер галереи, MAXT. Открытые
вопросы и следующие шаги — [70-status](tools/algo/re/notes/70-status.md).

## Инструменты / код

- `tools/fwre/dump.py` — CLI-дизассемблер PE (`func/range/calls/callers/xref/str/exports/all`).
- `tools/algo/winpe.c` — загрузчик Win64-DLL; `winpe_hook()`; поддержка AlgoChicago (Fls*, CS no-op,
  фиктивные события/потоки, `CreateFileW`/`FindFirstFileW` → INVALID_HANDLE_VALUE).
- `tools/algo/algolog.{c,h}` — перехват внутреннего логгера (Milan 0x1800088e0, Chicago 0x1800080e0), `ALGOLOG=1`.
- `tools/algo/algo_eval4.c` — вендорски-точный offline-замер (переключатели — в 00-overview).
- `tools/algo/algo_eval3.c` — эксперименты (`PAIRS`, `SHIFT`, `VIA_IDI`, `SELFONLY`, `MARK`);
  `algo_eval2.c`, `algo_eval.c` — устаревшие harness'ы.
- `tools/algo/export_raw.py` → `frames_raw.bin`, `bg_raw.bin`, `meta_raw.txt`.

Не менялось: протокол-слой `tools/goodix5125/*`, драйвер `libfprint/*`, состояние сенсора.

## Метод

Статический разбор DLL (`fwre.dump`) параллельными субагентами с ручной перепроверкой по дизасму,
затем динамическая проверка: gdb, перехват внутреннего лога DLL и offline-прогон датасета.
