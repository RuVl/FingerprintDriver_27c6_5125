// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * State serialization check (no DLL): one continuous session over the whole
 * dataset versus sessions saved after k frames, freed, restored with
 * oc_session_new_from_state() and continued.  Every later preprocessor output
 * (status, quality, coverage, cbuf[0..5] context, enhanced image) must be
 * identical, for several k.  The frame order is the algo_eval4 protocol's
 * mix of purposes: natural[0..11] as enrolment (purpose 1), the rest as
 * verification (purpose 0), after one warm pass over the impostor frames.
 *   test_state <data_dir> [--negative]   (--negative: flip one state byte
 *   in the PREP section after the CRC is recomputed; must fail)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openchicago.h"
#include "openchicago-private.h"

#define PX OC_FRAME_PIXELS
#define MAXREC 256
#define MAXSTEP 1024

static guint16 *raw, *bg;
static char labels[MAXREC][16];
static int nrec;

static gboolean
load (const char *dir)
{
  g_autofree gchar *fp = g_build_filename (dir, "frames_raw.bin", NULL);
  g_autofree gchar *bp = g_build_filename (dir, "bg_raw.bin", NULL);
  g_autofree gchar *mp = g_build_filename (dir, "meta_raw.txt", NULL);
  gchar *fdata, *bdata;
  gsize flen, blen;
  guint16 tmp[PX];
  FILE *m;
  int dummy;

  if (!g_file_get_contents (fp, &fdata, &flen, NULL) ||
      !g_file_get_contents (bp, &bdata, &blen, NULL) || blen != flen)
    return FALSE;
  nrec = MIN (MAXREC, (int) (flen / (PX * 2)));
  raw = (guint16 *) fdata;
  bg = (guint16 *) bdata;
  if (!(m = fopen (mp, "r")))
    return FALSE;
  for (int i = 0; i < nrec; i++)
    if (fscanf (m, "%15s %d", labels[i], &dummy) != 2)
      break;
  fclose (m);
  for (int i = 0; i < nrec; i++)
    {
      memcpy (tmp, raw + (gsize) i * PX, sizeof tmp);
      oc_frame_transpose (tmp, raw + (gsize) i * PX);
      memcpy (tmp, bg + (gsize) i * PX, sizeof tmp);
      oc_frame_transpose (tmp, bg + (gsize) i * PX);
    }
  return TRUE;
}

