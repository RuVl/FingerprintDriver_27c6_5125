# Проверенный пайплайн AlgoMilan (сводка 3 агентов + моя верификация)

Все находки перекрёстно подтверждены тремя независимыми разборами (препроцессинг /
шаблоны+identify / EngineAdapter) и выборочно перепроверены мной по дизассемблеру.

## Что мы делали НЕВЕРНО (причины провала «только матчер»)
1. **Пропускали вендорский `preprocessor`** — кормили enroll наши 8-бит кадры.
   Штатно каждый сырой кадр обязан пройти `preprocessor` до enroll/identify
   (F1, подтв. всеми тремя: `preprocessor` зовётся только из EngineAdapter).
2. **Неверный профиль**: `ppp_param_init(3)` = 112×132. Нужен **`ppp_param_init(10)`**
   = 64×80 (проверено дампом таблицы 0x180094270 idx10 и таблицей EngineAdapter).
3. **Вызывали деструктор как «study»**: `0x18000f810` = `freeTemplate(void** holder)`
   (зовётся из `templateDelete`/`enrolFinish`) → разрушал галерею → SIGSEGV в
   `identifytemplate`. Никакого study для галереи не нужно.

## Эталонный конвейер (для воспроизведения на C)

### INIT (один раз)
```
ppp_param_init(10)                      // профиль 64x80, ставит PARAM_INIT=1
preprocess_init_calidata()              // дефолтная калибровка kr=8192,b=0
// CalInit cal (обнулить); cal[+0x18]=ptr на 16-бит кадр-фон; cal[+0x24]=col; cal[+0x28]=row
preprocessor_init(&cal)                 // ставит CALIBRATED=1, иначе preprocessor→0x80
```
Штатно EngineAdapter: get_calidata_len → load_calidata(если есть файл под sensorid)
ИЛИ init_calidata(по умолчанию) → preprocessor_init(&{calidata, col,row,baseframelen}).
Для offline берём фон-кадр из датасета (idx0) как калибровку.

### На каждый сырой кадр
```
// src gimg: data=u16[5120]; +0x14=5120(длина); dims +0x08/+0x0a
// dst gimg: data=u8[5120]; +0x08/+0x0a=64/80; +0x0e=8; +0x0f=1; +0x18=1
preprocessor(&src, NULL, 2*5120, &dst, &qcov, 0, 0)   // 0=ok; dst = 8-бит результат
```

### ENROLL / галерея
```
sess = enrolStartEx(&maxt=16)
for each pp-кадр:  enrolAddImage(sess, &dst_gimg, &o3,&o4, 0, &o6)   // 0=ok
enrolGetTemplate(sess, &tpl)            // живой holder, объект готов; НЕ звать 0x...f810
// хранение: templateGetPackedSize + templatePack ДО enrolFinish
```
`enrolAddImage` читает gimg: +0x0e==8, +0x0f==1, +0x18!=0, data!=0, dims +0x08*+0x0a.
Поле +0x14 enroll НЕ читает (оно только для preprocessor).

### СРАВНЕНИЕ
```
identifytemplate(&ref_holder, &probe_holder, NULL, &idx)  // 0=ok; idx>=0 совпадение
```
Оба holder'а — живые; matcher 0x18001d1f0 общий. НИЧЕГО не разрушать до/во время.

## Две неоднозначности — разрешить экспериментом offline
- **col/row в preprocessor_init**: агент A (из профиля AlgoMilan) → col=64,row=80;
  агент C (таблица EngineAdapter) → col=80,row=64 (транспонирование конвенций).
  Буфер 5120 одинаков. Проверить оба варианта: правильный даст 0 и осмысленный
  quality/coverage в dst[+0x28/+0x29].
- **identifyImage vs identifytemplate**: агент C видит в эталоне InitIdentifyImage→
  identifyImage, но `InitIdentifyImage` (0x18000ba40) в ЭТОЙ сборке — заглушка
  `mov eax,0x83; ret` (агент B, проверено). Поэтому основной путь для нас —
  `identifytemplate` (template↔template, полностью верифицирован). `identifyImage`
  (0x18000de30, image↔template) — запасной, попробовать после.

Детали и дизасм: [10-preprocess], [20-template-identify], [30-engine-flow], [00-overview].
