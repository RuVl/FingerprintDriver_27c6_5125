# Реверс алгоритма Goodix (AlgoMilan / AlgoChicago): обзор

Задача: воспроизвести на Linux вендорский конвейер сравнения отпечатков через наш
загрузчик Win64-DLL `tools/algo/winpe.c` (без wine). DLL — `win-driver/`, imagebase
`0x180000000`, все адреса — VA. Уровни уверенности: **ТОЧНО** (дизасм/эксперимент),
*вероятно*, _гипотеза_/«не проверено».

## Файлы

| файл | что |
|---|---|
| 00-overview.md | этот индекс, инструменты, адреса, эталонный конвейер, грабли |
| 05-dataset.md | датасет и экспорт сырых кадров |
| 10-preprocess.md | препроцессор: профили, калибровка, `preprocessor` и ядро |
| 20-template-identify.md | шаблон, enroll, pack/unpack, identifyImage/identifytemplate, матчер и решение |
| 30-engine-flow.md | EngineAdapter: выбор DLL, 29 обёрток, WBDI-слоты, точные аргументы вызовов |
| 70-status.md | текущие результаты, что не помогает, открытые вопросы, следующие шаги |
| 80-calibration.md, 81-enroll-protocol.md | в работе: калибровка calidata; протокол регистрации EA |

## Инструменты

Дизассемблер (`tools/fwre/dump.py` поверх `fwre/pe.py`):
```
cd tools
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> func <addr>      # функция по .pdata
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> range <lo> <hi>
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> calls <addr>     # вызовы внутри функции
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> callers <addr>
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> xref <addr>      # rip-ссылки
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> str <substr>
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> exports
../.venv/bin/python -m fwre.dump ../win-driver/<DLL> all > x.asm      # вся .text по функциям
```
(RuntimeWarning про sys.prefix — игнорировать.)

Замер: `tools/algo/algo_eval4.c` — вендорски-точный offline-прогон датасета
(галерея из первых N natural, свежая копия галереи через pack/unpack на каждую пробу).
```
cd tools/algo && ALGO=milan ./algo_eval4 15      # или ALGO=chicago (по умолчанию)
```
env: `ALGO=chicago|milan`, `PPP=<профиль>` (12 / 10), `ALGOLOG=1` (внутренний лог DLL в stderr),
`VERBOSE=1`, `WINPE_V=1`, `MAXT=<n>` (enrolStartEx, по умолчанию 50), `STUDY=1`
(templateStudy+repack после совпадения), `PERBG=1` (калибровка фоном каждого касания).
Отклонения от вендора для сравнения: `NOUNPACK` (живая галерея без pack/unpack),
`PARAMZERO` (param identifyImage = нули), `BYTE0` (studyflag 0), `PURPOSE0` (purpose 0
для enroll), `EADD_O3` (arg3 enrolAddImage = мусорный int вместо cbuf).

Лог алгоритма: `winpe_hook(va, fn)` (winpe.c) подменяет внутренний логгер DLL на
`algolog.c` (ms_abi varargs → stderr). Логгер: AlgoMilan `0x1800088e0`, AlgoChicago
`0x1800080e0`. Включается `ALGOLOG=1`.

winpe.c поддерживает AlgoChicago: Fls*, критические секции — no-op, фиктивные
события/потоки, `CreateFileW`/`FindFirstFileW` → `INVALID_HANDLE_VALUE`.

Прочие: `algo_eval3.c` (режимы `PAIRS`, `SHIFT`, `VIA_IDI`, `SELFONLY`, `MARK`; аргументы
НЕ вендорские), `export_raw.py` (см. 05).

## Адреса

Экспорты-обёртки `*Wrapper` пробрасывают аргументы во внутренние функции 1:1.

