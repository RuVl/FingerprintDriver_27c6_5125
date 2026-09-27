# AlgoMilan — формат шаблона и путь идентификации

DLL: `win-driver/AlgoMilan.dll` (Milan_v_3.00.20), imagebase `0x180000000`.
Инструмент: `cd tools && ../.venv/bin/python -m fwre.dump AlgoMilan.dll func/range/... <addr>`.
Все адреса — VA (imagebase + RVA). Read-only, без железа.

---

## 0. Короткий вывод (детали ниже)

- **Формат шаблона enroll == формат, который ждёт identify.** Конвертация НЕ нужна.
  `enrolGetTemplate` возвращает готовый живой шаблон, пригодный для `identifytemplate`
  и для `identifyImage` без доп. шагов. (ТОЧНО)
- **`base+0xf810` (0x18000f810) — это НЕ study, а ДЕСТРУКТОР шаблона** (`freeTemplate`).
  Он освобождает все под-буферы записей, сами записи и объект, и обнуляет holder.
  Наш C-код зовёт его на галерейном шаблоне → шаблон освобождается → `identifytemplate`
  копирует освобождённый объект и разыменовывает висячие указатели записей → **падение
  на `[r14+0x148]`**. Это и есть причина краша. (ТОЧНО)
- Настоящий «study/finalize» происходит УЖЕ ВНУТРИ `enrolAddImage` (через
  `fingerFeatureRegister` 0x180017000), который заполняет счётчик `+0x1c` и записи `+0x28`.
  Экспорт `templateStudy` (0x18000e480) — это отдельная адаптивная до-регистрация
  (learning/update) поверх ГЛОБАЛЬНОГО контекста, к построению галереи отношения не имеет. (ТОЧНО)

---

## 1. Сигнатуры (MS x64 ABI) и возвраты

Соглашение об «указателе шаблона»: везде используется **holder** — 8-байтовый слот,
в котором лежит указатель на объект шаблона T: `*holder == T`. И enroll, и unpack
отдают именно holder; identify ждёт holder.

### enrolStartEx — 0x18000d9d0 (ТОЧНО)
```c
void* enrolStartEx(int* pMaxTemp);   // возвращает session (или NULL при ошибке)
```
- Проверяет глобальный флаг `PPLIB param initialized` ([rip+0xaf46f]==1), иначе `ppp_param_init` не вызван → NULL.
- Собирает flags (сдвиги/OR из глобалов версии), malloc(0x20)=session, malloc(8)=A, malloc(8)=B.
- `[session+8]=8; [session]=A;` вызывает конструктор `0x18000fbd0(B, *pMaxTemp, flags, 0)` → `[B]=T` (объект 0x8e08).
- `*pMaxTemp = [T+0x20]` (клампит вход до `0x32`), `[A]=B`.
- Цепочка: `session -> A -> B -> T`, т.е. `*(*session) == B` (holder), `*B == T`.
```
0x18000da8d: mov [rdi+8], r15d      ; session+8 = 8
0x18000da91: mov [rdi], r14         ; session -> A
0x18000daaa: call 0x18000fbd0       ; [B]=T
0x18000dabb: mov edx,[rcx+0x20]     ; [T+0x20] = nMaxTemp
0x18000dacc: mov [r14], rbx         ; A -> B
```

### enrolAddImage — 0x18000d590 (ТОЧНО)
```c
int enrolAddImage(void* session, GxImage* img, void* arg2, /*stack*/ void* pOut);
```
- Требует img->bits(+0xe)==8, img->channels(+0xf)==1, frame_count(+0x18)!=0, img->data([img])!=NULL.
- `getFeature` (0x180015150) из изображения → фичи в локале.
- `fingerFeatureRegister` (0x180017000) регистрирует фичи в объект T. Именно тут
  заполняются запись `T[+0x28+cur*8]` и счётчик `T[+0x1c]`.
- Возвраты: 0 OK; 0x81 bad param; 0x82 malloc; 0x80000001 getFeature; 0x83 register fail; 0x80000005 «шаблон полон».
```
0x18000d709: call 0x180015150       ; getFeature
0x18000d767: call 0x180017000       ; fingerFeatureRegister(rcx=&ret,rdx=feat,r8=T,r9=&img[0x1c])
```