static guint32
crc32_ieee (const guint8 *data, gsize len)
{
  guint32 crc = G_MAXUINT32;

  for (gsize i = 0; i < len; i++)
    {
      crc ^= data[i];
      for (guint b = 0; b < 8; b++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
  return ~crc;
}

/* --negative: flip a byte in the middle of the PREP section, fix the CRC */
static GBytes *
corrupt (GBytes *state)
{
  gsize n;
  guint8 *b = g_bytes_unref_to_data (state, &n);
  guint32 crc;

  for (gsize p = 8; p + 8 < n;)
    {
      guint32 len = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (guint32) b[p + 7] << 24;
      if (!memcmp (b + p, "PREP", 4))
        {
          b[p + 8 + 8] ^= 0x01;   /* kr[2] */
          break;
        }
      p += 8 + len;
    }
  crc = GUINT32_TO_LE (crc32_ieee (b, n - 4));
  memcpy (b + n - 4, &crc, 4);
  return g_bytes_new_take (b, n);
}

/* Set the u32 at @offset of section @tag and fix the CRC: a consistent file
 * with an out-of-range counter. */
static GBytes *
patch_u32 (GBytes *state, const char tag[4], gsize offset, guint32 value)
{
  gsize n;
  const guint8 *d = g_bytes_get_data (state, &n);
  guint8 *b = g_memdup2 (d, n);
  guint32 le = GUINT32_TO_LE (value), crc;

  for (gsize p = 8; p + 8 < n;)
    {
      guint32 len = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (guint32) b[p + 7] << 24;
      if (!memcmp (b + p, tag, 4))
        {
          g_assert (offset + 4 <= len);
          memcpy (b + p + 8 + offset, &le, 4);
          break;
        }
      p += 8 + len;
    }
  crc = GUINT32_TO_LE (crc32_ieee (b, n - 4));
  memcpy (b + n - 4, &crc, 4);
  return g_bytes_new_take (b, n);
}

int
main (int argc, char **argv)
{
  static int order[MAXSTEP], purpose[MAXSTEP];
  static OcPreprocessResult ref[MAXSTEP];
  const int ks[] = { 1, 7, 12, 40, 97, 150, 230 };
  int negative = argc > 2 && !strcmp (argv[2], "--negative");
  int nat0 = -1, nsteps = 0, nenroll = 0, bad = 0;
  g_autoptr(GError) error = NULL;
  g_autofree OcPreprocessResult *r = g_new (OcPreprocessResult, 1);

  if (!load (argc > 1 ? argv[1] : "."))
    {
      fprintf (stderr, "no dataset: skipped\n");
      return 77;
    }
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "impostor"))
      order[nsteps] = i, purpose[nsteps++] = 0;
  for (int i = 0; i < nrec; i++)
    {
      int enroll = !strcmp (labels[i], "natural") && nenroll < 12;
      if (!strcmp (labels[i], "natural") && nat0 < 0)
        nat0 = i;
      nenroll += enroll;
      order[nsteps] = i, purpose[nsteps++] = enroll;
    }

  /* continuous reference */
  {
    g_autoptr(OcSession) s = oc_session_new (bg + (gsize) nat0 * PX, &error);
    g_assert_no_error (error);
    for (int t = 0; t < nsteps; t++)
      oc_session_preprocess (s, raw + (gsize) order[t] * PX, purpose[t], &ref[t]);
  }

  for (guint ki = 0; ki < G_N_ELEMENTS (ks); ki++)
    {
      int k = ks[ki], diff = 0;
      g_autoptr(OcSession) s = oc_session_new (bg + (gsize) nat0 * PX, &error);
      g_autoptr(GBytes) state = NULL;
      g_autoptr(OcSession) c = NULL;

      if (k >= nsteps)
        continue;
      for (int t = 0; t < k; t++)
        oc_session_preprocess (s, raw + (gsize) order[t] * PX, purpose[t], r);
      state = oc_session_save_state (s);
      if (negative)
        state = corrupt (state);
      c = oc_session_new_from_state (state, &error);
      if (!c)
        {
          printf ("k=%d: restore failed: %s\n", k, error->message);
          return 1;
        }
      /* in-memory state of the restored session == the saved session's */
      gboolean same_now = _oc_session_state_equal (s, c);
      g_clear_pointer (&s, oc_session_free);
      for (int t = k; t < nsteps; t++)
        {
          oc_session_preprocess (c, raw + (gsize) order[t] * PX, purpose[t], r);
          if (memcmp (r, &ref[t], sizeof *r) != 0)
            diff++;
        }
      printf ("k=%3d: state %zu bytes, restored state %s, %d/%d later preprocessor outputs differ\n",
              k, g_bytes_get_size (state), same_now ? "identical" : "DIFFERS", diff, nsteps - k);
      bad += diff + !same_now;
    }

  /* a damaged state must be refused */
  {
    g_autoptr(OcSession) s = oc_session_new (bg + (gsize) nat0 * PX, &error);
    gsize n;
    g_autoptr(GBytes) st = oc_session_save_state (s);
    const guint8 *d = g_bytes_get_data (st, &n);
    guint8 *b = g_memdup2 (d, n);
    g_autoptr(GBytes) broken = NULL;
    g_autoptr(GError) e2 = NULL;
    b[n / 2] ^= 0x10;
    broken = g_bytes_new_take (b, n);
    if (oc_session_new_from_state (broken, &e2))
      {
        printf ("damaged state accepted\n");
        bad++;
      }
  }
  /* out-of-range counters with a valid CRC must be refused too:
   * FEAT history_count (index into history[]), PREP framenum (n + 1) */
  {
    static const struct { const char *tag; gsize offset; guint32 value; } cases[] = {
      { "FEAT", 4, G_MAXUINT32 }, { "FEAT", 4, 4 }, { "PREP", 0, G_MAXUINT32 },
    };
    g_autoptr(OcSession) s = oc_session_new (bg + (gsize) nat0 * PX, &error);
    g_autoptr(GBytes) st = oc_session_save_state (s);

    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
      {
        g_autoptr(GBytes) broken = patch_u32 (st, cases[i].tag, cases[i].offset,
                                              cases[i].value);
        g_autoptr(GError) e2 = NULL;
        g_autoptr(OcSession) c = oc_session_new_from_state (broken, &e2);

        if (c)
          {
            printf ("%s+%zu = %u accepted\n", cases[i].tag, cases[i].offset,
                    cases[i].value);
            bad++;
          }
      }
  }
  printf ("%s\n", bad ? "FAIL" : "OK");
  return bad ? 1 : 0;
}
