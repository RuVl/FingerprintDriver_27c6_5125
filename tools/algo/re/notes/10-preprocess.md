# Препроцессор (AlgoMilan; AlgoChicago — те же экспорты)

Адреса — AlgoMilan (`Milan_v_3.00.20`), если не указано иное. x64 ms_abi: rcx, rdx, r8, r9, стек.
Вызывающий код EngineAdapter — в 30.

**Препроцессинг — обязательная отдельная стадия (ТОЧНО).** `preprocessor` (0x18000efa0)
вызывается только из своей обёртки (`callers` → 0x18000b85b), обёртку внутри DLL никто не
зовёт. `enrolAddImage` (0x18000d590) зовёт только логгер и memcpy; identify-пути препроцессор
не зовут. Кадр прогоняет через `preprocessor` вызывающий (EngineAdapter).

## Глобалы (ТОЧНО, резолв rip: напр. `0x18000f281: mov esi,[rip+0xadbc3]` → 0x1800BCE44)

| VA | что | кто пишет |
|---|---|---|
| 0x1800BCE30 | GOODIX_ISFLOATING (поле 1 профиля) | ppp_param_init |
| 0x1800BCE34 | GOODIX_PIXEL_CANCEL (2) | -"- |
| 0x1800BCE38 | GOODIX_IS_COATING (3) | -"- |
| 0x1800BCE3C | THRESHOLD_SELECT_BMP (4) | -"- |
| 0x1800BCE40 | SENSOR_ROW (5) | -"- |
| 0x1800BCE44 | SENSOR_COL (6) | -"- |
| 0x1800BCE48 | поле 7 = тип шаблона | -"- |
| 0x1800BCE4C | PARAM_INIT (=1) | ppp_param_init |
| 0x1800BCE50 | блок калибровки: kr u16[] @+4, b u16[] @+0x9924, ctx до +0x3048c | init/load_calidata, preprocessor_init |
| 0x1800ED2DC | CALIBRATED | preprocessor_init (через ядро), preprocess_set_mode, exit |

## ppp_param_init(int type) — 0x18000e670

**ТОЧНО.** Milan: `cmp rdx,0xc; jb` — 0..11, иначе 0x81 (лог "unsuported sensor type").
Копирует поля 1..7 строки таблицы `0x180094270` (32 байта: `shl rdx,5`) в глобалы
0x1800BCE30..48, ставит PARAM_INIT=1. Ничего не аллоцирует.
Chicago (0x18000d9d0): `cmp edx,0xd; jb` — 0..12, отдельная ветка `cmp edx,0xc; cmove` для типа 12;
таблица `0x180079180`.

Строка профиля: `[idx, ISFLOAT, CANCEL, COATING, THRESH, ROW, COL, тип]`. AlgoMilan (ТОЧНО):

| idx | ISFLOAT | CANCEL | COATING | THRESH | ROW | COL | тип |
|---:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| 0 | 1 | 0 | 4 | 800 | 88 | 108 | 0 |
| 1 | 1 | 0 | 4 | 800 | 64 | 176 | 6 |
| 2 | 1 | 0 | 4 | 800 | 54 | 176 | 7 |
| 3 | 1 | 0 | 4 | 400 | 112 | 132 | 2 |
| 4 | 1 | 0 | 0 | 600 | 60 | 128 | 1 |
| 5 | 1 | 0 | 0 | 600 | 88 | 108 | 8 |
| 6 | 1 | 0 | 0 | 600 | 64 | 176 | 4 |
| 7 | 1 | 0 | 0 | 600 | 68 | 118 | 63 |
| 8 | 1 | 0 | 0 | 300 | 96 | 96 | 62 |
| 9 | 1 | 0 | 0 | 800 | 88 | 108 | 0 |
| **10** | 1 | 0 | 4 | 800 | **64** | **80** | **10** |
| 11 | 1 | 0 | 4 | 800 | 88 | 108 | 0 |

AlgoChicago: **idx 12 = (12, 1, 0, 4, 600, 64, 80, 24)** — штатный для 5125; idx 10 = (10,1,0,4,800,64,80,10).
Имена полей — по строкам preprocessor_init (`GOODIX_ISFLOATING…`, `SENSOR_ROW %d, SENSOR_COL %d`).
Вендор видит кадр как 64 строки × 80 столбцов.

## preprocessor_init(CalInit *cal) — 0x18000f200

**ТОЧНО.**
```c
struct CalInit { uint8_t _0[0x18];
                 uint16_t *bg;   /* +0x18: u16 фон-кадр ROW×COL (транспонированный) */
                 uint8_t _20[4];
                 int32_t row;    /* +0x24 = 64 */
                 int32_t col; }; /* +0x28 = 80 */
```
Лог `'PPLIB : col %d row %d, buffer 0x%x'`: `edx=[cal+0x28]` (col), `r8d=[cal+0x24]` (row),
`r9=[cal+0x18]` (0x18000f268..f281). Возвраты: 0x81 если cal==NULL; 0x80 если PARAM_INIT≠1
("PPLIB param not initialized"). Сбрасывает CALIBRATED=0 (0x18000f30d), зовёт ядро в режиме
калибровки (вычисляет b[] по фон-кадру), в конце `cmp CALIBRATED,1; cmovne` → 0 при успехе.
Лог `"cali B result %d, framenum=%d, kr[256]... b[256]..."`; у нас `framenum=0`, kr остаётся 8192.

## preprocessor — 0x18000efa0

