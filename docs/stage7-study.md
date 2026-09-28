# Стадия 7 — templateStudy (дообучение шаблона после совпадения)

Статус: **готово для N ≤ 20 без замены при полной ёмкости** (агент, 2026-09-29). Blob шаблона после
каждого study побайтно = DLL: N=12 23/23, N=12 WARM 26/26, N=15 20/20, N=20 16/16; решения и score/idx ≠ DLL 0
(127/124/124/119 проб), чужих 0. Режимы без SFIXES и SFIXES без STUDY: N=12 0/127, score/idx 0.

## Как EngineAdapter вызывает study (ТОЧНО)

`identifyImage(..., flag 0, studyflag 1)` → при `rc == 0 && idx >= 0 && score > 0`: `templateStudy(&upd)`
(0x18000b330 → 0x18000d7e0: `0x1800305a0(T=[0x1800943e8], feat=[0x1800943e0], ctx=0x1800943f0, &{upd, repl}, 1)`,
затем освобождение фич пробы). `upd > 0` → templatePack и сохранение, `upd >= 2` → identifyUpdate (заглушка
`xor eax,eax`). Шаблон затем удаляется; мутации identify (счётчики +0x124) сохраняются только через repack.

0x1800305a0 для типа 24: `ctx+0x688` (any_strong после сброса 0x180029fb1) → 0x18005d3b0: отношения из ctx
(ctx+4+0x20·i, inl ctx+0x1c+0x20·i); count < capacity → append 0x18005e0d0, иначе (quality > 15) замена
0x18005cba0; перестройка групп 0x18005cf70 только если T+0x87ec == 0 или (== 1 и слот == T+0x87e0);
0x1800320e0 для типа 24 ничего не делает. Затем 0x180027cf0 при repl ≥ 0 (флаг 5 → upd 5), счётчики
T+0x8e14/T+0x8e10 при upd > 3.

## Протокол оценки

`STUDY=1` в algo_eval4/oracle_feat и port_eval: после регистрации пробы идут в порядке записей (genuine,
затем оставшиеся natural — это порядок съёмки), совпадение своих → study, галерея накапливается; чужие
(записи 55..154, после всех своих) — против итоговой галереи без study. `STRACE` — строка на каждый study
(rec, score, upd, fnv blob), `STUDY_DIR` — blob после каждого study.

## Исправления порта (`port_study_fixes.patch`, `SFIXES=1`)

| Адрес DLL | Что было в upstream | Исправление |
|---|---|---|
| 0x180029eaf, 0x18002a3fd | +0x124 (study_a) растил append только у источника | identify: +1 каждому подшаблону со status ≠ 0 и всем допущенным на пути best27fb0 (`hit_increments`), применяются перед append |
| 0x180029fb1 | study_eligible = любая conf | `ctx+0x688` после сброса при остановке цикла |
| 0x18005e0d0 | ++ источника; mval_c = «есть группа» | копия +0x124/+0x128 источника; mval_c (+0xa7) = T+0x8d04++ |
| 0x18005c140 режим 1, 0x180029442 | отношения нового = все из identify | inl ≥ 1 из identify, иначе {-1, тождество}; распространение по графу (strength 2), переснятие непосещённых без метки -2; identify ставит -2 каждой оценённой геометрией записи |
| 0x18005b9f0 (после append 0x18005e200 и unpack 0x180032eb1) | только при полной ёмкости | замыкание отношений по галерее с распространением group_state и ниже ёмкости |
| 0x18005d4b8 | перестройка групп всегда | только без группы или при изменении якоря |
| 0x180026ce0, 0x180052d20 (матчер) | нет | первая оценка (таблицы 2×22) и уточнение преобразования: карты 0x180053050, пары 0x180052450, подгонка подобия 0x180052960, метрики 0x180050730, перекрытие 0x18005a160 |

## Как воспроизвести

```sh
cd tools/algo
./cmp_study.sh /tmp/s12 12                     # exit 0; "study steps: 23, differing: 0", score/idx differ: 0
WARM=1 WARMSET=impostor ./cmp_study.sh /tmp/s12w 12
NEGATIVE=1 ./cmp_study.sh /tmp/s12n 12         # exit 1 (1 бит в blob порта)
```

Проверено 2026-09-29: N=12 — 23 шага, 0 расхождений, exit 0; WARM — 26 шагов, 0, exit 0; NEGATIVE — 1 шаг
расходится, exit 1.

## Остаток

- Замена при полной ёмкости (0x18005cba0/upstream select_capacity_replacement + replace_study, mval_b
  T+0x8d00) на датасете не достигается (capacity 50, галерея не дорастает до неё) — не сверено; ++ источника в
  replace_study убран по аналогии с append.
- 0x180027cf0 (повторный identify/study нового слота) и счётчики T+0x8e10/0x8e14: на датасете upd ≤ 1.

## Чекпоинты

- [1] Chicago templateStudy: обёртка 0x18000b330 → 0x18000d7e0: T=[0x1800943e8], feat=[0x1800943e0],
  ctx=0x1800943f0; `rc = 0x1800305a0(T, feat, ctx, &out{upd=0,x=-1}, 1)`; `*pUpdate = out.upd`;
  затем освобождение фич 0x18000ecf0(&feat). В upstream MR648 есть своя реализация study
  (runtime_study_print_data, enrollment_append_study/replace_study).
