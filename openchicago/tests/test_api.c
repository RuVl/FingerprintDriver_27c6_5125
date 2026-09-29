// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * API checks without the DLL (unit suite):
 *   1. calidata: a `goodix_calib.dat` written in the MR !648 layout loads with
 *      oc_session_new_from_calidata() into a session that behaves exactly like
 *      oc_session_new() on the same ImageBase; another sensor id is refused;
 *   2. print_data round trip (oc_print_data_new / _get_template), foreign id refused;
 *   3. error paths: enrol without begin, corrupt template in oc_verify;
 *   4. OC_ENROLL_ENGINE on the natural touches in dataset order (EngineAdapter
 *      protocol, notes/81): completes, template matches own probes, no impostor;
 *   5. oc_enroll_add_pair (choose_enroll_img) keeps the frame the rule selects;
 *   6. oc_identify over {impostor template, own template} = the best of two
 *      oc_verify calls, and the frame is preprocessed once (sessions stay equal);
 *   7. oc_session_rebase = preprocessor_init with the new ImageBase (fresh
 *      sessions equal), keeps the adaptive state, survives save/restore.
 *   test_api <data_dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>

#include "goodix-chicago-calibration.h"
#include "openchicago.h"
#include "openchicago-private.h"

#define PX OC_FRAME_PIXELS
#define MAXREC 256

static guint16 *raw, *bg;
static char labels[MAXREC][16];
static int nrec;
static int failures;

#define CHECK(cond, ...) \
  do { if (!(cond)) { failures++; printf ("FAIL: " __VA_ARGS__); printf ("\n"); } } while (0)

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

