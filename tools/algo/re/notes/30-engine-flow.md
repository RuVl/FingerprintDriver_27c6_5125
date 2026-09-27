# EngineAdapter.dll — эталонный порядок вызовов AlgoMilan (enroll / verify / identify)

Цель: восстановить, как штатный движок Goodix (`EngineAdapter.dll`, сборка
`milan_watt\milanspi\adapters\eng...`) использует `AlgoMilan.dll` при регистрации и
проверке. Imagebase обоих DLL = `0x180000000`. Инструмент — `fwre.dump`.

Главный вывод (короткий): **препроцессинг обязателен и всегда идёт первым**. Каждый
кадр проходит через `preprocessor` ДО подачи в `enrolAddImage`/`identify`.
`EngineAdapter` реализует стандартный Windows WBDI-интерфейс движка, где препроцессинг —
это отдельная стадия конвейера `EngineAdapterAcceptSampleData`, а enroll/identify —
последующие стадии, потребляющие её результат.

`ppp_param_init` вызывается с аргументом **10** (профиль сенсора Milan 64×80) — доказано ниже.

---

## 1. Таблица имён AlgoMilan → указатель → место вызова

EngineAdapter грузит `AlgoMilan.dll` через `LoadLibraryW` и резолвит **29 обёрток** через
`GetProcAddress` по именам.

- **Загрузка DLL:** `0x180052f00` (`LoadAlgorithm`). Выбор алгоритма по типу сенсора:
  `case 2 → milanG`, `case 3 → milanL` (грузит `AlgoMilan.dll`, стр. `0x1800fa...`),
  `case 0xc → chicagoHS`, `case 0xe → chicagoT`.
  Хендл модуля сохраняется в глоб. `g_hAlgoModule = 0x18013bf38`.
- **Резолвер:** `0x1800533a0` (лог-тег `ExportFunctions`). Единый цикл
  `GetProcAddress(g_hAlgoModule, "<имя>")` → сохранение в глоб. таблицу указателей,
  при NULL — лог `L'<имя> is NULL.'` и выход с ошибкой.
- **Таблица указателей:** непрерывный массив 8-байтовых слотов
  `0x18013be50 … 0x18013bf30` (29 записей).

