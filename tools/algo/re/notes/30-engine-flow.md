# EngineAdapter.dll → алгоритм: выбор DLL, обёртки, точные вызовы

`win-driver/EngineAdapter.dll` (+ `gfusb.dll`), imagebase `0x180000000`. EngineAdapter —
debug-сборка (аргументы через стек-слоты `[rsp+0x300..]`), источники значений читаются однозначно.
Обозначения: `pipe` — WINBIO_PIPELINE*, `ctx = [pipe+0x38]` (EngineContext, calloc 0x368),
`FS = *ctx` — feature set (препроцессированный кадр), `cfg` — глобал `0x1801173d0`
(`0x1800295c0(0)`), `g_sensortype = [0x18012052c] = ctx[0xc8]`.

## Выбор DLL и профиля (ТОЧНО)

1. gfusb `init_FPSensor`: `0x180071e09` chipid = `buf[2]<<8 | buf[1]`; наш `probe.py` читает
   `a2 04 25 00` ⇒ **0x2504**.
2. gfusb `device_enable_init_by_chip` (0x18006a948), switch по chipid:
   `0x2205 → 3 "MilanL"`, `0x2208 → 2 "MilanG"`, **`0x2503..0x2504 → 0xc "ChicagoHS"`**
   (`0x18006ab0b: cmp [rsp+0x64],0x2504; jbe` → `mov [rax+0xf4],0xc`), `0x2507..0x2508, 0x2510 → 0xe "ChicagoT"`.
3. EA `_GetSensorInfoAndLoadAlgo` (0x180031fc0): `g_sensortype = ctx[0xc8]` (0x1800322c7) →
   `LoadAlgorithm` (0x180052f00): `2 → milanG → AlgoChicago.dll`, `3 → milanL → AlgoMilan.dll`
   (единственный случай AlgoMilan), **`0xc → chicagoHS → AlgoChicago.dll`**, `0xe → chicagoT → AlgoChicagoT.dll`,
   иначе "not supported sensor:%d". Хендл — `g_hAlgoModule 0x18013bf38`.
4. `_InitAlgorithmEngine` (0x18002e8d0) `@0x18002ebb7: ppp_param_init(ctx[0xc8])` ⇒ **ppp_param_init(12)**.
5. Таблица размеров EA `0x180113520` (14 байт/запись, индекс = sensorType): **idx 12 =
   (80, 64, 25, 64, 10240, 3200, 19008)**; idx 10 — тоже 80×64; idx 3 = 112×132.

Итог: 27c6:5125 = **AlgoChicago.dll + профиль 12** (тип шаблона 24). Не проверено лишь, что
Windows читает тот же регистр chip id, что и probe.py (узкий диапазон 0x2503..0x2504 делает это
очень вероятным). AlgoMilan/10 тоже совместим по кадру (64×80), см. 70.

## Резолв 29 обёрток

Резолвер `0x1800533a0` (тег `ExportFunctions`): `GetProcAddress(g_hAlgoModule, name)` → слоты
`0x18013be50..0x18013bf30`; NULL → лог `'<имя> is NULL.'` и ошибка. Места вызова — скан всех
`call [rip+slot]` (уверенность высокая).

