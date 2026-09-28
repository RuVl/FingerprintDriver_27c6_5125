// Offline eval of the open ChicagoHS port from libfprint MR !648
// (upstream/libfprint-mr648/libfprint/drivers/goodix5125/chicago/, LGPL) on our dataset,
// with exactly the protocol of algo_eval4.c (the original AlgoChicago.dll):
//   * frames transposed from our 80x64 buffer to the vendor 64 rows x 80 cols
//     (= goodix5125_image_decode_transposed(), = algo_eval4 transpose16);
//   * calibration as in the MR648 driver on a cold first run:
//     goodix_chicago_calibration_generate(ImageBase) + preprocessor_new(cal, ImageBase),
//     ImageBase = bg frame of natural[0] (== DLL preprocess_init_calidata + preprocessor_init(bg));
//   * one stateful preprocessor for the whole run, frames fed in the same order as algo_eval4;
//   * gallery = first N usable natural touches (goodix_chicago_runtime_prepare_probe),
//     enrolled with insert_first/insert_next (like enrolAddImage), packed into the driver's
//     (uayayay) print_data;
//   * each probe: runtime_match_print_data (fresh unpack of the gallery each time),
//     match <=> score > 0 (goodix5125.c match_decide).  The DLL's idx is the index of the
//     matched *template* in the candidate list (score>0 <=> idx>=0 there); the port's
//     selected_index is a *subtemplate* index for templateStudy (-1 on the fallback path),
//     so it is only reported ("idx>=0" count), not used as a criterion.
//
// Build: see port_eval.sh.  Usage: ./port_eval [enroll_n=15]
//   env OTP=<128 hex chars>  real sensor OTP (sensor_id = first 16 bytes; otherwise zeros)
//       ENROLL=driver        EngineAdapter tip/defer policy as in goodix5125.c enroll_accumulate,
//                            gallery grows until policy_complete (12 "used" samples)
//       DUMP_PROC=<file>     per preprocess call: {int32 rec, status, quality, coverage} + 5120
//                            enhanced bytes (same format as algo_eval4 DUMP_PROC)
//       INJECT_PROC=<file>   use the DLL's enhanced images/status (algo_eval4 DUMP_PROC, same
//                            protocol) instead of the port's: tests features+matcher alone
//       WARM=<passes> WARMSET=<label>  pre-train the temporal gain (kr) like algo_eval4
//       VERBOSE=1            per-probe lines
//   built with FFIXES=1 (-DPORT_FFIXES, port_feature_fixes.patch, docs/stage3-features.md):
//       gallery samples use the enroll-mode getFeature, probes the identify mode; WARM passes run
//       only the preprocessor (the DLL calls getFeature only from enrolAddImage/identifyImage);
//       with INJECT_PROC also the DLL's quality/coverage and, from <INJECT_PROC>.cbuf (written by
//       algo_eval4 DUMP_PROC), the preprocessor context bytes cbuf[0..5] are injected
//   built with OPENPP=1 (-DPORT_OPENPP, needs FFIXES=1): openchicago/src preprocessor (stage 1,
//       bit-exact) via goodix_chicago_preprocessor_process() with the DLL's purpose, quality, coverage
//       FEAT_DUMP=<file>     per usable preprocess call the probe's feature view, in the TLV
//                            block format of oracle_feat.c (compare with cmp_feat.py)
//       TPL_OUT=<file>       the packed gallery (DLL-compatible TLV, compare with cmp_tpl.py)
//       ETRACE=<file>        gallery state after every insertion (text, as algo_eval4 ETRACE)
//       STUDY=1              templateStudy after every genuine match, gallery accumulates (stage 7);
//                            STRACE=<file>, STUDY_DIR=<dir> as in algo_eval4 (docs/stage7-study.md)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goodix-chicago-calibration.h"
#include "goodix-chicago-enrollment.h"
#include "goodix-chicago-preprocess.h"
#include "goodix-chicago-runtime.h"
#include "goodix-chicago-template.h"
#include "goodix5125-calib.h"
#ifdef PORT_FFIXES
#include "goodix-chicago-feature-dll.h"
#endif

#define SW 64
#define SH 80
#define PX (SW * SH)
#define MAXREC 256

G_STATIC_ASSERT (PX == GOODIX_CHICAGO_PIXELS);

