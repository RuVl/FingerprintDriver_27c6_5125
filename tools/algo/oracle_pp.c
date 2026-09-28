// Oracle for stage 1 of openchicago (docs/stage1-preprocess.md): runs AlgoChicago.dll's
// preprocessor over the dataset in a fixed scenario from a cold start and writes a binary
// dump of every call (output image, quality/coverage, return code and the adaptive state
// in the DLL's globals). openchicago/tests/test_preprocess.c replays the same scenario
// with our code and compares byte by byte.
//
// Usage: ./oracle_pp <scenario> <out.bin>      (run from tools/algo, needs frames_raw.bin,
//                                               bg_raw.bin, meta_raw.txt, win-driver/)
//   scenario A: all 155 records in index order, purpose 0
//   scenario B: first 12 natural records with purpose 1, then all others in index order
//               with purpose 0
// Both: ppp_param_init(12) -> preprocess_init_calidata() -> preprocessor_init(bg of the
// first natural record) -> preprocessor(&src, &purpose, cbuf, &dst, qcov, 0, 0) per frame.
// One scenario per process: the DLL keeps static state (first-call flag, previous output
// image) that preprocessor_exit does not reset.
// Debug: TRACE=1 hooks internal functions (trampolines) and logs per frame to stderr the
// capture-policy scores (0x1800456a0, 0x18005e3b0), output marking (0x180054270), re-runs of
// 0x18004a950 etc.; TRACE_PLANES=<file> / TRACE_STRETCH=<file> additionally dump the inputs and
// outputs of 0x180043050 / 0x180045d40. Hooks do not change the results.
//
// Dump format (little endian):
//   header: char magic[4] = "OCPP", u32 version = 2, u32 nframes, u32 record_size
//   record: i32 seq, rec, purpose, rc, coverage, quality, framenum (G+0), gcount (0x1800fec14)
//           u8  dst[5120]            (64 rows x 80, the 8-bit preprocessor output)
//           u16 kr[5120]             (G+4, accumulated gain)
//           u16 gain[5120]           (0x1801144b0, normalized gain applied to the frame)
//           u16 mult[5120]           (0x18010ab90)
//           u16 aux[5120]            (0x18011ddd0)
//           u8  cbuf[16]             (first bytes of the per-frame buffer; 0..5 = 0x180043c70 context)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "algolog.h"
#include "winpe.h"

#define MS __attribute__((ms_abi))
#define SW 64
#define SH 80
#define PX (SW * SH)
#define MAXREC 256
#define CBUF_SZ 0x4d00

#define G_VA 0x180099d00ULL
#define GAIN_VA 0x1801144b0ULL
#define MULT_VA 0x18010ab90ULL
#define AUX_VA 0x18011ddd0ULL
#define GCOUNT_VA 0x1800fec14ULL

#pragma pack(push, 1)
struct gimg {
  void *data; uint16_t width, height, d0c; uint8_t bits, ch; uint16_t d10, d12;
  uint32_t size; uint16_t frames, d1a; uint32_t sensor_type; uint8_t d20[8];
  uint8_t cov_q[8]; uint8_t tail[0x10];
};
#pragma pack(pop)

typedef int MS (*ppp_t)(int);
typedef int MS (*v_t)(void);
typedef int MS (*ppinit_t)(void *);
typedef int MS (*pp_t)(struct gimg *, int *, void *, struct gimg *, int *, uint8_t, uint8_t);

// TRACE=1: log internal decisions of the DLL per frame to stderr (hooks with trampolines).
typedef uint64_t MS (*fn8_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void *tramp(uint64_t va, int n) {   // copy n prologue bytes (whole instructions) + jmp back
  static uint8_t *pool; static size_t used;
  if (!pool) pool = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uint8_t *t = pool + used; used += 32;
  memcpy(t, (void *)va, n);
  uint64_t back = va + n;
  t[n] = 0x48; t[n + 1] = 0xb8; memcpy(t + n + 2, &back, 8); t[n + 10] = 0xff; t[n + 11] = 0xe0;
  return t;
}
#define HOOK(name, va, n)                                                                        \
  static fn8_t name##_orig;                                                                      \
  static uint64_t MS name##_hook(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, \
                                 uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, \
                                 uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14) {       \
    uint64_t r = name##_orig(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);       \
    name##_log(r, a1, a2, a3, a4, a5, a6, a7, a8);                                                \
    return r;                                                                                    \
  }
