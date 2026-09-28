// SPDX-License-Identifier: LGPL-2.1-or-later
/* Differential fuzz of the per-frame context (0x180043c70, cbuf[0..5]) against
 * AlgoChicago.dll. The dataset replay (test_preprocess.c) never reaches several
 * branches (hold flag, class code 9 / 0xc351, enclosure levels, second history
 * lobe, history regions, component states 3/4), so the DLL's internal functions
 * are called directly on perturbed dataset frames and synthetic planes:
 *   1. 0x180043c70 end to end (fresh DLL state and a fresh preprocessor object);
 *   2. 0x18003bb40, 0x18003fff0, 0x18003cd60, 0x18003f8a0, 0x180039240;
 *   3. 0x180037df0 with the residue state copied into the DLL globals.
 * The openchicago source is included to reach its static helpers.
 *
 * Usage: test_context_fuzz <dataset dir (frames_raw.bin, bg_raw.bin)> <AlgoChicago.dll>
 * Exit status 0 only when every comparison matches.
 */
#include <stdio.h>
#include <stdlib.h>

#include "goodix-chicago-preprocess.c"
#include "winpe.h"

#define MS __attribute__((ms_abi))
typedef uint64_t MS (*DllFn) ();

static guint32 seed = 4242;

static guint
rnd (void)
{
  seed = seed * 1103515245u + 12345u;
  return seed >> 8;
}

static gint
gauss (gint mean, gint deviation)
{
  gint sum = 0;

  for (guint k = 0; k < 6; k++)
    sum += rnd () % 1000;
  return mean + (sum - 3000) * deviation / 700;
}

static guint16 *
read_plane_file (const char *dir, const char *name, gsize *len)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  gchar *data = NULL;

  if (!g_file_get_contents (path, &data, len, NULL))
    {
      fprintf (stderr, "cannot read %s\n", path);
      exit (2);
    }
  return (guint16 *) data;
}

static void
transpose (guint16 *frame)
{
  guint16 t[CTX_N];

  for (gint r = 0; r < 80; r++)
    for (gint c = 0; c < 64; c++)
      t[c * 80 + r] = frame[r * 64 + c];
  memcpy (frame, t, sizeof t);
}

/* disc of `value`, falling by `slope` per squared pixel of distance */
static void
blob (guint16 *plane, gint value, gint radius_max, gint slope)
{
  const gint cx = rnd () % CTX_W, cy = rnd () % CTX_H, r = 1 + rnd () % radius_max;

  for (gint y = 0; y < CTX_H; y++)
    for (gint x = 0; x < CTX_W; x++)
      {
        const gint d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);

        if (d2 <= r * r)
          plane[y * CTX_W + x] = (guint16) (value - d2 * slope);
      }
}

/* 1. 0x180043c70 on perturbed frames: partial masks with holes, bright and
 * dark patches, frame-counter resets and the hold flag. */