### enrolGetTemplate — 0x18000d950 (ТОЧНО)
```c
int enrolGetTemplate(void* session, void** pOutHolder);   // 0 OK, 0x81 param
```
```
0x18000d971: mov rax,[rbx]      ; A         (rbx=session)
0x18000d979: mov rax,[rax]      ; B (=[A])  holder
0x18000d97c: mov [rdi], rax     ; *pOut = B
```
- Отдаёт **B — живой holder**, где `*B == T`. Объект уже полностью построен (см. F: +0x1c заполнен).
- ВНИМАНИЕ: B/T принадлежат сессии и будут освобождены `enrolFinish`. Копию хранить
  через `templatePack`, либо не звать `enrolFinish` пока шаблон нужен.

### enrolFinish — 0x18000d8e0 (ТОЧНО)
```c
int enrolFinish(void* session);   // 0 OK, 0x81 если session==NULL
```
- `0x18000f810(B)` (уничтожает T), затем `free(B)`, `free(A)`, `[session]=0`, `free(session)`.
- Т.е. **enrolFinish разрушает и шаблон тоже.** Забрать/запаковать шаблон до вызова.
```
0x18000d91f: call 0x18000f810      ; freeTemplate(B)
0x18000d927/d92f/d93e: call 0x18002ab90  ; free B, A, session
```

### identifytemplate — 0x18000e1b0 (ТОЧНО)
```c
int identifytemplate(void** ref, void** probe, void* unused/*NULL*/, int* pIdx);
// 0 OK; 0x81 param; 0x82 malloc
```
- `rbp = *ref` (объект-A), проверка `rcx,rdx,r9 != NULL` (третий арг r8 может быть NULL).
- malloc(0x8e08)=scratch; **memcpy(scratch, *probe, 0x8e08)** — МЕЛКАЯ копия объекта-B:
  указатели записей в `scratch+0x28` продолжают указывать на записи ОРИГИНАЛА `*probe`.
- Внешний цикл `i in [0, scratch[+0x1c])`: `rec = scratch[+0x28 + i*8]`; вызывает matcher
  `0x18001d1f0(&score, rec, rbp, 0, ..., &scratch_local)`.
- Если `score(+0x40) > 0` → `*pIdx = i` и выход; иначе `*pIdx = -1`.
```
0x18000e212: mov ecx, 0x8e08
0x18000e236: mov rdx,[rbx]         ; *probe
0x18000e24a: call 0x18002abc0      ; memcpy(scratch, *probe, 0x8e08)
0x18000e252: cmp [rsi+0x1c], ebx   ; i < scratch.nCurrent
0x18000e260: mov rdx,[rdi]         ; rec = scratch[+0x28 + i*8]
0x18000e27f: mov r8, rbp           ; = *ref
0x18000e287: call 0x18001d1f0      ; matcher(&score, rec, *ref)
0x18000e28c: cmp [rsp+0x40], r15d  ; score > 0 ?
```
Роли: **arg0(`ref`)** — целый шаблон, чьи записи перебирает matcher внутри (см. ниже
`rbx+0x1c`); **arg1(`probe`)** — шаблон, чьи записи по одной подаются matcher-у;
`pIdx` — индекс записи в `probe`, которая совпала (или -1).

### matcher — 0x18001d1f0 (ТОЧНО; в наших логах RVA 0x1d1f0, краш на 0x1d249)
```c
int matcher(void* pScoreOut, RECORD* recA, TEMPLATE* tplB, int flag,
            /*stack*/ int, void* ctxA, void* ctxB, void* extra);
```
Маппинг регистров: rcx→r15(out), rdx→r14(recA), r8→rbx(tplB), r9d→ebp(flag).
```
0x18001d244: call 0x18001a170       ; сравнение фич recA vs (по [tplB] типу)
0x18001d249: mov ecx,[r14+0x148]    ; <<< КРАШ: читает recA+0x148 (r14=recA=arg1)
0x18001d25c: movsxd rcx,[rbx]        ; тип шаблона tplB (arg2)
0x18001d288..2bc: цикл i<[rbx+0x1c]: rec=[rbx+rcx*8+0x28]; if [rec+0x114]==5 →0
0x18001d315: call 0x18001bb50        ; тип 9/10
0x18001d343: call 0x18001b010        ; прочие типы
```
Т.е. matcher-у нужно: **arg1 = одиночная ЗАПИСЬ** (с полем `+0x148`), **arg2 = целый
ОБЪЕКТ-шаблон** (с `+0x1c` счётчик и `+0x28` массив записей). Краш `[r14+0x148]`
происходит из-за того, что `recA` (= запись из `*probe`-галереи) указывает на
освобождённую память (её убил `0x18000f810`).