static void s456_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  int mc = 0;
  for (int k = 0; k < PX; k++) mc += ((uint8_t *)a2)[0x10 + k] != 0;
  fprintf(stderr, "  456a0 score1=%d flag=%d mask=%d\n", (int)r, *(int *)a4, mc);
}
static void s5e3_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  fprintf(stderr, "  5e3b0 score2=%d\n", (int)r);
}
static void s542_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  fprintf(stderr, "  54270 esi=%d flag=%d\n", (int)a5, (int)a6);
}
static void s47e_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  fprintf(stderr, "  47ec0 -> %d\n", (int)r);
}
static void sa95_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  fprintf(stderr, "  a950 framenum=%d gc=%d\n", *(int *)G_VA, *(int *)GCOUNT_VA);
}
static void s463_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  fprintf(stderr, "  463e0 -> %#x\n", (int)r);
}
static void s100_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  int mc = 0;
  uint8_t *m = *(uint8_t **)(a2 + 0x18);
  for (int k = 0; k < PX; k++) mc += m[k] != 0;
  fprintf(stderr, "  100f0 quality=%d region=%d\n", (int)r, mc);
}
static void sfca_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  fprintf(stderr, "  fca0 base=%d annotate=%d\n", (int)r, (int)a3);
}
static void s430_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  static FILE *f;
  if (!getenv("TRACE_PLANES")) return;
  if (!f) f = fopen(getenv("TRACE_PLANES"), "wb");
  fwrite((void *)a1, 2, PX, f);            // source plane (G+0x13244)
  fwrite((uint8_t *)a4 + 0x10, 1, PX, f);  // mask
  fwrite((void *)a2, 1, PX, f);            // primary candidate
  fflush(f);
}
static void s102_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  uint8_t *m = *(uint8_t **)(a2 + 0x18), *l = *(uint8_t **)(a3 + 0x18);
  int h[10] = {0}, t = 0;
  for (int k = 0; k < PX; k++) if (m[k]) h[l[k] < 10 ? l[k] : 9]++;
  for (int k = 0; k < 9; k++) t += h[k];
  fprintf(stderr, "  10200 label4=%d/%d masked=%d\n", h[4], t, t + h[9]);
}
HOOK(s102, 0x180010200ULL, 12)
static void s45d_log(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6, uint64_t a7, uint64_t a8) {
  static FILE *f;
  if (!getenv("TRACE_STRETCH")) return;
  if (!f) f = fopen(getenv("TRACE_STRETCH"), "wb");
  fwrite((void *)a1, 2, PX, f);            // plane
  fwrite((uint8_t *)a3 + 0x10, 1, PX, f);  // mask
  fwrite((void *)a2, 1, PX, f);            // stretched candidate
  fflush(f);
}
HOOK(s45d, 0x180045d40ULL, 13)
HOOK(s430, 0x180043050ULL, 13)
HOOK(s100, 0x1800100f0ULL, 13)
HOOK(sfca, 0x18000fca0ULL, 12)
HOOK(s456, 0x1800456a0ULL, 13)
HOOK(s5e3, 0x18005e3b0ULL, 13)
HOOK(s542, 0x180054270ULL, 15)
HOOK(s47e, 0x180047ec0ULL, 15)
HOOK(sa95, 0x18004a950ULL, 15)
HOOK(s463, 0x1800463e0ULL, 12)
static void trace_install(void) {
#define INST(name, va, n) name##_orig = (fn8_t)tramp(va, n); winpe_hook(va, (void *)name##_hook);
  INST(s456, 0x1800456a0ULL, 13) INST(s5e3, 0x18005e3b0ULL, 13) INST(s542, 0x180054270ULL, 15)
  INST(sfca, 0x18000fca0ULL, 12) INST(s102, 0x180010200ULL, 12) INST(s45d, 0x180045d40ULL, 13) INST(s100, 0x1800100f0ULL, 13) INST(s430, 0x180043050ULL, 13)
  INST(s47e, 0x180047ec0ULL, 15) INST(sa95, 0x18004a950ULL, 15) INST(s463, 0x1800463e0ULL, 12)
}