static gint
fuzz_end_to_end (GoodixChicagoPreprocessor *self, const guint16 *raw, gint nrec,
                 const guint16 *image_base, gint iterations)
{
  static guint8 calidata[0x40000];
  static guint8 mask_struct[0x10 + GOODIX_CHICAGO_PIXELS];
  static guint8 labels[0x4c90];
  static guint16 current[CTX_N], base[CTX_N], kr[CTX_N];
  guint8 *mask = mask_struct + 0x10;
  gint bad = 0, framenum = 0, codes[10] = { 0, }, states[5] = { 0, }, holds = 0;

  for (gint pixel = 0; pixel < CTX_N; pixel++)
    kr[pixel] = 8192;
  for (gint it = 0; it < iterations; it++)
    {
      const gint rec = rnd () % nrec;
      const gboolean full = rnd () % 4 == 0;
      const gint cx = 20 + rnd () % 40, cy = 16 + rnd () % 32;
      const gint ax = 20 + rnd () % 60, ay = 16 + rnd () % 50;
      const gboolean hold = rnd () % 5 == 0;
      guint8 ours[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE], dll[16] = { 0, };
      gint code = 0, auxiliary = 0, count = 0, rc_dll, rc;

      goodix_chicago_preprocessor_prepare_raw (self, raw + (gsize) rec * CTX_N, current);
      goodix_chicago_preprocessor_prepare_raw (self, image_base, base);
      framenum = rnd () % 60 == 0 ? 1 : framenum + 1;
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        {
          const gint x = pixel % CTX_W, y = pixel / CTX_W;

          kr[pixel] = (guint16) (kr[pixel] + (gint) (rnd () % 21) - 10);
          mask[pixel] = full || (x - cx) * (x - cx) * ay * ay + (y - cy) * (y - cy) * ax * ax <=
                                  ax * ax * ay * ay ? 0xff : 0;
        }
      if (rnd () % 3 == 0)
        for (gint k = rnd () % 300; k > 0; k--)
          mask[rnd () % CTX_N] = 0;
      if (rnd () % 3 == 0)
        for (gint k = rnd () % 300; k > 0; k--)
          current[rnd () % CTX_N] = 3801 + rnd () % 290;
      if (rnd () % 5 == 0)
        for (gint k = rnd () % 200; k > 0; k--)
          current[rnd () % CTX_N] = rnd () % 60;
      if (rnd () % 4 == 0)
        {
          const gint bx = rnd () % 70, by = rnd () % 54, bw = 3 + rnd () % 25, bh = 3 + rnd () % 25;
          const gint drop = 200 + rnd () % 1500;

          for (gint y = by; y < by + bh && y < CTX_H; y++)
            for (gint x = bx; x < bx + bw && x < CTX_W; x++)
              current[y * CTX_W + x] = current[y * CTX_W + x] > drop ? current[y * CTX_W + x] - drop : 0;
        }
      if (rnd () % 6 == 0)
        {
          const gint bx = rnd () % 40, by = rnd () % 30, bw = 10 + rnd () % 40, bh = 10 + rnd () % 34;

          for (gint y = by; y < by + bh && y < CTX_H; y++)
            for (gint x = bx; x < bx + bw && x < CTX_W; x++)
              current[y * CTX_W + x] = rnd () % 50;
        }
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        count += mask[pixel] != 0;
      memcpy (mask_struct, &count, 4);
      memcpy (calidata, &framenum, 4);
      memcpy (calidata + 4, kr, sizeof kr);
      memcpy (self->kr, kr, sizeof kr);
      memset (labels, 0, sizeof labels);

      /* width 80, height 64, mode 0x18 */
      rc_dll = (gint) ((DllFn) 0x180043c70ULL) (current, base, calidata, 0x281000c0, mask_struct,
                                                &auxiliary, &code, labels, dll, hold ? 2 : 0);
      rc = context_build (self, current, base, mask, framenum, hold, ours);
      holds += hold;
      codes[MIN (dll[3], 9)]++;
      states[MIN (dll[0], 4)]++;
      if (memcmp (ours, dll, sizeof ours) != 0 ||
          (rc_dll == 0xc351) != (rc == GOODIX_CHICAGO_PREPROCESS_STATUS_CLASS_REJECT))
        {
          if (bad++ < 5)
            printf ("  e2e %d (rec %d, framenum %d, hold %d): ours %u,%u,%u,%u,%u rc %#x, dll %u,%u,%u,%u,%u rc %#x\n",
                    it, rec, framenum, hold, ours[0], ours[1], ours[2], ours[3], ours[4], rc,
                    dll[0], dll[1], dll[2], dll[3], dll[4], rc_dll);
        }
    }
  printf ("0x180043c70 end to end: %d/%d identical (hold %d; codes 0/5/6/7/8/9: %d/%d/%d/%d/%d/%d; "
          "states 0-4: %d/%d/%d/%d/%d)\n", iterations - bad, iterations, holds, codes[0], codes[5],
          codes[6], codes[7], codes[8], codes[9], states[0], states[1], states[2], states[3], states[4]);
  return bad;
}

/* 2. History threshold / second lobe, walk regions, level map, component
 * state and enclosure level on synthetic planes. */
