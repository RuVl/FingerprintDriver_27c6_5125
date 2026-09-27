// Full vendor-pipeline matcher eval: replays the AlgoMilan pipeline exactly as
// EngineAdapter does (RE in tools/algo/re/notes/40-pipeline-verified.md):
//   INIT : ppp_param_init(10) -> preprocess_init_calidata() -> preprocessor_init(&cal)
//   FRAME: preprocessor(raw16) -> 8-bit image
//   ENROL: enrolStartEx -> enrolAddImage(pp)... -> enrolGetTemplate   (no destructor!)
//   MATCH: identifytemplate(&ref,&probe,NULL,&idx)   idx>=0 => match
//
// Inputs: frames_raw.bin / bg_raw.bin (uint16 64x80) + meta_raw.txt.
// Usage: ./algo_eval2 [enroll_n=15] [ppp=10] [col=64] [row=80] [calib_mode=0]
//   calib_mode 0 = preprocessor_init with a background frame (flat-field)
//   calib_mode 1 = preprocess_set_mode(1) bypass (declare calibrated, no bg)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winpe.h"

#define MS __attribute__((ms_abi))
#define W 64
#define H 80
#define PX (W * H)   // 5120

#pragma pack(push, 1)
struct gimg {              // offsets verified against enrolAddImage + preprocessor
  void *data;             // +0x00
  uint16_t d08;           // +0x08  dimension A (enrolAddImage: d08*d0a == PX)
  uint16_t d0a;           // +0x0a  dimension B
  uint16_t d0c;           // +0x0c
  uint8_t bits;           // +0x0e  must be 8 (enrol)
  uint8_t one0f;          // +0x0f  must be 1 (enrol)
  uint16_t d10, d12;      // +0x10 +0x12
  uint32_t len14;         // +0x14  length ROW*COL for preprocessor
  uint16_t chan18;        // +0x18  must be != 0 (enrol)
  uint8_t tail[0x2e];     // +0x1a.. (coverage@+0x28, quality@+0x29 written by pp)
};
#pragma pack(pop)

typedef int MS (*ppp_t)(int);
typedef int MS (*initcali_t)(void);
typedef int MS (*ppinit_t)(void *);
// arg3 is a POINTER to a coating work buffer (>=0x4c98 bytes), NOT a byte count.
typedef int MS (*pp_t)(struct gimg *, void *, void *, struct gimg *, void *,
                       uint8_t, uint8_t);
#define CBUF_SZ 0x4d00   // >= 0x4c98 (19608), the core zeroes it itself
typedef int MS (*ppexit_t)(void);
typedef int MS (*setmode_t)(int);
typedef void *MS (*estart_t)(int *);
typedef int MS (*eadd_t)(void *, struct gimg *, void *, void *, uint8_t, void *);
typedef int MS (*eget_t)(void *, void **);
// identifytemplate(ref_holder, probe_holder, NULL, &idx): it derefs arg0/arg1 ONCE
// to reach the template object (holder -> T), so pass the holders BY VALUE.
typedef int MS (*idt_t)(void *, void *, void *, int *);

static ppp_t ppp; static initcali_t initcali; static ppinit_t ppinit;
static pp_t pp; static ppexit_t ppexit; static setmode_t setmode;
static estart_t estart; static eadd_t eadd; static eget_t eget; static idt_t idt;

static int dbg_enroll = 1;
static uint16_t *raw, *bg;        // [nrec][PX]
static uint8_t *proc;             // [nrec][PX] vendor-preprocessed output
static struct gimg gimgs[200];    // dst gimg per record (as preprocessor left it)
static char labels[200][16];
static int nrec, g_col = W, g_row = H;

// preprocess raw[idx] -> proc[idx]; returns pp() rc, fills *cov,*qual
static int preprocess_one(int idx, int *cov, int *qual) {
  struct gimg src, *dst = &gimgs[idx];
  memset(&src, 0, sizeof src);
  memset(dst, 0, sizeof *dst);
  src.data = raw + (size_t)idx * PX; src.len14 = 2 * PX;  // 16-bit input, BYTES
  dst->data = proc + (size_t)idx * PX; dst->len14 = PX;    // 8-bit output, bytes
  dst->d08 = W; dst->d0a = H; dst->bits = 8; dst->one0f = 1; dst->chan18 = 1;
  // arg2 (aux): scoring core reads *(int*)aux as a quality-threshold flag (0=normal).
  int32_t aux[16] = {0};
  // arg3 is a coating work buffer pointer (core memsets it); must be valid & sized.
  static uint8_t cbuf[CBUF_SZ];
  int32_t qcov[4] = {0};   // {quality, coverage}
  int rc = pp(&src, aux, cbuf, dst, qcov, 0, 0);
  if (cov) *cov = qcov[1];
  if (qual) *qual = qcov[0];
  return rc;
}