// TRACE_RES=<file>: tagged records {char tag[4]; u32 len; data} around 0x180038380 (resolution
// map + history behind the cbuf[0..5] context of 0x180043c70) and its stages, for the openchicago
// port of 0x180043c70.  See openchicago/tests (dev tool, not used by run.sh).
static FILE *tres;
static uint8_t *tres_local;   // 0x180038380 arg 7: the context struct of 0x180043c70
static void tput(const char *tag, const void *d, uint32_t n) {
  fwrite(tag, 1, 4, tres); fwrite(&n, 4, 1, tres); fwrite(d, 1, n, tres);
}
static void tglobals(void) {
  int32_t g[7] = {*(int32_t *)0x1800d8764ULL, *(int32_t *)0x1800d8768ULL, *(int32_t *)0x1800d8760ULL,
                  *(int32_t *)0x1800eb9b0ULL, *(int32_t *)0x180092350ULL, *(int32_t *)0x180092354ULL,
                  *(int32_t *)0x1800d876cULL};
  tput("GLOB", g, sizeof g);
  tput("E209", (void *)0x1800e2090ULL, PX);
  tput("D877", (void *)0x1800d8770ULL, 2 * PX);
  tput("E6D2", (void *)0x1800e6d20ULL, PX);
}
static fn8_t r383_orig, rcf0_orig, reb1_orig, rb5b_orig, r392_orig, r43c_orig, ra61_orig;
#define TARGS uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, \
              uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14
