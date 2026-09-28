// Vendor-pipeline matcher eval, v3 (superseded by algo_eval4.c; notes: tools/algo/re/notes/00-overview.md).
// Differences from algo_eval2:
//  * image layout is selectable. AlgoMilan treats gimg +0x08 as ROWS (height)
//    and +0x0a as COLS (stride); profile 10 is SENSOR_ROW 64 x SENSOR_COL 80.
//    Our buffer is 80 rows x 64 cols, so algo_eval2 (d08=64,d0a=80) handed the
//    algorithm a row-scrambled image.
//      layout 0: as algo_eval2 (d08=64, d0a=80, buffer untouched)   -- wrong
//      layout 1: honest dims  (d08=80, d0a=64, buffer untouched)
//      layout 2: transpose buffer to 64 rows x 80 cols, d08=64, d0a=80
//  * quality/coverage read correctly (qcov = {coverage, quality}).
//  * matching via identifyImage (vendor verify path): prints matcher ratio;
//    ratio > 0 => match. identifytemplate kept for comparison.
//
// Usage: ./algo_eval3 [layout=2] [enroll_n=15] [minq=0]
//   env ALGOLOG=1 routes AlgoMilan's own log to stderr.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "algolog.h"
#include "winpe.h"

#define MS __attribute__((ms_abi))
#define SW 64    // our buffer: SH rows of SW pixels
#define SH 80
#define PX (SW * SH)   // 5120
#define MAXREC 256

#pragma pack(push, 1)
struct gimg {              // offsets verified against enrolAddImage + preprocessor
  void *data;             // +0x00
  uint16_t rows;          // +0x08  height
  uint16_t cols;          // +0x0a  width == stride
  uint16_t d0c;           // +0x0c
  uint8_t bits;           // +0x0e  must be 8 (enrol/identify)
  uint8_t one0f;          // +0x0f  must be 1
  uint16_t d10, d12;      // +0x10 +0x12
  uint32_t len14;         // +0x14  byte length for preprocessor
  uint16_t chan18;        // +0x18  must be != 0
  uint8_t tail[0x2e];     // +0x1a.. coverage@+0x28, quality@+0x29 written by pp
};
#pragma pack(pop)

typedef int MS (*ppp_t)(int);
typedef int MS (*initcali_t)(void);
typedef int MS (*ppinit_t)(void *);
typedef int MS (*pp_t)(struct gimg *, void *, void *, struct gimg *, void *,
                       uint8_t, uint8_t);
#define CBUF_SZ 0x4d00
typedef int MS (*ppexit_t)(void);
typedef void *MS (*estart_t)(int *);
typedef int MS (*eadd_t)(void *, struct gimg *, void *, void *, uint8_t, void *);
typedef int MS (*eget_t)(void *, void **);
typedef int MS (*idt_t)(void *, void *, void *, int *);
// identifyImage(img, param, candidates(holder[]), nCand, pIdx, pRatio, qc[2],
//               flag, byte, ptr(unused), byte(unused))
typedef int MS (*idi_t)(struct gimg *, void *, void **, int, int *, int *,
                        int *, int, uint8_t, void *, uint8_t);

static ppp_t ppp; static initcali_t initcali; static ppinit_t ppinit;
static pp_t pp; static ppexit_t ppexit; static estart_t estart;
static eadd_t eadd; static eget_t eget; static idt_t idt; static idi_t idi;

static int layout = 2;
static uint16_t *raw, *bg;        // [nrec][PX], already in the chosen layout
static uint8_t *proc;             // [nrec][PX]
static struct gimg gimgs[MAXREC];
static int ppc[MAXREC], cov[MAXREC], qual[MAXREC];
static char labels[MAXREC][16];
static int nrec;

static void transpose16(uint16_t *f) {   // SH x SW  ->  SW x SH
  static uint16_t t[PX];
  for (int r = 0; r < SH; r++)
    for (int c = 0; c < SW; c++) t[c * SH + r] = f[r * SW + c];
  memcpy(f, t, sizeof t);
}

static int preprocess_one(int idx) {
  struct gimg src, *dst = &gimgs[idx];
  memset(&src, 0, sizeof src);
  memset(dst, 0, sizeof *dst);
  src.data = raw + (size_t)idx * PX; src.len14 = 2 * PX;
  dst->data = proc + (size_t)idx * PX; dst->len14 = PX;
  if (layout == 1) { dst->rows = SH; dst->cols = SW; }
  else if (layout == 3) { dst->rows = 80; dst->cols = 64; }  // +0x08=width 80, +0x0a=height 64
  else { dst->rows = 64; dst->cols = 80; }
  dst->bits = 8; dst->one0f = 1; dst->chan18 = 1;
  int32_t aux[16] = {0};
  static uint8_t cbuf[CBUF_SZ];
  int32_t qcov[4] = {0};   // {coverage, quality} (checked against the algo log)
  int rc = pp(&src, aux, cbuf, dst, qcov, 0, 0);
  cov[idx] = qcov[0]; qual[idx] = qcov[1];
  return rc;
}

