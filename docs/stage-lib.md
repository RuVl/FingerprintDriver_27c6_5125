# Библиотека openchicago

Самостоятельная библиотека ChicagoHS (AlgoChicago.dll, профиль 12), побайтно совпадающая с DLL на
датасете: порт MR !648 (LGPL-2.1+, © Berke Kabagöz) со всеми правками стадий 1–7, без DLL и без
`--wrap`. Публичный API — `openchicago/include/openchicago.h`. Сделано 2026-09-29.

## Структура

```
openchicago/
  meson.build, tests/meson.build        статическая libopenchicago.a + тесты (suite unit / dll)
  include/openchicago.h                 публичный API (GLib)
  src/openchicago.c                     сеанс, enroll PLAIN/ENGINE, verify+study, состояние OCST
  src/openchicago-private.h             только для тестов: сравнение состояний, enrolDeleteImage
  src/goodix-chicago-preprocess.[ch]    stage 1 (preprocessor + контекст 0x180043c70) + save/load состояния
  src/goodix-chicago-calibration.[ch]   calidata MR !648 (без изменений)
  src/goodix-chicago-feature.[ch]       getFeature 0x180013490 (бывш. FFIXES, feature-dll.h влит)
  src/goodix-chicago-enrollment.[ch]    enrolAddImage/templatePack/templateStudy (EFIXES+SFIXES)
  src/goodix-chicago-match.[ch]         identifyImage (MFIXES+SFIXES)
  src/goodix-chicago-late-rejection.c   поздний отказ type 24
  src/goodix-chicago-runtime.[ch]       preprocessor → getFeature → probe; match/study по blob
  src/goodix-chicago-template.[ch]      только print_data "(uayayay)" MR !648
  tests/test_e2e.c, e2e.sh, e2e_cmp.py  сквозная сверка с DLL (только через openchicago.h)
  tests/test_state.c                    сериализация состояния
  tests/test_api.c                      calidata, print_data, ошибки, ENGINE, add_pair
  tests/run.sh, test_preprocess.c, test_context_fuzz.c   стадия 1 (как раньше)
```

Сборка и тесты: `cd openchicago && meson setup build && ninja -C build && meson test -C build`
(`--suite unit` — без DLL; `--suite dll` — run.sh и e2e.sh, нужны win-driver/AlgoChicago.dll и
tools/algo/{frames_raw,bg_raw}.bin, meta_raw.txt).

## Что сделано с кодом порта

- Источник — `tools/algo/.port-openpp` (upstream + все patch-и FFIXES/EFIXES/MFIXES/SFIXES), SPDX и
  копирайт сохранены, в каждом файле строка «Modified 2026 for openchicago».
- Глобалов нет: `static feature_state` (runtime.c) → `OcSession.feature_state`; `static pairs[512][2]`
  (match.c) → локальный; MTRACE/`g_getenv` удалены. Состояние препроцессора было объектом и раньше.
- Обёртки `--wrap` port_eval.c перенесены как код: runtime зовёт `preprocessor_process_context()`
  (q/c/cbuf из stage 1) и getFeature с этим контекстом; приближения порта (`build_enhanced_checked`,
  `finalize_metrics`, `feature_preprocessor_context`) удалены.
- Мёртвый код удалён по `-ffunction-sections --gc-sections` от четырёх тестовых программ
  (пересечение) до неподвижной точки: файлы bir, late-rejection-compat, goodix-crc, feature-dll.h,
  late-rejection-private.h; ~2500 строк функций (старый путь препроцессора, engine_enrollment_policy
  MR, system template, fallback-агрегации, …), их прототипы и типы; write-only `group_edges`.
- Новое (сверено с DLL, см. ниже): результат регистрации 0x180019410 (позиционная потеря по
  несгруппированной evidence) и формула r = ev+0x1c + ev+0x20·100/5121 при наличии evidence
  (overlay/preoverlay = sess+0x14/+0x10); enrolDeleteImage 0x18000ec10 (порядок сравнения не
  трогается, сброс группы при удалении якоря).
- `tools/algo/port_eval.sh OPENPP=1` выведен из употребления (заменён e2e.sh); остальные режимы
  port_eval работают (cmp_study.sh N=12: 23/23, 0/127).

## API (include/openchicago.h)

- Сеанс: `oc_session_new(image_base)` (первый запуск), `oc_session_new_from_calidata(path, sensor_id
  = OTP[0:16], image_base)` (файл MR !648 + rebase), `oc_session_free`. Кадр — 64 строки × 80
  (`oc_frame_transpose` из транспортных 80×64).
