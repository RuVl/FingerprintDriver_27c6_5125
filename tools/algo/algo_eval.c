// Matcher-only accuracy eval: enroll N genuine frames into one AlgoMilan
// template, then classify every probe with identifytemplate (vendor decision).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winpe.h"

#define MS __attribute__((ms_abi))
#define W 64
#define H 80
#define PX (W * H)

#pragma pack(push, 1)
struct gimg {
  void *data; uint16_t f08, width, height; uint8_t bits, f0f;
  uint8_t pad10[4]; uint32_t frames; uint16_t chan; uint8_t pad1a[32];
};
#pragma pack(pop)

typedef int MS (*ppp_t)(int);
typedef void *MS (*estart_t)(int *);
typedef int MS (*eadd_t)(void *, struct gimg *, void *, void *, uint8_t, void *);
typedef int MS (*efin_t)(void *);
typedef int MS (*eget_t)(void *, void **);
typedef int MS (*idt_t)(void **, void **, void *, int *);
typedef int MS (*study_t)(void *);

static ppp_t ppp; static estart_t estart; static eadd_t eadd;
static efin_t efin; static eget_t eget; static idt_t idt; static study_t study;
static uint8_t *frames; static int nframes;
static char labels[200][16]; static int lindex[200];

static void set_img(struct gimg *img, uint8_t *px) {
  memset(img, 0, sizeof *img);
  img->data = px; img->f08 = H; img->width = W; img->height = H;
  img->bits = 8; img->f0f = 1; img->frames = 1; img->chan = 1;
}

// enroll frames[idx[0..n)] -> template object (returned); 0 on failure
static void *make_template(const int *idx, int n, int do_study) {
  int maxt = 16;
  void *sess = estart(&maxt);
  if (!sess) return NULL;
  int added = 0;
  for (int i = 0; i < n; i++) {
    struct gimg img; set_img(&img, frames + (size_t)idx[i] * PX);
    int o3 = 0, o4 = 0, o6 = 0;
    if (eadd(sess, &img, &o3, &o4, 0, &o6) == 0) added++;
  }
  if (added == 0) return NULL;
  void *tpl = NULL;
  eget(sess, &tpl);
  if (tpl && do_study && study) study(tpl);   // finalize gallery only
  return tpl;
}

int main(int argc, char **argv) {
  int enroll_n = argc > 1 ? atoi(argv[1]) : 15;
  winpe_set_verbose(0);
  if (!winpe_load("../../win-driver/AlgoMilan.dll")) return 1;
  ppp = winpe_getproc("ppp_param_init");
  estart = winpe_getproc("enrolStartEx");
  eadd = winpe_getproc("enrolAddImage");
  efin = winpe_getproc("enrolFinish");
  eget = winpe_getproc("enrolGetTemplate");
  idt = winpe_getproc("identifytemplate");
  study = (study_t)((unsigned char*)winpe_base() + 0xf810);
  ppp(3);

  FILE *f = fopen("frames.bin", "rb");
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  nframes = sz / PX;
  frames = malloc(sz);
  if (fread(frames, 1, sz, f) != (size_t)sz) return 1;
  fclose(f);
  FILE *m = fopen("meta.txt", "r");
  for (int i = 0; i < nframes; i++)
    if (fscanf(m, "%15s %d", labels[i], &lindex[i]) != 2) break;
  fclose(m);

  // indices by label
  int gen[200], ngen = 0, nat[200], nnat = 0, imp[200], nimp = 0;
  for (int i = 0; i < nframes; i++) {
    if (!strcmp(labels[i], "genuine")) gen[ngen++] = i;
    else if (!strcmp(labels[i], "natural")) nat[nnat++] = i;
    else imp[nimp++] = i;
  }
  printf("frames: genuine(varied)=%d natural=%d impostor=%d; enroll_n=%d\n",
         ngen, nnat, nimp, enroll_n);

  // enroll from first enroll_n natural, probe the rest of natural + all impostor
  void *tplA = make_template(nat, enroll_n, 1);
  printf("enrolled template A = %p\n", tplA);
  if (!tplA) { printf("enroll failed\n"); return 2; }

  int gp = 0, gm = 0, ip = 0, im = 0;
  for (int i = enroll_n; i < nnat; i++) {
    int one = nat[i];
    void *tplB = make_template(&one, 1, 0);
    int idx = -2;
    if (tplB) idt(&tplB, &tplA, NULL, &idx);
    if (idx >= 0) gm++; else gp++;
    printf("genuine probe nat[%d] -> idx %d %s\n", i, idx,
           idx >= 0 ? "MATCH" : "reject");
  }
  for (int i = 0; i < nimp; i++) {
    int one = imp[i];
    void *tplB = make_template(&one, 1, 0);
    int idx = -2;
    if (tplB) idt(&tplB, &tplA, NULL, &idx);
    if (idx >= 0) im++; else ip++;
  }
  int gtot = gm + gp, itot = im + ip;
  printf("\nGENUINE: %d/%d matched (FRR %.0f%%)\n", gm, gtot,
         gtot ? 100.0 * gp / gtot : 0);
  printf("IMPOSTOR: %d/%d matched (FAR %.0f%%)\n", im, itot,
         itot ? 100.0 * im / itot : 0);
  return 0;
}