- [2] Протокол: algo_eval4/oracle_feat `STUDY=1` (уже был) — пробы по порядку записей (genuine 21:25,
  затем оставшиеся natural 21:34 = порядок съёмки), при совпадении своих templateStudy + repack при
  update>0, галерея накапливается; чужие (записи 55..154, после всех своих) — против итоговой
  галереи без study. Добавлены STRACE/STUDY_DIR в algo_eval4.c и STUDY/STRACE/STUDY_DIR в port_eval.c.
  N=12: DLL 23 study, 18 update (append, 12→30 подшаблонов), natural 15/17, genuine 8/25, чужих 0/85.
  Upstream study (runtime_study_print_data) без правок: blob ≠ DLL уже после 1-го study (rec 0).
- [3] Разбор: 0x1800305a0(T,feat,ctx,&out,1), тип 24: ctx+0x688 (any_strong) → 0x18005d3b0 →
  count<cap: append 0x18005e0d0 (источник SEL=ctx+0x648; new+0x124/0x128 копия источника, без ++);
  перестройка групп 0x18005cf70 только если T+0x87ec==0 или (==1 и новый idx==T+0x87e0);
  out>=1 → 0x1800320e0(T). Счётчик +0x124 (study_a) растит identify: 0x180029eaf для каждой записи
  со status≠0, 0x18002a3fd — для всех допущенных (rbp+0x330) на пути best27fb0. Расхождения
  upstream после 1-го study: перегруппировка (f2 2→7), study_a только у SEL.
- [4] port_study_fixes.patch v1 (SFIXES=1; нужны EFIXES+MFIXES+FFIXES; .port-sfixes/ с копиями
  match/enrollment/runtime .c/.h): hit_increments в результате identify (0x180029eaf, 0x18002a3fd),
  study_eligible = ctx+0x688 (после сброса 0x180029fb1), append без ++ источника, mval_c (+0xa7 =
  T+0x8d04) = счётчик append, перестройка групп по условию 0x18005d4b8. N=12: blob = DLL на
  12 из 23 study (до rec 40 вкл.), расхождение с rec 41. Скрипт сверки: scratchpad runp.sh.
- [5] v2: + замыкание отношений только по галерее 0x18005b9f0(T,0) после append (upstream
  synthesize_capacity_relations, допущено count < capacity при probe_relations NULL; распространяет
  group_state). N=12: blob = DLL на всех 23 study (rec 41 — sub[0] входил в группу через замыкание).
- [6] v3: + замыкание и при unpack (0x180032eb1). N=12: 23/23 blob = DLL, decisions 0/127,
  score/idx 0/127, round trip identical. N=12 WARM: 26 study, blob ≠ с rec 46 (sub[28].group_state:
  DLL 0, порт 1), score rec 48 (38 vs 50). Причина (гипотеза): append в DLL вызывает 0x18005c140
  режим 1 — копирует из ctx только отношения inl>=1, затем распространяет отношения нового узла по
  графу (синтез strength 2 без групп, как upstream propagate_capacity_relations режима 2) и
  переснимает несвязанные (0x180059030/0x180058170/0x180050730). Порт этого не делает, поэтому
  замыкание 0x18005b9f0 синтезирует связи нового узла с распространением групп.
- [7] v4: append по 0x18005c140 режим 1 (fresh-отношения: inl>=1 из identify, иначе -1/тождество;
  propagate_capacity_relations; переснятие непосещённых без метки -2) + метка -2 для всех
  оценённых геометрией записей (0x180029442). N=12 WARM: 26/26 blob = DLL; остался score rec 48
  (DLL 38, порт 50) при одинаковом blob — матчер на накопленной галерее.
- [8] WARM rec 48 idx 10: DLL после префильтра улучшает запись уточнением преобразования
  0x180052d20 (вызов 0x1800297db; условие: rec+0x10 < порога [rbp-0x18] или status/conf == 0,
  и feat+0x50 ∉ {1,2}); selector 223→228, agreement 195→200, +0x24/+0x28. В порту 0x180052d20
  нет (стадия 5, на свежей галерее не меняла результат). Вызывает 0x1800526f0, 0x180053050,
  0x180057a00, 0x18005a160 (= upstream feature_overlap_type24), 0x180050730. Разбираю.
- [9] Сводка на 2026-09-29: совпадает — N=12: blob 23/23, решения 0/127, score/idx 0/127;
  N=12 WARM: blob 26/26, решения 0/124, score/idx 1/124 (rec 48: DLL 38, порт 50, запись idx 10
  допущена в DLL после уточнения 0x180052d20). Исправлено: [4]–[7]. Текущее: перенос 0x180052d20.
  Контекст вызова (0x1800297a8): rcx=r12 (галерейный подшаблон T[idx]), rdx=&S (rbp+0x120:
  S+0 probe feat, S+8 → преобразование rbp+0x310, S+0x18 = число геометрии, S+0x20/0x28 — ленивые
  буферы 0x180053050), r8 = преобразование, r9 = запись, 5-й = T+8 (тип 24).
- [10] v5: перенесены первая оценка 0x180026ce0 (таблицы 22×2, params: +0/+4 = 0, +0x3c = 24,
  +0x50 = 0, порог 207) и уточнение преобразования 0x180052d20 (карты 0x180053050 r=2, пары
  0x180052450, подгонка подобия 0x180052960, метрики 0x180050730, перекрытие 0x18005a160).
  N=12 WARM: blob 26/26, score/idx 0/124.
