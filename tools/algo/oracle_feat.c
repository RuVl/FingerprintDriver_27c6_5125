// Oracle for stage 3 of openchicago (docs/stage3-features.md): the feature object that
// AlgoChicago.dll's getFeature (0x180013490, reached through the thunk 0x1800163d0 from
// enrolAddImage 0x18000c820 and identifyImage 0x18000d170) builds for every image.
//
// The protocol is exactly algo_eval4.c (this file includes it): same preprocessor call order,
// gallery, WARM/WARMSET, fresh unpack per probe.  The thunk is patched to jump to feat_hook(),
// which calls the real getFeature and appends the resulting object to FEAT_OUT.
//
// Usage (from tools/algo):  FEAT_OUT=/tmp/feat12.bin ./oracle_feat 12
//                           [WARM=1 WARMSET=impostor] [DUMP_PROC=/tmp/d12.bin]  (as algo_eval4)
// Build: cc -O2 -g -o oracle_feat oracle_feat.c winpe.c algolog.c   (see oracle_feat.sh)
//
// Dump (little endian), one block per getFeature call:
//   "FEAT" u32 block_len  then TLV sections  char tag[4], u32 len, u8 data[len]:
//     HDR : i32 rec, mode (5th arg: 0 enroll / 1 identify), rc, quality, coverage, width, height
//     CBUF: first 16 bytes of the preprocessor cbuf passed in (getFeature reads [0..5] via
//           0x180052310: [0] -> classify threshold, [3] -> resolution class; F+0x148 prefix)
//     OBJ : the 0x230-byte feature object F (pointers included, for offsets only)
//     RECS: F+0xf8, F+0xf0 records of 0x3c bytes
//     I008 I010 I018 I020 I130: image objects at F+0x08 .. F+0x20, F+0x130
//           (0x20-byte header {w, h, stride, size, bpp, ...} + size data bytes); absent if NULL
//     B148: F+0x148 (w*h of the F+0x08 image), B218 B220 B228: F+0x218.. (F.w*F.h bytes)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winpe.h"

static void *oracle_winpe_load (const char *path);
#define winpe_load oracle_winpe_load
#define main algo_eval4_main
#include "algo_eval4.c"
#undef main
#undef winpe_load

#define GETFEATURE_VA 0x180013490ULL
#define GETFEATURE_THUNK_VA 0x1800163d0ULL

typedef int MS (*getfeature_t) (void **, void *, int, int, int, void *, void *, void *, void *);
static FILE *feat_out;
static int feat_calls[2];

static void
put_sec (uint8_t **p, const char tag[4], const void *data, uint32_t len)
{
  memcpy (*p, tag, 4);
  memcpy (*p + 4, &len, 4);
  if (len)
    memcpy (*p + 8, data, len);
  *p += 8 + len;
}

static void
put_img (uint8_t **p, const char tag[4], const uint8_t *img)
{
  if (!img)
    return;
  uint32_t size = *(const uint32_t *) (img + 0xc);
  static uint8_t tmp[0x20 + 0x40000];
  memcpy (tmp, img, 0x20);
  memcpy (tmp + 0x20, *(uint8_t *const *) (img + 0x18), size);
  put_sec (p, tag, tmp, 0x20 + size);
}

static int MS
feat_hook (void **pfeat, void *desc, int quality, int coverage, int mode, void *tpl, void *a7,
           void *cbuf, void *a9)
{
  int rc = ((getfeature_t) GETFEATURE_VA) (pfeat, desc, quality, coverage, mode, tpl, a7, cbuf, a9);
  if (!feat_out)
    return rc;
  int rec = cbufs && cbuf ? (int) (((uint8_t *) cbuf - cbufs) / CBUF_SZ) : -1;
  feat_calls[mode ? 1 : 0]++;
  static uint8_t buf[1 << 21];
  uint8_t *p = buf + 8;
  const uint8_t *F = *pfeat;
  int32_t w = F ? *(const int32_t *) F : 0, h = F ? *(const int32_t *) (F + 4) : 0;
  int32_t hdr[7] = { rec, mode, rc, quality, coverage, w, h };
  put_sec (&p, "HDR ", hdr, sizeof hdr);
  if (cbuf)
    put_sec (&p, "CBUF", cbuf, 16);
  if (F && rc == 0)
    {
      put_sec (&p, "OBJ ", F, 0x230);
      int n = *(const int32_t *) (F + 0xf0);
      put_sec (&p, "RECS", *(uint8_t *const *) (F + 0xf8), n > 0 ? (uint32_t) n * 0x3c : 0);
      static const struct { int off; const char *tag; } imgs[] = {
        { 0x08, "I008" }, { 0x10, "I010" }, { 0x18, "I018" }, { 0x20, "I020" }, { 0x130, "I130" },
      };
      for (size_t k = 0; k < sizeof imgs / sizeof imgs[0]; k++)
        put_img (&p, imgs[k].tag, *(uint8_t *const *) (F + imgs[k].off));
      const uint8_t *i8 = *(uint8_t *const *) (F + 0x08);
      const uint8_t *b148 = *(uint8_t *const *) (F + 0x148);
      if (b148 && i8)
        put_sec (&p, "B148", b148, (uint32_t) (*(const int32_t *) i8 * *(const int32_t *) (i8 + 4)));
      static const struct { int off; const char *tag; } bufs[] = {
        { 0x218, "B218" }, { 0x220, "B220" }, { 0x228, "B228" },
      };
      for (size_t k = 0; k < sizeof bufs / sizeof bufs[0]; k++)
        {
          const uint8_t *b = *(uint8_t *const *) (F + bufs[k].off);
          if (b)
            put_sec (&p, bufs[k].tag, b, (uint32_t) (w * h));
        }
    }
  uint32_t len = (uint32_t) (p - buf - 8);
  memcpy (buf, "FEAT", 4);
  memcpy (buf + 4, &len, 4);
  fwrite (buf, 1, p - buf, feat_out);
  fflush (feat_out);
  return rc;
}

#undef winpe_load
static void *
oracle_winpe_load (const char *path)
{
  void *base = winpe_load (path);
  if (base && strstr (path, "AlgoChicago"))
    {
      if (winpe_hook (GETFEATURE_THUNK_VA, (void *) feat_hook) != 0)
        {
          fprintf (stderr, "oracle_feat: cannot hook getFeature thunk\n");
          exit (1);
        }
    }
  return base;
}

int
main (int argc, char **argv)
{
  if (getenv ("FEAT_OUT"))
    feat_out = fopen (getenv ("FEAT_OUT"), "wb");
  int rc = algo_eval4_main (argc, argv);
  fprintf (stderr, "oracle_feat: getFeature calls: enroll %d, identify %d\n", feat_calls[0],
           feat_calls[1]);
  if (feat_out)
    fclose (feat_out);
  return rc;
}
