# AlgoMilan — NULL-разыменование в препроцессинге (50-preprocess-crash)

DLL: `win-driver/AlgoMilan.dll` (Milan_v_3.00.20), imagebase `0x180000000`. Все адреса — VA.
Уровни: **ТОЧНО** (прочитано в дизасме), *вероятно* (вывод из кода), _гипотеза_.

## TL;DR

**NULL-указатель = `aux` — 2-й аргумент `preprocessor` (регистр `rdx`), который мы передаём как `NULL`.**
`preprocessor` (0x18000efa0) НЕ разыменовывает aux сам, а прокидывает его в ядро
`0x18002dea0` как 10-й аргумент; ядро на «путях с f7≠20» вызывает функцию-скоринга
`0x18002c060`, передавая ей `aux` в `rcx`, и та первой же инструкцией делает
`mov eax,[rcx]` → чтение `[aux+0]` → SIGSEGV при aux==0.
Для профиля idx10 (f7=10) этот путь берётся всегда. Фикс: передать вместо NULL
указатель на обнулённую маленькую control-структуру (читается только `[aux+0]`, dword-флаг).

---

## 0. Цепочка вызова (ТОЧНО)

```
preprocessor (0x18000efa0)
   └─ call 0x18002dea0           @0x18000f0c3   (ядро calib/apply)
         └─ call 0x18002c060     @0x18002e2c8   (скоринг quality/coverage/decision)
               mov eax,[rcx]     @0x18002c0de   ← SIGSEGV, rcx=aux=0
```

`callers 0x18002c060` → единственный вызов из `0x18002dea0 @0x18002e2c8` (ТОЧНО).
Т.е. краш-функция вызывается ТОЛЬКО ядром, не напрямую из preprocessor.

### Поправка к 10-preprocess §0 (адреса глобалов)

Резолв rip-относительных ссылок из `preprocessor` (next_ip + disp), ТОЧНО:

| ссылка | адрес | что |
|---|---|---|
| `lea r9,[rip+0xaddba]` @0f096 | **0x1800BCE50** | CALIDATA/CTX base (r9→ядро→rdi) |
| `cmp [rip+0xade5d],1` @0efef | 0x1800BCE4C | PARAM_INIT |
| `cmp [rip+0xde29b],1` @0f041 | 0x1800ED2DC | CALIBRATED gate |
| `lea rax,[rip+0xde22c]` @0f0b0 | 0x1800ED2DC | ctx-ptr (тот же gate) |
| `mov [rip+...],ecx` @0bce44/40/48/34 | 0x1800BCE2A..48 | ppp-профиль (COL/ROW/f7/flags) |

**В 10-preprocess эти адреса записаны со сдвигом −0x10000** (там `0x18001CE50` вместо
`0x1800BCE50`, `0x18001D2DC` вместо `0x1800ED2DC` и т.д.). Значение/смысл верны, только
числовые VA нужно поправить. **Runtime подтверждает: r9=rdi=0x1800BCE50** — это статический
глобал в образе DLL (offset 0xBCE50), НЕ куча. Значит гипотеза (в) неверна — глобал
калибровки всегда ненулевой.

---

## 1. Как `preprocessor` формирует аргументы для ядра `0x18002dea0`

Пролог: `push rsi/rdi/r14` (0x18) + `sub rsp,0x80` → сдвиг 0x98. Раскладка входа
(ms_abi, wrapper 0x18000b810 прокидывает 1:1, проверено @0b856-b830):
`rcx=src, rdx=aux, r8=in_bytes, r9=dst, [rsp+0xc0]=qcov, [rsp+0xd0]=m1, [rsp+0xd8]=m2`.

Начальная перетасовка (ТОЧНО):
```
@0efbb r14 = [rsp+0xc0]      ; qcov
@0efc5 rsi = r9             ; dst
@0efc8 r9  = rdx            ; aux           ← aux уходит в r9
@0efcb r10 = r8            ; in_bytes
@0efd3 rdx = rcx            ; src
```
Гейты: `test rcx/rsi → 0x81`; `cmp PARAM_INIT,1 → 0x80`; `cmp CALIBRATED,1 → 0x80`.

