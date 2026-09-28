// Vendor-exact offline eval of the Goodix matcher for 27c6:5125.
// Replays EngineAdapter's calls as recovered in tools/algo/re/notes/30-engine-flow.md
// and 20-template-identify.md (canonical pipeline: 00-overview.md):
//   * chip id 0x2504 -> sensorType 0xc -> AlgoChicago.dll + ppp_param_init(12)
//     (AlgoMilan/10 kept selectable for comparison);
//   * frame transposed to 64 rows x 80 cols; gimg +0x08 = width 80, +0x0a = height 64;
//   * preprocessor(&src, &purpose, cbuf, &dst, qcov, 0, 0), purpose 1=enroll 0=verify;
//     cbuf (0x4c98 bytes) is per frame and is handed on to enrolAddImage / identifyImage;
//   * enrolAddImage(session, &dst, cbuf, raw16, 0, qcov);
//   * identifyImage(&dst, cbuf, &holder, 1, &idx, &score, cq, flag=0, study=1, raw16, 0),
//     match <=> rc==0 && idx>=0 && score>0;
//   * each probe is matched against a FRESH unpacked copy of the gallery (identifyImage
//     mutates the template), so results do not depend on probe order.
//
// Usage: ./algo_eval4 [enroll_n=15]
//   env ALGO=chicago|milan (default chicago), PPP=<profile> (default 12 / 10),
//       ALGOLOG=1 (vendor log to stderr), VERBOSE=1 (per-probe lines),
//       STUDY=1 (adaptive update: templateStudy + repack after each genuine match),
//       DUMP_PROC=<file> (every preprocessor output; also <file>.cbuf with cbuf[0..15]),
//       TPL_OUT=<file> (packed gallery, cmp_tpl.py), ETRACE=<file> (template T after every
//       enrolAddImage: sub count, group_state, relation table; docs/stage4-enroll.md).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "algolog.h"
#include "winpe.h"

#define MS __attribute__((ms_abi))
#define SW 64    // our buffer: SH rows of SW pixels
#define SH 80
#define PX (SW * SH)   // 5120
#define IW 80    // vendor image: IH rows of IW pixels (transposed)
#define IH 64
#define MAXREC 256
#define CBUF_SZ 0x4d00   // >= 0x4c98, memset by the preprocessor core

#pragma pack(push, 1)
struct gimg {
  void *data;             // +0x00
  uint16_t width;         // +0x08
  uint16_t height;        // +0x0a
  uint16_t d0c;           // +0x0c
  uint8_t bits;           // +0x0e  16 (src) / 8 (dst)
  uint8_t ch;             // +0x0f  1
  uint16_t d10, d12;      // +0x10
  uint32_t size;          // +0x14  bytes
  uint16_t frames;        // +0x18  1
  uint16_t d1a;           // +0x1a
  uint32_t sensor_type;   // +0x1c  0 (EngineAdapter ctx[0x32c])
  uint8_t d20[8];         // +0x20
  uint8_t cov_q[8];       // +0x28 (written by the preprocessor)
  uint8_t tail[0x10];
};
#pragma pack(pop)

typedef int MS (*ppp_t)(int);
typedef int MS (*v_t)(void);
typedef int MS (*ppinit_t)(void *);
typedef int MS (*pp_t)(struct gimg *, int *, void *, struct gimg *, int *, uint8_t, uint8_t);
typedef void *MS (*estart_t)(int *);
typedef int MS (*eadd_t)(void *, struct gimg *, void *, void *, uint8_t, int *);
typedef int MS (*eget_t)(void *, void **);
typedef int MS (*efin_t)(void *);
typedef int MS (*idi_t)(struct gimg *, void *, void **, int, int *, int *, int *, int,
                        uint8_t, void *, uint8_t);
typedef int MS (*tsize_t)(void *);
typedef int MS (*tpack_t)(void *, void *);
typedef int MS (*tunpack_t)(void *, int, void *, void **);
typedef int MS (*tdel_t)(void *);
typedef int MS (*tstudy_t)(int *);

static ppp_t ppp; static v_t initcali, ppexit; static pp_t pp;
static estart_t estart; static eadd_t eadd; static eget_t eget; static efin_t efin;
static idi_t idi; static tsize_t tsize; static tpack_t tpack; static tunpack_t tunpack;
static tdel_t tdel; static tstudy_t tstudy;