// Capture the enhanced 8-bit image produced inside prepare_probe without calling the
// stateful preprocessor twice (linked with -Wl,--wrap=...build_enhanced_checked).
GoodixChicagoPreprocessStatus __real_goodix_chicago_preprocessor_build_enhanced_checked (
  GoodixChicagoPreprocessor *, const guint16 *, guint8 *);
static guint8 last_enhanced[PX];
static GoodixChicagoPreprocessStatus last_status;
// INJECT_PROC=<algo_eval4 DUMP_PROC file>: replace the port's enhanced image and status by the
// DLL's for the same call (isolates the port's feature extraction + matcher from its preprocessor).
typedef struct { gint32 hdr[4]; guint8 img[PX]; } DumpRec;
static DumpRec *inject;
static gsize inject_n, inject_pos;
static int cur_rec;
#ifdef PORT_FFIXES
// <INJECT_PROC>.cbuf: per preprocessor call {int32 rec; uint8 cbuf[16]} (algo_eval4 DUMP_PROC)
typedef struct { gint32 rec; guint8 cbuf[16]; } CbufRec;
static CbufRec *inject_cbuf;
static gsize inject_cbuf_n;
static int inject_qc_pending;   // DLL quality/coverage of the current call not yet consumed
static guint8 inject_q, inject_c, inject_ctx[6];
static int inject_ctx_valid;
#endif
#ifdef PORT_OPENPP
static int cur_purpose;   // 1 = enrollment sample, 0 = verification (DLL preprocessor purpose)
#endif
GoodixChicagoPreprocessStatus
__wrap_goodix_chicago_preprocessor_build_enhanced_checked (GoodixChicagoPreprocessor *p,
                                                           const guint16 *raw, guint8 *enhanced)
{
#ifdef PORT_OPENPP
  // OPENPP=1: openchicago's bit-exact preprocessor() with purpose, quality, coverage and
  // the cbuf[0..5] context of 0x180043c70 (replaces the port's approximation)
  {
    guint8 q = 0, c = 0;
    last_status = goodix_chicago_preprocessor_process_context (p, raw, cur_purpose, FALSE, enhanced,
                                                               &q, &c, inject_ctx);
    inject_qc_pending = 1;
    inject_q = q;
    inject_c = c;
    inject_ctx_valid = 1;
  }
#else
  last_status = __real_goodix_chicago_preprocessor_build_enhanced_checked (p, raw, enhanced);
#endif
  if (inject)
    {
      if (inject_pos >= inject_n || inject[inject_pos].hdr[0] != cur_rec)
        {
          fprintf (stderr, "INJECT_PROC: call %zu is rec %d, dump has %d — protocols differ\n",
                   inject_pos, cur_rec, inject_pos < inject_n ? inject[inject_pos].hdr[0] : -1);
          exit (3);
        }
      memcpy (enhanced, inject[inject_pos].img, PX);
#ifdef PORT_FFIXES
      inject_qc_pending = 1;
      inject_q = inject[inject_pos].hdr[2];
      inject_c = inject[inject_pos].hdr[3];
      inject_ctx_valid = inject_cbuf && inject_pos < inject_cbuf_n &&
                         inject_cbuf[inject_pos].rec == cur_rec;
      if (inject_ctx_valid)
        memcpy (inject_ctx, inject_cbuf[inject_pos].cbuf, 6);
#endif
      // DLL rc 0x7531 = poor capture; any other non-zero rc treated as bad input
      last_status = inject[inject_pos].hdr[1] == 0 ? GOODIX_CHICAGO_PREPROCESS_STATUS_OK :
                    inject[inject_pos].hdr[1] == 0x7531 ? GOODIX_CHICAGO_PREPROCESS_STATUS_POOR_CAPTURE :
                    GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT;
      inject_pos++;
    }
  memcpy (last_enhanced, enhanced, PX);
  return last_status;
}

#ifdef PORT_FFIXES
void __real_goodix_chicago_preprocessor_finalize_metrics (gint, gint, guint8 *, guint8 *);
void
__wrap_goodix_chicago_preprocessor_finalize_metrics (gint base_quality, gint coverage,
                                                     guint8 *quality_out, guint8 *coverage_out)
{
  if (inject_qc_pending)
    {
      inject_qc_pending = 0;
      if (quality_out)
        *quality_out = inject_q;
      if (coverage_out)
        *coverage_out = inject_c;
      return;
    }
  __real_goodix_chicago_preprocessor_finalize_metrics (base_quality, coverage, quality_out,
                                                       coverage_out);
}