Упаковка config (ecx) из ppp-глобалов + `[src+0x14]`:
```
@0f007 ecx=[COL 0xBCE44]; @0f00d r8d=[src+0x14]; <<9; |=[ROW 0xBCE40]; <<0xb;
|=[f7 0xBCE48]; <<2; |=[PIXEL_CANCEL 0xBCE34]; +ecx(<<1); |=…; |=…
```
Раскладка бит config (декод в ядре, см. §2): `COL=config>>23`, `ROW=(config>>14)&0x1ff`,
`f7=(config>>3)&0x3f`, флаги в битах 0..2. Для idx10: COL=80, ROW=64, f7=10.

Подготовка стековых аргументов ядра (ТОЧНО):
```
@0f061 [rsp+0x58]=0
@0f066 [rsp+0x50]=r10   ; in_bytes
@0f06b [rsp+0x48]=r9    ; aux            ← aux кладётся в стек-слот
@0f070 [rsp+0x40]=r14   ; qcov
@0f082 rbx=[r14+4] ; @0f08a [rsp+0x38]=rbx   ; &qcov.coverage
@0f08f r9 = &CALIDATA (0x1800BCE50)     ; r9→rdi ядра (глобал)
@0f0a2 rdx = [rdx] = src->data          ; 16-бит вход
@0f0a9 rax = ctx(0x1800ED2DC); [rsp+0x28]=rax
@0f0ba [rsp+0x20]=ecx (config)
@0f0be rcx = &[rsp+0x60]
@0f0c3 call 0x18002dea0
```
Итог — ядро получает: `rcx=&локал`, `rdx=src->data`, `r8=len`, `r9=&CALIDATA`, и
**стек-аргументы**: arg5=config, arg6=ctx, arg7=глоб, arg8=&coverage, arg9=qcov,
**arg10=aux ([rsp+0x48])**, arg11=in_bytes, arg12=0.

---

## 2. Ядро `0x18002dea0`: где аргументы становятся аргументами `0x18002c060`

Пролог: `push` ×8 (0x40); `lea rbp,[rsp-0x68]`; `sub rsp,0x168` → `rbp = rsp_entry-0xA8`,
значит `[rbp+X] = вход[rsp+(X-0x80)]`. Отображение стек-аргументов ядра:

| ядро | [rbp+..] | источник (preproc) | смысл |
|---|---|---|---|
| arg5 | 0xD0 → r13d | config | packed config |
| arg6 | 0xD8 → r15 | ctx 0x1800ED2DC | режим-ctx |
| arg7 | 0xE0 | глоб | — |
| arg8 | 0xE8 → [rbp-0x60] | &qcov.coverage | out |
| arg9 | 0xF0 → [rbp-0x68] | qcov | out |
| **arg10** | **0xF8** | **aux** | **← наш NULL** |
| arg11 | 0x100 → [rbp-0x78] | in_bytes | — |
| arg12 | 0x108 → [rbp-0x58] | 0 | — |

`grep [rbp+0xf8]` по всему ядру → **единственная ссылка @0x18002e2a9** (загрузка в rcx
перед крашем). Т.е. aux в ядре нигде больше не используется, только прокидывается дальше.

Декод config (ТОЧНО, @0df0b-df87): `eax=config>>0x17=COL`, `ecx=(config>>0xe)&0x1ff=ROW`,
`r12d=ROW*COL`, `[rsp+0x6c]=(config>>3)&0x3f = f7`, флаги (config&1),(>>1)&1,(>>2)&1.
Ядро строит локальную param-структуру `[rbp-0x18]`:
```
[rbp-0x18]=COL  [rbp-0x14]=ROW  [rbp-0x10]=ROW*COL  [rbp-0xc]=isfloat
[rbp-8]=(cfg>>1)&1  [rbp-4]=(cfg>>2)&1  [rbp+0]=f7
```

