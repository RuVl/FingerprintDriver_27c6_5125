# 81. Протокол регистрации (EngineAdapter + AlgoChicago)

Метки: ТОЧНО (прочитано в коде) / вероятно / гипотеза.
EA = EngineAdapter.dll, AC = AlgoChicago.dll (imagebase 0x180000000). Сборка EA — debug-подобная,
все решения логируются (строки приведены как опоры для поиска).

## 0. Конфиг EA (глобальная структура cfg = 0x1801173d0, геттер 0x1800295c0)

Значения читаются из `HKLM\Software\Goodix\FP\<Name>` в `init_config` 0x180026020; если ключа нет —
остаются статические значения из .data (таблица). INF ключей не создаёт. Тип сенсора
`G_18012052c` = 0x0c (chicagoHS → AlgoChicago.dll; выбор DLL 0x180052f00: 2 milanG, 3 milanL,
0xc chicagoHS, 0xe chicagoT). Для 0xc в 0x18003d770 переопределяется только match_retry_max_count;
спец-значения GF3256 (samples 14, tip_start 7, …) относятся к типу 2. (ТОЧНО по коду; реестр
ноутбука Honor не видели — гипотеза, что OEM ничего не задавал.)

| смещ. | имя (лог «CONFIG DATA») | по умолч. |
|---|---|---|
| 0x429 | samples_num_per_template | **12** |
| 0x42c | min_image_quality | **25** |
| 0x42d | min_image_coverage | **65** |
| 0x42e | max_overlay_ratio | **30** |
| 0x42f | max_preoverlay_ratio | **20** |
| 0x431 | liveness_switch | 0 |
| 0x45b | touch_rich_tip_switch | 0 |
| 0x45c | max_enroll_overlay_tip_count | **8** |
| 0x460 / 0x464 | enroll_overlay_tip_start / _end | **5 / 12** |
| 0x468 | enroll_overlay_tip_step | **2** |
| 0x46c[12] | enroll_overlay_tip_info | 4,1,3,2 ×3 |
| 0x48e / 0x494 | match_retry_switch / one_key_boot_retry_switch | 1 / 1 |
| 0x4a4 | choose_enroll_img_switch | 1 |
| 0x4a5 | choose_less_overlay_switch | 1 |
| 0x4a8 | choose_enroll_img_num | **2** |
| 0x4ac | choose_enroll_img_quality_threshold | **2** |
| 0x4b0 / 0x4b4 | del_enroll_img_overlay / _preoverlay_threshold | **80 / 70** |

WINBIO_REJECT_DETAIL: 1 TOO_HIGH, 2 TOO_LOW, 3 TOO_LEFT, 4 TOO_RIGHT, 7 POOR_QUALITY, 9 TOO_SHORT,
10 MERGE_FAILURE. HRESULT: 0x80098008 WINBIO_E_BAD_CAPTURE, 0x90001 WINBIO_I_MORE_DATA.
EngineAdapterQueryExtendedInfo (0x18003c950) сообщает Windows EnrollmentRequirements:
GeneralSamples = Center = samples_num_per_template = 12 (ТОЧНО).

## 1. Критерий завершения и прогресс (ТОЧНО)

Счётчики EA (глобальные, обнуляются в enrolStart-обёртке 0x18003b5c0):

| глобал | имя в логе | смысл |
|---|---|---|
| 0x18012f49c | touched | успешных enrolAddImage |
| 0x18012f4a0 | enrolled (enrolled_no_tip) | принятых касаний (без подсказки) |
| 0x18012336c | used | кадров в шаблоне (+add, −delete) |
| 0x18012f498 | tipped | всего отказов-подсказок |
| 0x180123369 | cont_tipped_num | подсказок подряд |
| 0x180123368 | last_tipped_num | значение touched при последней подсказке |
| 0x18012336a / 0x18012336b | tip index / last | индекс в tip_info (для 0xc по кругу 0..3) |

- Сессия AC (0x20 байт): `+0 → holder → template`, `+8 int16 max`, `+0xa int16 count`,
  `+0xc progress`, `+0x10 preoverlay`, `+0x14 overlay`.
