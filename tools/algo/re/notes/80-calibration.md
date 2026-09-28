# 80 — Калибровка (calidata) препроцессора AlgoChicago

Метки: ТОЧНО (видно в дизасме) / вероятно / гипотеза. imagebase 0x180000000 у всех DLL.
Тип сенсора в EA/gfusb = 12 (таблица размеров EA 0x180113520 / gfusb 0x180540060, строка 12:
`80, 64, 25, 64, 10240, 3200, 19008` → COL 80, ROW 64, imagebase 10240 байт = u16[64×80],
navbase 3200 байт). Для типов 12/2/14 EA идёт по ветке «без Arr» (только calib-файл).

## 1. Источник calidata в Windows

### 1.1 Файлы
- EA: `C:\ProgramData\Goodix\goodix_calib.dat` (ТОЧНО имя, строка 0x1800aefb8; каталог —
  индекс 4 в path-builder 0x1800297c0 = `%c:\ProgramData\Goodix\%s`, вероятно).
  Чтение `read_calidata_Arr_from_file` 0x180045b10, запись `write_calidata_Arr_to_file`
  0x180045d80. Формат: `[16 байт sensorid][calidata 0x224b0 байт]` (malloc CaliLen+0x10,
  данные с +0x10). `goodix_Arr.dat` — для других типов сенсора, у нас не используется.
- gfusb: `goodix.dat` (строка 0x18024aed8) — **кэш фона**:
  `[OTP][fdt base][navbase 3200][imagebase 10240][crc32]`, загружается
  `device_check_imagebase_exist` 0x180069378 только при совпадении OTP
  («different otp, not to use base in file»), пишется `gf_savebaseTofile` 0x18006fc64.

### 1.2 sensorid (EngineContext+0x51, 16 байт) — ТОЧНО
`_GetSensorInfoAndLoadAlgo` 0x180031fc0: OTP приходит от драйвера (ctx+0x7d, «got OTP from
driver»); если OTP ≠ 32 нуля → `memcpy(ctx+0x51, ctx+0x7d, 0x10)` (0x1800322aa).
sensorid = **первые 16 байт OTP сенсора** (тот же OTP, что мы читаем для конфига).

### 1.3 Кто и когда вызывает калибровку (ТОЧНО)
`EngineAdapterAcceptSampleData` 0x180033c20 → `_AdapterInitPreprocessor(ctx, base, len)`
0x180031160 в двух местах:
- 0x180034b9a: если `ctx+0x2a8 (basevalid)==0` → `_GetImageBase` 0x18003fd00
  (`DeviceIoControl(0x442120)`, выход ROW*COL*2 = 10240 байт) → init с этим кадром.
- 0x18003560e: при каждом сэмпле, если `basevalid==0 || NeedUpdateImageBase || !same_sensor`.
  Буфер сэмпла от драйвера: кадр по +0, **фон по +0x7468**, флаги в байте +0xeb6c
  (bit1 = NeedUpdateImageBase, bit4 = data valid) — лог 0x1800b3300
  «sensortype, len, row, col, same_sensor, basevalid, NeedUpdateImageBase, data valid…».

`_AdapterInitPreprocessor` (тип 12):
```
len = preprocess_get_calidata_len(0,0)            ; = 0x224b0 (140464)
read goodix_calib.dat → buf
if memcmp(ctx+0x51, buf, 16)==0 : preprocess_load_calidata(buf+0x10, len, NULL, 0)
                                   (при ошибке → preprocess_init_calidata())
else                             : preprocess_init_calidata()   ; kr=8192, b=0
params(+0x18=base u16*, +0x24=ROW(64), +0x28=COL(80)); preprocessor_init(&params)
ctx+0x2a8 = 1 ; "Save Kr to file" → SaveKrbArrToFile(ctx,0) 0x18002ec30
```
`SaveKrbArrToFile` = `preprocess_save_calidata` + запись `[sensorid][calidata]`; также
вызывается из 0x180033130 и 0x18003b7c0 (прочие сохранения). 0x1800518d0 —
`RegroupConfigForPBA` (упаковка calidata+imagebase для BIOS/PBA, AES) — к калибровке не
относится.

### 1.4 Откуда фон (gfusb.dll) — ТОЧНО
`gf_update_all_base` 0x180070810 (при старте драйвера: «Init: update all base…»,
0x180016ec0 → 0x180069378 → 0x180070810):
1. `gf_get_fdtbase` ×1 (FDT-база 0), `gf_get_navbase`, `gf_get_fdtbase` 1;
   если |fdt0−fdt1| > fdt_delta (регистр 0x82) — «первое детектирование неверно», повтор;
2. `gf_get_oneframe` 0x18006f85c (`SetMode(2=image, 2000 мс)`) — **один обычный кадр**
   10240 байт u16;
3. `gf_get_fdtbase` 2, снова сравнение с fdt1 (палец не появился);
4. если база уже была валидна — кадр проверяется классификатором 0x180027f8c
   (temperature/finger down/void/bad), при «finger down» не обновляется;