### templateStudy — 0x18000e480 (ТОЧНО — это НЕ то, что нужно для галереи)
```c
int templateStudy(int* pOut);   // работает над ГЛОБАЛЬНЫМ шаблоном [rip+0xa9080]
```
- Если глобальный контекст задан → `0x18001f700(...)` (алг. слияния/обновления),
  печатает `alg_ret, nUpdate, nReplaceIdx`, пишет `*pOut = nUpdate`, чистит глобал через `0x18000fa90`.
- Это адаптивное дообучение (update-after-verify), НЕ финализация галерейного шаблона.

### templateGetPackedSize — 0x18000e380 / templatePack — 0x18000e3d0 / templateUnPack — 0x18000e590 (вероятно по деталям, ТОЧНО по смыслу)
```c
int  templateGetPackedSize(void** holder);                 // ->0x180023c50(T); size или 0
int  templatePack(void** holder, void* dst);               // ->EncodeFingerTemplate 0x180022930; 0 OK,0x80 enc,0x81 param
int  templateUnPack(void* ctx, int length, void* blob, void** pOutHolder); // ->DecodeFingerTemplate 0x1800221a0; 0 OK
```
- Pack: `T=[holder]`; `sz=0x180023c50(T)`; `0x180022930(&ctx,dst,&sz,T)`.
- UnPack: malloc(8)=B; `0x1800221a0(&in,&len,&outObj,blob)`; `[B]=outObj; *pOutHolder=B`.
  → отдаёт **живой holder** того же формата, что enrolGetTemplate. Это правильный способ
  восстановить сохранённый галерейный шаблон.

### templateDelete — 0x18000e2f0 (ТОЧНО)
```c
int templateDelete(void** holder);   // 0/void
```
- `local=[holder]; 0x18000f810(&local); free(holder)`. Т.е. правильное освобождение
  standalone-шаблона (unpack-нутого). Внутри корректно зовёт деструктор `0x18000f810`
  с ЛОКАЛЬНЫМ holder — не порти оригинал.

### getTemplateInfo (0x18000bcd0) и InitIdentifyImage (0x18000ba40) — ЗАГЛУШКИ (ТОЧНО)
Обе — тонкие враперы, которые просто `mov eax, 0x83; ret` (GF_NOSUPPORT). В этой сборке
DLL **не реализованы**. Значит «InitIdentifyImage» как отдельного пути НЕТ.
```
0x18000ba54: mov eax,0x83 ; ret     ; InitIdentifyImage
0x18000bce4: mov eax,0x83 ; ret     ; getTemplateInfo
```

### identifyImage (wrapper 0x18000ba60 → внутр. 0x18000de30) — РЕАЛИЗОВАН (ТОЧНО)
```c
int identifyImage(GxImage* probeImg, void* param, void** candidates, int nCand,
                  /*stack*/ int* pScoreOut, int* pIdxOut, void* featScratch,
                  int flag, void* g, byte b);
```
- Проверяет `candidates[0]` и `candidates[0][0] (pFingerTemplate)` != NULL.
- `getFeature`(0x180015150) из probe-изображения → глобальный фича-контекст.
- Цикл `i in [0,nCand)`: `T_i = candidates[i][0]`; вызывает **тот же matcher `0x18001d1f0`**:
  `matcher(&score, [rip+0xa9506]=probeFeatCtx, T_i, flag,...)`.
  Тут arg1=контекст фич пробы (валидная запись с `+0x148`), arg2=галерейный ОБЪЕКТ T_i.