#define TCALL(o) o(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14)
static void tlocal(const char *tag) {
  tput(tag, tres_local, 0x28);
  tput("LABL", *(void **)(tres_local + 0x28), PX);
}
static uint64_t MS r383_hook(TARGS) {
  tres_local = (uint8_t *)a7;
  int32_t fn = *(int32_t *)a3;
  tput("R0  ", (void *)a1, 2 * PX); tput("P2  ", (void *)a2, 2 * PX); tput("FN  ", &fn, 4);
  tput("MSKH", (void *)a4, 16); tput("MASK", (uint8_t *)a4 + 0x10, PX);
  tlocal("LOC0"); tglobals();
  uint64_t r = TCALL(r383_orig);
  tlocal("LOC1"); tglobals();
  return r;
}
static uint64_t MS rcf0_hook(TARGS) {
  uint64_t r = TCALL(rcf0_orig);
  tput("BASE", (void *)a4, 2 * PX); tput("EROD", (void *)a5, PX);
  return r;
}
static uint64_t MS ra61_hook(TARGS) {
  uint64_t r = TCALL(ra61_orig);
  tput("A610", (void *)a2, 2 * PX);
  return r;
}
static uint64_t MS reb1_hook(TARGS) {
  uint64_t r = TCALL(reb1_orig);
  tlocal("EB1 "); tglobals();
  return r;
}
static uint64_t MS rb5b_hook(TARGS) {
  tput("FLAG", (void *)a2, PX);
  uint64_t r = TCALL(rb5b_orig);
  tlocal("B5B1");
  return r;
}
static uint64_t MS r392_hook(TARGS) {
  uint64_t r = TCALL(r392_orig);
  int32_t v = (int32_t)r; tput("3924", &v, 4);
  return r;
}
static uint64_t MS r43c_hook(TARGS) {
  uint64_t r = TCALL(r43c_orig);
  int32_t v[3] = {(int32_t)r, *(int32_t *)a7, *(int32_t *)a6};
  tput("CTXR", v, sizeof v); tput("CBUF", (void *)a9, 16);
  return r;
}
static fn8_t r409_orig;
static uint64_t MS r409_hook(TARGS) {
  tput("SEC ", (void *)a2, 2 * PX); tput("SMSK", (void *)a3, PX);
  uint64_t r = TCALL(r409_orig);
  tput("ANA ", (void *)a6, 0x60); tput("ANAS", (void *)a8, 8); tput("ANAP", (void *)a5, 0x10);
  return r;
}
static fn8_t rbb4_orig, r70a_orig, re93_orig, rfff_orig, r015_orig, rcd6_orig;
static uint64_t MS rbb4_hook(TARGS) {
  uint64_t r = TCALL(rbb4_orig);
  int32_t v[2] = {*(int32_t *)a4, *(int32_t *)a6}; tput("BB40", v, sizeof v);
  return r;
}
static uint64_t MS r70a_hook(TARGS) {
  uint64_t r = TCALL(r70a_orig);
  tput("70A0", (void *)a5, 2 * PX); tput("70AD", (void *)a6, 2 * PX);
  return r;
}
static uint64_t MS re93_hook(TARGS) {
  uint64_t r = TCALL(re93_orig);
  tput("E930", (void *)a9, 2 * PX);
  return r;
}
static uint64_t MS rfff_hook(TARGS) {
  uint64_t r = TCALL(rfff_orig);
  tput("FFF0", (void *)a5, PX);
  return r;
}
static uint64_t MS r015_hook(TARGS) {
  uint64_t r = TCALL(r015_orig);
  tput("0150", (void *)a9, 2 * PX);
  return r;
}
static uint64_t MS rcd6_hook(TARGS) {
  uint64_t r = TCALL(rcd6_orig);
  tput("CD60", (void *)a5, PX);
  return r;
}
static fn8_t r37d_orig;
static uint64_t MS r37d_hook(TARGS) {
  tput("7DP ", (void *)a1, 2 * PX); tput("7DE0", (void *)a5, PX); tput("7DL0", *(void **)(a2 + 0x28), PX);
  uint64_t r = TCALL(r37d_orig);
  tput("7DE1", (void *)a5, PX); tput("7DL1", *(void **)(a2 + 0x28), PX);
  return r;
}
static void tres_install(const char *path) {
  tres = fopen(path, "wb");
  if (!tres) { perror(path); exit(1); }
  int32_t init[2] = {*(int32_t *)0x180092350ULL, *(int32_t *)0x180092354ULL};
  tput("INIT", init, sizeof init);
#define TINST(name, va, n) name##_orig = (fn8_t)tramp(va, n); winpe_hook(va, (void *)name##_hook);
  TINST(r383, 0x180038380ULL, 13) TINST(rcf0, 0x180037cf0ULL, 15) TINST(reb1, 0x18003eb10ULL, 12)
  TINST(rb5b, 0x18003b5b0ULL, 15) TINST(r392, 0x180039240ULL, 15) TINST(r43c, 0x180043c70ULL, 13)
  TINST(ra61, 0x18003a610ULL, 15) TINST(r409, 0x180040980ULL, 12)
  TINST(rbb4, 0x18003bb40ULL, 12) TINST(r70a, 0x1800370a0ULL, 12) TINST(re93, 0x18003e930ULL, 12)
  TINST(r37d, 0x180037df0ULL, 12) TINST(rfff, 0x18003fff0ULL, 15) TINST(r015, 0x180040150ULL, 15) TINST(rcd6, 0x18003cd60ULL, 15)
}

static void *need(const char *n) {
  void *p = winpe_getproc(n);
  if (!p) { fprintf(stderr, "missing export %s\n", n); exit(1); }
  return p;
}

static void transpose16(uint16_t *f) {
  static uint16_t t[PX];
  for (int r = 0; r < SH; r++)
    for (int c = 0; c < SW; c++) t[c * SH + r] = f[r * SW + c];
  memcpy(f, t, sizeof t);
}

static void set_hdr(struct gimg *g, void *data, int bits) {
  memset(g, 0, sizeof *g);
  g->data = data; g->width = 80; g->height = 64; g->bits = bits; g->ch = 1;
  g->size = PX * (bits / 8); g->frames = 1;
}