static uint16_t *raw, *bg;                 // [nrec][PX], transposed to IH x IW
static uint8_t *proc;                      // [nrec][PX]
static uint8_t *cbufs;                     // [nrec][CBUF_SZ]
static struct gimg dsts[MAXREC];
static int qcovs[MAXREC][2];               // {coverage, quality}
static int ppc[MAXREC];
static char labels[MAXREC][16];
static int nrec;
static int last_idx, last_rc;   // of the latest identifyImage call
static void *live_gallery;   // NOUNPACK=1: match against the live enrolled holder
static int opt(const char *n) { return getenv(n) != NULL; }

static void *need(const char *n) {
  void *p = winpe_getproc(n);
  if (!p) { fprintf(stderr, "missing export %s\n", n); exit(1); }
  return p;
}

static void transpose16(uint16_t *f) {   // SH x SW -> SW x SH (= IH x IW)
  static uint16_t t[PX];
  for (int r = 0; r < SH; r++)
    for (int c = 0; c < SW; c++) t[c * SH + r] = f[r * SW + c];
  memcpy(f, t, sizeof t);
}

static void set_hdr(struct gimg *g, void *data, int bits) {
  memset(g, 0, sizeof *g);
  g->data = data; g->width = IW; g->height = IH; g->bits = bits; g->ch = 1;
  g->size = PX * (bits / 8); g->frames = 1;
}

static ppinit_t ppinit;
static void calibrate(int bgrec) {  // +0x18 background frame, +0x24 ROW, +0x28 COL
  uint8_t cal[0x40]; memset(cal, 0, sizeof cal);
  *(void **)(cal + 0x18) = bg + (size_t)bgrec * PX;
  *(uint32_t *)(cal + 0x24) = IH;
  *(uint32_t *)(cal + 0x28) = IW;
  int rc = ppinit(cal);
  if (rc) { printf("preprocessor_init rc=0x%x\n", rc); exit(1); }
}

static int preprocess(int i, int purpose) {
  struct gimg src;
  if (opt("PERBG")) calibrate(i);   // flat-field with this touch's own background
  set_hdr(&src, raw + (size_t)i * PX, 16);
  set_hdr(&dsts[i], proc + (size_t)i * PX, 8);
  qcovs[i][0] = qcovs[i][1] = 0;
  if (opt("PURPOSE0")) purpose = 0;
  ppc[i] = pp(&src, &purpose, cbufs + (size_t)i * CBUF_SZ, &dsts[i], qcovs[i], 0, 0);
  // DUMP_PROC=<file>: per preprocessor call append {int32 rec, rc, quality, coverage}
  // + 5120-byte 8-bit dst (64 rows x 80), in call order (for tools/algo/cmp_proc.py).
  // Sidecar <DUMP_PROC>.cbuf: per call {int32 rec, first 16 bytes of cbuf} (the preprocessor
  // context getFeature reads; port_eval.c FFIXES injects it together with INJECT_PROC).
  static FILE *dump, *dump_cbuf; static int dump_init;
  if (!dump_init) {
    dump_init = 1;
    if (getenv("DUMP_PROC")) {
      dump = fopen(getenv("DUMP_PROC"), "wb");
      char path[4096]; snprintf(path, sizeof path, "%s.cbuf", getenv("DUMP_PROC"));
      dump_cbuf = fopen(path, "wb");
    }
  }
  if (dump) {
    int32_t hdr[4] = {i, ppc[i], qcovs[i][1], qcovs[i][0]};
    fwrite(hdr, sizeof hdr, 1, dump); fwrite(proc + (size_t)i * PX, 1, PX, dump); fflush(dump);
  }
  if (dump_cbuf) {
    fwrite(&i, sizeof i, 1, dump_cbuf); fwrite(cbufs + (size_t)i * CBUF_SZ, 1, 16, dump_cbuf);
    fflush(dump_cbuf);
  }
  return ppc[i];
}