static int tmpl_count(void *holder) {
  if (!holder || !*(char **)holder) return 0;
  return *(int *)(*(char **)holder + 0x1c);
}

static void *make_template(const int *idx, int n, int *added_out) {
  int maxt = 16;
  void *sess = estart(&maxt);
  if (!sess) return NULL;
  int added = 0;
  for (int i = 0; i < n; i++) {
    int o3 = 0, o4 = 0, o6 = 0;
    if (eadd(sess, &gimgs[idx[i]], &o3, &o4, 0, &o6) == 0) added++;
  }
  if (added_out) *added_out = added;
  void *tpl = NULL;
  eget(sess, &tpl);
  return tpl;
}

// identifyImage probe vs one gallery holder -> ratio (idx>=0 means match)
static int verify(void *gallery, int rec, int *ratio) {
  if (getenv("MARK")) { printf("@VERIFY %d %s\n", rec, labels[rec]); fflush(stdout); }
  void *cand[1] = {gallery};
  int idx = -2, r = 0, qc[2] = {0, 0};
  uint8_t param[256] = {0};
  int rc = idi(&gimgs[rec], param, cand, 1, &idx, &r, qc, 0, 0, NULL, 0);
  *ratio = r;
  return rc == 0 ? idx : -100 - rc;
}