5. memcpy в глобальный imagebase, `gf_savebaseTofile`.
Повтор до N раз (ctx+0x45a). Также `gf_check_baseisvalid` 0x18006e80c (из IRQ-обработчика
0x18006b390 при «fdt up / reset / temperature», если base_is_valid==0) снимает новый
одиночный кадр тем же способом.
**Итого: фон = один сырой кадр без пальца, снятый между двумя стабильными FDT-замерами.**
Без усреднения, без разных экспозиций.

## 2. Раскладка и вычисление kr/b (AlgoChicago)

### 2.1 Параметры `preprocessor_init(params)` 0x18000e4a0 — ТОЧНО
Читаются **только** три поля: `+0x18` u16* фон-кадр (может быть NULL), `+0x24` ROW, `+0x28` COL
(лог «PPLIB : col %d row %d, buffer» 0x18000e50c). Никакого framenum/числа кадров/флагов в
params нет; режимные флаги берутся из глобалов `ppp_param_init`. Длина буфера = ROW*COL*2.

### 2.2 Профиль (ppp_param_init, таблица 0x180079180 (8×i32 на тип), строка 12) — ТОЧНО
`ISFLOATING=1, PIXEL_CANCEL=0, IS_COATING=4, THRESHOLD_SELECT_BMP=600, ROW=64, COL=80,
mode=24 (0x18)`, C=1 (для типа 12). Слово флагов (0x18000e522):
`bit0=ISFLOATING, bit1=PIXEL_CANCEL, bit2=IS_COATING(4), bits3..8=mode, bits11..13=C,
bits14..22=ROW, bits23..=COL`.

### 2.3 Глобальная calidata G = 0x180099d00 (0x3048c байт, preprocessor_exit обнуляет)
| G+ | что | кто пишет |
|---|---|---|
| +0 | **framenum** (u32, счётчик накопления kr, ≤400) | init=0, load, `kr_update` 0x180048fe0 (`inc [G]`) |
| +4 | **kr[ROW*COL] u16** (макс. 0x9920 байт) | init=8192, load, `ce60` (=0 при framenum==0), `kr_update` |
| +0x9924 | **b[ROW*COL] u16** | init=0, load, калибровка 0x18004a5d0 |
| +0x13244, +0x1cb64, +0x217f4 | рабочие буферы кадра | core, каждый кадр |
| +0x26484 | флаг кадра (7-й арг `preprocessor`: 1→2, иначе 0; далее 0/1) | preprocessor, 0x180046aa0 |
| +0x26488 (0xa000), +0x30488 | сохраняются в файл; история контекста 0x180043c70: пишет 0x180036840 в конце 0x180038380 (вызывается хвостовым переходом из 0x180038e20, не мёртвый путь), читает 0x180036e50 на первом кадре |
Флаг «calibrated» = 0x180099cf4 (`preprocessor` без него → «not calibrated», 0x80).

### 2.4 Файл calidata (preprocess_save/load 0x18000df30/0x18000dc00) — ТОЧНО
Длина 0x224b0: `+0 crc32(kr) +4 crc32(b) +8 kr[] +0x9928 b[] +0x13248 0x800 байт (0x180094a90)
+0x13a48 0x4a40 байт (0x180095290) +0x18488 framenum +0x1848c 0xa000 (G+0x26488)
+0x2248c u32 (G+0x30488) +0x22490 строка версии (32)`. Load: версия должна совпасть
(иначе 0x80), CRC kr и b (иначе «cali data crc error», 0x80).

### 2.5 b — калибровка при preprocessor_init — ТОЧНО
`preprocessor_init`: calibrated=0 → core 0x180047f90 (buf=фон, G, флаги) → т.к. calibrated==0,
вызывается 0x18004a5d0(img, G, bit0, bit1, &mean@0x1800eb9c8, ROW, COL, mode):
```
mean = round(Σimg/N)                          -> 0x1800eb9c8 (используется при нормировке)
if PIXEL_CANCEL(bit1): b = img(+0xfff) или img-mean(+0xfff) ...
elif mode∈{1,8}: b = img+0xfff
elif mode∈{12,13,16,17,18,22,25}: b = img+0x1bb7
else: b = img                                 <-- наш mode=24: b[i] = фон[i] ровно
calibrated = 1
```
kr и framenum при этом **не трогаются** (лог «cali B … framenum=0, kr=8192» — просто
состояние после init_calidata). Т.е. `b` = один кадр без пальца, ничего больше.

### 2.6 kr — **обучается онлайн по кадрам с пальцем** внутри `preprocessor()` — ТОЧНО
Цепочка per-frame: core → 0x18004a950 (нормировка) → 0x18004ce60 (выбор усиления) и
0x180049650 → `kr_update` 0x180048fe0.
- 0x18004ce60 (при первом кадре процесса; params+0x14=coating=1 → ref=2000, thr=1600,
  limit=3 т.к. mode==0x18 && C==1; без coating: ref 8000/thr 6400):
  - framenum==0 → `kr[i]=0`, gain[i]=8192;
  - framenum≤limit → gain[i]=8192;
  - framenum>limit → gain[i] = kr[i]*8192/mean(kr).
