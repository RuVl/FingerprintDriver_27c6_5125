# AlgoMilan — подсистема ПРЕПРОЦЕССИНГА (10-preprocess)

DLL: `win-driver/AlgoMilan.dll` (Milan_v_3.00.20), imagebase `0x180000000`.
Все адреса — VA. Уровни уверенности: **ТОЧНО** (прочитано в дизасме),
*вероятно* (логичный вывод из кода), _гипотеза_ (требует проверки на железе).

Вспомогательные функции: `0x1800088e0` — лог/printf (PPLIB), `0x18002abc0/0x18002abe0`
— memcpy/memset, `0x18002dea0` — **ядро препроцессинга** (calib + apply, см. §2).

---

## 0. Ключевые глобалы (резолвнутые rip-адреса)

| VA | что | где выставляется |
|---|---|---|
| `0x18001CE4C` | **PARAM_INIT flag** (==1 после ppp_param_init) | ppp_param_init `0x18000e69e` |
| `0x18001D2DC` | **CALIBRATED/state gate** (первый dword ctx-структуры) | preprocess_set_mode; ядро в режиме калибровки |
| `0x18001CE50` | **CALIDATA base** (kr[]@+4, b[]@+0x9924) | init_calidata / load_calidata / preprocessor_init |
| `0x18001CE30` | ppp GOODIX_ISFLOATING (профиль f1) | ppp_param_init `0x18000e6ad` |
| `0x18001CE34` | ppp GOODIX_PIXEL_CANCEL (f2) | -"- |
| `0x18001CE38` | ppp GOODIX_IS_COATING (f3) | -"- |
| `0x18001CE3C` | ppp GOODIX_THRESHOLD_SELECT_BMP (f4) | -"- |
| `0x18001CE40` | ppp SENSOR_ROW (f5) | -"- |
| `0x18001CE44` | ppp SENSOR_COL (f6) | -"- |
| `0x18001CE48` | ppp f7 (вторичный индекс/порог) | -"- |

Резолв (пример, ТОЧНО): в `preprocessor` `0x18000f007 mov ecx,[rip+0xade37]` →
`0x18000f00d+0xade37 = 0x18001CE44` (=SENSOR_COL); `0x18000efe8 cmp [rip+0xade5d],1`
→ `0x18001CE4C` (=PARAM_INIT). В `preprocessor_init` `0x18000f249 cmp [rip+0xadbfc],1`
→ тот же `0x18001CE4C`. То есть init и preprocessor смотрят в один и тот же флаг ppp.

---

## 1. Сигнатуры (x64 ms_abi: rcx, rdx, r8, r9, далее стек)

### ppp_param_init — `0x18000e670`  (wrapper `0x18000b7b0`)
```c
int32_t ppp_param_init(int32_t sensor_type);   // 0 ok, 0x81 если >=12
```
**ТОЧНО.** `cmp rdx,0xc; jb` — валидный диапазон 0..11. Индексирует таблицу
профилей `0x180094270` (entry = 32 байта, `shl rdx,5`), копирует 7 dword'ов
(offsets +4..+0x1c) в глобалы `0x18001CE30..0x18001CE48`, ставит PARAM_INIT=1
(`0x18000e69e`). Лог "PPLIB: param index %d". Ничего не аллоцирует.