int main(int argc, char **argv) {
  layout = argc > 1 ? atoi(argv[1]) : 2;
  int enroll_n = argc > 2 ? atoi(argv[2]) : 15;
  int minq = argc > 3 ? atoi(argv[3]) : 0;

  winpe_set_verbose(0);
  if (!winpe_load("../../win-driver/AlgoMilan.dll")) return 1;
  if (getenv("ALGOLOG")) { winpe_hook(ALGO_LOG_VA, algolog_fn()); algolog_set(stderr, NULL); }
  ppp = winpe_getproc("ppp_param_init");
  initcali = winpe_getproc("preprocess_init_calidata");
  ppinit = winpe_getproc("preprocessor_init");
  pp = winpe_getproc("preprocessor");
  ppexit = winpe_getproc("preprocessor_exit");
  estart = winpe_getproc("enrolStartEx");
  eadd = winpe_getproc("enrolAddImage");
  eget = winpe_getproc("enrolGetTemplate");
  idt = winpe_getproc("identifytemplate");
  idi = winpe_getproc("identifyImageWrapper");
  if (!ppp || !initcali || !ppinit || !pp || !estart || !eadd || !eget || !idt || !idi) {
    printf("missing export\n"); return 1;
  }

  FILE *f = fopen("frames_raw.bin", "rb");
  if (!f) { perror("frames_raw.bin"); return 1; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  nrec = sz / (PX * 2);
  if (nrec > MAXREC) nrec = MAXREC;
  raw = malloc(sz); if (fread(raw, 1, sz, f) != (size_t)sz) return 1; fclose(f);
  f = fopen("bg_raw.bin", "rb");
  bg = malloc(sz); if (fread(bg, 1, sz, f) != (size_t)sz) return 1; fclose(f);
  FILE *m = fopen("meta_raw.txt", "r"); int tmp;
  for (int i = 0; i < nrec; i++)
    if (fscanf(m, "%15s %d", labels[i], &tmp) != 2) break;
  fclose(m);
  proc = malloc((size_t)nrec * PX);
  if (layout == 2 || layout == 3)
    for (int i = 0; i < nrec; i++) { transpose16(raw + (size_t)i * PX); transpose16(bg + (size_t)i * PX); }

  int gen[MAXREC], ngen = 0, nat[MAXREC], nnat = 0, imp[MAXREC], nimp = 0;
  for (int i = 0; i < nrec; i++) {
    if (!strcmp(labels[i], "genuine")) gen[ngen++] = i;
    else if (!strcmp(labels[i], "natural")) nat[nnat++] = i;
    else imp[nimp++] = i;
  }
  printf("layout=%d enroll_n=%d minq=%d  genuine=%d natural=%d impostor=%d\n",
         layout, enroll_n, minq, ngen, nnat, nimp);

  ppp(10);
  initcali();
  {  // +0x18 calib frame, +0x24 ROW, +0x28 COL (per the PPLIB log line)
    uint8_t cal[0x40]; memset(cal, 0, sizeof cal);
    *(void **)(cal + 0x18) = bg + (size_t)nat[0] * PX;
    *(uint32_t *)(cal + 0x24) = 64;
    *(uint32_t *)(cal + 0x28) = 80;
    int rc = ppinit(cal);
    if (rc) { printf("preprocessor_init rc=%d\n", rc); return 1; }
  }

  int okc = 0; double qs = 0;
  for (int i = 0; i < nrec; i++) {
    ppc[i] = preprocess_one(i);
    if (ppc[i] == 0) { okc++; qs += qual[i]; }
  }
  printf("preprocessor ok %d/%d, mean quality %.1f\n", okc, nrec, okc ? qs / okc : 0);
  printf("quality natural:");
  for (int i = 0; i < nnat; i++) printf(" %d", qual[nat[i]]);
  printf("\n");
  { FILE *pf = fopen("proc8_v3.bin", "wb"); if (pf) { fwrite(proc, 1, (size_t)nrec * PX, pf); fclose(pf); } }

  if (getenv("SHIFT")) {   // robustness: enrolled frame vs shifted/rotated copy
    static uint8_t syn[PX];
    struct gimg sg;
    int base[6], nb = 0;
    for (int i = 0; i < nnat && nb < 6; i++) if (ppc[nat[i]] == 0 && qual[nat[i]] > 80) base[nb++] = nat[i];
    const int dxs[] = {0, 1, 2, 3, 5, 8, 0, 0, 0, 0, 0, 0}, dys[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const double angs[] = {0, 0, 0, 0, 0, 0, 1, 2, 3, 6, 0, 0};
    for (int b = 0; b < nb; b++) {
      void *ta = make_template(&base[b], 1, NULL);
      uint8_t *src = proc + (size_t)base[b] * PX;
      printf("base rec %d:", base[b]);
      for (int v = 0; v < 12; v++) {
        double a = angs[v] * 3.14159265 / 180, ca = cos(a), sa = sin(a);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 80; x++) {
          double cx = x - 39.5, cy = y - 31.5;
          double sx = ca * cx + sa * cy + 39.5 - dxs[v], sy = -sa * cx + ca * cy + 31.5 - dys[v];
          if (v == 10) { sx = 79 - x; sy = 63 - y; }        // exact 180
          if (v == 11) { sx = 79 - x; sy = y; }             // mirror x
          int x0 = (int)floor(sx), y0 = (int)floor(sy);
          double fx = sx - x0, fy = sy - y0, acc = 0;
          for (int q = 0; q < 4; q++) {
            int xx = x0 + (q & 1), yy = y0 + (q >> 1);
            double w = ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy);
            acc += w * ((xx >= 0 && xx < 80 && yy >= 0 && yy < 64) ? src[yy * 80 + xx] : 255);
          }
          syn[y * 80 + x] = (uint8_t)lround(acc);
        }
        sg = gimgs[base[b]]; sg.data = syn;
        int maxt = 16; void *sess = estart(&maxt); int o3 = 0, o4 = 0, o6 = 0;
        int rc = eadd(sess, &sg, &o3, &o4, 0, &o6); void *tb = NULL; eget(sess, &tb);
        int idx = -2; if (!rc && tmpl_count(tb)) idt(ta, tb, NULL, &idx);
        if (getenv("VIA_IDI")) {   // vendor verify path: image vs template
          void *cand[1] = {ta}; int ii = -2, rr = 0, qc[2]; uint8_t prm[256] = {0};
          rc = idi(&sg, prm, cand, 1, &ii, &rr, qc, 0, 0, NULL, 0); idx = rc ? -3 : ii;
        }
        printf("  %s%d/r%.0f:%s", v == 10 ? "R180" : v == 11 ? "FLIP" : "d", dxs[v], angs[v], rc ? "rej" : idx >= 0 ? "OK" : "--");
      }
      printf("\n");
    }
    return 0;
  }
  if (getenv("PAIRS")) {   // all-pairs single-frame template matching
    int set[MAXREC], n = 0;
    for (int i = 0; i < nrec; i++)
      if (ppc[i] == 0 && strcmp(labels[i], "impostor")) set[n++] = i;
    static void *t1[MAXREC];
    for (int a = 0; a < n; a++) t1[a] = make_template(&set[a], 1, NULL);
    int gm = 0, gt = 0;
    FILE *pm = fopen("pairs_genuine.txt", "w");
    for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
      if (a == b || !tmpl_count(t1[a]) || !tmpl_count(t1[b])) continue;
      int idx = -2; idt(t1[a], t1[b], NULL, &idx);
      gt++; if (idx >= 0) { gm++; fprintf(pm, "%d %d\n", set[a], set[b]); }
    }
    fclose(pm);
    // impostor pairs: genuine template vs impostor frame
    int im = 0, it = 0;
    for (int a = 0; a < n; a++) for (int k = 0; k < nimp; k++) {
      int r = imp[k]; if (ppc[r]) continue;
      void *pt = make_template(&r, 1, NULL);
      if (!tmpl_count(t1[a]) || !tmpl_count(pt)) continue;
      int idx = -2; idt(t1[a], pt, NULL, &idx); it++; if (idx >= 0) im++;
    }
    printf("PAIRS genuine same-finger: %d/%d matched (%.2f%%); impostor: %d/%d (%.3f%%)\n",
           gm, gt, 100.0 * gm / gt, im, it, 100.0 * im / it);
    return 0;
  }

  // gallery from the first enroll_n natural records passing the quality floor
  int enr[MAXREC], ne = 0, last = 0;
  for (int i = 0; i < nnat && ne < enroll_n; i++, last = i)
    if (ppc[nat[i]] == 0 && qual[nat[i]] >= minq) enr[ne++] = nat[i];
  int added = 0;
  void *gal = make_template(enr, ne, &added);
  printf("gallery: %d frames offered, %d accepted, %d entries\n", ne, added, tmpl_count(gal));
  if (!gal || !tmpl_count(gal)) return 2;
  printf("gallery T type[+0]=%d [+4]=%d [+8]=%d [+0xc]=%d\n", **(int **)gal,
         (*(int **)gal)[1], (*(int **)gal)[2], (*(int **)gal)[3]);

  // control: probes that are themselves in the gallery must match
  printf("-- self-match (enrolled frames) --\n");
  for (int k = 0; k < ne && k < 5; k++) {
    int ratio, idx = verify(gal, enr[k], &ratio);
    int one = enr[k], didx = -2; void *pt = make_template(&one, 1, NULL);
    if (tmpl_count(pt)) idt(gal, pt, NULL, &didx);
    printf("  self rec %3d q=%2d : ratio=%d idx=%d | idt=%d\n", enr[k], qual[enr[k]], ratio, idx, didx);
  }
  if (getenv("SELFONLY")) return 0;

  // probes: remaining natural + all genuine (same finger), impostor
  int gm = 0, gt = 0, im = 0, it = 0, gu = 0, iu = 0;
  int tm = 0, tt = 0;   // identifytemplate on same genuine probes
  printf("\n-- genuine probes (label rec q c : ratio idx | idt) --\n");
  for (int pass = 0; pass < 2; pass++) {
    int *set = pass ? gen : nat, n = pass ? ngen : nnat, from = pass ? 0 : last;
    for (int k = from; k < n; k++) {
      int r = set[k];
      if (ppc[r] != 0) { gu++; continue; }
      int ratio, idx = verify(gal, r, &ratio);
      gt++; if (idx >= 0) gm++;
      int one = r, didx = -2; void *pt = make_template(&one, 1, NULL);
      if (getenv("MARK")) { printf("@IDT %d %s\n", r, labels[r]); fflush(stdout); }
      if (tmpl_count(pt)) { tt++; idt(gal, pt, NULL, &didx); if (didx >= 0) tm++; }
      printf("  %-8s %3d q=%2d c=%3d : ratio=%6d idx=%3d | idt=%d\n",
             labels[r], r, qual[r], cov[r], ratio, idx, didx);
    }
  }
  int ihist[8] = {0};
  for (int k = 0; k < nimp; k++) {
    int r = imp[k];
    if (ppc[r] != 0) { iu++; continue; }
    int ratio, idx = verify(gal, r, &ratio);
    it++; if (idx >= 0) { im++; printf("  IMPOSTOR MATCH rec %d ratio %d\n", r, ratio); }
    ihist[ratio <= 0 ? 0 : ratio < 10 ? 1 : ratio < 100 ? 2 : 3]++;
  }
  printf("\nidentifyImage   GENUINE : %d/%d matched (FRR %.1f%%) [%d unusable]\n",
         gm, gt, gt ? 100.0 * (gt - gm) / gt : 0, gu);
  printf("identifyImage   IMPOSTOR: %d/%d matched (FAR %.1f%%) [%d unusable]\n",
         im, it, it ? 100.0 * im / it : 0, iu);
  printf("identifytemplate GENUINE: %d/%d matched\n", tm, tt);
  printf("impostor ratio hist: <=0:%d 1-9:%d 10-99:%d >=100:%d\n",
         ihist[0], ihist[1], ihist[2], ihist[3]);
  if (ppexit) ppexit();
  return 0;
}
