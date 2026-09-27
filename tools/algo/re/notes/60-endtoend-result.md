# End-to-end результат матчер-пути (algo_eval2)

Дата: 2026-09-28. Harness: `tools/algo/algo_eval2.c` (+ загрузчик `winpe.c`).
Данные: `frames_raw.bin`/`bg_raw.bin` (сырые uint16 64×80), 155 записей.

## Что заработало
Полный вендорский конвейер воспроизведён на Linux без wine, все стадии возвращают 0:
```
ppp_param_init(10) = 0
preprocess_init_calidata() = 0
preprocessor_init(&cal{bg, col=64, row=80}) = 0
preprocessor(...) = 0 на 149/155 кадрах   (6 брак — низкое качество отдельных касаний)
enrolStartEx → enrolAddImage×N → enrolGetTemplate  → шаблон построен (nCurrent=8 из 15)
identifytemplate(ref, probe, NULL, &idx)  → без краша
```

## Качество препроцессинга — ОТЛИЧНОЕ (проверено визуально)
Montage обработанных `natural`-кадров (`$CLAUDE_JOB_DIR/tmp/natural_proc.png`):
чёткие гребни/впадины, полный контраст (0..255, ~90% ненулевых). Вендорский
flat-field + reconstruction работает корректно. Значит препроцессинг больше НЕ
является узким местом.

## Текущий результат сопоставления
```
GENUINE : 0/14 matched (FRR 100%)  [1 unusable]
IMPOSTOR: 0/94 matched (FAR 0%)    [6 unusable]
```
FAR=0% (ложных совпадений нет), но FRR=100% — свой палец не распознаётся ни разу.
Поскольку изображения качественные, причина — на уровне РЕШЕНИЯ о совпадении.

## Рабочие гипотезы о FRR=100% (приоритет)
1. **Не тот путь сравнения.** `identifytemplate` штатно используется движком как
   CheckForDuplicate при enroll (строгий поиск почти-дубликата записи), а НЕ как
   верификация. Порог совпадения в `identifytemplate` — `score > 0` (0x18000e28c
   `cmp [rsp+0x40],0; jg match`). Настоящая верификация у EngineAdapter идёт через
   `identifyImage` (image↔template, 0x18000de30) внутри core_identify — [30-engine-flow].
   → Попробовать `identifyImage(проба-изображение, gallery)`.
2. **Разные участки/углы пальца.** `natural` сняты под разными углами (до 90°+) и
   разными зонами (см. CLAUDE.md); перекрытие enroll- и probe-кадров может быть мало.
   Одна проба = 1 кадр. Возможно, нужно больше кадров/иная сборка галереи.
3. **Порог/нормализация.** Нужно вывести реальный SCORE матчера для genuine vs
   impostor, чтобы понять: score(genuine) > score(impostor)? Если да, но < порога —
   это калибровка порога; если ≈ — проблема геометрии/фич.

## Что дальше
- Инструментировать матчер: вывести score (значение в [rsp+0x40] после matcher) для
  каждой пары → распределение genuine/impostor. Это разделит гипотезы 1/3.
- Разобрать и подключить `identifyImage` (штатный verify-путь) вместо/вместе с
  `identifytemplate`. Наиболее вероятный правильный путь.
- При необходимости — сверка на железе (палец пользователя) финальной точности.

Связано: [40-pipeline-verified], [50-preprocess-crash], [20-template-identify], [30-engine-flow].