// enroll the given records (already preprocessed with purpose=1) -> packed blob
static uint8_t *enroll(const int *recs, int n, int *len, int *added, int *entries) {
  // EngineAdapter calls enrolStart() = enrolStartEx(&50)
  int maxt = getenv("MAXT") ? atoi(getenv("MAXT")) : 50;
  void *sess = estart(&maxt);
  if (!sess) return NULL;
  // EngineAdapter raises the session capacity right after enrolStart:
  // *(int16*)(sess+8) = samples_per_template(12) + max_tip_count(8) = 20 (notes/81).
  // Without it enrolAddImage silently stops adding after 8 frames.
  int scap = getenv("SESSCAP") ? atoi(getenv("SESSCAP")) : 20;
  if (scap > 0) *(int16_t *)((char *)sess + 8) = (int16_t)scap;
  *added = 0;
  for (int k = 0; k < n; k++) {
    int r = recs[k];
    static int o3;
    if (eadd(sess, &dsts[r], opt("EADD_O3") ? (void *)&o3 : cbufs + (size_t)r * CBUF_SZ, raw + (size_t)r * PX, 0, qcovs[r]) == 0)
      (*added)++;
    if (getenv("ETRACE")) {   // stage 4: template state after every enrolAddImage (cmp with diff)
      static FILE *et;
      if (!et) et = fopen(getenv("ETRACE"), "w");
      void *hh = NULL;
      eget(sess, &hh);
      const char *T = hh ? *(char **)hh : NULL;   // T: +0x24 n_sub, +0x2c n_rel, +0x30 sub[], +0x1c0 rel
      if (et && T) {
        int nsub = *(int *)(T + 0x24), nrel = *(int *)(T + 0x2c);
        fprintf(et, "step %d nsub %d nrel %d grp", k + 1, nsub, nrel);
        for (int j = 0; j < nsub; j++) fprintf(et, " %d", *(int *)(*(char **)(T + 0x30 + 8 * j) + 0x100));
        fprintf(et, "\n");
        for (int j = 0; j < nrel; j++) {
          const int *rl = (const int *)(T + 0x1c0 + 0x1c * j);
          fprintf(et, "  rel %d inl %d t %d %d %d %d %d %d\n", j, rl[0], rl[1], rl[2], rl[3], rl[4], rl[5], rl[6]);
        }
        fflush(et);
      }
    }
  }
  void *h = NULL;
  eget(sess, &h);
  if (opt("NOUNPACK")) live_gallery = h;
  *entries = h && *(char **)h ? *(int *)(*(char **)h + 0x1c) : 0;
  *len = h ? tsize(h) : 0;
  uint8_t *blob = *len > 0 ? malloc(*len) : NULL;
  if (blob && tpack(h, blob) != 0) { free(blob); blob = NULL; }
  if (blob && getenv("TPL_OUT")) {   // stage 4: packed gallery for cmp_tpl.py
    FILE *tf = fopen(getenv("TPL_OUT"), "wb");
    if (tf) { fwrite(blob, 1, *len, tf); fclose(tf); }
  }
  if (!opt("NOUNPACK")) efin(sess);
  return blob;
}


// verify record r against the packed gallery; returns 1 match / 0 no / -1 error.
// With study != 0 a matched gallery is updated (templateStudy) and repacked in place.
static int verify(uint8_t **blob, int *len, int r, int *score, int study) {
  void *h = NULL;
  if (opt("NOUNPACK")) h = live_gallery;
  else if (tunpack(*blob, *len, NULL, &h)   /* EngineAdapter passes NULL */ != 0 || !h) return -1;
  void *cand[1] = {h};
  int idx = -1, cq[2] = {0, 0};
  *score = 0;
  static uint8_t zparam[CBUF_SZ];
  int rc = idi(&dsts[r], opt("PARAMZERO") ? zparam : cbufs + (size_t)r * CBUF_SZ, cand, 1,
               &idx, score, cq, 0, opt("BYTE0") ? 0 : 1, raw + (size_t)r * PX, 0);
  last_idx = idx; last_rc = rc;
  int match = rc == 0 && idx >= 0 && *score > 0;
  if (match && study) {
    int upd = 0;
    tstudy(&upd);
    if (upd > 0) {
      int nl = tsize(h);
      uint8_t *nb = nl > 0 ? malloc(nl) : NULL;
      if (nb && tpack(h, nb) == 0) { free(*blob); *blob = nb; *len = nl; }
      else free(nb);
    }
  }
  if (!opt("NOUNPACK")) tdel(h);
  return rc ? -1 : match;
}

