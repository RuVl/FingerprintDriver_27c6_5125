// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * End-to-end run of the dataset through the public openchicago API only
 * (include/openchicago.h, no DLL, no port_eval wrappers), with exactly the
 * protocol of tools/algo/algo_eval4.c / port_eval.c:
 *   * frames transposed to 64 rows x 80 columns (oc_frame_transpose);
 *   * calibration generated from the bg frame of natural[0] (oc_session_new);
 *   * WARM=<passes> [WARMSET=<label>]: frames through the preprocessor only;
 *   * gallery = first N usable natural touches (OC_ENROLL_PLAIN);
 *   * every other record is a probe (oc_verify); STUDY=1: templateStudy after
 *     every matching natural/genuine probe, the template accumulates.
 * Output (stdout) in the port_eval format, for tests/e2e_cmp.py:
 *   '  <label> <rec> q= c= score= idx= MATCH|-' or '  <label> <rec> reject=<n>'.
 *
 *   test_e2e <data_dir> [enroll_n=12]
 * data_dir holds frames_raw.bin, bg_raw.bin, meta_raw.txt (tools/algo/export_raw.py).
 * env TPL_OUT=<file>   packed gallery;  OVTRACE=<file>  count/progress/overlay/preoverlay
 *                      after every enrolment frame (algo_eval4 OVTRACE format);  STRACE=<file>, STUDY_DIR=<dir>  as algo_eval4
 *     EDEL=r1,r2,..        enrolDeleteImage right after adding record r (as algo_eval4)
 *     RESTORE_AT=k1,k2,..  after the k-th frame given to the session: save the
 *                          state, free the session, continue in a new session
 *                          restored with oc_session_new_from_state()
 *                          (only outside an enrolment: the state does not hold
 *                          an enrolment in progress)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openchicago.h"
#include "openchicago-private.h"

#define PX OC_FRAME_PIXELS
#define MAXREC 256

static guint16 *raw, *bg;
static char labels[MAXREC][16];
static int nrec;
static OcSession *session;
static int frames_seen;
static int restore_at[64], n_restore, restores, enrolling;

/* RESTORE_AT: count one frame given to the session, maybe save/restore. */
static void
frame_done (void)
{
  frames_seen++;
  for (int k = 0; k < n_restore; k++)
    if (restore_at[k] == frames_seen)
      {
        if (enrolling)
          {
            fprintf (stderr, "RESTORE_AT %d falls inside the enrolment\n", frames_seen);
            exit (2);
          }
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) state = oc_session_save_state (session);

        oc_session_free (session);
        session = oc_session_new_from_state (state, &error);
        if (!session)
          {
            fprintf (stderr, "restore at %d: %s\n", frames_seen, error->message);
            exit (2);
          }
        restores++;
      }
}

static FILE *
ovtrace (void)
{
  static FILE *ot;

  if (!ot)
    ot = fopen (getenv ("OVTRACE"), "w");
  return ot;
}

static void
study_trace (int r, int score, int upd, GBytes *blob)
{
  static FILE *st;
  gsize n = 0;
  const guint8 *b = g_bytes_get_data (blob, &n);
  guint32 h = 2166136261u;

  for (gsize k = 0; k < n; k++)
    h = (h ^ b[k]) * 16777619u;
  if (getenv ("STRACE"))
    {
      if (!st)
        st = fopen (getenv ("STRACE"), "w");
      fprintf (st, "study rec %d score %d upd %d rc 0 len %zu fnv %08x\n", r, score, upd, n, h);
      fflush (st);
    }
  if (getenv ("STUDY_DIR"))
    {
      g_autofree gchar *path = g_strdup_printf ("%s/s%03d.tpl", getenv ("STUDY_DIR"), r);
      g_file_set_contents (path, (const gchar *) b, n, NULL);
    }
}

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
  m = fopen (mp, "r");
  if (!m)
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