| имя обёртки (GetProcAddress)     | глоб. указатель | где ВЫЗЫВАЕТСЯ (функция EngineAdapter) |
|----------------------------------|-----------------|----------------------------------------|
| `ppp_param_init_wrapper`         | `0x18013be50`   | `_InitAlgorithmEngine` `0x18002e8d0` @`0x18002ebb7` |
| `preprocessor_init_wrapper`      | `0x18013be58`   | `_AdapterInitPreprocessor` `0x180031160` @`0x18003184b`,`0x1800318a8` |
| `preprocessor_exit_wrapper`      | `0x18013be60`   | `EngineAdapterDetach` `0x180033130` @`0x1800334de` |
| `preprocessor_wrapper`           | `0x18013be68`   | **AcceptSampleData** `0x180033c20` @`0x1800358ea`; **IdentifyFeatureSet** `0x180037850` @`0x180038456`; choose-enroll `0x180041700`/`0x180041d40`; self-test `0x18004aec0` |
| `preprocess_save_calidata_wrapper`| `0x18013be70`  | `0x18002ec30`, `0x1800518d0` |
| `preprocess_load_calidata_wrapper`| `0x18013be78`  | `_AdapterInitPreprocessor` `0x180031160` @`0x1800316b9`,`0x1800316db` |
| `preprocess_get_calidata_len_wrapper`| `0x18013be80`| `0x18002ec30`, `_AdapterInitPreprocessor` @`0x1800312f7`,`0x180031401`, `0x1800518d0` |
| `preprocess_init_calidata_wrapper`| `0x18013be88`  | `_AdapterInitPreprocessor` @`0x180031734`,`0x1800317dc` |
| `enrolStartWrapper`              | `0x18013be90`   | `0x18003b5c0` (из CreateEnrollment); self-test `0x1800483e0` |
| `enrolStartExWrapper`            | `0x18013be98`   | (не вызывается) |
| `enrolAddImageWrapper`           | `0x18013bea0`   | `0x1800441b0`, `0x180044880` (из enroll-add `0x180044b00`); self-test |
| `enrolDeleteImageWrapper`        | `0x18013bea8`   | `0x1800441b0`, `0x180044b00` |
| `enrolGetTemplateWrapper`        | `0x18013beb0`   | `0x180045690` (из GetEnrollmentStatus); self-test `0x180048610` |
| `enrolFinishWrapper`             | `0x18013beb8`   | `0x180032a70` (finish/cleanup), Detach, ClearContext, self-test |
| `InitIdentifyImageWrapper`       | `0x18013bec0`   | **core_identify** `0x180042d50` @`0x1800431e7`; self-test |
| `identifyImageWrapper`           | `0x18013bec8`   | **core_identify** `0x180042d50` @`0x180043300`; self-test |
| `FreeIdentifyImageWhenFailWrapper`| `0x18013bed0`  | core_identify `0x180043c38`; self-test |
| `PostIdentifyImageWhenFailWrapper`| `0x18013bed8`  | IdentifyFeatureSet `0x180037850`; self-test |
| `templateStudyWrapper`           | `0x18013bee0`   | core_identify `0x180042d50` @`0x180043643`; self-test |
| `templateGetPackedSizeWrapper`   | `0x18013bee8`   | pack-хелпер `0x18002f6d0` @`0x18002f952` |
| `templatePackWrapper`            | `0x18013bef0`   | pack-хелпер `0x18002f6d0` @`0x18002f9e6` |
| `templateUnPackWrapper`          | `0x18013bef8`   | unpack-хелпер `0x180030750` |
| `templateDeleteWrapper`          | `0x18013bf00`   | core_identify; CheckForDuplicate-inner `0x180045fd0`; self-test |
| `getAlgorithmVersionWrapper`     | `0x18013bf08`   | `_InitAlgorithmEngine` @`0x18002eae2` |
| `identifyUpdateWrapper`          | `0x18013bf10`   | core_identify `0x180042d50` @`0x180043750`; self-test |
| `gx_sensorCheckWrapper`          | `0x18013bf18`   | (не вызывается) |
| `identifytemplateWrapper`        | `0x18013bf20`   | CheckForDuplicate-inner `0x180045fd0` @`0x180046349` |
| `getTemplateInfoWrapper`         | `0x18013bf28`   | core_identify `0x180042d50` @`0x1800438f7`; enrolGetTemplate-wrap `0x180045690` |
| `getCalibParamWrapper`           | `0x18013bf30`   | (не вызывается) |

Уверенность: **высокая** (таблица извлечена скриптом по паттерну
`lea rdx,<name>; call GetProcAddress; mov [rip+glob],rax` в `0x1800533a0`; места вызова —
сканом всех `call qword ptr [rip+glob]` по всей .text).

---

## 2. Интерфейс движка (WBDI) — конвейер

`WbioQueryEngineInterface` (`0x18002e420`, единственный экспорт) возвращает указатель на
структуру `WINBIO_ENGINE_INTERFACE` по адресу `0x180120530`
(`Version=3.2`, далее GUID адаптера, далее указатели на методы). Слоты (имена — из
собственных лог-тегов функций):

| слот | адрес | метод WBDI | что делает / что зовёт из AlgoMilan |
|------|-------|-----------|--------------------------------------|
| 12 | `0x180033c20` | **AcceptSampleData** | `preprocessor` (пер-кадровый препроцессинг!) |
| 13 | `0x180036a90` | ExportEngineData | — |
| 14 | `0x180036e60` | **VerifyFeatureSet** | → core_identify `0x180042d50` |
| 15 | `0x180037850` | **IdentifyFeatureSet** | `preprocessor` (повторно) → core_identify `0x180042d50`; `PostIdentifyImageWhenFail` |
| 16 | `0x1800398f0` | **CreateEnrollment** | → `enrolStart` (`0x18003b5c0`) |
| 17 | `0x180039e60` | **UpdateEnrollment** | → enroll-add `0x180044b00` (`enrolAddImage`) → GetEnrollmentStatus |
| 18 | `0x18003a0c0` | **GetEnrollmentStatus** | → `enrolGetTemplate` (`0x180045690`) → templatePack |
| 20 | `0x18003a410` | **CheckForDuplicate** | → `identifytemplate` (`0x180045fd0`, template-vs-template) |
| 21 | `0x18003ab30` | **CommitEnrollment** | → `enrolFinish`(`0x180032a70`), templatePack, сохранение |
| 22 | `0x18003b190` | DiscardEnrollment | → `enrolFinish` (cleanup) |

