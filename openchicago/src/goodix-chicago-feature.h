// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Modified 2026 for openchicago: getFeature 0x180013490 as in AlgoChicago.dll
 * (docs/stage3-features.md, formerly goodix-chicago-feature-dll.h). */

/* Native state for recovered Chicago feature extraction. */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GOODIX_CHICAGO_FEATURE_WIDTH 80u
#define GOODIX_CHICAGO_FEATURE_HEIGHT 64u
#define GOODIX_CHICAGO_FEATURE_PIXELS \
  (GOODIX_CHICAGO_FEATURE_WIDTH * GOODIX_CHICAGO_FEATURE_HEIGHT)
#define GOODIX_CHICAGO_FEATURE_SCALES 9u
#define GOODIX_CHICAGO_FEATURE_RECORD_LIMIT 120u

typedef struct
{
  gint32 x;
  gint32 y;
  gint32 scale;
  gint32 response;
} GoodixChicagoExtremum;

/* Exact 24-byte candidate layout consumed and produced by
 * AlgoChicago+0x15210. strength initially contains the raw DoG response. */
typedef struct
{
  gint32 x;
  gint32 y;
  gint32 scale;
  gint32 strength;
  gint16 refined_x;
  gint16 refined_y;
  gint32 scale_value;
} GoodixChicagoCandidate;

typedef struct
{
  guint16 foreground;
  gint16  refined_x;
  gint16  refined_y;
  gint16  orientation;
  gint32  strength;
  guint8  descriptor[48];
} GoodixChicagoFeatureRecord;

typedef struct
{
  gint32 strength;
  gint32 index;
} GoodixChicagoFeatureRank;

typedef struct
{
  gint32  x;
  gint32  y;
  gint32  scale_value;
  guint32 peak;
  guint32 selected_peak;
  guint16 secondary_orientation;
  guint16 reserved;
} GoodixChicagoFeatureAux;

/* Transient image-derived fields written at subtemplate +0x158/+0x15c/
 * +0x160 by AlgoChicago+0x12400. They are used while matching a live probe
 * but are not copied into the packed gallery subtemplate. */
typedef struct
{
  guint negative_percent;
  guint positive_percent;
  guint neutral_percent;
  guint density_class;
  guint inactive_count;
} GoodixChicagoFeatureConsensus;

/* Live-only six-byte record allocated at subtemplate +0x148.  The production
 * type-24 path obtains byte zero from the preprocessor resolution context and
 * byte three from the decoded packed-resolution scale. */
typedef struct
{
  guint8 values[6];
} GoodixChicagoFeatureLiveAuxiliary;

/* Exact radius-six structure-tensor orientation map built by
 * AlgoChicago+0x55a70 from the enhanced image. */