| обёртка | слот | где вызывается |
|---|---|---|
| ppp_param_init | 0x18013be50 | `_InitAlgorithmEngine` 0x18002e8d0 @0x18002ebb7 |
| preprocessor_init | 0x18013be58 | `_AdapterInitPreprocessor` 0x180031160 @0x18003184b, 0x1800318a8 |
| preprocessor_exit | 0x18013be60 | Detach 0x180033130 @0x1800334de |
| preprocessor | 0x18013be68 | AcceptSampleData 0x180033c20 @0x1800358ea; IdentifyFeatureSet @0x180038456; choose_enroll_img 0x180041700/0x180041d40; self-test 0x18004aec0 |
| preprocess_save_calidata | 0x18013be70 | 0x18002ec30, 0x1800518d0 |
| preprocess_load_calidata | 0x18013be78 | `_AdapterInitPreprocessor` @0x1800316b9, 0x1800316db |
| preprocess_get_calidata_len | 0x18013be80 | 0x18002ec30; `_AdapterInitPreprocessor` @0x1800312f7, 0x180031401; 0x1800518d0 |
| preprocess_init_calidata | 0x18013be88 | `_AdapterInitPreprocessor` @0x180031734, 0x1800317dc |
| enrolStart | 0x18013be90 | 0x18003b5c0 (CreateEnrollment); self-test |
| enrolStartEx | 0x18013be98 | не вызывается |
| enrolAddImage | 0x18013bea0 | 0x180044b00 @0x180044cd0; 0x1800441b0, 0x180044880 (backup-кадр); self-test |
| enrolDeleteImage | 0x18013bea8 | 0x1800441b0, 0x180044b00 |
| enrolGetTemplate | 0x18013beb0 | 0x180045690 (GetEnrollmentStatus); self-test |
| enrolFinish | 0x18013beb8 | 0x180032a70, Detach, ClearContext, self-test |
| InitIdentifyImage | 0x18013bec0 | core_identify @0x1800431e7 — только sensorType 0xe |
| identifyImage | 0x18013bec8 | core_identify @0x180043300 |
| FreeIdentifyImageWhenFail | 0x18013bed0 | core_identify @0x180043c38 — только 0xe |
| PostIdentifyImageWhenFail | 0x18013bed8 | IdentifyFeatureSet 0x180037850 |
| templateStudy | 0x18013bee0 | core_identify @0x180043643 |
| templateGetPackedSize / templatePack | 0x18013bee8 / 0x18013bef0 | pack-хелпер 0x18002f6d0 @0x18002f952 / @0x18002f9e6 |
| templateUnPack | 0x18013bef8 | unpack-хелпер 0x180030750 |
| templateDelete | 0x18013bf00 | core_identify @0x180043ce4; CheckForDuplicate 0x180045fd0 |
| getAlgorithmVersion | 0x18013bf08 | `_InitAlgorithmEngine` @0x18002eae2 |
| identifyUpdate | 0x18013bf10 | core_identify @0x180043750 |
| gx_sensorCheck | 0x18013bf18 | не вызывается |
| identifytemplate | 0x18013bf20 | CheckForDuplicate 0x180045fd0 @0x180046349 |
| getTemplateInfo | 0x18013bf28 | core_identify @0x1800438f7; 0x180045690 |
| getCalibParam | 0x18013bf30 | не вызывается |

## WBDI

`WbioQueryEngineInterface` (0x18002e420, единственный экспорт) → `WINBIO_ENGINE_INTERFACE`
@0x180120530. Имена — по лог-тегам `L'EngineAdapter<Method>'` (уверенность высокая).

| слот | адрес | метод | что зовёт |
|---|---|---|---|
| 12 | 0x180033c20 | AcceptSampleData | `preprocessor` на каждый кадр |
| 14 | 0x180036e60 | VerifyFeatureSet | core_identify @0x1800372d5 |
| 15 | 0x180037850 | IdentifyFeatureSet | (ветка повторного захвата: `preprocessor`) → core_identify @0x1800388ef; PostIdentifyImageWhenFail |
| 16 | 0x1800398f0 | CreateEnrollment | enrolStart (0x18003b5c0) |
| 17 | 0x180039e60 | UpdateEnrollment | enroll-add 0x180044b00 (enrolAddImage) |
| 18 | 0x18003a0c0 | GetEnrollmentStatus | enrolGetTemplate (0x180045690) → pack |
| 20 | 0x18003a410 | CheckForDuplicate | identifytemplate (0x180045fd0) |
| 21 | 0x18003ab30 | CommitEnrollment | enrolFinish (0x180032a70), pack, сохранение |
| 22 | 0x18003b190 | DiscardEnrollment | enrolFinish |

(13 ExportEngineData; 4–11 Attach/Detach/ClearContext/Query*/SetHashAlgorithm/QuerySampleHint;
23–30 ControlUnit/NotifyPowerChange/PipelineInit/Cleanup/Activate/Deactivate.)

## Конфиг cfg (ТОЧНО)

`init_config` (0x180026020 / 0x180026ca0): `HKLM\Software\Goodix\FP\<Key>`, при отсутствии —
статический дефолт (gfusb.inf ключи не создаёт ⇒ *вероятно* действуют дефолты).