(слоты 4–11: Attach/Detach/ClearContext/QueryPreferredFormat/QueryIndexVectorSize/
QueryHashAlgorithms/SetHashAlgorithm/QuerySampleHint; 23–30: ControlUnit(+Privileged)/
NotifyPowerChange/PipelineInit/PipelineCleanup/Activate/Deactivate.)

Уверенность: **высокая** — имена взяты из лог-строк `L'EngineAdapter<Method>'` в каждой
функции; порядок слотов совпадает с каноническим `WINBIO_ENGINE_INTERFACE`.

---

## 3. ГЛАВНЫЙ ВОПРОС: обязателен ли препроцессинг перед enroll/identify?

**ДА, в обоих сценариях. Подтверждено.**

- `preprocessor` (`0x18000efa0` в AlgoMilan) вызывается ТОЛЬКО из EngineAdapter (5 мест,
  см. таблицу). Ни `enrolAddImage`, ни `InitIdentifyImage`/`identifyImage`,
  ни `identifytemplate` внутри себя препроцессинг не делают — они работают по уже
  препроцессированному изображению/шаблону (подтверждает F1/F2 из `00-overview.md`).

- **Enroll:** сырой кадр препроцессируется в `EngineAdapterAcceptSampleData` (слот 12),
  результат кладётся в EngineContext; отбор лучшего кадра — `choose_enroll_img`
  (`0x180041700`/`0x180041d40`, тоже через `preprocessor`). Только потом
  `UpdateEnrollment`→`enrolAddImage` берёт готовое изображение. `enroll-add` `0x180044b00`
  сам `preprocessor` НЕ зовёт.

- **Verify (VerifyFeatureSet, слот 14):** препроцессинг сделан ранее в AcceptSampleData;
  `VerifyFeatureSet` сам `preprocessor` не зовёт, сразу идёт в core_identify.

- **Identify (IdentifyFeatureSet, слот 15):** дополнительно САМ вызывает `preprocessor`
  (`0x180038456`) по буферу кадра ДО core_identify. Т.е. и здесь кадр обязательно
  препроцессирован. Ветка `ret==0x84` → `LIVENESS_FAIL` → `WINBIO_E_BAD_CAPTURE`.

Итог: **между сырым кадром и `enrolAddImage`/`identifyImage` всегда стоит `preprocessor`.**
Наши прежние шаблоны из НЕпрепроцессированных кадров — вероятная причина плохого сравнения.

Фрагмент вызова `preprocessor` в AcceptSampleData (`0x180033c20`):
```
0x1800358da: lea    rdx, [rsp + 0x104]            ; out: tcode/adjust
0x1800358e2: lea    rcx, [rsp + 0x1e0]            ; out: указатель препроц. изображения
0x1800358d4: mov    r9, rcx                       ; in: frame base
0x1800358d7: mov    r8, rdx                        ; in: frame+0x40
0x1800358ea: call   qword ptr [rip + 0x106578]    ; <preprocessor_wrapper>
0x1800358f0: mov    dword ptr [rsp + 0xd8], eax    ; algo_ret
...
0x180035cfd: lea    rax, [...]  ; "preprocessor success, coverage:%u quality:%u"
```

---

## 4. Аргумент `ppp_param_init` и параметры препроцессора

### 4a. `ppp_param_init` — аргумент = 10 (профиль сенсора Milan 64×80)