**Гейт пути к крашу (ТОЧНО):**
```
@0e141 r13d=0
@0e144 cmp [rsp+0x6c],0x14         ; f7 == 20 ?
@0e149 je  …                        ; если f7==20 → r13d остаётся 0 (краш-путь пропущен)
@0e14f r13d=1                        ; иначе флаг=1
…
@0e26d test r13d,r13d ; je 0x18002e2d7 (пропуск)   ; r13 — callee-saved, переживает call'ы
@0e272… формирование аргументов
@0e2c8 call 0x18002c060
```
Для **idx10 f7=10 ≠ 20 ⇒ r13d=1 ⇒ путь берётся всегда** → aux обязателен.
(Runtime: rsi=0xA=f7=10, r10=0x4F=COL−1=79 — согласуется с COL=80,f7=10.)

Формирование аргументов `0x18002c060` (@0e272-e2c8, ТОЧНО):
```
rcx = [rbp+0xf8]           ; = aux              ← в rcx уходит NULL
rdx = &[rbp-0x18]          ; param-структура (COL/ROW/N/flags/f7)
r8  = &[rbp-0x50]          ; ptr на рабочий буфер (аллоцирован, т.к. f7≠20)
r9  = rdi = 0x1800BCE50    ; CALIDATA/CTX глобал
[rsp+0x20]=r15d ; [rsp+0x28]=r14 ; [rsp+0x30]=[rbp-0x48] ; [rsp+0x38]=[ptr+0x18]
[rsp+0x40]=[rbp+0x10] ; [rsp+0x48]=rbx ; [rsp+0x50]=глоб ; [rsp+0x58]=[rbp-0x20]
```

---

## 3. Функция `0x18002c060` (0x18002c060..0x18002c353): сигнатура и поля

Пролог: `push`×7 + chkstk `sub rsp,0x4d30` → сдвиг 0x4D68; стек-арг N = `[rsp+0x4D68+off]`.
Сигнатура (ms_abi, 12 аргументов):
```c
int32_t f_score(void   *aux,      // rcx  — control-флаг, читается ТОЛЬКО [aux+0] (dword)
                Param  *p,        // rdx  — param-структура ядра [rbp-0x18]
                void  **workbuf,  // r8   — [r8] = рабочий буфер
                CTX    *ctx,      // r9   — 0x1800BCE50 (calib/ctx глобал)
                int32_t a5, void* a6, void* a7, void* a8_covptr,
                void* a9, void* a10, int32_t a11_crc, int32_t* a12_out);
```

Ключевые чтения (ТОЧНО):
```
@0c0af movsxd rsi,[rdx+0x18]     ; rsi = p->f7   (=10 в runtime)
@0c0de mov  eax,[rcx]            ; ★ eax = aux[0]   ← КРАШ при rcx=0
@0c0e0 movsxd rcx,[rdx+8]        ; N = ROW*COL
@0c0e8 mov  eax,[rdx+4]          ; ROW
@0c0f4 mov  eax,[rdx]            ; COL
@0c0fd mov  rax,[r8]             ; workbuf ptr → [rsp+0x48]
@0c109 call 0x18006c27c          ; alloc
@0c115 movzx r8d,word[rdi+0x26484]
@0c142 call 0x18002c6d0
…
@0c190 cmp [rdi],0x64            ; ctx[0] < 100 ?
@0c195 cmp [rsp+0x5c],1          ; ★ aux[0] == 1 ? (единственное использование aux[0])
@0c19a jne …                     ; aux[0]!=1 → пропустить смягчение порога
@0c1ab/1b0 sub ebx,0xa / 0xf     ; иначе понизить порог качества
@0c214 call 0x180042f80          ; скоринг → eax
@0c219 cmp eax,ebx ; …           ; сравнение с порогом, решение
@0c2be call 0x180037d80 ; @0c2cd mov [a12_out],ebp   ; запись результата
```

