// SPDX-License-Identifier: LGPL-2.1-or-later
/* Byte-exact comparison of the openchicago preprocessor with AlgoChicago.dll.
 *
 * Replays the scenario of tools/algo/oracle_pp.c (A: all records in index order with
 * purpose 0; B: the first 12 natural records with purpose 1, then all others with
 * purpose 0) from a cold start and compares every call with the oracle dump:
 * return code, coverage, quality, the 8-bit image, the adaptive state (framenum,
 * kr, gain, multiplier count, multiplier and auxiliary plane) and the context bytes
 * cbuf[0..5] of 0x180043c70 (oracle format 2).
 *
 * Usage: test_preprocess <A|B> <oracle.bin> <dataset dir (frames_raw.bin, bg_raw.bin,
 *        meta_raw.txt)>          env VERBOSE=1: one line per frame
 * Exit status 0 only when everything matches.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goodix-chicago-calibration.h"
#include "goodix-chicago-preprocess.h"

#define PX GOODIX_CHICAGO_PIXELS
#define MAXREC 256
#define NHDR 8

typedef struct
{
  gint32  h[NHDR];   /* seq, rec, purpose, rc, coverage, quality, framenum, mult count */
  guint8  dst[PX];
  guint16 kr[PX], gain[PX], mult[PX], aux[PX];
  guint8  cbuf[16];  /* first bytes of the DLL's cbuf; 0..5 are compared */
} OracleRecord;

static guint16 *
read_file (const char *dir, const char *name, gsize *len)
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

/* dataset frames are 80 rows x 64 columns; the algorithm sees 64 rows x 80 */
static void
transpose (guint16 *f)
{
  guint16 t[PX];

  for (int r = 0; r < 80; r++)
    for (int c = 0; c < 64; c++)
      t[c * 80 + r] = f[r * 64 + c];
  memcpy (f, t, sizeof t);
}

static int
first_diff_u16 (const guint16 *a, const guint16 *b, int *n)
{
  int first = -1;

  *n = 0;
  for (int i = 0; i < PX; i++)
    if (a[i] != b[i])
      {
        if (first < 0)
          first = i;
        (*n)++;
      }
  return first;
}