void goodix_chicago_feature_build_orientation_map (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   guint8       orientation[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Exact orientation-guided seven-sample image returned by
 * AlgoChicago+0x559c0 and passed as the first +0x114e0 argument. */
void goodix_chicago_feature_build_prepared_image (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                  guint8       prepared[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Exact mode-24 input to AlgoChicago+0x114e0. EngineAdapter keeps the sensor
 * transport geometry (80 columns x 64 rows) in the image header consumed by
 * enrolAddImage; the byte stream is filtered with that geometry by selector
 * 6. */
void goodix_chicago_feature_build_source (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                          guint8       source[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Exact nine-plane Q16 Gaussian scale space built by AlgoChicago+0x12770.
 * Plane zero and one share the feature source; planes two through eight are
 * successively filtered from the preceding plane. */
void goodix_chicago_feature_build_scale_space (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                               guint16      scales[GOODIX_CHICAGO_FEATURE_SCALES]
                                               [GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Exact mode-24 +0x167f0 difference-of-Gaussian extrema scan. The returned
 * records are ordered by scale 6..1, then y, then x, as in +0x15810. */
guint goodix_chicago_feature_collect_extrema (const guint16          scales[GOODIX_CHICAGO_FEATURE_SCALES]
                                              [GOODIX_CHICAGO_FEATURE_PIXELS],
                                              GoodixChicagoExtremum *extrema,
                                              guint                  capacity);

/* Exact mode-24 subpixel refinement and acceptance boundary at
 * AlgoChicago+0x15210. The candidate may be moved even when rejected, just
 * as in the official function. On success, the fixed-point coordinates,
 * scale value, strength, and curvature are populated. */
gboolean goodix_chicago_feature_refine_extremum (const guint16           scales[GOODIX_CHICAGO_FEATURE_SCALES]
                                                 [GOODIX_CHICAGO_FEATURE_PIXELS],
                                                 GoodixChicagoCandidate *candidate,
                                                 guint32                *curvature);

/* Exact mode-24 single-orientation materialization at AlgoChicago+0x149c0.
 * Descriptor bytes remain zero at this boundary and are populated by the
 * following +0x15c30 stage. */
void goodix_chicago_feature_materialize_candidate (const guint8                  feature_source[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   const guint32                 magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   const gint16                  orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                   const GoodixChicagoCandidate *candidate,
                                                   guint                         index,
                                                   GoodixChicagoFeatureRecord   *record,
                                                   GoodixChicagoFeatureRank     *rank,
                                                   GoodixChicagoFeatureAux      *aux);

/* Exact mode-24 +0x15c30 descriptor stage. This fills the binary descriptor
 * fields and status byte of an already materialized feature record. */
void goodix_chicago_feature_build_descriptor (const guint32                  magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                                              const gint16                   orientation[GOODIX_CHICAGO_FEATURE_PIXELS],
                                              const GoodixChicagoFeatureAux *aux,
                                              GoodixChicagoFeatureRecord    *record);

/* Exact mode-24 caller-side finalization immediately after +0x15c30. */
void goodix_chicago_feature_finalize_record (GoodixChicagoFeatureRecord *record);

/* Exact mode-24 AlgoChicago+0x509b0 feature mask: selector 7 smoothing,
 * reflected Sobel/local mean, threshold 110, and output value 1. */
void goodix_chicago_feature_build_mask (const guint8 enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                        guint8       mask[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Exact production call to AlgoChicago+0x16d90. Annotations correspond to
 * the original finalized record order and intentionally precede mask
 * compaction in the official pipeline. */
void goodix_chicago_feature_build_annotations (const guint8                      enhanced[GOODIX_CHICAGO_FEATURE_PIXELS],
                                               const guint8                      mask[GOODIX_CHICAGO_FEATURE_PIXELS],
                                               const GoodixChicagoFeatureRecord *records,
                                               guint                             count,
                                               guint8                           *annotations);

/* The same production status pass, retaining its otherwise transient result
 * scalars. @template_type controls the official 5/10 percent class threshold;
 * the normal sensor path uses type 24. */
gboolean goodix_chicago_feature_classify_status_full (GoodixChicagoFeatureRecord    *records,
                                                      guint                          count,
                                                      guint                          template_type,
                                                      GoodixChicagoFeatureConsensus *consensus);

/* Exact AlgoChicago+0x122f0 mask gate. Records whose rounded coordinate
 * lands on a zero mask pixel are removed by replacing them with the current
 * tail record, preserving the production DLL's resulting order. */
guint goodix_chicago_feature_filter_mask (const guint8                mask[GOODIX_CHICAGO_FEATURE_PIXELS],
                                          GoodixChicagoFeatureRecord *records,
                                          guint                       count);

/* Exact AlgoChicago+0x16680 on-record representation conversion. The normal
 * enrollment path passes reverse_bits=FALSE; TRUE reproduces the alternate
 * descriptor bit/tail conversion used by other modes. */
void goodix_chicago_feature_pack_record (GoodixChicagoFeatureRecord *record,
                                         gboolean                    reverse_bits);

/* Exact AlgoChicago+0x0f240 in-place class partition. Class zero records are
 * moved ahead of class one records. The returned value is context +0x108,
 * which is distinct from the total record count. If supplied, annotations
 * are swapped in parallel with their records, as at context +0x164. */
guint goodix_chicago_feature_partition_records (GoodixChicagoFeatureRecord *records,
                                                guint8                     *annotations,
                                                guint                       count);

/* Exact record[0x39] local-neighbor score from AlgoChicago+0x17030. The
 * annotation array must already have undergone the same +0x0f240 swaps as
 * its records. */
void goodix_chicago_feature_score_neighbors (GoodixChicagoFeatureRecord *records,
                                             const guint8               *annotations,
                                             guint                       count);

/* Exact first-pass signed input assembled by AlgoChicago+0x114e0. Selector 0
 * filters its prepared byte-image argument, selector 1 filters that result,
 * then the two planes are combined as 10 * filter0 - 9 * filter1. */
void goodix_chicago_feature_build_gradient_input (const guint8 feature_image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                                  gint32       input[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* Exact central-difference magnitude and Q12-radian orientation images built
 * by AlgoChicago+0x15e70. Its signed 32-bit input is the scale-pair image
 * assembled by +0x114e0. Only the inner 62 x 78 pixels are populated. */
void goodix_chicago_feature_build_gradients (const gint32 input[GOODIX_CHICAGO_FEATURE_PIXELS],
                                             guint32      magnitude[GOODIX_CHICAGO_FEATURE_PIXELS],
                                             gint16       orientation[GOODIX_CHICAGO_FEATURE_PIXELS]);

/* AlgoChicago.dll getFeature 0x180013490 (template type 24) as a whole,
 * docs/stage3-features.md.  The DLL keeps cross-frame state in globals
 * (0x1800ca198.., 0x1800d8750..) and reuses one feature object for identify;
 * here both live in this struct, owned by the openchicago session.  Initialise with goodix_chicago_feature_state_init(). */
typedef struct
{
  gint                          last_coverage;  /* 0x1800ca19c */
  gint                          history_count;  /* 0x1800ca198 */
  gint                          stable_count;   /* 0x1800d8750 */
  gint                          last_level;     /* 0x1800d8754 */
  guint8                        history[3][GOODIX_CHICAGO_FEATURE_PIXELS]; /* 0x1800ca1a0 */
  GoodixChicagoFeatureConsensus consensus;      /* identify object F+0x158..0x160 */
} GoodixChicagoFeatureState;

void goodix_chicago_feature_state_init (GoodixChicagoFeatureState *state);

gboolean goodix_chicago_feature_decode_mark (const guint8 image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                             guint32     *packed,
                                             guint8       region[GOODIX_CHICAGO_FEATURE_PIXELS]);

void goodix_chicago_feature_pack_record_mode (GoodixChicagoFeatureRecord *record,
                                              gboolean                    identify);

/* @context: bytes 0..5 of the preprocessor's per-frame buffer (cbuf).
 * @identify: 5th getFeature argument (0 enrolAddImage, 1 identifyImage).
 * Returns the record count (F+0xf0). */
guint goodix_chicago_feature_get_feature (GoodixChicagoFeatureState     *state,
                                          const guint8                   image[GOODIX_CHICAGO_FEATURE_PIXELS],
                                          const guint8                   context[6],
                                          guint                          quality,
                                          guint                          coverage,
                                          gboolean                       identify,
                                          GoodixChicagoFeatureRecord    *records,
                                          guint                          capacity,
                                          guint                         *active_count,
                                          GoodixChicagoFeatureConsensus *consensus,
                                          guint32                       *packed_resolution,
                                          guint8                         live_auxiliary[6]);

G_END_DECLS