### ASCII-раскладки структур

`rcx = aux` (наш вход, читается только offset 0):
```
+0x00  int32  mode/relax_flag   ← читается @0c0de; ==1 ⇒ смягчить порог качества
+0x04… (не читаются на этом пути)
```

`rdx = p` (param-структура, ядро заполнило из config; НЕ наш буфер):
```
+0x00 int32 COL(=80)   +0x04 int32 ROW(=64)   +0x08 int32 N=ROW*COL(=5120)
+0x0c int32 isfloat    +0x10 int32 flag1      +0x14 int32 flag2
+0x18 int32 f7(=10)  ← rsi = p->f7
```

`r8 = workbuf` (стековый локал ядра [rbp-0x50], указатель на буфер, ненулевой при f7≠20):
```
[r8+0] → void* buffer   (rax=[r8]; далее рабочая область)
```

`r9 = ctx` (0x1800BCE50, calib/ctx глобал):
```
[ctx+0]        int32  состояние калибровки (cmp <100)
[ctx+0x26484]  u16    параметр
[ctx+0x1cb64], [ctx+0x217f4]  рабочие блоки
[ctx+0x30488]  флаг   (полный размер ctx ≈ 0x3048c = preprocess_get_CalibParam len)
```

---

## 4. Глобал `0x1800BCE50` (r9/rdi) — проверка (ТОЧНО)

- Это **статический глобал в образе DLL** (offset 0xBCE50), НЕ куча. Runtime r9=rdi=
  0x1800BCE50 совпадает с `lea r9,[rip+0xaddba]` в preprocessor.
- Пишется нашим пайплайном: `preprocess_init_calidata` (kr@+4=0x2000, b@+0x9924=0) и
  `preprocessor_init` (b-калибровка) заполняют таблицы в этом блоке; `preprocessor_exit`
  делает `memset(0x1800BCE50, 0, 0x3048c)`. Размер ctx 0x3048c = `preprocess_get_CalibParam`.
- Ядро читает `[rdi+0x26484]`, `[rdi+0x1cb64]`, `[rdi+0x217f4]`, `[rdi+0x30488]` — всё это
  внутри блока 0x3048c, инициализированного нашими вызовами.

**Вывод:** глобал калибровки НЕ причём в краше — он корректно инициализирован нашим
порядком вызовов (гипотеза «в» отклонена). Причина краша — исключительно aux.

---

## 5. Разбор гипотез (из ТЗ)

- **(а) aux обязателен — ПОДТВЕРЖДЕНО (ТОЧНО).** aux = 2-й арг preprocessor (rdx), мы
  передаём NULL. Ядро прокидывает его в `0x18002c060/rcx`, там `mov eax,[rcx]` → SIGSEGV.
  Путь безусловно берётся для профиля idx10 (f7=10≠20).
  Размер: на этом пути читается ТОЛЬКО `[aux+0]` (dword). aux — **НЕ** буфер размера от
  col/row, а маленькая control-структура. Достаточно указателя на ≥4 обнулённых байта;
  для запаса берём 16–32 обнулённых байта.
- **(б) поле-указатель в src/dst gimg — ОТКЛОНЕНО.** preprocessor читает из gimg только
  `[+0]=data` и `[+0x14]=len`, пишет `dst[+0x28/0x29]`. Ни ядро, ни `0x18002c060` не
  читают других полей gimg. Наши src/dst заполнены корректно (data, len=5120). NULL в
  крашащем rcx приходит из aux, а не из gimg.
- **(в) глобал калибровки NULL — ОТКЛОНЕНО.** См. §4: 0x1800BCE50 — статический, заполнен
  init_calidata+preprocessor_init, ненулевой (runtime это и показывает: r9=rdi≠0).

---

## ФИКС ДЛЯ C