| функция | Milan wrapper | Milan внутр. | Chicago wrapper | Chicago внутр. |
|---|---|---|---|---|
| ppp_param_init | 0x18000b7b0 | 0x18000e670 | 0x18000afb0 | 0x18000d9d0 |
| preprocessor_init | 0x18000b7d0 | 0x18000f200 | 0x18000afd0 | 0x18000e4a0 |
| preprocessor_exit | 0x18000b7f0 | 0x18000f160 | 0x18000aff0 | 0x18000e400 |
| preprocessor | 0x18000b810 | 0x18000efa0 | 0x18000b010 | 0x18000e230 |
| preprocess_save_calidata | 0x18000b870 | 0x18000ebd0 | 0x18000b070 | 0x18000df30 |
| preprocess_load_calidata | 0x18000b8b0 | 0x18000e8b0 | 0x18000b0b0 | 0x18000dc00 |
| preprocess_get_calidata_len | 0x18000b8f0 | 0x18000e800 | 0x18000b0e0 | 0x18000da80 |
| preprocess_init_calidata | 0x18000b920 | 0x18000e820 | 0x18000b100 | 0x18000da90 |
| preprocess_set_mode | — | 0x18000ef90 | — | 0x18000e220 |
| preprocessor_get_CalibParam | — | 0x18000f1a0 | — | 0x18000e440 |
| enrolStart | 0x18000b940 | 0x18000d9b0 | 0x18000b120 | 0x18000cc40 |
| enrolStartEx | 0x18000b960 | 0x18000d9d0 | 0x18000b140 | 0x18000cc60 |
| enrolAddImage | 0x18000b980 | 0x18000d590 | 0x18000b160 | 0x18000c820 |
| enrolDeleteImage | 0x18000b9d0 | 0x18000d860 | 0x18000b1b0 | 0x18000caf0 |
| enrolGetTemplate | 0x18000b9f0 | 0x18000d950 | 0x18000b1d0 | 0x18000cbe0 |
| enrolFinish | 0x18000ba20 | 0x18000d8e0 | 0x18000b200 | 0x18000cb70 |
| InitIdentifyImage | 0x18000ba40 (заглушка 0x83) | — | 0x18000b220 | ? |
| identifyImage | 0x18000ba60 | 0x18000de30 | 0x18000b240 | 0x18000d170 |
| templateStudy | 0x18000bb40 | 0x18000e480 | 0x18000b330 | 0x18000d7e0 |
| templateGetPackedSize | 0x18000bb60 | 0x18000e380 | 0x18000b350 | 0x18000d6e0 |
| templatePack | 0x18000bb80 | 0x18000e3d0 | 0x18000b370 | 0x18000d730 |
| templateUnPack | 0x18000bbb0 | 0x18000e590 | 0x18000b3a0 | 0x18000d8f0 |
| templateDelete | 0x18000bbf0 | 0x18000e2f0 | 0x18000b3e0 | 0x18000d650 |
| identifyUpdate | 0x18000bc30 | 0x18000e1a0 (заглушка `xor eax,eax`) | 0x18000b420 | 0x18000d500 |
| identifytemplate | 0x18000bc90 | 0x18000e1b0 | 0x18000b480 | 0x18000d510 |
| getTemplateInfo | 0x18000bcd0 (заглушка 0x83) | — | 0x18000b4c0 | 0x18000d000 |
| getLastMatchedFingerTemplatesNum | — | — | — | 0x18000ced0 |

Внутренние AlgoMilan (подробности — в 10/20):

| адрес | что |
|---|---|
| 0x1800088e0 | логгер |
| 0x18002dea0 | ядро препроцессора (калибровка и применение) |
| 0x18002c060 | скоринг качества в ядре (читает `[purpose]`) |
| 0x1800BCE30..0x1800BCE4C | глобалы профиля ppp и флаг PARAM_INIT |
| 0x1800BCE50 | блок калибровки (kr, b, ctx; 0x3048c байт) |
| 0x1800ED2DC | флаг CALIBRATED |
| 0x18000fbd0 | конструктор шаблона T (0x8e08 байт) |
| 0x18000f810 | деструктор шаблона (НЕ study) |
| 0x180015150 | getFeature |
| 0x180017000 | fingerFeatureRegister |
| 0x18001d1f0 | матчер (диспетчер по типу шаблона) |
| 0x18001b010 | матчер типов 9/10 |
| 0x1800b7550 / 0x1800b7560 | фичи пробы / ctx матчера |
| 0x180094270 | таблица профилей ppp (12×32 байта) |

AlgoChicago: таблица профилей `0x180079180` (`ppp_param_init` принимает 0..12: `cmp edx,0xd; jb`),
логгер `0x1800080e0`, версия `Milan_v_3.02.00.15` (путь исходников
`Milan_coating_3.02.00.15\fp_core\...`). Внутренности AlgoChicago детально не разбирались.
AlgoMilan: версия `Milan_v_3.00.20`.

## Эталонный конвейер (как у EngineAdapter; проверено algo_eval4)

Выбор DLL: 27c6:5125 → chip id 0x2504 → sensorType 0xc → **AlgoChicago + профиль 12**
(см. 30). AlgoMilan + профиль 10 тоже работает (кадр 64×80) и на датасете даже лучше (70).