### preprocessor_init — `0x18000f200`  (wrapper `0x18000b7d0`)
```c
int32_t preprocessor_init(CalInit *cal);   // rcx
```
**ТОЧНО.** `CalInit` (читаются поля):
```c
struct CalInit { /*+0x18*/ uint16_t *buffer;   // сырой 16-бит калибровочный кадр
                 /*+0x24*/ int32_t  col;
                 /*+0x28*/ int32_t  row; };
```
Строка `'PPLIB : col %d row %d, buffer 0x%x'` читает `[rcx+0x24]`, `[rcx+0x28]`,
`[rcx+0x18]` (`0x18000f268-0x18000f281`). Возвраты: `0x81` если cal==NULL
(`0x18000f23f`); `0x80` если PARAM_INIT!=1 (`0x18000f25e`, лог "PPLIB param not
initialized"); иначе результат калибровки. Сбрасывает CALIBRATED=0 (`0x18000f30d`),
зовёт ядро `0x18002dea0` в режиме «calib B value», в конце `cmp CALIBRATED,1;
cmovne ebx,ebp` → **0 если калибровка успела выставить CALIBRATED=1**, иначе код
ошибки ядра. Лог результата: "cali B result %d, framenum=%d, kr[256]... b[256]...".

### preprocessor — `0x18000efa0`  (wrapper `0x18000b810`)
```c
int32_t preprocessor(GImg *src,        // rcx  — вход, 16-бит
                     void *aux,        // rdx  — доп. параметр (в init тут был 0) — *вероятно* NULL
                     uint32_t in_bytes,// r8   — размер входа в байтах = 2*ROW*COL
                     GImg *dst,        // r9   — выход, 8-бит (результат + quality/coverage)
                     QCov *qcov,       // [stack5] — out: {int32 quality; int32 coverage}
                     uint8_t mode_b1,  // [stack6]
                     uint8_t mode_b2); // [stack7]  (cmp [rsp+0xd0],1 → выбор ветки)
```
**ТОЧНО (порядок аргументов).** Разбор: внутри `mov rsi,r9`(dst), `mov rdx,rcx`(src),
`r10=r8`, `r14=[rsp+0xc0]`(qcov). Проверки: `test rcx` / `test r9` → `0x81`
"bad parameter, src=%p result=%p". `cmp PARAM_INIT,1` → `0x80` "param not
initialized". `cmp CALIBRATED,1` → `0x80` **"preprocessor is not calibrated"**
(`0x18000f043`). Порядок аргументов wrapper→internal подтверждён из
`0x18000b810-0x18000b85b`.

### preprocessor_exit — `0x18000f160`  (wrapper `0x18000b7f0`)
```c
int32_t preprocessor_exit(void);   // всегда 0
```
**ТОЧНО.** Лог "preprocessor_exit", CALIBRATED=0 (`0x18000f17f`), `memset(ctx@0x18001CE50-обл., 0, 0x3048c)` (`0x18000f189`), ret 0.

### preprocess_set_mode — `0x18000ef90`  (внутренняя, без wrapper)
```c
int32_t preprocess_set_mode(int32_t v);   // всегда 0
```
**ТОЧНО.** Тело целиком: `mov [rip+0xde346], ecx; xor eax,eax; ret`.
`0x18000ef96+0xde346 = 0x18001D2DC` = **тот самый CALIBRATED gate**.
Т.е. `preprocess_set_mode(1)` руками выставляет флаг «откалибровано»,
минуя реальную калибровку (см. §5, §2 — качество без реальной калибровки под вопросом).

### preprocess_get_calidata_len — `0x18000e800`  (wrapper `0x18000b8f0`)
```c
void preprocess_get_calidata_len(uint32_t *cali_len, uint32_t *extra_len);
```
**ТОЧНО.** `[rcx]=0x184ac (99500)`, `[rdx]=0xa004 (40964)`. Первый — размер
основного blob calidata, второй — «extra».

### preprocess_init_calidata — `0x18000e820`  (wrapper `0x18000b920`)
```c
int32_t preprocess_init_calidata(void);   // всегда 0
```
**ТОЧНО.** "load calidata with init value". Заполняет две u16-таблицы в blob
`0x18001CE50`: `kr[i]=0x2000 (8192)` (по смещению base+4), `b[i]=0` (по base+0x9924).
Кол-во = `[0x...]*[0x...]` (ROW*COL из ppp). **Это нейтральная калибровка**
(усиление 1.0, смещение 0). НЕ ставит CALIBRATED — только заполняет таблицы.

### preprocess_load_calidata — `0x18000e8b0`  (wrapper `0x18000b8b0`)
```c
int32_t preprocess_load_calidata(const uint8_t *blob, uint32_t blob_len,
                                 const uint8_t *extra, uint32_t extra_len);
```
**ТОЧНО.** Проверки: `blob!=0 && blob_len>=0x184ac && (extra!=0 ? extra_len>=4 : ok)`
→ иначе `0x81` "params error". Сверяет версию `strcmp(current, blob+0x1848c)`
(`0x1800441b0`) → `0x80` "preprocess version error". Считает CRC (`0x180030a40`)
над `blob+8` (kr) и `blob+0x9928` (b), сверяет с `[blob+0]`/`[blob+4]` → `0x80`
"cali data crc error". Затем копирует u16 kr← blob+8, b← blob+0x9928 в глобал-таблицы,
плюс блоки по `blob+0x13248` и `blob+0x13a48` и dword `blob+0x18488`. extra (если есть):
размер `[extra]` должен быть `extra_len-4` и `<0xa000`, копируется во внутренний буфер.
**Формат blob:** `[0]=crc_kr, [4]=crc_b, [8..]=kr u16[], [0x9928..]=b u16[],
[0x13248..], [0x13a48..], [0x18488]=dword, [0x1848c]=version C-строка`.

### preprocess_save_calidata — `0x18000ebd0`  (wrapper `0x18000b870`)
```c
int32_t preprocess_save_calidata(uint8_t *blob, uint32_t *blob_len /*>=0x184ac*/,
                                 uint8_t *extra, uint32_t *extra_len /*>=0xa004*/);
```
**ТОЧНО (зеркально load).** Требует `[r12]>=0x184ac`, `[r15]>=0xa004`. Сериализует
текущие kr/b + версию + CRC обратно в blob. Симметрична load_calidata.

### preprocessor_get_CalibParam — `0x18000f1a0`  (внутренняя)
```c
int32_t preprocessor_get_CalibParam(void **pptr, uint32_t *plen);
```
**ТОЧНО.** `[rcx] = &calib_blob (0x18001CE53-обл.)`, `[rdx] = 0x3048c (197260)` —
полный размер внутреннего калибровочного контекста. Возврат 0 (или 0x80 если NULL).

---

## 2. ЧТО делает `preprocessor` с изображением 64×80

**Вывод: это НЕ геометрический ремап, а per-pixel flat-field коррекция
(усиление kr + вычитание фона b) + реконструкция + расчёт quality/coverage,
из 16-бит сырого кадра в 8-бит нормализованный.** Уровень: *вероятно* (по устройству
ядра и таблицам), детали формулы — _гипотеза_.

Доказательства:
1. **Вход 16-бит, выход 8-бит.** Ядро `0x18002dea0` декодирует packed-config r13d:
   `eax=r13d>>0x17`=COL, `ecx=(r13d>>0xe)&0x1ff`=ROW, `r12d=ROW*COL`,
   `rbx=2*ROW*COL`, и `cmp rbx, r8d` (r8=in_bytes) — т.е. **вход = ROW*COL слов по 2 байта**
   (`0x18002df0b-0x18002df3e`, ТОЧНО). Копирование результата в `dst` идёт **байтами**:
   `movzx ecx,byte[rax+r8]; mov byte[rax+rdx],cl`, счётчик `[dst+0x14]=ROW*COL`
   (`0x18000f103-0x18000f117`, ТОЧНО) → **выход 8-бит**.
2. **Таблицы калибровки читаются, новый буфер пишется.** preprocessor передаёт в ядро
   `r9 = &calidata (0x18001CE50)` (`0x18000f08f lea r9,[rip+0xaddba]`), ядро аллоцирует
   temp-буфер (`0x18006c27c`), заполняет, а preprocessor копирует его в `dst->data`.
   kr[]/b[] по умолчанию — усиление 8192 (=1.0 при `>>13`) и смещение 0
   (см. init_calidata) → формула *вероятно* `out = clamp((in*kr[i]>>13) - b[i])`.
3. **quality/coverage.** После ядра: `quality=[r14]`, `coverage=[r14+4]`, лог
   "preprocessor: quality %d, coverage %d" (`0x18000f0c8`); байты пишутся в
   `dst[+0x29]=quality`, `dst[+0x28]=coverage` (ТОЧНО).
4. Флаги ISFLOATING/PIXEL_CANCEL/IS_COATING/THRESHOLD из ppp упакованы в config
   (`0x18000f007-0x18000f03a`) и управляют ветками ядра (реконструкция floating-режима,
   вычитание «coating» и т.п.) — *вероятно*.

**Калибровка обязательна для работы:** без CALIBRATED==1 preprocessor выходит с
`0x80` "preprocessor is not calibrated" ещё ДО обработки (`0x18000f03a`, ТОЧНО).
Если не инициализировать calidata, но выставить CALIBRATED (set_mode) — ядро отработает
на таблицах, лежащих в памяти (мусор/нули → искажённый выход). Поэтому нужно либо
`init_calidata` (нейтральные kr=8192,b=0), либо `load_calidata` (сохранённая),
либо реальный `preprocessor_init` по калибровочному кадру.

Ядро `0x18002dea0` — **общее** для init (режим «calib B», вычисляет b[]/kr[] из кадра
и ставит CALIBRATED через ctx-указатель) и для preprocessor (режим «apply»). Различие —
в аргументах-указателях на ctx/таблицы (init: ctx=`0x18001D2DC` через `lea[rip+0xddf74]`;
apply: тот же ctx через `lea[rip+0xde22c]`). Ядро НЕ трогает глобалы rip-относительно
(проверено: ни одной ссылки в диапазон `0x18001c000..0x18001e000`), всё через параметры.

---

## 3. Формат входной/выходной структуры (сравнение с нашей `struct gimg`)

`preprocessor` из структуры изображения читает/пишет ТОЛЬКО:
- `[+0x00]` = `void *data` (для src — 16-бит буфер, для dst — 8-бит буфер) — **ТОЧНО**
- `[+0x14]` = **длина в пикселях = ROW*COL** (условие копирования `cmp [dst+0x14],edi`) — **ТОЧНО**
- `[+0x28]` = coverage (пишется), `[+0x29]` = quality (пишется) в dst — **ТОЧНО**

Наша `struct gimg` (algo_eval.c): `data@0; f08@8; width@0xa; height@0xc; bits@0xe;
f0f@0xf; frames@0x14; chan@0x18`. Раскладка **совпадает по критичным полям**:
`data@0` ✓, поле `@0x14` ✓ (у нас названо `frames`, для preprocessor это **длина
пикселей ROW*COL**, а не «1»!). Поля width/height/bits/chan **preprocessor не читает** —
размеры он берёт из ppp-профиля (SENSOR_ROW/COL), НЕ из структуры (см. §4).

**Критично для dst:** перед вызовом задать `dst->data` = валидный буфер ≥ ROW*COL байт
и `dst->[0x14]` = ROW*COL (=5120 для 64×80). Для src: `src->data` = 16-бит буфер,
`in_bytes(r8)` = 2*ROW*COL (=10240). Поле `src->[0x14]` тоже участвует (packed-config
берёт `[src+0x14]`), *вероятно* должно быть = ROW*COL.

> ВНИМАНИЕ: в algo_eval.c `set_img` кладёт в `@0x14` значение `1` (frames=1). Для
> enroll это работало, но для **preprocessor это поле — длина ROW*COL**, и «1» приведёт
> к копированию 1 байта. Для препроцессинга структуру надо заполнять иначе.

---

## 4. ppp_param_init и профили 0..3 (таблица `0x180094270`, 12×32 байта)

Каждая запись: `[idx, ISFLOATING, PIXEL_CANCEL, IS_COATING, THRESHOLD_SELECT_BMP,
SENSOR_ROW, SENSOR_COL, f7]` (int32×8). **ТОЧНО** (прочитано из образа):

| idx | ISFLOAT | CANCEL | COATING | THRESH | ROW | COL | f7 |
|----:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| 0 | 1 | 0 | 4 | 800 | 88 | 108 | 0 |
| 1 | 1 | 0 | 4 | 800 | 64 | 176 | 6 |
| 2 | 1 | 0 | 4 | 800 | 54 | 176 | 7 |
| **3** | 1 | 0 | 4 | 400 | **112** | **132** | 2 |
| 4 | 1 | 0 | 0 | 600 | 60 | 128 | 1 |
| 5 | 1 | 0 | 0 | 600 | 88 | 108 | 8 |
| 6 | 1 | 0 | 0 | 600 | 64 | 176 | 4 |
| 7 | 1 | 0 | 0 | 600 | 68 | 118 | 63 |
| 8 | 1 | 0 | 0 | 300 | 96 | 96 | 62 |
| 9 | 1 | 0 | 0 | 800 | 88 | 108 | 0 |
| **10** | 1 | 0 | 4 | 800 | **64** | **80** | 10 |
| 11 | 1 | 0 | 4 | 800 | 88 | 108 | 0 |

Маппинг поле→имя восстановлен по строкам preprocessor_init
("GOODIX_ISFLOATING/PIXEL_CANCEL/IS_COATING", "THRESHOLD_SELECT_BMP",
"SENSOR_ROW/SENSOR_COL") и адресам глобалов (§0) — **ТОЧНО**.

`ppp_param_init(N)` = выбрать профиль сенсора: задаёт размеры кадра (ROW*COL),
режимы реконструкции и порог. **Влияет напрямую на preprocessor**: размеры ROW×COL
из профиля определяют ожидаемый размер входа (2*ROW*COL байт) и число пикселей выхода.

**Ключевое замечание по 5125 (64×80):** размерам нашего кадра соответствует
**профиль idx 10 (ROW=64, COL=80)**, а НЕ idx 3 (112×132). В algo_eval.c вызывался
`ppp_param_init(3)` — этого хватило, чтобы enroll принял кадры (enroll не гоняет
preprocessor, F1/F2), но для **preprocessor нужен профиль с 64×80** (idx 10) —
иначе `in_bytes` и число пикселей не сойдутся. Это _гипотеза_ (проверить на железе:
ориентацию row/col могли перепутать вендоры; кандидаты — 10, при неверной ориентации
профиль с 80×64 в таблице отсутствует, ближайший 64×80 = idx 10).

---

## 5. Минимальный корректный порядок вызовов

**ТОЧНО** по проверкам-гейтам (PARAM_INIT, CALIBRATED):

Вариант А (реальная калибровка по кадру-пустышке):
```
ppp_param_init(profile)         // PARAM_INIT=1, размеры ROW×COL
preprocess_init_calidata()      // kr=8192,b=0 (нейтраль) — задаёт базу таблиц
preprocessor_init(&cal)         // cal.buffer=16-бит кадр, cal.col=COL, cal.row=ROW
                                //  → вычисляет b[], ставит CALIBRATED=1, вернёт 0
// для каждого кадра:
preprocessor(src, aux, 2*ROW*COL, dst, &qcov, m1, m2)
...
preprocessor_exit()
```
Вариант Б (есть сохранённый blob):
```
ppp_param_init(profile)
preprocess_load_calidata(blob, blob_len, extra, extra_len)  // грузит kr/b + CRC/версия
preprocessor_init(&cal)   // или preprocess_set_mode(1), если init недоступен
preprocessor(...)
```
Вариант В (быстрый обход, для отладки): `ppp_param_init(N)` →
`preprocess_init_calidata()` → `preprocess_set_mode(1)` (руками ставит CALIBRATED) →
`preprocessor(...)`. Работает механически, но на нейтральных kr/b без реальной
b-калибровки качество/фон будут не как в Windows. — _гипотеза_, проверить.

Обязательные предусловия перед `preprocessor` (иначе ранний выход):
`PARAM_INIT==1` (ppp_param_init) **и** `CALIBRATED==1` (preprocessor_init ИЛИ set_mode(1)).

---

## 6. Проверка F1/F2 (косвенные вызовы preprocessor)

- **F1 ПОДТВЕРЖДЁН (ТОЧНО).** `callers 0x18000efa0` → единственный вызов из
  `preprocessor_wrapper @ 0x18000b85b`. `callers 0x18000b810` (wrapper) → **пусто**:
  wrapper — экспорт, зовётся только снаружи (EngineAdapter). Внутри DLL ни enrol*,
  ни identify* к preprocessor не ведут.
- **F2 ПОДТВЕРЖДЁН (ТОЧНО).** `calls 0x18000d590` (enrolAddImage) = только лог
  `0x1800088e0` и `0x18002aba0` (memcpy). Препроцессинга внутри нет — работает по
  уже препроцессированному кадру.
- `calls 0x18000e1b0` (identifytemplate) — прямых call нет (хвостовой переход),
  preprocessor среди целей отсутствует.

**Итог:** препроцессинг — обязательный ОТДЕЛЬНЫЙ шаг, выполняемый вызывающим кодом
между захватом кадра и enroll/identify. Наши прежние шаблоны строились из сырых
кадров → отсюда деградация сравнения.

---

## Итог для реализации на C

Размеры для 5125: `ROW=64, COL=80, N=ROW*COL=5120`. Вход 16-бит (2*N=10240 байт),
выход 8-бит (N=5120 байт).

```c
#define MS __attribute__((ms_abi))
// профиль сенсора; для 64x80 кандидат — idx 10 (в таблице ROW=64,COL=80)
typedef int32_t MS (*ppp_t)(int32_t);
typedef int32_t MS (*ppinit_t)(void *cal);          // CalInit*
typedef int32_t MS (*ppinitcali_t)(void);
typedef int32_t MS (*ppload_t)(const void*,uint32_t,const void*,uint32_t);
typedef int32_t MS (*ppsetmode_t)(int32_t);
typedef int32_t MS (*pp_t)(void *src, void *aux, uint32_t in_bytes,
                           void *dst, void *qcov, uint8_t m1, uint8_t m2);
typedef int32_t MS (*ppexit_t)(void);

// CalInit для preprocessor_init:
#pragma pack(push,1)
struct CalInit { uint8_t _0[0x18]; uint16_t *buffer; /*+0x18*/
                 uint8_t _20[4];  int32_t col; /*+0x24*/ int32_t row; /*+0x28*/ };
// GImg (совместима с нашей gimg); для preprocessor критичны поля:
struct GImg { void *data; /*+0*/ uint8_t _08[0x0c];
              uint32_t len;  /*+0x14 = ROW*COL пикселей*/
              uint8_t _18[0x10];
              uint8_t coverage; /*+0x28 (out)*/ uint8_t quality; /*+0x29 (out)*/
              uint8_t _2a[0x16]; };
struct QCov { int32_t quality; int32_t coverage; };
#pragma pack(pop)

// Последовательность (Вариант А):
ppp_param_init(10);                 // или подобрать профиль под 64x80
preprocess_init_calidata();         // kr=8192,b=0
struct CalInit ci={0}; ci.buffer=cal16; ci.col=80; ci.row=64;
preprocessor_init(&ci);             // → CALIBRATED=1, вернёт 0
// на каждый кадр:
uint16_t *raw16;                    // 5120 слов сырого кадра
uint8_t   out8[5120];
struct GImg src={0}, dst={0}; struct QCov qc={0};
src.data=raw16; src.len=5120;
dst.data=out8;  dst.len=5120;       // ВАЖНО: len=ROW*COL, не 1
preprocessor(&src, NULL/*aux*/, 5120*2, &dst, &qc, 0, 0);
// out8 → отдать в enrolStart/enrolAddImage / identify
...
preprocessor_exit();
```

Открытые вопросы (проверить на железе):
1. Точный профиль под 64×80 и ориентация ROW/COL (idx 10 vs подстройка).
2. Роль `aux`(rdx) и байтов `m1/m2` — в init aux был NULL; *вероятно* NULL/0 ок.
3. Достаточно ли `init_calidata`+`set_mode(1)` без реального калибровочного кадра
   (Вариант В) для приемлемого качества, или нужен темновой/пустой кадр в
   preprocessor_init.
4. Точная формула ядра `0x18002dea0` (kr>>13 - b + reconstruction) — при желании
   дореверсить отдельно (функция `0x18002dea0..0x18002e566`).