```c
int preprocessor(GImg *src,          /* rcx: вход u16 */
                 const int *purpose, /* rdx: 1 enroll, 0 verify; читается [purpose+0] */
                 uint8_t *cbuf,      /* r8 : буфер ≥ 0x4c98 байт, свой на кадр */
                 GImg *dst,          /* r9 : выход u8 */
                 int qcov[2],        /* stk: out {coverage, quality} */
                 uint8_t m1,         /* stk: EA — liveness_switch (0); не читается */
                 uint8_t m2);        /* stk: 0; ==1 → глобальный режим 2 */
```
**ТОЧНО** (порядок — вызов EA 0x1800358ea и перетасовка регистров 0x18000efbb..efd3; смысл
qcov — лог и эксперимент).

Гейты: src/dst NULL → 0x81; PARAM_INIT≠1 → 0x80; CALIBRATED≠1 → 0x80 "preprocessor is not
calibrated" (до какой-либо обработки).

Из gimg читает только `src+0x00` (data), `src+0x14` (длина в байтах), `dst+0x00`, `dst+0x14`;
размеры берёт из профиля. Пишет (0x18000f0c8..f0ef):
```
0x18000f0c8: r8d=[qcov+0]; edx=[qcov+4]  ; лог 'preprocessor: quality %d, coverage %d' (edx, r8d)
0x18000f0e8: mov [dst+0x28], (u8)qcov[1] ; quality
0x18000f0ef: mov [dst+0x29], (u8)qcov[0] ; coverage
```
затем побайтно копирует результат в `dst->data`, счётчик — `dst+0x14`.

Длины: `src+0x14 = 2·ROW·COL = 10240`, `dst+0x14 = ROW·COL = 5120`. Ядро сравнивает
`2·ROW·COL == src+0x14` (`0x18002df3e sete`) — не жёсткий гейт, влияет на режим ядра (*вероятно*).

### Путь к ядру 0x18002dea0 (ТОЧНО)

config упаковывается из глобалов профиля и `src+0x14`; декод в ядре: `COL = cfg>>23`,
`ROW = (cfg>>14)&0x1ff`, `тип = (cfg>>3)&0x3f`, флаги в битах 0..2. Для idx 10 cfg = 0x28100055.
Аргументы ядра: `rdx = src->data`, `r9 = &блок калибровки 0x1800BCE50`, стек (arg5..12): cfg,
ctx 0x1800ED2DC, глобал, `&qcov[1]`, qcov, **purpose (arg10)**, **cbuf (arg11)**, 0.

- **purpose**: при типе ≠ 20 ядро всегда зовёт скоринг `0x18002c060` (@0x18002e2c8) с
  `rcx = purpose`; там `0x18002c0de mov eax,[rcx]` (NULL → SIGSEGV), `0x18002c195 cmp …,1` —
  при 1 (enroll) пороги качества понижаются на 10/15 (`sub ebx,0xa/0xf`). Больше purpose нигде не читается.
- **cbuf**: при бите IS_COATING (для idx 10/12 выставлен) ядро делает
  `0x18002e347: memset(cbuf, 0, 0x4c98)`; при `cbuf == NULL` вся coating-ветка пропускается
  (`0x18002e2fb test r13,r13; je`). В ветке: `tmp[i] = clamp(b[i] − src16[i])` (вычитание фона,
  0x18002e3a0..e3c4), `0x180029e80(…, cbuf, …)`, `cbuf[0] = результат 0x180029610`, `cbuf[1] = 1`
  при условии. Далее этот cbuf потребляет getFeature через `param` enrolAddImage/identifyImage (20).
- Выход — 8-бит нормализованный кадр + quality/coverage. Общая формула (усиление kr>>13, фон b,
  реконструкция floating) — _гипотеза_, ядро 0x18002dea0..0x18002e566 целиком не разобрано.

Ядро общее для калибровки (preprocessor_init) и применения (preprocessor); различаются ctx/режимом.

## Калибровочные данные (ТОЧНО)

| функция | сигнатура / поведение |
|---|---|
| preprocess_get_calidata_len 0x18000e800 | `void (uint32_t *cali_len, uint32_t *extra_len)` → 0x184ac, 0xa004 |
| preprocess_init_calidata 0x18000e820 | `int (void)`: kr[i]=0x2000, b[i]=0 для ROW·COL; CALIBRATED не ставит; всегда 0 |
| preprocess_load_calidata 0x18000e8b0 | `int (blob, blob_len ≥0x184ac, extra, extra_len)`: 0x81 params; версия `strcmp(…, blob+0x1848c)` → 0x80; CRC (0x180030a40) над blob+8 и blob+0x9928 против [blob+0]/[blob+4] → 0x80; копирует kr, b, блоки +0x13248, +0x13a48, dword +0x18488; extra: `[extra] == extra_len−4`, < 0xa000 |
| preprocess_save_calidata 0x18000ebd0 | `int (blob, *blob_len ≥0x184ac, extra, *extra_len ≥0xa004)` — зеркально load |
| preprocessor_get_CalibParam 0x18000f1a0 | не экспорт; `*pp = 0x1800BCE50`, `*plen = 0x3048c` |
| preprocess_set_mode 0x18000ef90 | не экспорт; `mov [0x1800ED2DC], ecx; xor eax,eax; ret` — только флаг CALIBRATED, **не калибровка** |
| preprocessor_exit 0x18000f160 | CALIBRATED=0, `memset(0x1800BCE50, 0, 0x3048c)`, ret 0 |

Формат blob: `[0]=crc_kr, [4]=crc_b, [8..]=kr u16[], [0x9928..]=b u16[], [0x13248..], [0x13a48..],
[0x18488]=dword, [0x1848c]=версия (C-строка)`.

Порядок: `ppp_param_init` → `preprocess_init_calidata` (или `load_calidata`) → `preprocessor_init(&cal)`
→ `preprocessor` на каждый кадр → `preprocessor_exit`. Как это делает EA — 30 (§ init).