void __real_goodix_chicago_feature_preprocessor_context (GoodixChicagoPreprocessor *,
                                                         const guint16 *, guint8 *);
void
__wrap_goodix_chicago_feature_preprocessor_context (GoodixChicagoPreprocessor *p,
                                                    const guint16 *raw, guint8 *context)
{
#ifdef PORT_OPENPP
  // OPENPP=1: the context of the same openchicago preprocessor call (or the DLL's with INJECT)
  if (inject_ctx_valid)
    {
      memcpy (context, inject_ctx, 6);
      return;
    }
#endif
  if (inject && inject_ctx_valid)
    {
      memcpy (context, inject_ctx, 6);
      return;
    }
  if (inject && inject_cbuf == NULL)
    {
      static int warned;
      if (!warned++)
        fprintf (stderr, "INJECT_PROC without .cbuf sidecar: port's own preprocessor context\n");
    }
  __real_goodix_chicago_feature_preprocessor_context (p, raw, context);
}
#endif

static guint16 *raw, *bg;
static char labels[MAXREC][16];
static int nrec;
static GoodixChicagoPreprocessor *pp;
static GoodixChicagoRuntimeProbe *probes[MAXREC];
static int rejects[MAXREC];
static FILE *dump, *feat_dump;

static void
put_sec (guint8 **p, const char tag[4], const void *data, guint32 len)
{
  memcpy (*p, tag, 4);
  memcpy (*p + 4, &len, 4);
  if (len)
    memcpy (*p + 8, data, len);
  *p += 8 + len;
}

// FEAT_DUMP: normalized feature view of the probe (tags documented in cmp_feat.py)
static void
dump_features (int rec, const GoodixChicagoSubtemplateView *v)
{
  static guint8 buf[1 << 20];
  guint8 *p = buf + 8;
  gint32 hdr[7] = { rec, -1, 0, (gint32) v->quality, (gint32) v->coverage, 80, 64 };
  guint32 dens[3] = { v->density_positive_percent, v->density_class, v->density_inactive_count };
  guint32 active = v->active_count;
  put_sec (&p, "HDR ", hdr, sizeof hdr);
  put_sec (&p, "RECS", v->records, v->record_count * sizeof (GoodixChicagoFeatureRecord));
  put_sec (&p, "ACTV", &active, 4);
  put_sec (&p, "DENS", dens, sizeof dens);
  put_sec (&p, "LAUX", v->live_auxiliary.values, 6);
  if (v->metric_data)
    {
      guint32 pres = v->metric_data->packed_resolution;
      put_sec (&p, "PRES", &pres, 4);
      put_sec (&p, "PRIM", v->metric_data->primary, sizeof v->metric_data->primary);
      put_sec (&p, "SECO", v->metric_data->secondary, sizeof v->metric_data->secondary);
      put_sec (&p, "VALI", v->metric_data->validity, sizeof v->metric_data->validity);
      put_sec (&p, "COAR", v->metric_data->coarse_mask, sizeof v->metric_data->coarse_mask);
      put_sec (&p, "POSM", v->metric_data->position_map, sizeof v->metric_data->position_map);
    }
  guint32 len = (guint32) (p - buf - 8);
  memcpy (buf, "FEAT", 4);
  memcpy (buf + 4, &len, 4);
  fwrite (buf, 1, p - buf, feat_dump);
}

static void
transpose16 (guint16 *f)   // SH x SW -> SW x SH (64 rows x 80 cols)
{
  static guint16 t[PX];
  for (int r = 0; r < SH; r++)
    for (int c = 0; c < SW; c++)
      t[c * SH + r] = f[r * SW + c];
  memcpy (f, t, sizeof t);
}

enum { PP_PROBE, PP_ENROLL, PP_WARM };