- `oc_session_preprocess(frame, enroll, &res)` — один вызов preprocessor() (прогрев/диагностика).
- Enroll: `oc_enroll_begin(protocol, n_stages)`, `oc_enroll_add(frame, &res)` →
  {accepted, reject (причина), status, quality, coverage, overlay, preoverlay, tip_direction, stage,
  samples, progress, complete}, `oc_enroll_add_pair(frame, extra)` (choose_enroll_img),
  `oc_enroll_finish()` → blob (templatePack), `oc_enroll_cancel`.
- Verify: `oc_verify(blob, frame, &res, &updated)` → {match, score, subtemplate, reject, q, c};
  `updated` ≠ NULL → templateStudy после совпадения, `*updated` = новый blob или NULL.
- Состояние: `oc_session_save_state()` → GBytes формата OCST v1 (секции CALI, IBAS, PREP — все
  адаптивные переменные препроцессора, включая историю контекста 0x180038380 = то, что DLL кладёт в
  calidata G+0x26488, — и FEAT — getFeature; CRC-32), `oc_session_new_from_state()`. Регистрация в
  процессе в состояние не входит. 235 318 байт.
- Хранение: `oc_print_data_new/get_template` — GVariant MR !648.

### Протокол регистрации EngineAdapter (notes/81) — где живёт

В библиотеке, `OC_ENROLL_ENGINE` (openchicago.c, `engine_after_add`): пороги coverage ≥ 65 / quality
≥ 25 до enrolAddImage, подсказки для кадров 5..12 при overlay > 30 или preoverlay > 20 (≤ 8 всего,
≤ 2 подряд, направления 4,1,3,2), backup-кадр (ov > 80 и pov > 70 на первой подсказке → удалить,
вторая подсказка → оставить кадр с меньшим overlay, принятие → вернуть backup), завершение при 12
засчитанных или 20 кадрах в шаблоне; progress = count·100/20. choose_enroll_img —
`oc_enroll_add_pair`. Драйверу остаётся: захват, второй кадр того же касания, отображение
reject/tip_direction в `FP_DEVICE_RETRY_*`, стадии = `stage` из 12.

## Проверка (2026-09-29)

| проверка | результат |
|---|---|
| e2e N=12 (STUDY=1) | пробы 127/127, q/c/score/subtemplate/решение ≠ 0, gallery blob =, study 23/23 blob = |
| e2e N=12 WARM | 124/124, ≠ 0, study 26/26 |
| e2e с save/restore сеанса (RESTORE_AT 4 и 5 точек) | то же, 0 расхождений |
| чужие | 0 совпадений во всех прогонах |
| enroll: overlay/preoverlay/count + шаблон (N=20; N=30 WARM) | 20/20, 29/29, шаблон = DLL |
| enrolDeleteImage (3 сценария EDEL в e2e) | трасса и шаблон = DLL |
| отрицательные | 1 бит study-blob и изменённый score ловятся; порча состояния (PREP, CRC пересчитан) → 65/105 и т.п. выходов ≠ (should_fail); битый blob/состояние отвергаются |
| сериализация, k = 1, 7, 12, 40, 97, 150, 230 | выходы препроцессора после load 0/254…0/25 ≠, состояние в памяти идентично; без полей history/presence/residue тест падает (чувствительность) |
| stage 1 run.sh | A/B 155/155, fuzz 0 |

## Остаток

- enrolDeleteImage сразу после кадра, вызвавшего вливание в группу (0x180018140; на датасете rec 41,
  42): T (связи, группы, ETRACE) = DLL, но упакованный список связей (секция 0x96) отличается — DLL
  строит лес иначе (вероятно, через 0x1800320e0 → 0x180031840). Затрагивает только backup-путь
  ENGINE (нужны ov > 80 и pov > 70) — не перенесено.
- ENGINE как целое не сверено с EngineAdapter.dll (оракула EA нет): логика по notes/81, входы
  (overlay/preoverlay, удаление) сверены с AlgoChicago.dll.
- Rebase сохранённого состояния на новый ImageBase (preprocessor_init при каждом открытии) — в API
  нет; `oc_session_new_from_state` восстанавливает сеанс как был. Семантику DLL (что сбрасывает
  preprocessor_init в глобалах) нужно выяснить на стадии 8.
- Путь замены при полной ёмкости (templateStudy, 50 подшаблонов) по-прежнему не проверен.