int
main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  const char *dir = argc > 1 ? argv[1] : ".";
  int enroll_n = argc > 2 ? atoi (argv[2]) : 12;
  int study = getenv ("STUDY") != NULL;
  int nat[MAXREC], nnat = 0;

  if (!load (dir))
    {
      fprintf (stderr, "%s: no frames_raw.bin/bg_raw.bin/meta_raw.txt\n", dir);
      return 77;
    }
  if (getenv ("RESTORE_AT"))
    {
      gchar **v = g_strsplit (getenv ("RESTORE_AT"), ",", -1);
      for (int k = 0; v[k] && n_restore < 64; k++)
        restore_at[n_restore++] = atoi (v[k]);
      g_strfreev (v);
    }
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "natural"))
      nat[nnat++] = i;
  printf ("algo=openchicago enroll_n=%d study=%d\n", enroll_n, study);

  session = oc_session_new (bg + (gsize) nat[0] * PX, &error);
  if (!session)
    {
      printf ("session: %s\n", error->message);
      return 1;
    }

  int warm = getenv ("WARM") ? atoi (getenv ("WARM")) : 0;
  g_autofree OcPreprocessResult *pr = g_new (OcPreprocessResult, 1);
  for (int w = 0; w < warm; w++)
    for (int i = 0; i < nrec; i++)
      if (!getenv ("WARMSET") || !strcmp (labels[i], getenv ("WARMSET")))
        {
          oc_session_preprocess (session, raw + (gsize) i * PX, FALSE, pr);
          frame_done ();
        }

  /* gallery: first enroll_n usable natural touches */
  if (!oc_enroll_begin (session, OC_ENROLL_PLAIN, enroll_n, &error))
    return 1;
  int ne = 0, added = 0, gal_end = 0;
  enrolling = 1;
  for (int k = 0; k < nnat && ne < enroll_n; k++)
    {
      OcEnrollResult er;

      gal_end = k + 1;
      if (!oc_enroll_add (session, raw + (gsize) nat[k] * PX, &er, &error))
        {
          printf ("enroll: %s\n", error->message);
          return 1;
        }
      frame_done ();
      if (getenv ("OVTRACE") && (er.reject == OC_REJECT_NONE || er.reject == OC_REJECT_MERGE_FAILURE))
        {
          FILE *ot = ovtrace ();
          fprintf (ot, "add rec %d rc 0x%x count %u progress %u overlay %u preoverlay %u\n", nat[k],
                   er.reject ? 0x83 : 0, er.samples, er.progress, er.overlay, er.preoverlay);
          fflush (ot);
        }
      if (getenv ("EDEL") && er.reject == OC_REJECT_NONE)
        {
          g_autofree gchar *list = g_strdup_printf (",%s,", getenv ("EDEL"));
          g_autofree gchar *key = g_strdup_printf (",%d,", nat[k]);
          if (strstr (list, key))
            {
              gboolean ok = _oc_enroll_delete_last (session);
              if (getenv ("OVTRACE"))
                {
                  fprintf (ovtrace (), "del rec %d rc 0x%x count %u\n", nat[k], ok ? 0 : 0x81,
                           er.samples - (ok ? 1 : 0));
                  fflush (ovtrace ());
                }
            }
        }
      if (er.reject != OC_REJECT_NONE && er.reject != OC_REJECT_MERGE_FAILURE)
        continue;
      ne++;
      added += er.accepted;
    }
  g_autoptr(GBytes) blob = oc_enroll_finish (session, &error);
  enrolling = 0;
  if (!blob)
    {
      printf ("finish: %s\n", error->message);
      return 2;
    }
  if (getenv ("TPL_OUT"))
    g_file_set_contents (getenv ("TPL_OUT"), g_bytes_get_data (blob, NULL),
                         g_bytes_get_size (blob), NULL);
  printf ("gallery: %d natural consumed, %d offered, %d accepted, packed %zu bytes\n",
          gal_end, ne, added, g_bytes_get_size (blob));

  const char *grp[3] = { "natural", "genuine", "impostor" };
  int tot[3] = { 0 }, hit[3] = { 0 }, bad[3] = { 0 };
  int is_gal[MAXREC] = { 0 };
  for (int k = 0; k < gal_end; k++)
    is_gal[nat[k]] = 1;
  for (int i = 0; i < nrec; i++)
    {
      OcVerifyResult vr;
      g_autoptr(GBytes) updated = NULL;
      g_autoptr(GError) verr = NULL;
      int g;

      if (is_gal[i])
        continue;
      g = !strcmp (labels[i], "natural") ? 0 : !strcmp (labels[i], "genuine") ? 1 : 2;
      if (!oc_verify (session, blob, raw + (gsize) i * PX, &vr,
                      study && g < 2 ? &updated : NULL, &verr))
        {
          printf ("  %-8s %3d error: %s\n", labels[i], i, verr->message);
          bad[g]++;
          frame_done ();
          continue;
        }
      frame_done ();
      if (vr.reject != OC_REJECT_NONE)
        {
          bad[g]++;
          printf ("  %-8s %3d reject=%s rc=0x%x\n", labels[i], i,
                  oc_reject_to_string (vr.reject), vr.status);
          continue;
        }
      if (study && vr.match && g < 2)
        {
          if (updated)
            {
              g_bytes_unref (blob);
              blob = g_bytes_ref (updated);
            }
          study_trace (i, vr.score, updated != NULL, blob);
        }
      tot[g]++;
      hit[g] += vr.match;
      printf ("  %-8s %3d q=%3u c=%3u score=%5d idx=%2d %s\n", labels[i], i, vr.quality,
              vr.coverage, vr.score, vr.subtemplate, vr.match ? "MATCH" : "-");
    }
  for (int g = 0; g < 3; g++)
    printf ("%-8s: %3d/%3d matched (%5.1f%%)  [%d unusable]\n", grp[g], hit[g], tot[g],
            tot[g] ? 100.0 * hit[g] / tot[g] : 0, bad[g]);
  int gt = tot[0] + tot[1], gh = hit[0] + hit[1];
  printf ("FRR(genuine all) %.1f%%  FAR %.2f%%\n", gt ? 100.0 * (gt - gh) / gt : 0,
          tot[2] ? 100.0 * hit[2] / tot[2] : 0);
  if (n_restore)
    printf ("session restored %d times (RESTORE_AT)\n", restores);
  oc_session_free (session);
  return 0;
}