В `_InitAlgorithmEngine` (`0x18002e8d0`):
```
0x18002ebac: mov    rax, qword ptr [rsp + 0x58]   ; EngineContext
0x18002ebb1: mov    ecx, dword ptr [rax + 0xc8]   ; arg = EngineContext->sensorType (offset 0xc8)
0x18002ebb7: call   qword ptr [rip + 0x10d293]    ; ppp_param_init_wrapper(ecx)
0x18002ebc9: lea    rax, [...]  ; "algo_ret for param init 0x%x"
```
Аргумент — поле `EngineContext[0xc8]` (int «sensor type»), задаётся при инициализации по
данным сенсора, а не константой в пути сравнения.

Конкретное значение доказывается через таблицы:

- Внутренний `ppp_param_init` (AlgoMilan `0x18000e670`): аргумент `type` должен быть
  `< 0xC` (иначе `preprocessor_init: unsuported sensor type`, ret `0x81`). Индексирует
  таблицу профилей (шаг 32 байта) по адресу `0x180094270`. Запись **idx 10**:
  `[0x0a, 1, 0, 4, 0x320, 0x40, 0x50, 0x0a]` → поля `0x40=64` и `0x50=80` = **кадр 64×80**.
  (Для сравнения idx 3 = 112×132 Chicago-T, idx 2 = 54×176 Chicago-HS.)
- Таблица размеров кадра в EngineAdapter (`0x180113520`, шаг 14 байт, индекс =
  «sensorTypeforpub»): **idx 10 = col=80, row=64** (5120 px = наш кадр 64×80); idx 3 =
  112×132; idx 2 = 176×54. Т.е. Milan-сенсор = индекс **10** в обеих таблицах.

**Вывод: для 5125 `ppp_param_init(10)`.** Уверенность: **высокая** (две независимые
таблицы дают 64×80 именно на индексе 10; диапазон валиден: 10 < 0xC).

### 4b. `preprocessor_init` — инициализация препроцессора и калибровка

`_AdapterInitPreprocessor` (`0x180031160`, лог `_AdapterInitPreprocessor`) — вызывается
лениво из AcceptSampleData при первом кадре (`0x180034b9a`,`0x18003560e`). Порядок:

```
preprocess_get_calidata_len(&col,&len)        ; узнать длину калиданных
  → alloc(len+0x10)
чтение sensorid из EngineContext+0x51 (16 байт)
сравнение sensorid с файлом calidata:
  если совпал  → preprocess_load_calidata(buf,len, arr, arrlen)   ; калибровка из файла
  если нет      → preprocess_init_calidata()                       ; калибровка по умолчанию
[при загрузке] → preprocess_init_calidata()
preprocessor_init(&params)                     ; params @ [rsp+0x98]:
                                               ;   +0x18 = calidata buffer (arg[0xe8])
                                               ;   +0x24 = row, +0x28 = col (из таблицы 0x180113520)
                                               ; baseframelen (arg r9d) → EngineContext+0x2ac
[при ошибке init повтор один раз]
EngineContext+0x2a8 (basevalid) = 1
```
Лог по пути: `baseframelen:%d, col:%d, row:%d, CaliLen:%d, ArrLen:%d`,
`algo_ret for preprocess init 0x%x`, `basevalid:%d`.

Уверенность параметров init: **средняя-высокая** (col/row приходят из таблицы 0x180113520
= 80/64; точная раскладка struct `params` для `preprocessor_init` частично выведена —
детали полей лучше сверить с внутренним `preprocessor_init` `0x18000f200` в AlgoMilan).

---

## 5. Детальный порядок core-функций

### core_identify (`0x180042d50`) — сердце verify/identify (кадр-против-шаблона)
```
templateUnPack(stored_template)        @0x1800430b9 / 0x1800430e6
InitIdentifyImage(preproc_image)       @0x1800431e7   ; лог "InitIdentifyImage gxErr:0x%x"
identifyImage(...)                      @0x180043300   ; result<0 → не совпало
if match:
    templateStudy(...)                 @0x180043643   ; "templateStudy updatestatus:%d"
    identifyUpdate(...)                @0x180043750   ; дообучение шаблона
    templatePack(updated_template)     @0x18004387b   ; (через 0x18002f6d0) сохранить обновл.
getTemplateInfo(...)                   @0x1800438f7   ; "success with kp %d/%d, tpl %d/%d"
on fail: FreeIdentifyImageWhenFail(...) @0x180043c38
templateDelete(unpacked)               @0x180043ce4   ; очистка
```