- 0x180049650: для ISFLOATING сигнал `s = max(b − img, 0)` (0x180049a90); если кадр прошёл
  проверки (покрытие/качество 0x18004ae40, однородность 0x180049270, framenum-гейт,
  **params+0x20 ≠ 2**, т.е. 7-й аргумент `preprocessor` = 0) — строится per-pixel int32 карта (смысл «локальная амплитуда гребней» — вероятно; сам расчёт не разобран)
  амплитуды `v` (0x18004d1a0/0x18004af10) и вызывается `kr_update`:
```
for i: if |v[i]-ref| < thr: kr[i] = (kr[i]*n + v[i] + (n+1)/2)/(n+1)   ; n=framenum
framenum = min(framenum+1, 400)
if framenum > limit && mean(kr)!=0: gain[i] = kr[i]*8192/mean(kr)
if framenum <= 200: gain сглаживается 5×5 (0x18004dbc0)
```
- Применение: в конце 0x180049650 `out = ((x*gain+4096)>>13)…` (0x18004a510).
**Итог:** kr = бегущее среднее локальной амплитуды гребней по пикселям за последние ≤400
«хороших» кадров с пальцем; нормированное kr/mean — это flat-field коэффициент. Чтобы kr
стал ≠8192, нужно ≥4 прошедших проверку кадров с пальцем через `preprocessor()` в одном
состоянии G (или загруженный calidata с framenum>3).

## 3. Данные сенсора и обновление фона во время работы

- **OTP в препроцессор не идёт** (ТОЧНО): вход только фон-кадр + профиль. OTP используется
  лишь как sensorid (1.2) и в gfusb для проверки кэша фона и конфига.
- «baseframelen» = ROW*COL*2 = 10240 (ТОЧНО, 0x180034b84), сохраняется в ctx+0x2ac.
- **Фон обновляется во время работы** (ТОЧНО): gfusb снимает новый imagebase при старте и
  по событиям FDT-up/reset/temperature (1.4), передаёт его в каждом сэмпле (+0x7468) с флагом
  NeedUpdateImageBase → EA повторяет `_AdapterInitPreprocessor` → новое `b`. kr/framenum при
  этом сохраняются (загружаются из файла того же sensorid и не трогаются калибровкой).
- **kr обновляется каждым кадром с пальцем** (2.6) и сохраняется в goodix_calib.dat при
  init, `EngineAdapterDetach` 0x180033130 и `PBT_APMSUSPEND` 0x18003b7c0 (data_type 2,
  если ctx+0x344). Т.е. на рабочей Windows-машине kr накоплен за сотни касаний.
- Накопления фоновых кадров (усреднения) нет нигде: один кадр (ТОЧНО по gf_update_all_base).
- EA не вызывает `getCalibParamWrapper`, `gx_sensorCheckWrapper`, `preprocess_set_mode`
  (ТОЧНО, только резолвит). `preprocess_set_mode` пишет флаг calibrated (0x180099cf4).

## 4. Рецепт офлайн-калибровки

```c
/* один раз на «сессию» (новый фон) */
static uint8_t cal[0x224b0]; uint32_t len = sizeof cal;
if (have_saved && preprocess_load_calidata(cal, len, NULL, 0) == 0) ; /* kr, framenum из прошлого */
else preprocess_init_calidata();                   /* kr=8192, b=0, framenum=0 */
struct { uint8_t pad[0x18]; uint16_t *base; uint32_t pad2; int row; int col; } p = {0};
p.base = bg;      /* u16[64*80] — ОДИН кадр без пальца, декодирован как обычный кадр,
                     снят в тех же условиях (температура/конфиг), что и кадры с пальцем */
p.row = 64; p.col = 80;
preprocessor_init(&p);                             /* b = bg, calibrated = 1 */

/* каждый кадр — тот же процесс, тот же G */
preprocessor(src, &tcode, ..., /*6-й*/cfg431, /*7-й*/0);   /* 0 → kr учится */

/* в конце сессии */
preprocess_save_calidata(cal, &len);  /* сохранить blob (+ свой sensorid = OTP[0..16]) */
```
Эквивалент Windows-состояния: прогнать через `preprocessor()` серию кадров с пальцем
(≥4 для включения gain, ~200–400 для сходимости; порядок — хронологический), сохранить blob,
дальше грузить его перед каждым `preprocessor_init` со свежим фоном. Для офлайн-оценки на
датасете: «прогрев» kr на кадрах регистрации (не на тестовых), затем тестовые кадры
(kr продолжит медленно дообучаться — как в Windows).

Что снять с железа (если в `dumps/dataset` нет фона на каждую сессию): в начале каждой
сессии 1 кадр без пальца (лучше 5–10 подряд: Windows берёт 1-й, остальные — для проверки
стабильности/медианы, это уже гипотеза-улучшение), тем же `capture.py`-путём, что и кадры с
пальцем, сразу перед ними (без смены конфига/температуры); плюс сами кадры с пальцем.
Разных экспозиций/спец-процедур не требуется.