Проблема ровно одна: **2-й аргумент `preprocessor` (`aux`) нельзя передавать `NULL`.**
На активном пути (профиль idx10, f7=10) читается только `aux[0]` (int32-флаг):
`0` = штатный порог качества, `1` = смягчённый порог (лениентный режим первого кадра).
Для воспроизведения штатного поведения передаём `aux[0]=0`.

Изменения в вызывающем коде (`tools/algo/algo_eval.c` / winpe-caller), точечно:

1. Завести обнулённую control-структуру aux (первый dword = 0 = default):
   ```c
   /* aux control-block для preprocessor: читается только [+0] (dword).
      0 = штатный порог качества. Размер с запасом. */
   int32_t pp_aux[8] = {0};   /* 32 байта, все нули */
   ```

2. Заменить вызов
   ```c
   /* было: */
   preprocessor(&src, NULL, 10240, &dst, qcov, 0, 0);
   /* стало: */
   preprocessor(&src, pp_aux, 10240, &dst, qcov, 0, 0);   /* arg2 = aux != NULL */
   ```
   (`in_bytes=10240=2*ROW*COL`, `qcov`=uint8[64] обнулён, `arg6=arg7=0` — без изменений.)

3. Остальное не трогать — src/dst/qcov уже верны; глобал калибровки инициализируется
   существующей последовательностью `ppp_param_init(10)` → `preprocess_init_calidata()` →
   `preprocessor_init(&cal)`.

Ничего больше добавлять не нужно: aux — не рабочий буфер, размер от col/row не зависит.
После правки `mov eax,[rcx]` @0x18002c0de читает `pp_aux[0]==0`, краш-путь проходит,
`0x18002c060` выполняет скоринг и возвращает управление; `preprocessor` должен вернуть 0
и записать quality/coverage в `dst[+0x29]/[+0x28]`.

Проверить на железе: значение `aux[0]` (0 vs 1) — влияет только на порог качества, не на
факт работы; если штатный порог режет наши кадры, попробовать `aux[0]=1`.

---

# ДОРАБОТКА 2 — второй краш и ИСТИННАЯ сигнатура preprocessor (arg3/arg4/arg5 — указатели)

После фикса aux≠NULL краш сместился: SIGSEGV в memset-хелпере `0x18002ac50`
(`rep stosb @0x18002ac6e`) с `rcx(dst)=0x2800=10240`, `r8d(count)=0x4c98=19608`.
Разбор показал: **наша модель аргументов была неверна**. `preprocessor` — это НЕ
`(src, aux, in_bytes, dst, qcov, m1, m2)`, а функция над ЕДИНЫМ контекстом рабочих
буферов. `arg3` — это НЕ число байт, а **указатель на рабочий/выходной буфер**.

## 6. Кто и как крашит в memset (ТОЧНО)

`callers 0x18002ac50` → на нашем пути это ядро `0x18002dea0`, call-site **0x18002e352**:
```
@0x18002e2f7 mov r13,[rbp-0x78]        ; r13 = arg3-буфер (см. ниже)
@0x18002e2fb test r13,r13 ; je 0x18002e482   ; ЕСЛИ arg3==0 → вся coating-ветка ПРОПУСКАЕТСЯ
@0x18002e347 xor edx,edx ; mov r8d,0x4c98(19608) ; mov rcx,r13 ; call 0x18002ac50
             ; → memset(arg3, 0, 19608)  ← КРАШ: arg3=10240 (наше число) как адрес
```

### r13 = [rbp-0x78] = arg11 ядра = arg3 preprocessor (ТОЧНО, разрешает путаницу)