- Печатает `maxTempNum=[T+0x20], curTempNum=[T+0x1c]`. Пишет `*pIdxOut=i`, `*pScoreOut=score`; при отсутствии — idx=-1, score=-1.
```
0x18000e024: mov rdi,[rax]          ; T_i = candidates[i]->pFingerTemplate
0x18000e059: call 0x18001d1f0       ; matcher(&score, probeFeatCtx, T_i)
0x18000e066: mov eax,[rdi+0x1c]     ; curTempNum
0x18000e069: mov r9d,[rdi+0x20]     ; maxTempNum
```

---

## 2. Раскладка объекта шаблона T (size = 0x8e08 = 36360 байт)

Аллокация: `0x18000fbd0 → 0x18006c27c(0x8e08)` (зануляется). ТОЧНО по размеру.

```
 off      тип         значение / роль                         доказательство
 -----    ---------   -------------------------------------   -----------------------------
 +0x00    dword       ТИП/версия шаблона (switch 9,10,...)     matcher [rbx]; study [rbx]; 0x18001a170 [rcx]
 +0x04    dword       sensor/params (r12d при register)        register [r8+4]
 +0x08    dword       sensor/params (r13d)                     register [r8+8]
 +0x0c    dword       копия поля                               register [r8+0xc]
 +0x1c    dword       nCurrent — ЧИСЛО ЗАПИСЕЙ (текущее)       register cmp [r8+0x1c]<max; ИМЕННО ЭТО перебирают
                                                               matcher (rbx+0x1c) и identifytemplate (rsi+0x1c)
 +0x20    dword       nMax — ёмкость (nMaxTemp)                enrolStart [T+0x20]; register max; деструктор итерирует
 +0x24    dword       поле                                     register [r8+0x24]
 +0x28    RECORD*[]   массив указателей на записи              rec_i = [T + 0x28 + i*8]  (register/identify/matcher/destructor)
 +0x640   dword       флаг для алг. study                      0x18001f700 [r8+0x640]
 +0x87d0  ptr         accessor                                 0x18000fbb0 lea [rcx+0x87d0]
 +0x88b4  char*       строка версии алгоритма                  0x18000fbc0; enrolStart "Algorithm version %s"
 +0x88f4  ptr         accessor                                 0x18000fba0 lea [rcx+0x88f4]
 +0x8d10  ptr[20]     вспом. буферы (stitch/cache)             деструктор: 0x14 шт через 0x18000fa90
 ...
 +0x8e08              конец объекта
```

### Раскладка ЗАПИСИ (RECORD, элемент массива +0x28; размер > 0x158)
```
 off      роль                                    доказательство
 -----    ------------------------------------    ------------------------------------
 +0x08    под-буфер (фичи)                        деструктор free [rec+8]  (0x18002ba50)
 +0x10    под-буфер                               free [rec+0x10]
 +0x18    под-буфер                               free [rec+0x18]
 +0x20    под-буфер                               free [rec+0x20]
 +0xf8    под-буфер                               free [rec+0xf8]  (0x180068638)
 +0x100   dword флаг                              study 0x18000fa6a пишет 0
 +0x104   dword индекс                            0x18000f9de читает при replace
 +0x114   dword состояние (==5 → сброс в 0)       matcher 0x18001d2a7/0x18001d2b0
 +0x130   под-буфер                               free [rec+0x130]
 +0x148   dword/ptr данные фич                    matcher 0x18001d249 [r14+0x148]  <<< САЙТ КРАША
 +0x158   под-буфер                               free [rec+0x158]
```

---

## 3. enroll-формат vs identify-формат — СОВПАДАЮТ (ТОЧНО)

- `enrolGetTemplate` отдаёт holder B, где `*B == T` — полноценный объект 0x8e08 с уже
  заполненными `+0x1c`/`+0x28` (их пишет `fingerFeatureRegister` внутри `enrolAddImage`).
- `identifytemplate` ждёт ровно holder (`*arg == T`) и читает `T[+0x1c]`, `T[+0x28]`,
  запись `+0x148` — то же самое, что производит enroll.
- `templatePack`→`templateUnPack` даёт holder идентичного формата.
- **Никакая конвертация не нужна.** Проблема НЕ в формате, а в том, что галерейный
  шаблон разрушается `0x18000f810` до сравнения.

---