static int
preprocess_mode (int i, int mode)   // 0 ok, else reject code
{
  GoodixChicagoRuntimeReject reject = GOODIX_CHICAGO_RUNTIME_REJECT_NONE;
  g_autoptr(GError) error = NULL;

  memset (last_enhanced, 0, PX);
  last_status = 0;
  cur_rec = i;
#ifdef PORT_OPENPP
  cur_purpose = mode == PP_ENROLL;
#endif
#ifdef PORT_FFIXES
  if (mode == PP_WARM)
    {
      static guint8 scratch[PX];
      GoodixChicagoPreprocessStatus st =
        __wrap_goodix_chicago_preprocessor_build_enhanced_checked (pp, raw + (size_t) i * PX, scratch);
      probes[i] = NULL;
      reject = st == GOODIX_CHICAGO_PREPROCESS_STATUS_OK ? GOODIX_CHICAGO_RUNTIME_REJECT_NONE :
               st == GOODIX_CHICAGO_PREPROCESS_STATUS_BAD_INPUT ? GOODIX_CHICAGO_RUNTIME_REJECT_BAD_INPUT :
               GOODIX_CHICAGO_RUNTIME_REJECT_POOR_CAPTURE;
    }
  else
    probes[i] = goodix_chicago_runtime_prepare_probe_mode (pp, raw + (size_t) i * PX,
                                                           mode != PP_ENROLL, &reject, &error);
#else
  probes[i] = goodix_chicago_runtime_prepare_probe (pp, raw + (size_t) i * PX, &reject, &error);
#endif
  if (!probes[i] && reject == GOODIX_CHICAGO_RUNTIME_REJECT_NONE && mode != PP_WARM)
    reject = 99;
  rejects[i] = reject;
  if (feat_dump && probes[i])
    dump_features (i, goodix_chicago_runtime_probe_get_view (probes[i]));
  if (dump)
    {
      guint8 q = 0, c = 0;
      goodix_chicago_preprocessor_finalize_metrics (
        goodix_chicago_preprocessor_compute_base_quality_from_enhanced (last_enhanced),
        goodix_chicago_preprocessor_compute_coverage (last_enhanced), &q, &c);
      gint32 hdr[4] = { i, reject ? (last_status ? (gint32) last_status : 0x1000 + (gint32) reject) : 0, q, c };
      fwrite (hdr, sizeof hdr, 1, dump);
      fwrite (last_enhanced, 1, PX, dump);
    }
  return reject;
}

static int
preprocess (int i)
{
  return preprocess_mode (i, PP_PROBE);
}

static gboolean
add_view (GoodixChicagoEnrollment *e, const GoodixChicagoSubtemplateView *v,
          GoodixChicagoEnrollmentResult *res)
{
  g_autoptr(GError) error = NULL;
  gboolean ok;

  if (goodix_chicago_enrollment_get_count (e) == 0)
    ok = goodix_chicago_enrollment_insert_first (e, v->records, v->record_count, v->active_count,
                                                 v->quality, v->coverage, v->metric_data, res, &error);
  else
    ok = goodix_chicago_enrollment_insert_next (e, v->records, v->record_count, v->active_count,
                                                v->quality, v->coverage, v->metric_data, res, &error);
  if (!ok)
    fprintf (stderr, "enroll insert: %s\n", error ? error->message : "?");
  return ok;
}

static int
hexval (int ch)
{
  return g_ascii_xdigit_value (ch);
}


/* stage 7 (docs/stage7-study.md): same STRACE/STUDY_DIR output as algo_eval4 study_trace();
 * upd is 1 when the port produced an updated gallery, 0 otherwise. */
static void
study_trace (int r, int score, int upd, GVariant *print_data, const guint8 *sensor_id, GBytes *cal)
{
  static FILE *st;
  g_autoptr(GBytes) packed = NULL;
  const guint8 *b = NULL;
  gsize n = 0;
  guint32 h = 2166136261u;

  if (goodix_chicago_print_data_parse (print_data, sensor_id, cal, &packed, NULL))
    b = g_bytes_get_data (packed, &n);
  for (gsize k = 0; k < n; k++)
    h = (h ^ b[k]) * 16777619u;
  if (getenv ("STRACE"))
    {
      if (!st)
        st = fopen (getenv ("STRACE"), "w");
      if (st)
        {
          fprintf (st, "study rec %d score %d upd %d rc 0 len %zu fnv %08x\n", r, score, upd, n, h);
          fflush (st);
        }
    }
  if (getenv ("STUDY_DIR") && b)
    {
      g_autofree gchar *path = g_strdup_printf ("%s/s%03d.tpl", getenv ("STUDY_DIR"), r);
      g_file_set_contents (path, (const gchar *) b, n, NULL);
    }
}