| off | имя | дефолт | где |
|---|---|---|---|
| 0x42c / 0x42d | MinImageQuality / MinImageCoverage | 25 / 65 | core_identify, только при hlk |
| 0x42e / 0x42f | пороги overlay/preoverlay регистрации | ? | enroll-add |
| 0x431 | LivenessSwitch | 0 | preprocessor m1, enrolAddImage arg5, identifyImage arg11 |
| 0x48c | hlk_test_switch | 0 | включает проверку качества в core_identify |
| 0x4a4 | ChooseEnrollImgSwitch | 1 | AcceptSampleData |
| 0x4d8 | af switch | 0 | core_identify unpack/AF |

## Feature set FS (0x4d08 байт, ТОЧНО)

Выделение `_AdapterProcessFeaturesetResource` 0x18002e6e8: `FS = alloc(0x4d08)`,
`FS+0 = alloc(80·64)`, `FS+0x4cf8 = alloc(80·64·2)`. Заголовки src/dst копируются из
глобальных шаблонов 0x18012f4b8 / 0x18012f4c8 (заполняет `_InitAlgorithmEngine`), EA в логах
зовёт +0x08 "col", +0x0a "row".
```
          src (локал [rsp+0x1e0], 0x30 байт)   dst = FS
+0x00     u16* сырой кадр                      u8* выход (5120)
+0x08     80                                   80
+0x0a     64                                   64
+0x0e     bits 16                              bits 8
+0x0f     1                                    1
+0x14     10240                                5120
+0x18     1                                    1
+0x1c     sensor_type = ctx[0x32c] = 0         0   (единственная запись ctx[0x32c] — 0 @0x18002e9f2)
+0x28/29  —                                    quality / coverage (пишет preprocessor)
+0x30/34  —                                    qcov: coverage / quality
+0x40     —                                    cbuf (0x4cb8 места, используется 0x4c98)
+0x4cf8   —                                    u16* копия сырого кадра (memcpy @0x18003587b)
```
Размер FS-области cbuf 0x4cb8; последнее поле таблицы EA для idx 12 (19008 = 0x4a40) — _гипотеза_:
реальный объём буфера.

## Инициализация препроцессора

`_AdapterInitPreprocessor` (0x180031160) — лениво при первом кадре (из AcceptSampleData
@0x180034b9a, 0x18003560e): `preprocess_get_calidata_len` → alloc; сравнение sensorid
(`ctx+0x51`, 16 байт) с файлом калибровки: совпал → `preprocess_load_calidata`, иначе
`preprocess_init_calidata`; затем `preprocessor_init(&params)`, params @[rsp+0x98]:
`+0x18` = u16 base-кадр, `+0x24` = row 64, `+0x28` = col 80 (таблица 0x180113520);
baseframelen → `ctx+0x2ac`; при ошибке init — повтор один раз; `ctx+0x2a8` (basevalid) = 1.
Лог: `baseframelen:%d, col:%d, row:%d, CaliLen:%d, ArrLen:%d`. Base-кадр — второй кадр сэмпла
(`[rsp+0xf8]`, смещение +0x7468). Точная логика ветвей load/init — *вероятно*.

## AcceptSampleData → preprocessor @0x1800358ea (ТОЧНО)

```
0x18003588e: rax = FS+0x30 → [rsp+0x20]      ; arg5 qcov
0x1800358a8: rdx = FS+0x40 → r8              ; arg3 cbuf
0x1800358a5: rcx = FS → r9                   ; arg4 dst
0x1800358b7: byte [rsp+0x30] = 0             ; arg7 m2
0x1800358c4: movzx ebx, byte [cfg+0x431]     ; arg6 m1 = liveness_switch
0x1800358da: lea rdx, [rsp+0x104]            ; arg2 &purpose
0x1800358e2: lea rcx, [rsp+0x1e0]            ; arg1 src
```
`[rsp+0x104] = (Purpose == 4 /*WINBIO_PURPOSE_ENROLL*/) ? 1 : 0` (0x180035750). В IdentifyFeatureSet
(@0x180038456, повторный захват) то же, purpose = 0 (0x180037974). Коды: 0x84 → LIVENESS_FAIL →
BAD_CAPTURE; ≠0 → BAD_CAPTURE. Лог `"preprocessor success, coverage:%u quality:%u"`.

## enroll-add → enrolAddImage @0x180044cd0 (ТОЧНО)