static gint
fuzz_helpers (gint iterations)
{
  static gint16 history[CTX_N], walk[CTX_N];
  static guint16 secondary[CTX_N], current[CTX_N];
  static guint8 flags[CTX_N], mask[CTX_N], eroded[CTX_N], map[CTX_N], map_dll[CTX_N];
  static const gint32 walk_params[7] = { 1600, 768, 384, 0, 700, 800, 0 };
  static const gint32 level_params[2] = { 600, 640 };
  gint bad[5] = { 0, }, lobes = 0, regions = 0, component_states = 0, enclosures = 0;

  for (gint it = 0; it < iterations; it++)
    {
      const gint m1 = 6000 + rnd () % 2000, m2 = m1 + 300 + rnd () % 3000;
      const gint deviation = 50 + rnd () % 400, share = rnd () % 100;
      const gint threshold = 8000 + rnd () % 1000;
      gint thr_dll = 0, lobe_dll = 0, lobe = 0, thr, count = 0, state = 0, level, level_dll;
      gint32 local[16] = { 0, };

      for (gint pixel = 0; pixel < CTX_N; pixel++)
        {
          flags[pixel] = rnd () % 100 < 85;
          history[pixel] = it % 7 == 0 ? (gint16) (rnd () % 20000 - 2000) :
                           (gint16) ((gint) (rnd () % 100) < share ? gauss (m1, deviation) :
                                                                     gauss (m2, deviation));
        }
      ((DllFn) 0x18003bb40ULL) (history, flags, CTX_N, &thr_dll, 9000, &lobe_dll);
      thr = context_history_threshold (history, flags, &lobe);
      bad[0] += thr != thr_dll || lobe != lobe_dll;
      lobes += lobe;

      for (gint pixel = 0; pixel < CTX_N; pixel++)
        walk[pixel] = (gint16) (rnd () % (rnd () % 700 + 1));
      for (gint b = rnd () % 6; b > 0; b--)
        blob ((guint16 *) walk, 800 + rnd () % 3000, 11, 20);
      ((DllFn) 0x18003fff0ULL) (walk, walk_params, CTX_H, CTX_W, map_dll, &count);
      context_walk_regions (walk, map);
      bad[1] += memcmp (map, map_dll, CTX_N) != 0;
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        if (map[pixel])
          {
            regions++;
            break;
          }
      ((DllFn) 0x18003cd60ULL) (walk, level_params, CTX_H, CTX_W, map_dll);
      context_walk_level_map (walk, map);
      bad[1] += memcmp (map, map_dll, CTX_N) != 0;

      for (gint pixel = 0; pixel < CTX_N; pixel++)
        {
          secondary[pixel] = 7000 + rnd () % 900;
          mask[pixel] = rnd () % 100 < 90 ? 0xff : 0;
        }
      for (gint b = rnd () % 8; b > 0; b--)
        blob (secondary, 9000, 14, 0);
      ((DllFn) 0x18003f8a0ULL) (secondary, mask, CTX_W, CTX_H, threshold, local);
      context_component_state (secondary, mask, threshold, &state);
      bad[2] += state != local[0];
      component_states += state > 0;

      count = 0;
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        {
          const gint x = pixel % CTX_W, y = pixel / CTX_W;

          eroded[pixel] = x > 8 && x < 72 && y > 6 && y < 58 ? 0xff : 0;
          current[pixel] = 2000 + rnd () % 1000;
        }
      for (gint k = rnd () % 40; k > 0; k--)
        eroded[rnd () % CTX_N] = 0;
      for (gint k = rnd () % 400; k > 0; k--)
        current[rnd () % CTX_N] = 3801 + rnd () % 300;
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        count += eroded[pixel] != 0;
      count = count * (50 + rnd () % 60) / 100;
      level_dll = (guint8) ((DllFn) 0x180039240ULL) (current, eroded, CTX_H, CTX_W, count);
      level = context_enclosure_level (current, eroded, count);
      bad[3] += level != level_dll;
      enclosures += level > 0;
    }
  printf ("helpers, %d cases each: 0x18003bb40 %d, 0x18003fff0/0x18003cd60 %d, 0x18003f8a0 %d, "
          "0x180039240 %d differ (lobe %d, regions %d, state 3/4 %d, enclosure %d)\n", iterations,
          bad[0], bad[1], bad[2], bad[3], lobes, regions, component_states, enclosures);
  return bad[0] + bad[1] + bad[2] + bad[3];
}