int main(int argc, char **argv) {
  int enroll_n = argc > 1 ? atoi(argv[1]) : 15;
  const char *algo = getenv("ALGO") ? getenv("ALGO") : "chicago";
  int chicago = strcmp(algo, "milan") != 0;
  int profile = getenv("PPP") ? atoi(getenv("PPP")) : chicago ? 12 : 10;
  int verbose = getenv("VERBOSE") != NULL, study = getenv("STUDY") != NULL;

  winpe_set_verbose(getenv("WINPE_V") != NULL);
  if (!winpe_load(chicago ? "../../win-driver/AlgoChicago.dll"
                          : "../../win-driver/AlgoMilan.dll")) return 1;
  if (getenv("ALGOLOG")) {
    winpe_hook(chicago ? 0x1800080e0ULL : ALGO_LOG_VA, algolog_fn());
    algolog_set(stderr, NULL);
  }
  ppp = need("ppp_param_init"); initcali = need("preprocess_init_calidata");
  ppinit = need("preprocessor_init"); pp = need("preprocessor"); ppexit = need("preprocessor_exit");
  estart = need("enrolStartEx"); eadd = need("enrolAddImage"); eget = need("enrolGetTemplate");
  efin = need("enrolFinish"); idi = need("identifyImageWrapper");
  tsize = need("templateGetPackedSize"); tpack = need("templatePack");
  tunpack = need("templateUnPack"); tdel = need("templateDelete"); tstudy = need("templateStudy");

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
  for (int i = 0; i < nrec; i++) { transpose16(raw + (size_t)i * PX); transpose16(bg + (size_t)i * PX); }
  proc = malloc((size_t)nrec * PX);
  cbufs = calloc(nrec, CBUF_SZ);

  int nat[MAXREC], nnat = 0;
  for (int i = 0; i < nrec; i++) if (!strcmp(labels[i], "natural")) nat[nnat++] = i;

  printf("algo=%s ppp=%d enroll_n=%d study=%d\n", chicago ? "AlgoChicago" : "AlgoMilan",
         profile, enroll_n, study);
  int rc = ppp(profile);
  if (rc) { printf("ppp_param_init(%d) rc=0x%x\n", profile, rc); return 1; }
  initcali();
  calibrate(nat[0]);

  // kr (gain) is learned inside preprocessor() from finger frames (notes/80): optionally
  // warm it up with WARM=<passes> passes over the whole dataset before the evaluation.
  int warm = getenv("WARM") ? atoi(getenv("WARM")) : 0;
  for (int w = 0; w < warm; w++)
    for (int i = 0; i < nrec; i++)   // WARMSET=impostor: learn gain only on other fingers
      if (!getenv("WARMSET") || !strcmp(labels[i], getenv("WARMSET"))) preprocess(i, 0);
  if (chicago) printf("chicago calib: framenum=%d\n", *(int *)0x180099d00ULL);

  // gallery: first enroll_n usable natural touches, preprocessed for enrollment
  int enr[MAXREC], ne = 0, gal_end = 0;
  for (int k = 0; k < nnat && ne < enroll_n; k++, gal_end = k)
    if (preprocess(nat[k], 1) == 0) enr[ne++] = nat[k];
  int len = 0, added = 0, entries = 0;
  uint8_t *blob = enroll(enr, ne, &len, &added, &entries);
  printf("gallery: %d offered, %d accepted, %d entries, packed %d bytes\n", ne, added, entries, len);
  if (!blob) return 2;

  // probes: all other records, preprocessed for verification
  int is_gal[MAXREC] = {0};
  for (int k = 0; k < gal_end; k++) is_gal[nat[k]] = 1;
  const char *grp[3] = {"natural", "genuine", "impostor"};
  int tot[3] = {0}, hit[3] = {0}, bad[3] = {0};
  for (int i = 0; i < nrec; i++) {
    if (is_gal[i]) continue;
    int g = !strcmp(labels[i], "natural") ? 0 : !strcmp(labels[i], "genuine") ? 1 : 2;
    if (preprocess(i, 0) != 0) { bad[g]++; continue; }
    int score, res = verify(&blob, &len, i, &score, study && g < 2);
    if (res < 0) { bad[g]++; continue; }
    tot[g]++; hit[g] += res;
    if (verbose || (g == 2 && res))
      printf("  %-8s %3d q=%3d c=%3d d28=%d d29=%d score=%5d idx=%2d rc=0x%x %s\n", labels[i], i, qcovs[i][1],
             qcovs[i][0], dsts[i].cov_q[0], dsts[i].cov_q[1], score, last_idx, last_rc, res ? "MATCH" : "-");
  }
  if (chicago) printf("chicago calib after eval: framenum=%d\n", *(int *)0x180099d00ULL);
  for (int g = 0; g < 3; g++)
    printf("%-8s: %3d/%3d matched (%5.1f%%)  [%d unusable]\n", grp[g], hit[g], tot[g],
           tot[g] ? 100.0 * hit[g] / tot[g] : 0, bad[g]);
  int gt = tot[0] + tot[1], gh = hit[0] + hit[1];
  printf("FRR(genuine all) %.1f%%  FAR %.2f%%\n", gt ? 100.0 * (gt - gh) / gt : 0,
         tot[2] ? 100.0 * hit[2] / tot[2] : 0);
  ppexit();
  free(blob);
  return 0;
}