### CheckForDuplicate-inner (`0x180045fd0`) — template-vs-template (при enroll)
```
templateUnPack(...) x2
identifytemplate(a,b)                   @0x180046349   ; "identifytemplate error, algo_ret=0x%x"
templateDelete(...) x2
```

### Template pack/unpack хелперы
- pack `0x18002f6d0`: `templateGetPackedSize` → alloc → `templatePack`.
- unpack `0x180030750`: `templateUnPack` (x3 варианта).

---

## Эталонный пайплайн (псевдокод для воспроизведения в C)

```c
/* ---------- INIT (один раз, лениво при первом кадре) ---------- */
LoadLibrary("AlgoMilan.dll"); resolve_all_wrappers();     // 0x180052f00 + 0x1800533a0
getAlgorithmVersion();
ppp_param_init(10);                                        // 10 = профиль Milan 64x80  (обязательно ДО препроцессинга)

// _AdapterInitPreprocessor:
len = preprocess_get_calidata_len(&col);
buf = alloc(len + 0x10);
if (sensor_id == calidata_file.sensor_id)
    preprocess_load_calidata(buf, len, arr, arr_len);
else
    preprocess_init_calidata();                            // калибровка по умолчанию
preprocess_init_calidata();                                // (в ветке load вызывается ещё раз)
preprocessor_init(&params{ .calidata=buf, .col=80, .row=64, .baseframelen=... });

/* ---------- ПЕР-КАДРОВЫЙ ПРЕПРОЦЕССИНГ (AcceptSampleData) ------ */
// ОБЯЗАТЕЛЬНО для каждого кадра, и при enroll, и при verify/identify:
ret = preprocessor(&pp_image_out, &tcode_out, frame, frame+0x40, frame+0x30, flags);
// ret==0 → success (coverage, quality); ret==0x84 → LIVENESS_FAIL → BAD_CAPTURE
// pp_image_out складывается в контекст, дальше идёт в enroll/identify.

/* ---------- ENROLL ---------- */
enrolStart(session);                                       // CreateEnrollment
for (each captured frame) {
    preprocessor(...);                                     // <-- кадр препроцессирован (см. выше)
    enrolAddImage(pp_image);                               // UpdateEnrollment; НЕ препроцессирует сам
    status = enrolGetTemplate(...);                        // GetEnrollmentStatus: прогресс/готовность
}
// (опц.) identifytemplate(new, existing)  // CheckForDuplicate: дубликат?
enrolGetTemplate(&tpl);                                    // финальный шаблон
templatePack(tpl -> blob);                                 // упаковка для хранения
enrolFinish(session);                                      // CommitEnrollment / очистка

/* ---------- VERIFY (кадр против одного шаблона) ---------- */
preprocessor(frame -> pp_image);                           // (в AcceptSampleData)
templateUnPack(stored_blob -> tpl);
InitIdentifyImage(pp_image);
res = identifyImage(...);                                  // res>=0 → совпало
if (res >= 0) { templateStudy(); identifyUpdate(); templatePack(updated); }
getTemplateInfo();
if (fail) FreeIdentifyImageWhenFail();
templateDelete(tpl);

/* ---------- IDENTIFY (кадр против базы) ---------- */
// IdentifyFeatureSet сам ещё раз препроцессирует буфер кадра:
ret = preprocessor(frame -> pp_image);                     // 0x180038456; ret==0x84 → LIVENESS_FAIL
// затем тот же core_identify по кандидатам:
for (candidate template) { templateUnPack; InitIdentifyImage(pp_image); identifyImage; ... }
// на неуспехе — PostIdentifyImageWhenFail().
```

Ключевое для нашего драйвера: последовательность
`ppp_param_init(10)` → `preprocessor_init(+calidata)` (один раз) → **`preprocessor(frame)`
для КАЖДОГО кадра** → `enrolAddImage`/`InitIdentifyImage`+`identifyImage`. Пропуск
`preprocessor` — не соответствует вендорскому пайплайну.