int
main (int argc, char **argv)
{
  static OcPreprocessResult a, b;
  g_autoptr(GError) error = NULL;
  const guint8 sensor_id[OC_SENSOR_ID_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
  guint8 other_id[OC_SENSOR_ID_LEN];
  int nat[MAXREC], nnat = 0;
  const guint16 *base;

  if (!load (argc > 1 ? argv[1] : "."))
    {
      fprintf (stderr, "no dataset: skipped\n");
      return 77;
    }
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "natural"))
      nat[nnat++] = i;
  base = bg + (gsize) nat[0] * PX;
  memcpy (other_id, sensor_id, sizeof other_id);
  other_id[0] ^= 0xff;

  /* 1. calidata */
  {
    g_autofree gchar *dir = g_dir_make_tmp ("openchicago-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename (dir, "goodix_calib.dat", NULL);
    g_autoptr(GBytes) cal = goodix_chicago_calibration_generate (base);
    g_autoptr(OcSession) s1 = oc_session_new (base, &error);
    g_autoptr(OcSession) s2 = NULL;
    g_autoptr(OcSession) s3 = NULL;
    g_autoptr(GError) e3 = NULL;
    int diff = 0;

    CHECK (goodix_chicago_calibration_save (path, sensor_id, cal, &error), "calibration_save");
    s2 = oc_session_new_from_calidata (path, sensor_id, base, &error);
    CHECK (s2 != NULL, "new_from_calidata: %s", error ? error->message : "?");
    s3 = oc_session_new_from_calidata (path, other_id, base, &e3);
    CHECK (s3 == NULL && e3 != NULL, "calidata of another sensor accepted");
    for (int i = 0; s2 && i < 40; i++)
      {
        oc_session_preprocess (s1, raw + (gsize) i * PX, i % 3 == 0, &a);
        oc_session_preprocess (s2, raw + (gsize) i * PX, i % 3 == 0, &b);
        diff += memcmp (&a, &b, sizeof a) != 0;
      }
    CHECK (diff == 0, "calidata session differs from generated one in %d/40 frames", diff);
    printf ("calidata: load ok, foreign id refused, 40 frames identical to oc_session_new: %s\n",
            diff ? "no" : "yes");
    g_unlink (path);
    g_rmdir (dir);
  }

  /* 5. choose_enroll_img: oc_enroll_add_pair keeps the better of two frames */
  {
    g_autoptr(OcSession) x = oc_session_new (base, &error);
    g_autoptr(OcSession) y = oc_session_new (base, &error);
    OcEnrollResult er;
    int picks[2] = { 0 };

    oc_enroll_begin (y, OC_ENROLL_PLAIN, 30, &error);
    for (int k = 0; k + 1 < nnat && k < 20; k += 2)
      {
        const guint16 *fa = raw + (gsize) nat[k] * PX, *fb = raw + (gsize) nat[k + 1] * PX;
        gboolean oka = oc_session_preprocess (x, fa, TRUE, &a);
        gboolean okb = oka && oc_session_preprocess (x, fb, TRUE, &b);
        gboolean better = okb && (b.quality > a.quality + 2 ||
                                  (ABS ((int) b.quality - (int) a.quality) < 2 && b.coverage > a.coverage) ||
                                  (b.quality > a.quality && b.coverage >= a.coverage));
        const OcPreprocessResult *want = better ? &b : &a;

        oc_enroll_add_pair (y, fa, fb, &er, &error);
        picks[better]++;
        /* x preprocesses the same frames in the same order as y (getFeature
         * does not touch the preprocessor), so the outputs are comparable */
        CHECK (er.quality == want->quality && er.coverage == want->coverage,
               "add_pair picked q=%u c=%u, expected q=%u c=%u", er.quality, er.coverage,
               want->quality, want->coverage);
      }
    printf ("add_pair: first frame kept %d, second frame chosen %d\n", picks[0], picks[1]);
  }

  g_autoptr(OcSession) s = oc_session_new (base, &error);
  g_assert_no_error (error);

  /* 3. error paths */
  {
    OcEnrollResult er;
    OcVerifyResult vr;
    g_autoptr(GError) e1 = NULL;
    g_autoptr(GError) e2 = NULL;
    g_autoptr(GBytes) junk = g_bytes_new_static ("not a template", 14);

    CHECK (!oc_enroll_add (s, raw, &er, &e1) && e1, "enroll_add without begin");
    CHECK (!oc_verify (s, junk, raw + (gsize) nat[1] * PX, &vr, NULL, &e2) && e2,
           "corrupt template accepted");
  }

  /* 4. EngineAdapter enrolment protocol over the natural touches */
  g_autoptr(GBytes) blob = NULL;
  int used_until = 0, tips = 0, rejected = 0;
  {
    OcEnrollResult er = { 0 };

    CHECK (oc_enroll_begin (s, OC_ENROLL_ENGINE, OC_ENGINE_STAGES, &error), "enroll_begin");
    for (int k = 0; k < nnat && !er.complete; k++)
      {
        CHECK (oc_enroll_add (s, raw + (gsize) nat[k] * PX, &er, &error), "enroll_add");
        used_until = k + 1;
        tips += er.reject == OC_REJECT_TIP;
        rejected += !er.accepted && er.reject != OC_REJECT_TIP;
        printf ("  engine rec %3d q=%3u c=%3u ov=%3u pov=%3u %s%s stage %u/%u samples %u progress %u%s\n",
                nat[k], er.quality, er.coverage, er.overlay, er.preoverlay,
                er.accepted ? "accepted" : oc_reject_to_string (er.reject),
                er.reject == OC_REJECT_TIP ? (const char *[]){ "", " up", " down", " left", " right" }[er.tip_direction] : "",
                er.stage, er.n_stages, er.samples, er.progress, er.complete ? " COMPLETE" : "");
      }
    CHECK (er.complete, "engine enrolment did not complete");
    blob = oc_enroll_finish (s, &error);
    CHECK (blob != NULL, "enroll_finish");
  }

  /* 2. print_data */
  {
    g_autoptr(GVariant) pd = oc_print_data_new (s, sensor_id, blob);
    g_autoptr(GBytes) back = oc_print_data_get_template (pd, sensor_id, &error);
    g_autoptr(GError) e4 = NULL;
    g_autoptr(GBytes) foreign = oc_print_data_get_template (pd, other_id, &e4);

    CHECK (back && g_bytes_equal (back, blob), "print_data round trip");
    CHECK (!foreign && e4, "print_data of another sensor accepted");
  }

  /* verify with the engine template (no study) */
  {
    int tot[3] = { 0 }, hit[3] = { 0 };
    for (int i = 0; i < nrec; i++)
      {
        OcVerifyResult vr;
        int g = !strcmp (labels[i], "natural") ? 0 : !strcmp (labels[i], "genuine") ? 1 : 2;
        int k;

        for (k = 0; k < used_until && nat[k] != i; k++)
          ;
        if (k < used_until)
          continue;
        CHECK (oc_verify (s, blob, raw + (gsize) i * PX, &vr, NULL, &error), "verify");
        if (vr.reject != OC_REJECT_NONE)
          continue;
        tot[g]++;
        hit[g] += vr.match;
      }
    printf ("engine enrolment: %d touches (%d tips, %d rejected), template %zu bytes; "
            "natural %d/%d, genuine %d/%d, impostor %d/%d\n", used_until, tips, rejected,
            g_bytes_get_size (blob), hit[0], tot[0], hit[1], tot[1], hit[2], tot[2]);
    CHECK (hit[2] == 0, "impostor matched");
    CHECK (hit[0] + hit[1] > 0, "no genuine match");
  }
  /* 6. identify = best of verify; one preprocessing per frame */
  {
    g_autoptr(GBytes) state = oc_session_save_state (s);
    g_autoptr(OcSession) sa = oc_session_new_from_state (state, &error);
    g_autoptr(OcSession) sb = oc_session_new_from_state (state, &error);
    g_autoptr(OcSession) sc = oc_session_new_from_state (state, &error);
    g_autoptr(GBytes) imp = NULL;
    int n = 0, own = 0, other = 0;

    /* impostor template, built in a throw-away copy of the session */
    {
      g_autoptr(OcSession) st = oc_session_new_from_state (state, &error);
      OcEnrollResult er = { 0 };

      CHECK (oc_enroll_begin (st, OC_ENROLL_PLAIN, 8, &error), "impostor begin");
      for (int i = 0; i < nrec && !er.complete; i++)
        if (!strcmp (labels[i], "impostor"))
          CHECK (oc_enroll_add (st, raw + (gsize) i * PX, &er, &error), "impostor add");
      imp = oc_enroll_finish (st, &error);
      CHECK (imp != NULL, "impostor template");
    }
    for (int i = 0; imp && i < nrec; i += 3)
      {
        OcVerifyResult va, vb, vc;
        GBytes *gallery[2] = { imp, blob };
        gint idx, want = -1;
        gint32 want_score;

        CHECK (oc_verify (sa, blob, raw + (gsize) i * PX, &va, NULL, &error), "verify own");
        CHECK (oc_verify (sc, imp, raw + (gsize) i * PX, &vc, NULL, &error), "verify impostor");
        CHECK (oc_identify (sb, gallery, 2, raw + (gsize) i * PX, &vb, &idx, NULL, &error),
               "identify");
        if (va.reject != OC_REJECT_NONE)
          {
            CHECK (vb.reject == va.reject && idx == -1, "identify reject rec %d", i);
            continue;
          }
        want_score = vc.score;
        if (vc.score > 0)
          want = 0;
        if (va.score > 0 && va.score > vc.score)
          want = 1, want_score = va.score;
        CHECK (idx == want && vb.match == (want >= 0) &&
               (want < 0 || vb.score == want_score),
               "identify rec %d: idx %d score %d, want %d (%d / %d)", i, idx, vb.score,
               want, vc.score, va.score);
        n++;
        own += idx == 1;
        other += idx == 0;
      }
    CHECK (_oc_session_state_equal (sa, sb) && _oc_session_state_equal (sa, sc),
           "identify changed the session state differently from verify");
    printf ("identify: %d frames, own template %d, impostor template %d\n", n, own, other);
  }

  /* 7. rebase */
  {
    const guint16 *base2 = bg + (gsize) nat[nnat - 1] * PX;
    g_autoptr(OcSession) f1 = oc_session_new (base2, &error);
    g_autoptr(OcSession) f2 = oc_session_new (base, &error);
    g_autoptr(GBytes) state = oc_session_save_state (s);
    g_autoptr(OcSession) r1 = oc_session_new_from_state (state, &error);
    g_autoptr(OcSession) r2 = oc_session_new_from_state (state, &error);
    g_autoptr(OcSession) r3 = NULL;
    g_autoptr(GBytes) state3 = NULL;

    CHECK (memcmp (base, base2, PX * 2) != 0, "dataset has one ImageBase only");
    CHECK (oc_session_rebase (f2, base2, &error), "rebase fresh");
    CHECK (_oc_session_state_equal (f1, f2), "rebased fresh session != new session");
    CHECK (oc_session_rebase (r2, oc_session_get_image_base (r1), &error) &&
           _oc_session_state_equal (r1, r2), "rebase onto the same ImageBase changed the state");
    CHECK (oc_session_rebase (r2, base2, &error), "rebase");
    CHECK (!_oc_session_state_equal (r1, r2), "rebase did not change the ImageBase");
    state3 = oc_session_save_state (r2);
    r3 = oc_session_new_from_state (state3, &error);
    CHECK (r3 && _oc_session_state_equal (r2, r3), "rebased state round trip");
    CHECK (oc_session_rebase (r2, oc_session_get_image_base (r1), &error) &&
           _oc_session_state_equal (r1, r2), "rebase back != original");
  }

  printf ("%s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}