// number of registered feature entries in a template holder (0 if empty/NULL)
static int tmpl_count(void *holder) {
  if (!holder) return 0;
  char *T = *(char **)holder;
  if (!T) return 0;
  return *(int *)(T + 0x1c);
}

// enroll proc[idx[0..n)] -> live template holder (returned); NULL on failure
static void *make_template(const int *idx, int n) {
  int maxt = 16;
  void *sess = estart(&maxt);
  if (!sess) return NULL;
  int added = 0;
  for (int i = 0; i < n; i++) {
    // reuse the exact dst gimg the preprocessor produced (carries its fields)
    int o3 = 0, o4 = 0, o6 = 0;
    int rc = eadd(sess, &gimgs[idx[i]], &o3, &o4, 0, &o6);
    if (rc == 0) added++;
    else if (dbg_enroll && i < 6)
      printf("  enrolAddImage[%d] rc=%d (o3=%d o4=%d o6=%d)\n", i, rc, o3, o4, o6);
  }
  if (!added) return NULL;
  void *tpl = NULL;
  eget(sess, &tpl);
  return tpl;
}

int main(int argc, char **argv) {
  int enroll_n = argc > 1 ? atoi(argv[1]) : 15;
  int ppp_code = argc > 2 ? atoi(argv[2]) : 10;
  g_col = argc > 3 ? atoi(argv[3]) : W;
  g_row = argc > 4 ? atoi(argv[4]) : H;
  int calib_mode = argc > 5 ? atoi(argv[5]) : 0;

  winpe_set_verbose(0);
  if (!winpe_load("../../win-driver/AlgoMilan.dll")) return 1;
  ppp = winpe_getproc("ppp_param_init");
  initcali = winpe_getproc("preprocess_init_calidata");
  ppinit = winpe_getproc("preprocessor_init");
  pp = winpe_getproc("preprocessor");
  ppexit = winpe_getproc("preprocessor_exit");
  setmode = winpe_getproc("preprocess_set_mode");
  estart = winpe_getproc("enrolStartEx");
  eadd = winpe_getproc("enrolAddImage");
  eget = winpe_getproc("enrolGetTemplate");
  idt = winpe_getproc("identifytemplate");
  if (!ppp || !initcali || !ppinit || !pp || !estart || !eadd || !eget || !idt) {
    printf("missing export: ppp=%p init=%p ppinit=%p pp=%p es=%p ea=%p eg=%p idt=%p\n",
           (void*)ppp,(void*)initcali,(void*)ppinit,(void*)pp,(void*)estart,
           (void*)eadd,(void*)eget,(void*)idt);
    return 1;
  }

  // load raw dataset
  FILE *f = fopen("frames_raw.bin", "rb");
  if (!f) { perror("frames_raw.bin"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  nrec = sz / (PX * 2);
  raw = malloc(sz); if (fread(raw, 1, sz, f) != (size_t)sz) return 1; fclose(f);
  f = fopen("bg_raw.bin", "rb");
  bg = malloc(sz); if (fread(bg, 1, sz, f) != (size_t)sz) return 1; fclose(f);
  FILE *m = fopen("meta_raw.txt", "r"); int tmp;
  for (int i = 0; i < nrec; i++)
    if (fscanf(m, "%15s %d", labels[i], &tmp) != 2) break;
  fclose(m);
  proc = malloc((size_t)nrec * PX);

  // ---- INIT pipeline ----
  printf("ppp_param_init(%d) -> %d\n", ppp_code, ppp(ppp_code));
  printf("preprocess_init_calidata() -> %d\n", initcali());

  // indices by label
  int gen[200], ngen = 0, nat[200], nnat = 0, imp[200], nimp = 0;
  for (int i = 0; i < nrec; i++) {
    if (!strcmp(labels[i], "genuine")) gen[ngen++] = i;
    else if (!strcmp(labels[i], "natural")) nat[nnat++] = i;
    else imp[nimp++] = i;
  }
  printf("records: genuine=%d natural=%d impostor=%d; enroll_n=%d col=%d row=%d mode=%d\n",
         ngen, nnat, nimp, enroll_n, g_col, g_row, calib_mode);

  if (calib_mode == 1) {
    if (setmode) printf("preprocess_set_mode(1) -> %d\n", setmode(1));
  } else {
    // calibrate with the first natural background (flat-field reference)
    uint8_t cal[0x40]; memset(cal, 0, sizeof cal);
    *(void **)(cal + 0x18) = bg + (size_t)nat[0] * PX;
    *(uint32_t *)(cal + 0x24) = (uint32_t)g_col;
    *(uint32_t *)(cal + 0x28) = (uint32_t)g_row;
    printf("preprocessor_init(&cal{bg=nat[%d],col=%d,row=%d}) -> %d\n",
           nat[0], g_col, g_row, ppinit(cal));
  }

  // preprocess everything, report a few
  int okc = 0;
  for (int i = 0; i < nrec; i++) {
    int cov = -1, q = -1, rc = preprocess_one(i, &cov, &q);
    if (rc == 0) okc++;
    if (i < 5 || (i >= nat[0] && i < nat[0] + 3))
      printf("  pp rec %d (%s): rc=%d cov=%d qual=%d\n", i, labels[i], rc, cov, q);
  }
  printf("preprocessor ok on %d/%d frames\n", okc, nrec);
  { FILE *pf = fopen("proc8.bin", "wb"); if (pf) { fwrite(proc, 1, (size_t)nrec * PX, pf); fclose(pf); } }
  if (okc == 0) { printf("preprocessing failed for all frames; aborting\n"); return 3; }

  // ---- enroll gallery from first enroll_n natural ----
  void *ref = make_template(nat, enroll_n);
  printf("gallery template = %p\n", ref);
  if (!ref) { printf("enroll failed\n"); return 2; }
  {
    char *T = *(char **)ref;   // holder -> object
    printf("  T=%p  nCurrent[+0x1c]=%d  nMax[+0x20]=%d\n", (void*)T,
           *(int *)(T + 0x1c), *(int *)(T + 0x20));
    for (int i = 0; i < 4; i++)
      printf("    entry[%d] @+0x28+%d = %p\n", i, i * 8,
             *(void **)(T + 0x28 + i * 8));
  }

  int refc = tmpl_count(ref);
  printf("gallery entries = %d\n\n", refc);

  int gp = 0, gm = 0, gu = 0, ip = 0, im = 0, iu = 0;
  for (int i = enroll_n; i < nnat; i++) {
    int one = nat[i]; void *probe = make_template(&one, 1);
    if (tmpl_count(probe) == 0) { gu++; continue; }  // unusable capture
    if (i == enroll_n) {          // one-shot: inspect first usable probe
      char *T = *(char **)probe;
      printf("  PROBE nat[%d]: T=%p nCur=%d nMax=%d entry[0]=%p entry[1]=%p\n",
             i, (void*)T, *(int*)(T+0x1c), *(int*)(T+0x20),
             *(void**)(T+0x28), *(void**)(T+0x30));
      fflush(stdout);
    }
    int idx = -2; idt(ref, probe, NULL, &idx);
    if (idx >= 0) gm++; else gp++;
  }
  for (int i = 0; i < nimp; i++) {
    int one = imp[i]; void *probe = make_template(&one, 1);
    if (tmpl_count(probe) == 0) { iu++; continue; }
    int idx = -2; idt(ref, probe, NULL, &idx);
    if (idx >= 0) im++; else ip++;
  }
  int gtot = gm + gp, itot = im + ip;
  printf("\nGENUINE : %d/%d matched (FRR %.1f%%)  [%d unusable]\n", gm, gtot,
         gtot ? 100.0 * gp / gtot : 0, gu);
  printf("IMPOSTOR: %d/%d matched (FAR %.1f%%)  [%d unusable]\n", im, itot,
         itot ? 100.0 * im / itot : 0, iu);
  if (ppexit) ppexit();
  return 0;
}
