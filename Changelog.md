# Changelog — реверс алгоритма AlgoMilan и воспроизведение конвейера

Хронология работы по воспроизведению проприетарного алгоритма Goodix
(`win-driver/AlgoMilan.dll`, Milan_v_3.00.20) на Linux через нативный загрузчик
Win64-DLL (`tools/algo/winpe.c`, без wine). Цель — точное сравнение отпечатков для
драйвера 27c6:5125. Все подробные разборы — в `tools/algo/re/notes/` (адреса,
дизассемблер-доказательства, уровни уверенности).

## Находки RE (перекрёстно проверены агентами + вручную по дизассемблеру)

Обзор и карта адресов всех функций: [notes/00-overview.md](tools/algo/re/notes/00-overview.md).

1. **Препроцессинг — обязательная отдельная стадия конвейера.**
   `preprocessor` (0x18000efa0) не вызывается ни enroll, ни identify изнутри; штатный
   движок (EngineAdapter) обязан прогнать каждый сырой кадр через него ДО enroll/verify.
   Подтверждено тремя независимыми разборами. → [notes/10-preprocess.md](tools/algo/re/notes/10-preprocess.md),
   [notes/30-engine-flow.md](tools/algo/re/notes/30-engine-flow.md).

2. **Правильный профиль сенсора — `ppp_param_init(10)` (64×80), не 3 (112×132).**
   Таблица профилей 0x180094270 (idx10 = col 64/row 80), подтверждено и таблицей
   EngineAdapter. Раньше ошибочно использовался профиль 3. → [notes/00-overview.md](tools/algo/re/notes/00-overview.md) (F4),
   [notes/10-preprocess.md](tools/algo/re/notes/10-preprocess.md).

3. **`0x18000f810` — это ДЕСТРУКТОР шаблона, а не «study».**
   Вызывается из `templateDelete`/`enrolFinish`. Прежний вызов его на галерее
   разрушал шаблон → SIGSEGV в `identifytemplate`. Формат enroll-шаблона == identify,
   конвертация не нужна. → [notes/20-template-identify.md](tools/algo/re/notes/20-template-identify.md).

4. **Истинная сигнатура `preprocessor`:** `(src*, aux*, cbuf*, dst*, qcov*, m1, m2)`.
   - `aux` (arg2) — не NULL: ядро читает `[aux+0]` как флаг порога качества.
   - `arg3` — УКАЗАТЕЛЬ на coating-буфер ≥0x4c98 (19608) байт, НЕ число байт.
   - `src.len(+0x14)` = 2·ROW·COL (10240, байты 16-бит); `dst.len(+0x14)` = 5120.
   - `dst` получает coverage@+0x28, quality@+0x29.
   Восстановлено по реальному вызову EngineAdapter (0x1800358ea). → [notes/50-preprocess-crash.md](tools/algo/re/notes/50-preprocess-crash.md).

5. **Уровень косвенности вызова `identifytemplate`.** Функция делает ОДИН дереференс
   `[arg0]`, ожидая объект T; поэтому передавать нужно **holder по значению**
   (`idt(ref, probe, ...)`), а не его адрес. Матчер — 0x18001d1f0. → [notes/20-template-identify.md](tools/algo/re/notes/20-template-identify.md),
   [notes/60-endtoend-result.md](tools/algo/re/notes/60-endtoend-result.md).

6. **Эталонный порядок вызовов** (init / enroll / verify) EngineAdapter и то, что
   verify штатно идёт через `identifyImage` (image↔template), а `identifytemplate`
   используется как CheckForDuplicate при enroll. → [notes/30-engine-flow.md](tools/algo/re/notes/30-engine-flow.md),
   сводка проверенного конвейера — [notes/40-pipeline-verified.md](tools/algo/re/notes/40-pipeline-verified.md).

7. **Датасет содержит сырые 12-битные кадры** (`dumps/dataset/*.npz`, `(N,4,5120) uint16`:
   фон=кадр 0, контакт=кадр 2) — их и нужно подавать в вендорский препроцессор.
   → [notes/05-dataset.md](tools/algo/re/notes/05-dataset.md).

## Текущий результат

Полный конвейер воспроизведён end-to-end, краши устранены, препроцессинг даёт
**визуально чёткие отпечатки**. Сопоставление пока: **FRR 100% / FAR 0%** — свой палец
не распознаётся. Диагноз: проблема на уровне решения о совпадении (вероятно, нужен
штатный verify-путь `identifyImage`, а не `identifytemplate`). → [notes/60-endtoend-result.md](tools/algo/re/notes/60-endtoend-result.md).

## Изменения в коде (эта серия работ)

Создано:
- `tools/fwre/dump.py` — CLI-дизассемблер PE (func/range/calls/callers/xref/str/exports)
  поверх `tools/fwre/pe.py`; единый инструмент для RE и проверки агентов.
- `tools/algo/export_raw.py` → `tools/algo/{frames_raw.bin, bg_raw.bin, meta_raw.txt}` —
  экспорт сырых 16-бит кадров (контакт) + фонов (калибровка) из датасета.
- `tools/algo/algo_eval2.c` — harness полного вендорского конвейера
  (init → preprocessor → enroll → identifytemplate), offline-оценка FRR/FAR.
- `tools/algo/re/notes/00..60-*.md` — 8 md-файлов разбора (см. ссылки выше).
- Память: `feedback-use-subagents.md` (предпочтение пользователя активнее использовать
  и проверять субагентов).

Изменено:
- `tools/algo/re/notes/00-overview.md` — добавлены проверенные находки F3 (деструктор)
  и F4 (профиль/препроцессинг).

Не менялось: протокол-слой `tools/goodix5125/*`, драйвер `libfprint/*`, состояние
сенсора. Прежний `tools/algo/algo_eval.c` оставлен как есть (заменён на `algo_eval2.c`).

## Метод

Крупный RE раздроблен на независимых субагентов (препроцессинг / формат шаблонов /
оркестрация EngineAdapter / трассировка краша), каждый писал свой md; выводы каждого
проверялись вручную по дизассемблеру и динамически (gdb). Это дало перекрёстную
валидацию (профиль 10 подтверждён двумя агентами независимо) и ускорило поиск.