- AC `enrolStartEx` ставит `max = 8`; **EA сразу перезаписывает** `sess->max = samples_num_per_template
  + max_enroll_overlay_tip_count = 12 + 8 = 20` (0x18003b6cf..6e6).
- AC `enrolAddImage` при `count >= max` ничего не добавляет и возвращает 0 (progress = 100).
  **Отсюда «всегда 8 записей» в офлайн-прогоне**: харнесс не делал перезапись max. Исправление:
  после enrolStart записать `*(int16_t*)(sess+8) = 20`.
- `progress = min(100, count*100/max)` (AC 0x18000ca74, так же в enrolDeleteImage).
- GetEnrollmentStatus (0x180045690): `if (count < max && enrolled < samples_num(12)) return 0x90001;`
  иначе enrolGetTemplate → populate → getTemplateInfo (только для лога) → S_OK = готово.
  Лог: `enroll progress %d, used:max %d:%d, enrolled_no_tip:max %d:%d`.
- Итог: регистрация завершается при **12 принятых касаниях** или **20 кадрах в шаблоне**
  (12 + до 8 «подсказочных», которые тоже остаются в шаблоне). Порог покрытия/площади нет.
- Шаблон AC: `nMaxTemp = min(50, лимит типа)`; для ppp-кода 10 (64×80) лимит 50 → ёмкость 50 ≥ 20.

## 2. Отказы кадров (порядок проверок)

A. AcceptSampleData 0x180033c20 (purpose == 4 = ENROLL), ТОЧНО:
1. preprocessor rc ≠ 0 → BAD_CAPTURE, detail 7 (`preprocessor failed, ret`).
2. choose_enroll_img (п.3) — может заменить кадр на лучший.
3. `coverage < 65` → BAD_CAPTURE, detail **9 TOO_SHORT**; иначе `quality < 25` → detail **7**.
   (touch_rich_tip_switch = 0 → подсказки по TouchFlag не используются.)

B. _AdapterMergeFeatureSetWithEnrollment 0x180044b00 (UpdateEnrollment 0x180039e60), ТОЧНО:
1. `rc = enrolAddImage(...)`; лог `algo_ret for enroll add 0x%x, quality %d, coverage %d, preoverlay %d, overlay %d`.
2. `rc == 0x83` → enrolFinish + DiscardEnrollment, BAD_CAPTURE: **регистрация сброшена**.
3. `rc ≠ 0` → BAD_CAPTURE, detail **10 MERGE_FAILURE** (кадр не добавлен).
4. `rc == 0`: touched++, used++. Проверка «тот же участок» — **только по выходам enrolAddImage**
   (overlay/preoverlay), не через identifytemplate:
   `if (enrolled+1 ∈ [5,12] && (overlay > 30 || preoverlay > 20))` — «слишком похоже»;
   если `tipped < 8 && cont_tipped < 2` → BAD_CAPTURE, detail = `tip_info[tip_idx]`
   (4,1,3,2 = RIGHT,HIGH,LEFT,LOW), cont_tipped++, tipped++, last_tipped = touched.
   **Кадр при этом остаётся в шаблоне** (count растёт, enrolled — нет). Иначе (лимиты исчерпаны)
   кадр принимается как обычный.
5. Принят: если предыдущий кадр был подсказкой → tip_idx = (tip_idx+1) % 4; cont_tipped = 0; enrolled++.
   Затем UpdateEnrollment зовёт GetEnrollmentStatus (п.1).

C. enrolDeleteImage (удаляет последний добавленный кадр, count−−) — только в механизме
choose_less_overlay (switch = 1), ТОЧНО:
- 1-я подсказка подряд и `overlay > 80 && preoverlay > 70`: кадр копируется в backup
  (0x180043e70) и удаляется из шаблона.
- 2-я подсказка подряд: если снова >80/>70 и backup есть (0x1800441b0) — из двух оставляется
  кадр с меньшим overlay (при равенстве — с большим quality); иначе (0x180044880) backup
  возвращается в шаблон enrolAddImage'ом.
