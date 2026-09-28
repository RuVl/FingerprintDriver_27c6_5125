// Stage 5 (docs/stage5-match.md): differential test of the ported flag re-evaluation rule trees
// against AlgoChicago.dll itself (loaded with winpe, functions called at their VAs).
//
//   0x180019a30 (rec, params, conf)                 <-> match_reeval_conf_tree_type24 ()
//   0x18001fdd0 (rec, params, conf, status)         <-> match_reeval_status_tree_type24 ()
//   0x180019800 (rec, q, c, params, &conf, &status) <-> match_reevaluate_flags_type24 ()
//     (whole re-evaluation: 0x1800258c0 without penalty, 0x180023340, both trees)
// The port side is the "BEGIN reeval".."END reeval" block of the MFIXES goodix-chicago-match.c plus
// the upstream goodix_chicago_match_scheduler_evidence_type24 (extracted by reeval_difftest.sh).
//
// Records: the real ones from a dll_reeval_trace.py log (" rec ..." lines, optional), then n cases
// of which half are uniform in the ranges the matcher produces and half are real records with
// every field moved by -3..+3 (dense around the values that occur); each with every initial
// conf 0..2 / status 0..1 and packed_resolution 512/1024.  Any mismatch is printed, exit code 1.
// Build/run (tools/algo):  ./reeval_difftest.sh [n] [dll_reeval_trace.py log]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winpe.h"

#define MS __attribute__ ((ms_abi))
typedef int32_t gint32;
typedef uint32_t guint32;
typedef int gboolean;
typedef struct
{
  gint32 geometry_count;
  gint32 secondary_geometry_count;
  gint32 reserved08[2];
  gint32 selector;
  gint32 agreement;
  gint32 study_metric_18;
  gint32 study_metric_1c;
  gint32 study_metric_20;
  gint32 normalized_coverage;
  gint32 matched_percent;
  gint32 geometry_percent;
  gint32 penalty_flag_a;
  gint32 penalty_flag_b;
  gint32 reserved38[7];
  gint32 probe_quality;
  gint32 probe_coverage;
  gint32 gallery_quality;
  gint32 gallery_coverage;
  gint32 auxiliary_score;
} GoodixChicagoMatchScoreRecord;
_Static_assert (sizeof (GoodixChicagoMatchScoreRecord) == 0x68, "record layout");
typedef struct
{
  gint32 confidence;
  gint32 status;
  gint32 special;
} GoodixChicagoMatchSchedulerEvidence;
#define FALSE 0
#define TRUE 1
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define g_return_if_fail(e) do { } while (0)

#include TREES_C   /* evidence (upstream) + the reeval block (MFIXES) */

typedef void MS (*dll_conf_t) (const void *rec, const void *params, gint32 *conf);
typedef void MS (*dll_status_t) (const void *rec, const void *params, gint32 *conf, gint32 *status);
typedef void MS (*dll_reeval_t) (const void *rec, gint32 q, gint32 c, const void *params, gint32 *conf,
                                 gint32 *status);

static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint32_t
rnd (void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return (uint32_t) rng;
}
static int
pick (int lo, int hi)
{
  return lo + (int) (rnd () % (uint32_t) (hi - lo + 1));
}

// value ranges per record dword (index = offset / 4), as seen in the matcher's records
static const int lo_[26] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int hi_[26] = { 40, 40, 40, 40, 300, 300, 300, 300, 300, 260, 100, 100, 1, 1, 1, 1, 1, 1, 1, 1, 1, 100, 100, 100, 100, 100 };

static long bad, tested, changed[2], rbad, rchanged;
static dll_conf_t dconf;
static dll_status_t dstatus;
static dll_reeval_t dreeval;
// params as built by 0x18002f300 for type 24, flag 0, studyflag 1 (dumped at 0x180019800)
static gint32 params[24] = { 0, 0, 5, 218, 1, 1, 23, 47, 40, 38, -1, 16, 137, 1, 0, 24, 1, 0, 512, 1, 0, 14613, 5120, 0 };