int
main (int argc, char **argv)
{
  int enroll_n = argc > 1 ? atoi (argv[1]) : 15;
  int verbose = getenv ("VERBOSE") != NULL;
  int driver_policy = getenv ("ENROLL") && !strcmp (getenv ("ENROLL"), "driver");
  guint8 sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN] = { 0 };
  const char *otp_hex = getenv ("OTP");

  if (otp_hex && strlen (otp_hex) == 2 * GOODIX5125_OTP_SIZE)
    {
      guint8 otp[GOODIX5125_OTP_SIZE];
      Goodix5125OtpInfo info;
      g_autoptr(GError) error = NULL;
      for (int k = 0; k < GOODIX5125_OTP_SIZE; k++)
        otp[k] = hexval (otp_hex[2 * k]) << 4 | hexval (otp_hex[2 * k + 1]);
      if (!goodix5125_otp_parse (otp, sizeof otp, &info, &error))
        {
          printf ("OTP rejected by goodix5125_otp_parse: %s\n", error->message);
          return 1;
        }
      memcpy (sensor_id, otp, sizeof sensor_id);
      printf ("OTP ok: tcode=0x%x fdt_delta=0x%x dac=%02x %02x %02x %02x (sensor_id = OTP[0:16])\n",
              info.tcode, info.fdt_delta, info.dac[0], info.dac[1], info.dac[2], info.dac[3]);
    }
  else
    {
      printf ("no OTP given: sensor_id = zeros (only binds print_data, not used by the algorithm)\n");
    }

  FILE *f = fopen ("frames_raw.bin", "rb");
  if (!f)
    {
      perror ("frames_raw.bin");
      return 1;
    }
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  nrec = sz / (PX * 2);
  if (nrec > MAXREC)
    nrec = MAXREC;
  raw = malloc (sz);
  if (fread (raw, 1, sz, f) != (size_t) sz)
    return 1;
  fclose (f);
  f = fopen ("bg_raw.bin", "rb");
  bg = malloc (sz);
  if (!f || fread (bg, 1, sz, f) != (size_t) sz)
    return 1;
  fclose (f);
  FILE *m = fopen ("meta_raw.txt", "r");
  int tmp;
  for (int i = 0; i < nrec; i++)
    if (fscanf (m, "%15s %d", labels[i], &tmp) != 2)
      break;
  fclose (m);
  for (int i = 0; i < nrec; i++)
    {
      transpose16 (raw + (size_t) i * PX);
      transpose16 (bg + (size_t) i * PX);
    }
  if (getenv ("DUMP_PROC"))
    dump = fopen (getenv ("DUMP_PROC"), "wb");
  if (getenv ("FEAT_DUMP"))
    feat_dump = fopen (getenv ("FEAT_DUMP"), "wb");
  if (getenv ("INJECT_PROC"))
    {
      gchar *buf;
      gsize len;
      if (!g_file_get_contents (getenv ("INJECT_PROC"), &buf, &len, NULL))
        return 1;
      inject = (DumpRec *) buf;
      inject_n = len / sizeof (DumpRec);
      printf ("INJECT_PROC: %zu DLL preprocessor outputs\n", inject_n);
#ifdef PORT_FFIXES
      g_autofree gchar *cpath = g_strconcat (getenv ("INJECT_PROC"), ".cbuf", NULL);
      gchar *cb;
      gsize clen;
      if (g_file_get_contents (cpath, &cb, &clen, NULL))
        {
          inject_cbuf = (CbufRec *) cb;
          inject_cbuf_n = clen / sizeof (CbufRec);
          printf ("INJECT_PROC: %zu preprocessor contexts from %s\n", inject_cbuf_n, cpath);
        }
#endif
    }

  int nat[MAXREC], nnat = 0;
  for (int i = 0; i < nrec; i++)
    if (!strcmp (labels[i], "natural"))
      nat[nnat++] = i;

  printf ("algo=MR648-port enroll_n=%d enroll=%s\n", enroll_n, driver_policy ? "driver-policy" : "plain");

  // calibration exactly as ensure_chicago_calibration() on first run + preprocessor_new()
  const guint16 *image_base = bg + (size_t) nat[0] * PX;
  g_autoptr(GBytes) cal = goodix_chicago_calibration_generate (image_base);
  g_autoptr(GError) error = NULL;
  pp = goodix_chicago_preprocessor_new (cal, image_base, &error);
  if (!pp)
    {
      printf ("preprocessor_new: %s\n", error->message);
      return 1;
    }

  // WARM=<passes> [WARMSET=<label>]: feed frames through the (stateful) preprocessor
  // before the evaluation, exactly like algo_eval4 (kr / temporal gain learning).
  int warm = getenv ("WARM") ? atoi (getenv ("WARM")) : 0;
  for (int w = 0; w < warm; w++)
    for (int i = 0; i < nrec; i++)
      if (!getenv ("WARMSET") || !strcmp (labels[i], getenv ("WARMSET")))
        {
          preprocess_mode (i, PP_WARM);
          g_clear_pointer (&probes[i], goodix_chicago_runtime_probe_free);
        }

  // gallery
  g_autoptr(GoodixChicagoEnrollment) acc = goodix_chicago_enrollment_new ();
  GoodixChicagoEngineEnrollmentPolicy policy;
  GoodixChicagoRuntimeProbe *deferred = NULL;
  goodix_chicago_engine_enrollment_policy_init (&policy);
  int ne = 0, added = 0, gal_end = 0, tips = 0;
  for (int k = 0; k < nnat; k++)
    {
      if (driver_policy ? goodix_chicago_engine_enrollment_policy_complete (&policy) : ne >= enroll_n)
        break;
      gal_end = k + 1;
      int r = nat[k];
      if (preprocess_mode (r, PP_ENROLL) != 0)
        continue;
      ne++;
      GoodixChicagoEnrollmentResult res = { 0 };
      if (!add_view (acc, goodix_chicago_runtime_probe_get_view (probes[r]), &res))
        continue;
      added++;
      if (getenv ("ETRACE"))   // stage 4: same text as algo_eval4 ETRACE
        {
          static FILE *et;
          if (!et)
            et = fopen (getenv ("ETRACE"), "w");
          guint nsub = goodix_chicago_enrollment_get_count (acc);
          guint nrel = goodix_chicago_enrollment_get_transform_count (acc);
          fprintf (et, "step %d nsub %u nrel %u grp", ne, nsub, nrel);
          for (guint j = 0; j < nsub; j++)
            {
              GoodixChicagoSubtemplateView sv;
              goodix_chicago_enrollment_get_subtemplate (acc, j, &sv);
              fprintf (et, " %u", sv.group_state);
            }
          fprintf (et, "\n");
          for (guint j = 0; j < nrel; j++)
            {
              GoodixChicagoRelation rl;
              goodix_chicago_enrollment_get_relation (acc, j, &rl);
              fprintf (et, "  rel %u inl %d t %d %d %d %d %d %d\n", j, rl.inlier_count, rl.transform[0],
                       rl.transform[1], rl.transform[2], rl.transform[3], rl.transform[4], rl.transform[5]);
            }
          fflush (et);
        }
      if (!driver_policy)
        continue;
      guint rej = 0;
      if (!goodix_chicago_engine_enrollment_policy_accept (&policy, res.position_x, res.position_y, &rej))
        tips++;
      if (policy.defer_current_sample)
        {
          goodix_chicago_enrollment_drop_last (acc, NULL);
          deferred = probes[r];
        }
      if (policy.restore_deferred_sample && deferred)
        {
          GoodixChicagoEnrollmentResult dres = { 0 };
          add_view (acc, goodix_chicago_runtime_probe_get_view (deferred), &dres);
          deferred = NULL;
        }
    }
  g_autoptr(GBytes) packed = goodix_chicago_enrollment_pack (acc, &error);
  if (!packed)
    {
      printf ("pack: %s\n", error->message);
      return 2;
    }
  if (getenv ("TPL_OUT"))   // stage 4: packed gallery for cmp_tpl.py, plus a pack(unpack()) round trip
    {
      gsize plen = 0;
      const guint8 *pdata = g_bytes_get_data (packed, &plen);
      g_file_set_contents (getenv ("TPL_OUT"), (const gchar *) pdata, plen, NULL);
      g_autoptr(GError) rerr = NULL;
      g_autoptr(GoodixChicagoEnrollment) back = goodix_chicago_enrollment_unpack (pdata, plen, &rerr);
      g_autoptr(GBytes) repacked = back ? goodix_chicago_enrollment_pack (back, &rerr) : NULL;
      printf ("template round trip: %s\n", repacked && g_bytes_equal (repacked, packed) ? "identical"
              : rerr ? rerr->message : "DIFFERENT");
    }
  g_autoptr(GVariant) print_data = goodix_chicago_print_data_build (sensor_id, cal, packed);
  printf ("gallery: %d natural consumed, %d offered, %d accepted, %u subtemplates%s, packed %zu bytes\n",
          gal_end, ne, added, goodix_chicago_enrollment_get_count (acc),
          driver_policy ? (tips ? " (policy tips seen)" : "") : "", g_bytes_get_size (packed));

  // probes (STUDY=1: the gallery accumulates templateStudy updates, as algo_eval4 STUDY=1)
  int study = getenv ("STUDY") != NULL;
  const char *grp[3] = { "natural", "genuine", "impostor" };
  int tot[3] = { 0 }, hit[3] = { 0 }, hitd[3] = { 0 }, bad[3] = { 0 };
  int is_gal[MAXREC] = { 0 };
  for (int k = 0; k < gal_end; k++)
    is_gal[nat[k]] = 1;
  for (int i = 0; i < nrec; i++)
    {
      if (is_gal[i])
        continue;
      int g = !strcmp (labels[i], "natural") ? 0 : !strcmp (labels[i], "genuine") ? 1 : 2;
      if (preprocess (i) != 0)
        {
          bad[g]++;
          if (verbose)
            printf ("  %-8s %3d reject=%d\n", labels[i], i, rejects[i]);
          continue;
        }
      GoodixChicagoMatchTemplateResult res;
      g_autoptr(GError) merr = NULL;
      memset (&res, 0, sizeof res);
      if (getenv ("MTRACE"))   /* probe number for the MTRACE "PROBE" block that follows */
        fprintf (stderr, "PROBENO %d\n", i);
      if (!goodix_chicago_runtime_match_print_data (probes[i], print_data, sensor_id, cal, &res, &merr))
        {
          printf ("match error rec %d: %s\n", i, merr->message);
          bad[g]++;
          continue;
        }
      int match = res.score > 0;
      int match_idx = res.score > 0 && res.selected_index >= 0;
      if (study && match && g < 2)   /* stage 7: EngineAdapter score > 0 -> templateStudy */
        {
          g_autoptr(GVariant) updated = NULL;
          g_autoptr(GError) serr = NULL;
          if (!goodix_chicago_runtime_study_print_data (probes[i], print_data, sensor_id, cal, &res,
                                                        &updated, &serr))
            printf ("study error rec %d: %s\n", i, serr->message);
          if (updated)
            {
              g_variant_unref (print_data);
              print_data = g_variant_ref (updated);
            }
          study_trace (i, res.score, updated != NULL, print_data, sensor_id, cal);
        }
      tot[g]++;
      hit[g] += match;
      hitd[g] += match_idx;
      if (verbose || (g == 2 && match))
        {
          const GoodixChicagoSubtemplateView *v = goodix_chicago_runtime_probe_get_view (probes[i]);
          printf ("  %-8s %3d q=%3u c=%3u rec=%3u score=%5d idx=%2d %s\n", labels[i], i, v->quality,
                  v->coverage, v->record_count, res.score, res.selected_index,
                  match ? "MATCH" : "-");
        }
    }
  for (int g = 0; g < 3; g++)
    printf ("%-8s: %3d/%3d matched (%5.1f%%)  [%d unusable]  (with subtemplate idx>=0: %d)\n", grp[g], hit[g],
            tot[g], tot[g] ? 100.0 * hit[g] / tot[g] : 0, bad[g], hitd[g]);
  int gt = tot[0] + tot[1], gh = hit[0] + hit[1];
  printf ("FRR(genuine all) %.1f%%  FAR %.2f%%\n", gt ? 100.0 * (gt - gh) / gt : 0,
          tot[2] ? 100.0 * hit[2] / tot[2] : 0);
  if (dump)
    fclose (dump);
  if (feat_dump)
    fclose (feat_dump);
  for (int i = 0; i < nrec; i++)
    goodix_chicago_runtime_probe_free (probes[i]);
  goodix_chicago_preprocessor_free (pp);
  return 0;
}