int main(int argc, char **argv) {
  if (argc != 3 || (strcmp(argv[1], "A") && strcmp(argv[1], "B"))) {
    fprintf(stderr, "usage: %s A|B out.bin\n", argv[0]);
    return 2;
  }
  char scen = argv[1][0];
  if (!winpe_load("../../win-driver/AlgoChicago.dll")) return 1;
  if (getenv("ALGOLOG")) { winpe_hook(0x1800080e0ULL, algolog_fn()); algolog_set(stderr, NULL); }
  if (getenv("TRACE")) trace_install();
  if (getenv("TRACE_RES")) tres_install(getenv("TRACE_RES"));
  ppp_t ppp = need("ppp_param_init");
  v_t initcali = need("preprocess_init_calidata");
  ppinit_t ppinit = need("preprocessor_init");
  pp_t pp = need("preprocessor");

  FILE *f = fopen("frames_raw.bin", "rb");
  if (!f) { perror("frames_raw.bin"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  int nrec = sz / (PX * 2);
  if (nrec > MAXREC) nrec = MAXREC;
  uint16_t *raw = malloc(sz), *bg = malloc(sz);
  if (fread(raw, 1, sz, f) != (size_t)sz) return 1;
  fclose(f);
  f = fopen("bg_raw.bin", "rb");
  if (!f || fread(bg, 1, sz, f) != (size_t)sz) return 1;
  fclose(f);
  static char labels[MAXREC][16];
  FILE *m = fopen("meta_raw.txt", "r");
  int tmp;
  for (int i = 0; i < nrec; i++)
    if (fscanf(m, "%15s %d", labels[i], &tmp) != 2) return 1;
  fclose(m);
  for (int i = 0; i < nrec; i++) { transpose16(raw + (size_t)i * PX); transpose16(bg + (size_t)i * PX); }

  int nat[MAXREC], nnat = 0;
  for (int i = 0; i < nrec; i++) if (!strcmp(labels[i], "natural")) nat[nnat++] = i;

  // call order
  int order[MAXREC], purp[MAXREC], n = 0, used[MAXREC] = {0};
  if (scen == 'B')
    for (int k = 0; k < 12 && k < nnat; k++) { order[n] = nat[k]; purp[n++] = 1; used[nat[k]] = 1; }
  for (int i = 0; i < nrec; i++)
    if (!used[i]) { order[n] = i; purp[n++] = 0; }

  if (ppp(12)) { fprintf(stderr, "ppp_param_init failed\n"); return 1; }
  initcali();
  uint8_t cal[0x40] = {0};
  *(void **)(cal + 0x18) = bg + (size_t)nat[0] * PX;
  *(uint32_t *)(cal + 0x24) = 64;
  *(uint32_t *)(cal + 0x28) = 80;
  if (ppinit(cal)) { fprintf(stderr, "preprocessor_init failed\n"); return 1; }

  FILE *out = fopen(argv[2], "wb");
  if (!out) { perror(argv[2]); return 1; }
  uint32_t recsz = 8 * 4 + PX + 4 * PX * 2 + 16;
  uint32_t fh[3] = {2, (uint32_t)n, recsz};
  fwrite("OCPP", 1, 4, out);
  fwrite(fh, sizeof fh, 1, out);

  static uint8_t cbuf[CBUF_SZ], dstbuf[PX];
  for (int s = 0; s < n; s++) {
    int i = order[s], purpose = purp[s], qcov[2] = {0, 0};
    struct gimg src, dst;
    set_hdr(&src, raw + (size_t)i * PX, 16);
    set_hdr(&dst, dstbuf, 8);
    memset(dstbuf, 0, PX);
    memset(cbuf, 0, 16);   // as algo_eval4 (calloc per record); BAD_INPUT leaves cbuf untouched
    if (getenv("TRACE")) fprintf(stderr, "seq %d rec %d purpose %d\n", s, i, purpose);
    if (tres) { int32_t sq[2] = {s, i}; tput("CALL", sq, sizeof sq); }
    int rc = pp(&src, &purpose, cbuf, &dst, qcov, 0, 0);
    int32_t hdr[8] = {s, i, purp[s], rc, qcov[0], qcov[1], *(int32_t *)G_VA, *(int32_t *)GCOUNT_VA};
    fwrite(hdr, sizeof hdr, 1, out);
    fwrite(dstbuf, 1, PX, out);
    fwrite((void *)(G_VA + 4), 2, PX, out);
    fwrite((void *)GAIN_VA, 2, PX, out);
    fwrite((void *)MULT_VA, 2, PX, out);
    fwrite((void *)AUX_VA, 2, PX, out);
    fwrite(cbuf, 1, 16, out);
  }
  fclose(out);
  fprintf(stderr, "oracle_pp %c: %d frames, framenum=%d\n", scen, n, *(int32_t *)G_VA);
  return 0;
}