`_AdapterMergeFeatureSetWithEnrollment` 0x180044b00 (из UpdateEnrollment 0x18003a00c):
```c
rc = enrolAddImage([enrollObj+8] /*session*/, FS, FS+0x40 /*cbuf*/, [FS+0x4cf8] /*raw16*/,
                   cfg[0x431] /*0*/, (int*)(FS+0x30) /*{coverage, quality}*/);
```
0x1800441b0/0x180044880 — то же с резервным блоком 0x180123330 («backup enroll image»).
Реакция: 0x83 → enrolFinish + Discard, hr 0x80098008; ≠0 → BAD_CAPTURE (RejectDetail 0xa);
0 → overlay/preoverlay против cfg[0x42e]/[0x42f], слишком перекрывающиеся кадры — enrolDeleteImage.
Лог `"algo_ret for enroll add 0x%x, quality %d, coverage %d, preoverlay..."`.
Протокол регистрации (число касаний, отбор кадров, критерий завершения) не разобран — 70.

## core_identify = `_AdapterCompareTemplateToCurrentFeatureSet` 0x180042d50 (ТОЧНО)

```c
HRESULT core_identify(pipe, ctx, FS, TEMPLATE_REC *rec /*+0x20 blob*, +0x28 size*/,
                      BOOLEAN *pMatch, BYTE b6, DWORD *pReject);
```
Вызывают VerifyFeatureSet @0x1800372d5, IdentifyFeatureSet @0x1800388ef, self-test @0x18004b7b2.
Кадр сравнивается **с одним шаблоном за вызов**, перебор записей БД — снаружи.

1. `templateUnPack` через хелпер 0x180030750 (@0x1800430b9/0x1800430e6) — с NULL 3-м аргументом.
2. `0x180043193: cmp [g_sensortype],0xe; jne` → InitIdentifyImage только для ChicagoT; код не проверяется.
3. identifyImage @0x180043300 — EA передаёт 14 аргументов, обёртка пробрасывает первые 11:
   ```
   0x1800432fd rdx = FS+0x40             ; arg2 param = cbuf
   0x1800432f5 r8  = &holder             ; arg3
   0x1800432ef r9d = 1                   ; arg4 nCand
   [rsp+0x20] = &result                  ; arg5 idx
   [rsp+0x28] = &score                   ; arg6
   [rsp+0x30] = &cq                      ; arg7 {coverage, quality}
   0x1800432c3 [rsp+0x38] = 0            ; arg8 flag
   0x1800432b0 byte ctx[0x2c0]           ; arg9 studyflag = 1 (пишется только в Attach 0x180032ee9)
   [FS+0x4cf8]                           ; arg10 raw16
   cfg[0x431]                            ; arg11 liveness
   ```
   ```c
   rc = identifyImage(FS, FS+0x40, &holder, 1, &result, &score, cq, 0, 1, FS->raw16, 0);
   ```
4. Решение:
   ```c
   if (rc == 0x84 || rc == 0x83 || rc != 0) { *pMatch = 0; hr = 0x80098005; }
   else if (cfg[0x48c] && (cq[0] < cfg[0x42d] || cq[1] < cfg[0x42c])) BAD_CAPTURE;   /* только hlk */
   else if (result < 0) no match;
   else {
     if (score > 0) templateStudy(&update);                      /* @0x180043643 */
     if (score > 0) { *pMatch = 1;
                      if (update >= 2) identifyUpdate(holder);   /* "Replacement occurs" */
                      if (update > 0) { repack(holder) → ctx+0x318; getTemplateInfo; сохранить; } }
     else BAD_CAPTURE (RejectDetail 0xa);
   }
   templateDelete(holder);                                        /* всегда, @0x180043ce4 */
   ```
   **Совпадение ⇔ rc == 0 && result ≥ 0 && score > 0.** Своего порога по score у EA нет;
   пороги качества/покрытия — только при hlk_test_switch.
   После вызова `FS+0x30 = cq[0]` (coverage), `FS+0x34 = cq[1]` (quality); лог
   `"algo_ret for identify algo_ret=%d, result = %d, score = %d, quality=%d, coverage=%d, studyflag=%d"`.

## CheckForDuplicate 0x180045fd0 (ТОЧНО)

`templateUnPack` ×2 → `identifytemplate(a, b, NULL, &idx)` @0x180046349
(`"identifytemplate error, algo_ret=0x%x"`) → `templateDelete` ×2. Шаблон×шаблон при регистрации.

Хелперы: pack 0x18002f6d0 = `templateGetPackedSize` → alloc → `templatePack`; unpack 0x180030750.