- Принятый кадр сразу после подсказки: backup (если есть) тоже возвращается (0x180044880).

D. CheckForDuplicate 0x18003a410 → 0x180045fd0: identifytemplate (шаблон↔шаблон) против
сохранённых записей — проверка «палец уже зарегистрирован», не покадровая.

## 3. choose_enroll_img (ТОЧНО по EA, вероятно по драйверу)

- 0x180041d40 вызывается из AcceptSampleData для ENROLL, если preprocessor OK,
  choose_enroll_img_switch = 1 и retry switch = 1 (для типа 0xc всегда: 0x180040010).
- Для idx = 1 .. choose_enroll_img_num−1 (т.е. **один доп. кадр**): IOCTL 0x442140
  {retry_type, retry_switch, idx} (0x18003f9f0) — драйвер отдаёт ещё один кадр того же касания
  (в ответе result и finger_down; гипотеза: новый захват, пока палец лежит). Доп. кадр
  препроцессится (purpose 1); при ошибке — пропуск.
- Выбор (T = 2, q = quality, c = coverage): новый кадр заменяет текущий, если
  `new.q > cur.q + T` || `(|new.q − cur.q| < T && new.c > cur.c)` || `(new.q > cur.q && new.c >= cur.c)`.
  Копируются raw16, 8-бит кадр, qcov и cbuf (0x4c98). Пороги п.2A применяются к выбранному.
- 0x180041700 — другой механизм: повторный захват при провале препроцессора в verify
  (match_retry, до match_retry_max_count), к регистрации не относится.

## 4. Сигнатуры AlgoChicago (ТОЧНО, если не оговорено)

- `void *enrolStart(void)` = `enrolStartEx(&50)`; `enrolStartEx(int *n)`: n ≤ 50, создаёт шаблон
  `newTemp(n, flags)`, пишет обратно `*n = nMaxTemp`; сессия `max = 8`, count = 0.
- `int enrolAddImage(sess, gx_image *img, void *cbuf, uint16 *raw, uint8 liveness, int qcov[2])`
  - img: `+0 data`, `+8/+0xa int16 w/h`, `+0xe bits == 8`, `+0xf channels == 1`, `+0x18 frame_count != 0`,
    `+0x1c sensor_type`, `+0x28 quality (u8)`, `+0x29 coverage (u8)`.
  - raw и liveness в AC не используются.
  - Выходы: `qcov[0] = img[0x29]` (coverage), `qcov[1] = img[0x28]` (quality);
    `sess+0x10 preoverlay = 100 − (r>>24)`, `sess+0x14 overlay = 100 − (r & 0xff)`, где r —
    результат fingerFeatureRegister 0x180018fa0; count++, progress.
    overlay — % площади нового кадра, уже покрытой шаблоном; preoverlay — перекрытие с предыдущим
    кадром (вероятно). Для 1-го кадра r = 0x64000000 → overlay 100, preoverlay 0; нет совпадений →
    0x64000064 → 0/0.
  - rc: 0 ok (в т.ч. «шаблон полон, пропущено»); 0x81 bad param/формат; 0x82 malloc;
    0x80000001 getFeature fail; **0x83** = fingerFeatureRegister fail (внутри: 0x80000005
    шаблон ≥ nMaxTemp, 0x80000006 нет признаков, 0x80000001 для типов 1/8). При любой ошибке
    шаблон не меняется (инкремент — после проверок), повтор безопасен.
- `int enrolDeleteImage(sess)`: удаляет последний кадр шаблона (0x18000ec10), count−−,
  пересчёт progress; rc 0 (0x81 при NULL).
- `int enrolGetTemplate(sess, void **tpl)`: `*tpl = *sess->holder` (без копии; освобождает enrolFinish).
- `int getTemplateInfo(tpl, int *a, int *b, int *c, int *d)`: `a = t[0x1c]`, `b = t[0x20]`,
  `c = t[0x28]` (nMaxTemp), `d = t[0x24]` (число кадров в шаблоне). Лог EA
  `kp %d / %d, tpl %d / %d` = `b/a, d/c`. kp — константы типа сенсора из newTemp
  (для типа 10: 120/120; смысл — лимит keypoints, вероятно). EA используется только для лога
  и `ctx+0x28 = d`.