Пролог ядра: `push`×8; `lea rbp,[rsp-0xA8-relative]`; значит `[rbp+0x100]` = 11-й
аргумент ядра. `@0x18002def6 rax=[rbp+0x100]; @0x18002df01 [rbp-0x78]=rax`. А
`@0x18002e2f7 r13=[rbp-0x78]`. Цепочка arg-слотов:
preprocessor кладёт `arg3(r8→r10)` в `[rsp+0x50]` (`@0x18000f066`) → это **arg11 ядра**
(`[rbp+0x100]`) → `[rbp-0x78]` → r13 → memset-dst.
> Прим.: `r13d` из пролога (`@0x18002dece mov r13d,[rbp+0xd0]` = packed config) —
> ЛОЖНЫЙ след: этот r13 перезаписывается на `[rbp-0x78]` (=arg3) непосредственно
> перед memset (`@0x18002e2f7`). Значение 0x2800=10240 в r13 — это наш arg3, а не config.

### Почему coating-ветка вообще берётся (ТОЧНО)

Гейт `@0x18002e2ee test r13d,r13d` где `r13d=[rsp+0x70]=(config>>2)&1`. Для профиля
idx10: config строится как (COL=80,ROW=64,f7=10,PIXEL_CANCEL=0,IS_COATING=4,ISFLOAT=1):
```
config = ((((((80<<9)|64)<<11)|10)<<2|0)<<1|4)|1 = 0x28100055
(config>>2)&1 = 1   ← бит IS_COATING(=4) выставлен ⇒ coating-ветка АКТИВНА для 5125
```
(Проверка декода: COL=config>>23=80 ✓, ROW=(config>>14)&0x1ff=64 ✓, f7=(config>>3)&0x3f=10 ✓,
runtime rsi=0xA=f7, r10=0x4F=COL-1=79.) Т.е. для нашего сенсора arg3-буфер ОБЯЗАТЕЛЕН.

### Что делает coating-ветка с arg3 (ТОЧНО)

```
memset(arg3,0,19608)                            ; @0x18002e347 — обнуление буфера-результата
newbuf = alloc(N); memset(newbuf,0,N)           ; @0x18002e357-e36a
tmp = alloc(2N)                                  ; @0x18002e376
loop N: word[tmp+i] = clamp( b[i](@ctx+0x9924) - src16[i] )   ; @0x18002e3a0-e3c4 (вычитание фона)
call 0x180029e80(rcx=newbuf, rdx=arg3, r9=ROW)   ; @0x18002e415 — coating-обработка В arg3
...
byte[arg3+1] = 1 (или byte[arg3+0]=sil)          ; @0x18002e46a / @0x18002e472 — флаги результата
```
→ **arg3 = выходной+scratch буфер coating, размер ≥ 19608 (0x4C98) байт (фиксированный
immediate, НЕ зависит от col/row).** Первым делом обнуляется, дальше заполняется.
Если arg3=NULL — вся ветка пропускается (`@0x18002e2fb`), краша нет, но coating-коррекция
не выполняется.

## 7. РЕАЛЬНАЯ сигнатура preprocessor — из вызова EngineAdapter (ТОЧНО)