```c
/* INIT — один раз */
ppp_param_init(12);                 /* AlgoChicago; для AlgoMilan — 10 */
preprocess_init_calidata();         /* kr = 8192 (×1.0), b = 0 */
struct { uint8_t _0[0x18]; uint16_t *bg;  /* +0x18: u16 фон-кадр 64×80 (транспонирован) */
         uint8_t _20[4]; int32_t row;     /* +0x24 = 64 */
         int32_t col;                     /* +0x28 = 80 */ } cal = {0};
preprocessor_init(&cal);            /* 0 → CALIBRATED=1 */

/* НА КАЖДЫЙ КАДР. Наш буфер — 80 строк × 64 столбца → транспонировать в 64 строки × 80. */
struct gimg src = { .data = raw16, [+0x08] = 80, [+0x0a] = 64, [+0x0e] = 16, [+0x0f] = 1,
                    [+0x14] = 10240, [+0x18] = 1, [+0x1c] = 0 };
struct gimg dst = { .data = u8[5120], [+0x08] = 80, [+0x0a] = 64, [+0x0e] = 8, [+0x0f] = 1,
                    [+0x14] = 5120, [+0x18] = 1, [+0x1c] = 0 };
int purpose = enroll ? 1 : 0;
int qcov[2];                         /* {coverage, quality} */
uint8_t *cbuf = malloc(0x4c98);      /* свой на каждый кадр, живёт до enrol/identify */
rc = preprocessor(&src, &purpose, cbuf, &dst, qcov, 0, 0);   /* dst+0x28=quality, +0x29=coverage */

/* ENROLL */
void *sess = enrolStart();                                    /* = enrolStartEx(&50) */
rc = enrolAddImage(sess, &dst, cbuf, raw16, 0, qcov);         /* × N кадров */
void *holder; enrolGetTemplate(sess, &holder);                /* *holder == T */
int n = templateGetPackedSize(holder); templatePack(holder, blob);
enrolFinish(sess);                                            /* разрушает holder — паковать ДО */

/* VERIFY — один шаблон за вызов */
void *h = NULL; templateUnPack(blob, n, NULL, &h);            /* 3-й арг строго NULL */
int idx, score, cq[2];                                        /* cq = {coverage, quality} */
rc = identifyImage(&dst, cbuf, &h, 1, &idx, &score, cq, 0 /*flag*/, 1 /*studyflag*/, raw16, 0);
match = (rc == 0 && idx >= 0 && score > 0);
templateDelete(h);

preprocessor_exit();
```
identifyImage мутирует шаблон кандидата → для воспроизводимого замера брать свежую
копию (unpack) на каждую пробу. `InitIdentifyImage` не нужен.

## Грабли

- `gimg+0x08` = высота/rows → **ложь**: +0x08 = ширина 80, +0x0a = высота 64 (как у записей шаблона).
- Кормить сырой буфер как есть → транспонировать 80×64 → 64 строки × 80 до препроцессора.
- `cal+0x24` = col → ложь: +0x24 = ROW 64, +0x28 = COL 80.
- `preprocessor(&src, NULL, 10240, …)` → arg2 = `&purpose` (int), arg3 = указатель cbuf ≥ 0x4c98.
- `qcov = {quality, coverage}`, `dst+0x28 = coverage` → qcov = {coverage, quality}, dst+0x28 = quality.
- `src+0x14` = число пикселей → src: 10240 байт, dst: 5120 байт.
- Один статический cbuf на все кадры / нули в `param` → cbuf этого кадра передаётся в enrolAddImage и identifyImage.
- `templateUnPack(ctx, len, blob, &h)` или указатель на нули в arg3 → `templateUnPack(blob, len, NULL, &h)` (иначе пустой шаблон, maxTempNum 0).
- Звать `0x18000f810` как «study» → это деструктор; галерея уничтожается → SIGSEGV в матчере.
- `enrolFinish` до `templatePack` → шаблон уже уничтожен.
- `identifytemplate` как проверка → это CheckForDuplicate при регистрации; проверка — `identifyImage`.
- `InitIdentifyImage` перед identifyImage → в AlgoMilan заглушка 0x83; EA зовёт его только при sensorType 0xe.
- Профиль 3 (112×132) → 12 (AlgoChicago) или 10 (AlgoMilan), оба 64×80.
- `ppp_param_init(12)` в AlgoMilan → вернёт 0x81 (Milan принимает только 0..11).
- Кормить наш 8-битный `process()` в enroll/identify → только сырые u16 через вендорский `preprocessor`.
- `preprocess_set_mode(1)` как «калибровка» → это просто запись флага CALIBRATED, не калибровка.
- ratio AlgoMilan = непрерывный score → это код исхода (101/104/111, −100…); у AlgoChicago score > 0 — величина сходства.
- Сравнивать все пробы с одной живой галереей → identifyImage мутирует шаблон; копия на пробу.