- `void getLastMatchedFingerTemplatesNum(int *a, int *b)`: копия двух глобалов, пишет их
  identifyImage (0x18000d440/451); EA не импортирует.

## 5. Цикл регистрации для libfprint

```c
/* константы из cfg по умолчанию */
#define N_ACCEPT 12   /* samples_num_per_template = число стадий */
#define N_TIPMAX 8    /* max_enroll_overlay_tip_count */
#define SESS_MAX (N_ACCEPT + N_TIPMAX)   /* 20 */
static const int tip_info[4] = {4, 1, 3, 2};  /* RIGHT, HIGH, LEFT, LOW */

sess = enrolStart(); *(int16_t *)((char *)sess + 8) = SESS_MAX;
touched = enrolled = tipped = cont_tip = last_tip = tip_idx = 0; backup = NULL;

for (;;) {                                   /* одно касание */
  frame = capture(); if (pp(frame) != 0) { retry(POOR_QUALITY); continue; }
  if (finger_still_down) {                   /* choose_enroll_img: 1 доп. кадр */
    f2 = capture(); if (pp(f2) == 0 && better(f2, frame, 2)) frame = f2;
  }
  if (frame.cov < 65) { retry(TOO_SHORT /* сдвиньте/приложите полнее */); continue; }
  if (frame.q   < 25) { retry(POOR_QUALITY); continue; }
  rc = enrolAddImage(sess, &img, cbuf, raw, 0, qcov);
  if (rc != 0) { retry(GENERAL /* MERGE_FAILURE */); continue; }  /* EA при 0x83 сбрасывает всё */
  touched++;
  ov = sess->overlay; pov = sess->preoverlay;
  if (enrolled + 1 >= 5 && enrolled + 1 <= 12 && (ov > 30 || pov > 20)
      && tipped < N_TIPMAX && cont_tip < 2) {
    cont_tip++; tipped++; last_tip = touched;
    if (cont_tip == 1 && ov > 80 && pov > 70) { backup = frame; enrolDeleteImage(sess); }
    else if (cont_tip == 2 && backup) {
      if (ov > 80 && pov > 70) keep_less_overlay(sess, frame, backup); /* 0x1800441b0 */
      else enrolAddImage(sess, backup...);                             /* 0x180044880 */
      backup = NULL;
    }
    retry(CENTER_FINGER /* «сдвиньте палец», tip_info[tip_idx] */);
    continue;                                /* кадр (обычно) остался в шаблоне */
  }
  if (last_tip && last_tip + 1 == touched) {
    if (backup) { enrolAddImage(sess, backup...); backup = NULL; }
    tip_idx = (tip_idx + 1) % 4;
  }
  cont_tip = 0; enrolled++;
  progress(enrolled /* из 12 */);
  if (sess->count >= SESS_MAX || enrolled >= N_ACCEPT) break;
}
enrolGetTemplate(sess, &tpl); templatePack(tpl, ...); enrolFinish(sess);
```

- Прогресс для libfprint: `nr_enroll_stages = 12`, стадия = enrolled. Подсказка —
  `FP_DEVICE_RETRY_CENTER_FINGER` (кадр в шаблон попал, но стадия не засчитана); coverage < 65 —
  `FP_DEVICE_RETRY_TOO_SHORT`/`CENTER_FINGER`; quality < 25 — `FP_DEVICE_RETRY_GENERAL`.
- Упрощение без backup-логики (вероятно, почти без потерь): на подсказке ничего не удалять.
- Verify после совпадения (0x180042d50, вероятно): если identifyImage вернул флаг обновления
  (> 0) и rc ≠ 0x84 (liveness) → `templateStudy(&nUpdate)`; `nUpdate >= 2` →
  `identifyUpdate(tpl)` (замена записи); `nUpdate > 0` → перепаковать и сохранить шаблон
  (лог «Learning occurs»). В verify пороги q/c (25/65) проверяются только при cfg[0x48c] ≠ 0.