Call-site `preprocessor_wrapper` в `AcceptSampleData` (EngineAdapter.dll, WBDI слот 12),
`0x1800358ea` (внутри `0x180033c20`). Обёртка `0x18000b810` прокидывает аргументы 1:1
в internal `0x18000efa0`. Формирование (ТОЧНО):
```
ctx = *(void**)( *(rsp+0x290) + 0x38 )   ; объект-контекст препроцессора (@0x180033e43-e4f)
rcx(arg1) = lea[rsp+0x1e0]               ; &SRC-структура (стек)
rdx(arg2) = lea[rsp+0x104]               ; &AUX-структура (стек)
r8 (arg3) = ctx + 0x40                   ; УКАЗАТЕЛЬ на coating-буфер (внутри ctx)
r9 (arg4) = ctx + 0x00                   ; УКАЗАТЕЛЬ на DST-gimg (выход. изображение)
[rsp+0x20](arg5) = ctx + 0x30            ; УКАЗАТЕЛЬ на quality/coverage (2×int32)
[rsp+0x28](arg6) = byte[*(rsp+0xd0)+0x431]  ; байт-флаг режима (enroll/verify)
[rsp+0x30](arg7) = 0
```
**Итоговая сигнатура (ТОЧНО по факту вызова + чтениям в AlgoMilan):**
```c
int32_t preprocessor(GImg   *src,      // rcx  — вход 16-бит
                     Aux    *aux,      // rdx  — контроль/выход (флаг [aux+0])
                     void   *cbuf,     // r8   — coating-буфер (≥19608 байт)  ← НЕ in_bytes!
                     GImg   *dst,      // r9   — выход 8-бит (data + coverage/quality)
                     QCov   *qcov,     // stk5 — {int32 quality; int32 coverage}
                     uint8_t m1,       // stk6 — режим
                     uint8_t m2);      // stk7 — 0
```
`arg3`, `arg4`, `arg5` в EngineAdapter — под-поля одного объекта `ctx`
(`+0x40`/`+0x00`/`+0x30`), но в AlgoMilan они используются НЕЗАВИСИМО (arg3→arg11 ядра,
arg4→rsi preprocessor, arg5→r14). **Можно передавать три отдельных буфера** — межполевых
вычислений адресов нет (проверено). ctx = `*(EngineContext+0x38)`, аллоцируется в WBDI-
пайплайне (Attach/CreateContext), инициализируется в `AdapterInitPreprocessor 0x180031160`
(там же init/load_calidata + preprocessor_init). Размер объекта ≥ `0x40 + 19608`.

## 8. Раскладки структур (что реально читает/пишет AlgoMilan)

### SRC (arg1) — как заполняет EngineAdapter (@0x1800348a0-0x180034923, ТОЧНО)
```c
struct GImg {                     // AlgoMilan читает ТОЛЬКО data(+0) и len(+0x14)
  void*    data;   /* +0x00 */    // src: uint16[ROW*COL] сырой кадр
  uint8_t  _08[0x0c];
  uint32_t len;    /* +0x14 */    // src: = 2*ROW*COL = 10240 (ДЛИНА В БАЙТАХ!)  ← у нас было 5120 (баг)
  uint16_t f18;    /* +0x18 */    // EngineAdapter пишет 1 (AlgoMilan не читает)
  ...
};
```
Длина src: EngineAdapter вычисляет `src->len = 2*COL*ROW` (`imul; shl rax,1` @0x180034920).
AlgoMilan сверяет `2*ROW*COL == src->len` (`@0x18002df3e sete cl`) — не жёсткий гейт, но
влияет на режим ядра. **Для 5125: `src.len(+0x14) = 10240`, НЕ 5120.**

### DST (arg4) — что пишет preprocessor (@0x18000f0e8-0x18000f117, ТОЧНО)
```c
// dst: uint8-выход. preprocessor:
//   coverage → dst[+0x28], quality → dst[+0x29]
//   если dst[+0x14] > i: копирует байтами результат в dst->data ([dst+0])
//   dst.data ≥ ROW*COL байт;  dst.len(+0x14) = ROW*COL = 5120 (число ПИКСЕЛЕЙ = байт вывода)
```
> Внимание на асимметрию: **src.len(+0x14)=10240 (байты 16-бит), dst.len(+0x14)=5120
> (байты 8-бит)**. Оба = размер кадра в БАЙТАХ соответствующей разрядности.

### AUX (arg2) — читается [aux+0] (ТОЧНО), EngineAdapter даёт стек-структуру
```c
struct Aux { int32_t mode; /* +0x00: 0/1, ==1 смягчает порог качества (0x18002c195) */
             uint8_t _04[0x3c]; };   // AlgoMilan читает только +0; даём обнулённой
```
EngineAdapter ставит `aux.mode = 1` в enroll-ветке (`@0x180033d72`) и `0`/`1` в др.

### QCOV (arg5) — quality/coverage
```c
struct QCov { int32_t quality; int32_t coverage; };  // ядро пишет сюда, ≥8 байт
```

### CBUF (arg3) — coating-буфер
```c
uint8_t cbuf[19608];   // ≥0x4C98 байт; обнуляется самим ядром (memset); фикс. размер
```

