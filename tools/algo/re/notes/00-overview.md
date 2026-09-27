# AlgoMilan RE — обзор и метод

Цель: восстановить **полный вендорский пайплайн** сравнения отпечатков из
`win-driver/AlgoMilan.dll` (Milan_v_3.00.20), чтобы воспроизвести его на Linux
через наш загрузчик `tools/algo/winpe.c`. Особый вопрос: **является ли препроцессинг
частью алгоритма сравнения** (а не косметикой).

Базовый адрес обоих DLL: `0x180000000`. Экспортируются **обёртки** (`*Wrapper`),
которые вызывают внутренние функции; их и грузим через `winpe_getproc`.

## Инструмент

```
cd tools
../.venv/bin/python -m fwre.dump <DLL> func <addr>      # дизасм функции по .pdata
../.venv/bin/python -m fwre.dump <DLL> range <lo> <hi>
../.venv/bin/python -m fwre.dump <DLL> calls <addr>     # вызовы внутри функции
../.venv/bin/python -m fwre.dump <DLL> callers <addr>   # кто вызывает addr
../.venv/bin/python -m fwre.dump <DLL> xref <addr>      # rip-ссылки на addr
../.venv/bin/python -m fwre.dump <DLL> str <substr>
../.venv/bin/python -m fwre.dump <DLL> exports
```
(предупреждения RuntimeWarning про sys.prefix из-за symlink venv — игнорировать).

## Ключевые адреса AlgoMilan (внутренние функции)

| функция | wrapper (export) | внутренняя |
|---|---|---|
| ppp_param_init | 0x18000b7b0 | 0x18000e670 |
| preprocessor_init | 0x18000b7d0 | 0x18000f200 |
| preprocessor_exit | 0x18000b7f0 | 0x18000f160 |
| preprocessor | 0x18000b810 | 0x18000efa0 |
| preprocess_save_calidata | 0x18000b870 | 0x18000ebd0 |
| preprocess_load_calidata | 0x18000b8b0 | 0x18000e8b0 |
| preprocess_get_calidata_len | 0x18000b8f0 | 0x18000e800 |
| preprocess_init_calidata | 0x18000b920 | 0x18000e820 |
| preprocess_set_mode | — | 0x18000ef90 |
| preprocessor_get_CalibParam | — | 0x18000f1a0 |
| enrolStart | 0x18000b940 | 0x18000d9b0 |
| enrolStartEx | 0x18000b960 | 0x18000d9d0 |
| enrolAddImage | 0x18000b980 | 0x18000d590 |
| enrolGetTemplate | 0x18000b9f0 | 0x18000d950 |
| enrolFinish | 0x18000ba20 | 0x18000d8e0 |
| InitIdentifyImage | 0x18000ba40 | (см. wrapper) |
| identifyImage | 0x18000ba60 | (см. wrapper) |
| templateStudy | 0x18000bb40 | 0x18000e480 |
| templateGetPackedSize | 0x18000bb60 | 0x18000e380 |
| templatePack | 0x18000bb80 | 0x18000e3d0 |
| templateUnPack | 0x18000bbb0 | 0x18000e590 |
| templateDelete | 0x18000bbf0 | 0x18000e2f0 |
| identifytemplate | 0x18000bc90 | 0x18000e1b0 |
| identifyUpdate | 0x18000bc30 | 0x18000e1a0 |
| getTemplateInfo | 0x18000bcd0 | — |
| getCalibParam | 0x18000bcf0 | — |
| getQuality | (export) | 0x18000dc20 |

## Подтверждённые находки (проверено вручную)

- **F1. Препроцессинг — обязательный отдельный шаг.** `preprocessor` (0x18000efa0)
  вызывается ТОЛЬКО из `preprocessor_wrapper` (0x18000b85b). Ни `enrolAddImage`,
  ни identify внутри его не вызывают. Значит вызывающий (EngineAdapter) обязан
  прогнать каждый кадр через `preprocessor` перед enroll/identify.
- **F2. enrolAddImage (0x18000d590)** внутри вызывает только `0x1800088e0` (x2) и
  `0x18002aba0` — работает по уже препроцессированному изображению.

Отсюда гипотеза: наши прежние enroll-шаблоны строились из НЕпрепроцессированных
кадров → сравнение неадекватно / identifytemplate падает. Нужно воспроизвести
`preprocessor_init(+calidata)` → `preprocessor(frame)` → enroll/identify.

- **F3. Краш identifytemplate — ДВЕ причины (агент B, проверено мной).**
  (1) `0x18000f810` (что мы звали как «study») — это ДЕСТРУКТОР шаблона
  (`freeTemplate(void** holder)`): вызывается из `templateDelete` (0x18000e2f0) и
  `enrolFinish` (0x18000d8e0), тело `holder→obj`+null-checks. Вызов его на галерее
  уничтожал шаблон → висячий указатель записи rec+0x148 → SIGSEGV.
  (2) `InitIdentifyImage` (0x18000ba40) в этой сборке — ЗАГЛУШКА `mov eax,0x83; ret`.
  Правильный image-путь — `identifyImage` (0x18000ba60, реализован, внутр. 0x18000de30).
  Формат enroll-шаблона == identify-шаблона, конвертация НЕ нужна. Сравнение:
  `identifytemplate(&ref, &probe, NULL, &idx)` живыми holder'ами, ничего не разрушая.
  `enrolFinish` тоже разрушает шаблон — для хранения `templateGetPackedSize`+`templatePack`
  ДО finish, восстановление `templateUnPack`. Подробности: [20-template-identify].

- **F4. Препроцессинг обязателен + НЕВЕРНЫЙ ПРОФИЛЬ (агент A, проверено мной).**
  `preprocessor` (0x18000efa0) — per-pixel flat-field коррекция (усиление `kr>>13`
  минус фон `b`) + quality/coverage, вход 16-бит ROW*COL, выход 8-бит. Без флага
  CALIBRATED==1 возвращает 0x80 «not calibrated», без PARAM_INIT==1 — 0x80. Т.е.
  калибровка обязательна.
  `ppp_param_init(type)` (0x18000e670): валидно 0..11, выбирает профиль из таблицы
  `0x180094270` (12×0x20). Проверено дампом: **idx 10 = col 64 / row 80 — НАШ сенсор.**
  idx 3 = 112×132 — мы ошибочно использовали именно его в enroll (`ppp(3)`).
  Раскладка `gimg`: поле @0x14 = ДЛИНА ROW*COL (=5120), а не «frames»; в algo_eval
  стояло 1 — баг. Размеры preprocessor берёт из профиля, не из gimg.
  Порядок: `ppp_param_init(10)` → `preprocess_init_calidata()` → `preprocessor_init(&cal)`
  [cal+0x18=фон-кадр 16-бит, +0x24=col, +0x28=row; ставит CALIBRATED] → на каждый
  кадр `preprocessor(&src, NULL, 2*5120, &dst, &qcov, 0, 0)` → `preprocessor_exit()`.
  Подробности: [10-preprocess]. NB: idx профиля агент пометил как требующий
  проверки на железе, но дамп таблицы однозначно даёт 64×80 = idx 10.