static void
check (const gint32 *r)
{
  params[18] = (rnd () & 1) ? 512 : 1024;
  for (int c0 = 0; c0 < 3; c0++)
    for (int s0 = 0; s0 < 2; s0++)
      {
        gint32 dc = c0, pc = c0, ds = s0, ps = s0;
        dconf (r, params, &dc);
        match_reeval_conf_tree_type24 ((const GoodixChicagoMatchScoreRecord *) r, &pc);
        gint32 dc2 = c0, pc2 = c0;
        dstatus (r, params, &dc2, &ds);
        match_reeval_status_tree_type24 ((const GoodixChicagoMatchScoreRecord *) r, params[18], &pc2, &ps);
        {
          gint32 q = (rnd () & 1) ? pick (0, 100) : r[21], c = (rnd () & 1) ? pick (0, 100) : r[22];
          gint32 rc_d = c0, rs_d = s0, rc_p = c0, rs_p = s0;
          dreeval (r, q, c, params, &rc_d, &rs_d);
          match_reevaluate_flags_type24 ((const GoodixChicagoMatchScoreRecord *) r, q, c,
                                         (guint32) params[18], &rc_p, &rs_p);
          rchanged += rc_d != c0 || rs_d != s0;
          if (rc_d != rc_p || rs_d != rs_p)
            {
              if (rbad++ < 5)
                {
                  printf ("REEVAL MISMATCH q=%d c=%d conf %d status %d -> dll %d/%d port %d/%d; rec", q, c, c0, s0,
                          rc_d, rs_d, rc_p, rs_p);
                  for (int k = 0; k < 26; k++)
                    printf (" %d", r[k]);
                  printf ("\n");
                }
            }
        }
        tested++;
        changed[0] += dc != c0;
        changed[1] += ds != s0;
        if (dc != pc || dc2 != pc2 || ds != ps)
          {
            if (bad++ < 5)
              {
                printf ("MISMATCH conf %d/%d -> dll %d port %d; status tree conf %d->%d/%d status %d->%d/%d; rec",
                        c0, c0, dc, pc, c0, dc2, pc2, s0, ds, ps);
                for (int k = 0; k < 26; k++)
                  printf (" %d", r[k]);
                printf ("\n");
              }
          }
      }
}

int
main (int argc, char **argv)
{
  long n = argc > 1 ? atol (argv[1]) : 2000000;
  const char *real = argc > 2 ? argv[2] : NULL;
  if (!winpe_load ("../../win-driver/AlgoChicago.dll"))
    return 2;
  uint8_t *base = winpe_base ();
  dconf = (dll_conf_t) (base + (0x180019a30 - 0x180000000));
  dstatus = (dll_status_t) (base + (0x18001fdd0 - 0x180000000));
  dreeval = (dll_reeval_t) (base + (0x180019800 - 0x180000000));
  FILE *rf = real ? fopen (real, "r") : NULL;
  char line[4096];
  static gint32 pool[4096][26];
  int npool = 0;
  for (long i = 0; i < n || rf; i++)
    {
      gint32 r[26];
      if (rf)
        {
          int ok = 0;
          while (fgets (line, sizeof line, rf))
            if (!strncmp (line, "  rec ", 6))
              {
                char *p = line + 6;
                for (int k = 0; k < 26; k++)
                  r[k] = (gint32) strtol (p, &p, 10);
                if (npool < 4096)
                  memcpy (pool[npool++], r, sizeof r);
                ok = 1;
                break;
              }
          if (!ok)
            {
              fclose (rf);
              rf = NULL;
              i = -1;
              continue;
            }
        }
      else if (npool && (rnd () & 1))
        for (int k = 0, q = (int) (rnd () % npool); k < 26; k++)
          r[k] = pool[q][k] + ((rnd () & 3) ? pick (-3, 3) : 0);
      else
        for (int k = 0; k < 26; k++)
          r[k] = pick (lo_[k], hi_[k]);
      check (r);
    }
  // sweep: every field of every real record (and of as many random ones) over its whole range
  for (int q = 0; q < 2 * npool; q++)
    {
      gint32 base_r[26], r[26];
      for (int k = 0; k < 26; k++)
        base_r[k] = q < npool ? pool[q][k] : pick (lo_[k], hi_[k]);
      for (int k = 0; k < 26; k++)
        for (int v = lo_[k] - 3; v <= hi_[k] + 3; v++)
          {
            memcpy (r, base_r, sizeof r);
            r[k] = v;
            check (r);
          }
    }
  printf ("reeval trees: %ld cases, %ld mismatches (conf changed in %ld, status in %ld)\n", tested, bad,
          changed[0], changed[1]);
  printf ("reeval 0x180019800: %ld cases, %ld mismatches (flags changed in %ld)\n", tested, rbad, rchanged);
  return bad != 0 || rbad != 0;
}
