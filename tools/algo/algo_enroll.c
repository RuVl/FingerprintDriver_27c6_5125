// First matcher-only gate: load AlgoMilan via winpe, init PPLIB, enroll one
// processed frame and fetch a template — verify it runs without crashing.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winpe.h"

#define MS __attribute__((ms_abi))
#define W 64
#define H 80

// Goodix image struct (offsets from enrolAddImage/printImage RE)
#pragma pack(push,1)
struct gimg {
  void *data;       // +0x00 8-bit pixels
  uint16_t f08;     // +0x08 height dimension (w*f08 = buffer size)
  uint16_t width;   // +0x0a
  uint16_t height;  // +0x0c
  uint8_t bits;     // +0x0e must be 8
  uint8_t f0f;      // +0x0f must be 1
  uint8_t pad10[4]; // +0x10
  uint32_t frames;  // +0x14 frame_count
  uint16_t chan;    // +0x18 must be != 0
  uint8_t pad1a[6];
};
#pragma pack(pop)

typedef int MS (*ppp_init_t)(int);
typedef void *MS (*enrolstartex_t)(int *);
typedef int MS (*enroladd_t)(void *sess, struct gimg *img, void *a3, void *a4,
                             uint8_t a5, void *a6);
typedef int MS (*enrolfinish_t)(void *sess);
typedef int MS (*enrolget_t)(void *sess, void **out_tpl);

int main(int argc, char **argv) {
  int ppp_code = argc > 1 ? atoi(argv[1]) : 0;
  winpe_set_verbose(0);
  if (!winpe_load("../../win-driver/AlgoMilan.dll")) return 1;
  // enable AlgoMilan internal logging: *(base+0xb4830) -> log ctx;
  // ctx[0x30c]=enable, ctx[0x100]=level threshold
  extern void *winpe_base(void);
  unsigned char *b = winpe_base();
  if (b) {
    unsigned char *ctx = *(unsigned char **)(b + 0xb4830);
    printf("log ctx = %p\n", (void*)ctx);
    if (ctx) { *(int*)(ctx + 0x30c) = 1; *(int*)(ctx + 0x100) = 0xffff; }
  }

  ppp_init_t ppp = winpe_getproc("ppp_param_init");
  enrolstartex_t estart = winpe_getproc("enrolStartEx");
  enroladd_t eadd = winpe_getproc("enrolAddImage");
  enrolfinish_t efin = winpe_getproc("enrolFinish");
  enrolget_t eget = winpe_getproc("enrolGetTemplate");
  printf("ppp=%p estart=%p eadd=%p efin=%p eget=%p\n", (void*)ppp,
         (void*)estart, (void*)eadd, (void*)efin, (void*)eget);

  printf("== ppp_param_init(%d) ==\n", ppp_code);
  int pr = ppp(ppp_code);
  printf("ppp_param_init -> %d\n", pr);

  // load one genuine frame (record 25 = first 'natural')
  FILE *f = fopen("frames.bin", "rb");
  if (!f) { perror("frames.bin"); return 1; }
  static uint8_t px[W * H];
  fseek(f, 25L * W * H, SEEK_SET);
  if (fread(px, 1, W * H, f) != W * H) { perror("read"); return 1; }
  fclose(f);

  struct gimg img; memset(&img, 0, sizeof img);
  img.data = px; img.f08 = H; img.width = W; img.height = H;
  img.bits = 8; img.f0f = 1; img.frames = 1; img.chan = 1;

  int maxt = 16;
  printf("== enrolStartEx(&maxt=%d) ==\n", maxt);
  void *sess = estart(&maxt);
  printf("enrolStartEx -> sess=%p, maxt now %d\n", sess, maxt);
  if (!sess) { printf("no session\n"); return 2; }

  int out3 = 0, out4 = 0, out6 = 0;
  printf("== enrolAddImage ==\n");
  int ar = eadd(sess, &img, &out3, &out4, 0, &out6);
  printf("enrolAddImage -> %d (out3=%d out4=%d out6=%d)\n", ar, out3, out4,
         out6);

  printf("== enrolFinish ==\n");
  int fr = efin(sess);
  printf("enrolFinish -> %d\n", fr);

  void *tpl = NULL;
  printf("== enrolGetTemplate ==\n");
  int gr = eget(sess, &tpl);
  printf("enrolGetTemplate -> %d, tpl=%p\n", gr, tpl);
  return 0;
}