int
main (int argc, char **argv)
{
  if (argc != 4 || (strcmp (argv[1], "A") && strcmp (argv[1], "B")))
    {
      fprintf (stderr, "usage: %s A|B oracle.bin dataset_dir\n", argv[0]);
      return 2;
    }
  const char scen = argv[1][0];
  const gboolean verbose = g_getenv ("VERBOSE") != NULL;
  gsize len, bglen, olen;
  guint16 *raw = read_file (argv[3], "frames_raw.bin", &len);
  guint16 *bg = read_file (argv[3], "bg_raw.bin", &bglen);
  int nrec = MIN (len / (PX * 2), MAXREC);
  g_autofree char *meta_path = g_build_filename (argv[3], "meta_raw.txt", NULL);
  FILE *m = fopen (meta_path, "r");
  char labels[MAXREC][16];
  int tmp;

  if (!m)
    {
      perror (meta_path);
      return 2;
    }
  for (int i = 0; i < nrec; i++)
    if (fscanf (m, "%15s %d", labels[i], &tmp) != 2)
      return 2;
  fclose (m);
  for (int i = 0; i < nrec; i++)
    {
      transpose (raw + (gsize) i * PX);
      transpose (bg + (gsize) i * PX);
    }

  g_autofree gchar *obuf = NULL;
  if (!g_file_get_contents (argv[2], &obuf, &olen, NULL) || olen < 16 || memcmp (obuf, "OCPP", 4))
    {
      fprintf (stderr, "bad oracle dump %s\n", argv[2]);
      return 2;
    }
  guint32 hdr[3];
  memcpy (hdr, obuf + 4, sizeof hdr);
  if (hdr[0] != 2 || hdr[2] != sizeof (OracleRecord) || olen != 16 + (gsize) hdr[1] * hdr[2])
    {
      fprintf (stderr, "oracle dump format mismatch\n");
      return 2;
    }
  const OracleRecord *orc = (const OracleRecord *) (obuf + 16);
  const int n = hdr[1];

  int nat[MAXREC], nnat = 0, order[MAXREC], purp[MAXREC], no = 0, used[MAXREC] = { 0 };
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "natural"))
      nat[nnat++] = i;
  if (scen == 'B')
    for (int k = 0; k < 12 && k < nnat; k++)
      {
        order[no] = nat[k];
        purp[no++] = 1;
        used[nat[k]] = 1;
      }
  for (int i = 0; i < nrec; i++)
    if (!used[i])
      {
        order[no] = i;
        purp[no++] = 0;
      }
  if (no != n)
    {
      fprintf (stderr, "scenario has %d calls, oracle %d\n", no, n);
      return 2;
    }

  const guint16 *image_base = bg + (gsize) nat[0] * PX;
  g_autoptr(GBytes) cal = goodix_chicago_calibration_generate (image_base);
  g_autoptr(GError) error = NULL;
  g_autoptr(GoodixChicagoPreprocessor) pp = goodix_chicago_preprocessor_new (cal, image_base, &error);
  if (!pp)
    {
      fprintf (stderr, "preprocessor_new: %s\n", error->message);
      return 2;
    }

  int frames_ok = 0, first_bad = -1, ctx_same = 0;
  long px_same = 0;
  for (int s = 0; s < n; s++)
    {
      const OracleRecord *o = &orc[s];
      guint8 dst[PX], q = 0, c = 0, ctx[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE];
      GoodixChicagoPreprocessState st;
      int rc;

      if (o->h[1] != order[s] || o->h[2] != purp[s])
        {
          fprintf (stderr, "call %d: oracle rec %d/purpose %d, expected %d/%d\n", s, o->h[1], o->h[2],
                   order[s], purp[s]);
          return 2;
        }
      memset (dst, 0, sizeof dst);
      memset (ctx, 0xa5, sizeof ctx);
      rc = goodix_chicago_preprocessor_process_context (pp, raw + (gsize) order[s] * PX, purp[s], FALSE,
                                                        dst, &q, &c, ctx);
      goodix_chicago_preprocessor_get_state (pp, &st);

      int ndst = 0, fdst = -1, nkr, ngain, nmult;
      for (int i = 0; i < PX; i++)
        if (dst[i] != o->dst[i])
          {
            if (fdst < 0)
              fdst = i;
            ndst++;
          }
      px_same += PX - ndst;
      int naux, faux = first_diff_u16 (st.auxiliary, o->aux, &naux);
      int fkr = first_diff_u16 (st.kr, o->kr, &nkr);
      int fgain = first_diff_u16 (st.gain, o->gain, &ngain);
      int fmult = first_diff_u16 (st.multiplier, o->mult, &nmult);
      gboolean hdr_ok = rc == o->h[3] && c == o->h[4] && q == o->h[5] &&
                        (gint32) st.framenum == o->h[6] && (gint32) st.multiplier_count == o->h[7];
      gboolean ctx_ok = memcmp (ctx, o->cbuf, sizeof ctx) == 0;
      gboolean ok = hdr_ok && ctx_ok && ndst == 0 && nkr == 0 && ngain == 0 && nmult == 0 && naux == 0;

      ctx_same += ctx_ok;

      frames_ok += ok;
      if (verbose || (!ok && first_bad < 0))
        {
          printf ("%s call %3d rec %3d p%d: rc %#x/%#x cov %u/%d q %u/%d fn %u/%d mc %u/%d "
                  "dst %d kr %d gain %d mult %d aux %d ctx %u,%u,%u,%u,%u,%u/%u,%u,%u,%u,%u,%u\n",
                  ok ? "ok  " : "DIFF", s, order[s], purp[s],
                  rc, o->h[3], c, o->h[4], q, o->h[5], st.framenum, o->h[6],
                  st.multiplier_count, o->h[7], ndst, nkr, ngain, nmult, naux,
                  ctx[0], ctx[1], ctx[2], ctx[3], ctx[4], ctx[5], o->cbuf[0], o->cbuf[1], o->cbuf[2],
                  o->cbuf[3], o->cbuf[4], o->cbuf[5]);
        }
      if (!ok && first_bad < 0)
        {
          first_bad = s;
          if (fdst >= 0)
            printf ("  first dst diff: pixel %d (row %d col %d): ours %u dll %u\n", fdst, fdst / 80,
                    fdst % 80, dst[fdst], o->dst[fdst]);
          if (fkr >= 0)
            printf ("  first kr diff: pixel %d: ours %u dll %u\n", fkr, st.kr[fkr], o->kr[fkr]);
          if (fgain >= 0)
            printf ("  first gain diff: pixel %d: ours %u dll %u\n", fgain, st.gain[fgain], o->gain[fgain]);
          if (fmult >= 0)
            printf ("  first mult diff: pixel %d: ours %u dll %u\n", fmult, st.multiplier[fmult],
                    o->mult[fmult]);
          if (faux >= 0)
            printf ("  first aux diff: pixel %d: ours %u dll %u\n", faux, st.auxiliary[faux],
                    o->aux[faux]);
        }
    }
  printf ("scenario %c: %d/%d calls identical (cbuf[0..5] %d/%d), %.2f%% dst pixels identical%s\n",
          scen, frames_ok, n, ctx_same, n, 100.0 * px_same / ((double) n * PX),
          first_bad < 0 ? "" : g_strdup_printf (", first difference at call %d", first_bad));
  g_free (raw);
  g_free (bg);
  return frames_ok == n ? 0 : 1;
}