## 9. Глобальные флаги гейта memset-логирования (ТОЧНО, побочно)
`@0x18002e304 cmp byte[rip+0xbefe6],0` и `@0x18002e30d cmp dword[rdi+0x30488],0` (rdi=ctx
калибровки 0x1800BCE50) гейтят только ДОП. ветку логирования/дампа перед основным memset
(`@0x18002e347`). При штатной инициализации `[rip+0xbefe6]` (debug-флаг) = 0, а
`[ctx+0x30488]` (счётчик кадров) заполняется по ходу; на основной memset они НЕ влияют —
memset(arg3) выполняется в любом случае, если arg3≠NULL и coating-бит=1.

---

# ФИКС ДЛЯ C (ФИНАЛЬНЫЙ — заменяет разделы фикса выше)

Причина обоих крашей: **мы передавали `arg3` как ЧИСЛО (in_bytes=10240), а это УКАЗАТЕЛЬ
на coating-буфер.** Плюс мелочь: `src.len(+0x14)` должно быть 10240 (байты), а не 5120.

```c
#define ROW 64
#define COL 80
#define PX  (ROW*COL)          /* 5120 пикселей */
#define CBUF_SZ 19608          /* 0x4C98, фикс. размер coating-буфера ядра */

/* Буферы (все обнулить) */
uint16_t raw16[PX];            /* сырой кадр 16-бит (заполнить кадром) */
uint8_t  out8 [PX];            /* выход 8-бит */
uint8_t  cbuf [CBUF_SZ] = {0}; /* arg3: coating scratch/выход — ОБЯЗАТЕЛЕН для 64x80 */
struct Aux  aux  = {0};        /* arg2: aux.mode=0 (нейтрально) */
struct QCov qc   = {0};        /* arg5 */

struct GImg src = {0}, dst = {0};
src.data = raw16;  src.len /*+0x14*/ = 2*PX;   /* = 10240 БАЙТ (16-бит!)  ← ИЗМЕНЕНО с 5120 */
dst.data = out8;   dst.len /*+0x14*/ = PX;     /* = 5120 (байты 8-бит) */
/* dst-геометрия как раньше: +0x08=64,+0x0a=80,+0x0e=8,+0x0f=1,+0x18=1 */

/* INIT (без изменений): */
ppp_param_init(10);
preprocess_init_calidata();
/* CalInit cal={0}; cal.buffer=cal16; cal.col=80; cal.row=64; */
preprocessor_init(&cal);

/* ВЫЗОВ — arg3 теперь УКАЗАТЕЛЬ, не число: */
int rc = preprocessor(&src,        /* arg1 */
                      &aux,        /* arg2  (было NULL → int32[8]; теперь Aux, mode=0) */
                      cbuf,        /* arg3  ← БЫЛО 10240; теперь указатель на uint8[19608] */
                      &dst,        /* arg4 */
                      &qc,         /* arg5 */
                      0, 0);       /* arg6=m1, arg7=m2 */
/* ожидаем rc==0; out8 = обработанный кадр; qc.quality/qc.coverage и dst[+0x29]/[+0x28] */
```

Итог изменений относительно текущего кода:
1. **arg3: было `10240` (число) → стало `cbuf` — указатель на обнулённый `uint8[19608]`.**
   Это и есть исправление второго краша. (Альтернатива-костыль: `arg3=NULL` пропускает
   coating-ветку без краша, но теряет coating-коррекцию — для профиля с IS_COATING нежелательно.)
2. **src.len (`src+0x14`): было 5120 → стало 10240** (2*ROW*COL, длина в байтах 16-бит кадра).
3. aux: обнулённая структура (≥0x40 байт), aux.mode(+0)=0 — уже было (int32[8] годится).
4. dst / qcov / init-последовательность — без изменений.

Размер `cbuf` фиксирован (19608), от col/row НЕ зависит. Буфер обнуляется самим ядром,
но передать нужно валидную запись ≥19608 байт.