/* 3. 0x180037df0: residue counters and class-3 groups, with the hold flag. */
static gint
fuzz_residue (GoodixChicagoPreprocessor *self, gint iterations)
{
  static guint16 gradient[CTX_N];
  static guint8 labels[CTX_N], labels_dll[CTX_N], residue_dll[CTX_N];
  gint bad = 0, marked = 0;

  memset (self->residue, 0, sizeof (self->residue));
  self->residue_count = 0;
  self->residue_coverage = 0;
  for (gint it = 0; it < iterations; it++)
    {
      const gint flag = rnd () % 4 == 0 ? CTX_FLAG_HOLD : 0;
      const gint count = 3000 + rnd () % 2121;
      gint32 local[12] = { 0, };
      gpointer labels_pointer = labels_dll;

      for (gint pixel = 0; pixel < CTX_N; pixel++)
        {
          gradient[pixel] = rnd () % 200;
          labels[pixel] = rnd () % 10 == 0 ? rnd () % 4 : 0;
        }
      for (gint b = rnd () % 6; b > 0; b--)
        blob (gradient, 400 + rnd () % 300, 10, 0);
      memcpy (labels_dll, labels, CTX_N);
      local[4] = flag;
      local[5] = 0x18;
      local[6] = count;
      memcpy (local + 10, &labels_pointer, sizeof labels_pointer);
      *(gint32 *) 0x1800eb9b0ULL = self->residue_count;
      *(gint32 *) 0x1800d8760ULL = self->residue_coverage;
      memcpy (residue_dll, self->residue, CTX_N);
      ((DllFn) 0x180037df0ULL) (gradient, local, CTX_H, CTX_W, residue_dll);
      context_apply_residue (self, gradient, count, flag, labels);
      if (memcmp (labels, labels_dll, CTX_N) != 0 || memcmp (self->residue, residue_dll, CTX_N) != 0 ||
          self->residue_count != *(gint32 *) 0x1800eb9b0ULL ||
          self->residue_coverage != *(gint32 *) 0x1800d8760ULL)
        bad++;
      for (gint pixel = 0; pixel < CTX_N; pixel++)
        if (labels[pixel] == 3 && gradient[pixel] >= 400)
          {
            marked++;
            break;
          }
    }
  printf ("0x180037df0: %d/%d identical (%d with residue groups)\n", iterations - bad, iterations, marked);
  return bad;
}

int
main (int argc, char **argv)
{
  gsize len, bg_len;
  guint16 *raw, *bg;
  gint nrec, bad = 0;
  g_autoptr(GBytes) calibration = NULL;
  g_autoptr(GoodixChicagoPreprocessor) self = NULL;

  if (argc != 3)
    {
      fprintf (stderr, "usage: %s dataset_dir AlgoChicago.dll\n", argv[0]);
      return 2;
    }
  if (!winpe_load (argv[2]))
    return 2;
  raw = read_plane_file (argv[1], "frames_raw.bin", &len);
  bg = read_plane_file (argv[1], "bg_raw.bin", &bg_len);
  nrec = len / (CTX_N * 2);
  for (gint rec = 0; rec < nrec; rec++)
    {
      transpose (raw + (gsize) rec * CTX_N);
      transpose (bg + (gsize) rec * CTX_N);
    }
  calibration = goodix_chicago_calibration_generate (bg);
  self = goodix_chicago_preprocessor_new (calibration, bg, NULL);

  bad += fuzz_end_to_end (self, raw, nrec, bg, 3000);
  bad += fuzz_helpers (1000);
  bad += fuzz_residue (self, 1000);
  g_free (raw);
  g_free (bg);
  return bad == 0 ? 0 : 1;
}