## 4. Правильный путь сравнения штатного движка

Два реализованных варианта (оба через один matcher `0x18001d1f0`):

1. **identifyImage(проба-ИЗОБРАЖЕНИЕ, candidates[])** — основной путь верификации:
   getFeature(проба) → matcher(фичи_пробы, T_кандидата) по каждому кандидату.
   `candidates` — массив holder'ов (`candidates[i][0] == T_i`). Возвращает idx кандидата
   и score. (`InitIdentifyImage`/`getTemplateInfo` — заглушки, не нужны.)

2. **identifytemplate(ref_holder, probe_holder, NULL, &idx)** — шаблон×шаблон:
   перебирает записи `probe` (arg1), каждую матчит против всего `ref` (arg0);
   `idx` = индекс записи в `probe`, что совпала, или -1.

Первый аргумент identifytemplate — «эталон, чьи записи перебираются matcher-ом внутри»
(arg0/`ref`); второй — «шаблон, чьи записи по одной пробуются» (arg1/`probe`).
Для симметричного матча важно лишь, чтобы ОБА были валидными живыми шаблонами.

---

## 5. Что такое `base+0xf810` (0x18000f810) — ДЕСТРУКТОР, не study (ТОЧНО)

```c
int freeTemplate(void** holder);  // 0 OK; 0x80000002 если holder/ *holder == NULL
```
- `T=[holder]`. Цикл `i in [0, T[+0x20])`: `rec=[T+0x28+i*8]`; освобождает под-буферы
  `+8,+0x10,+0x18,+0x20,+0xf8,+0x130,+0x158`, затем `free(rec)`, `[slot]=0`.
- Цикл 20× по `T+0x8d10` через `0x18000fa90` (освобождение вспом. буферов).
- `free(T)`, `[holder]=0`.
- Зовётся из `enrolFinish` и `templateDelete` — там это уместно. **В нашем C он ошибочно
  применён к галерейному шаблону как «study» → шаблон уничтожен → identifytemplate
  разыменовывает висячий `rec+0x148` → SIGSEGV на 0x1d249.**

Доказательства: `templateDelete` (0x18000e2f0) явно вызывает `0x18000f810` затем
`free(holder)`; `enrolFinish` (0x18000d8e0) вызывает `0x18000f810(B)` перед освобождением.

---

## Итог для C

**Причина падения:** вызов `0x18000f810` (freeTemplate) на галерейном шаблоне
уничтожает его записи; `identifytemplate` затем читает висячий `rec+0x148`.

**Как правильно построить галерейный шаблон:**
```
enrolStartEx(&maxt=16)              // maxt клампится до 0x32
for each PREPROCESSED кадр:
    enrolAddImage(session, img, ...)   // тут же выполняется finalize (fingerFeatureRegister)
enrolGetTemplate(session, &tpl)     // tpl — ЖИВОЙ holder, готов к сравнению; НЕ трогать 0xf810
// хранить/переносить:
sz = templateGetPackedSize(tpl); buf = malloc(sz); templatePack(tpl, buf);
// освобождать сессию только ПОСЛЕ pack (enrolFinish уничтожит tpl):
enrolFinish(session);
// восстановить перед сравнением:
templateUnPack(ctx, len, buf, &galleryHolder);
```

**Как корректно сравнивать (без падения):**
```
// оба аргумента — валидные живые holder'ы (*holder == T); третий = NULL
int idx = -1;
int rc = identifytemplate(&refHolder, &probeHolder, NULL, &idx);
// idx>=0 → совпадение (индекс записи probe); idx==-1 → нет
// НЕ вызывать 0x18000f810 ни на одном из шаблонов до/во время сравнения.
// освобождать standalone-шаблоны через templateDelete(&holder), а сессию — enrolFinish.
```

Альтернатива, ближе к штатному движку: держать пробу как ИЗОБРАЖЕНИЕ и звать
`identifyImage(probeImg, param, candidates[], n, &score, &idx, featScratch, flag, ...)`,
где `candidates[i]` — holder'ы enroll-шаблонов. Тот же matcher, меньше риска перепутать
роли записей. (`InitIdentifyImage`/`getTemplateInfo` в этой DLL — заглушки, не использовать.)
