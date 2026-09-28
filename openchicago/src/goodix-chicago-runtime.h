// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2026 Berke Kabagöz <berkekbgz@gmail.com> */
/* Modified 2026 for openchicago: preprocessor() + getFeature as in
 * AlgoChicago.dll (stages 1, 3), getFeature state passed in by the caller
 * instead of a process global, templates as packed blobs (no print_data). */

/* High-level native Chicago composition used by the openchicago session. */

#pragma once

#include <gio/gio.h>

#include "goodix-chicago-enrollment.h"
#include "goodix-chicago-feature.h"
#include "goodix-chicago-match.h"
#include "goodix-chicago-preprocess.h"

G_BEGIN_DECLS

#define GOODIX_CHICAGO_RUNTIME_SELECTOR_THRESHOLD 207

typedef struct _GoodixChicagoRuntimeProbe GoodixChicagoRuntimeProbe;

typedef enum {
  GOODIX_CHICAGO_RUNTIME_REJECT_NONE,
  GOODIX_CHICAGO_RUNTIME_REJECT_PREPROCESS,   /* preprocessor() rc != 0, see *status */
  GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES,  /* getFeature returned no records */
} GoodixChicagoRuntimeReject;

/* Per-frame outputs of preprocessor() (0x180043c70 context included). */
typedef struct
{
  GoodixChicagoPreprocessStatus status;
  guint8                        quality;
  guint8                        coverage;
  guint8                        context[GOODIX_CHICAGO_PREPROCESS_CONTEXT_SIZE];
  guint8                        enhanced[GOODIX_CHICAGO_PIXELS];
} GoodixChicagoRuntimeFrame;

/* preprocessor(): purpose 1 (@enroll) or 0.  Returns frame->status == OK. */
gboolean goodix_chicago_runtime_preprocess (GoodixChicagoPreprocessor *preprocessor,
                                            const guint16              raw[GOODIX_CHICAGO_PIXELS],
                                            gboolean                   enroll,
                                            GoodixChicagoRuntimeFrame *frame);

/* getFeature 0x180013490 on a preprocessed frame (@identify: identifyImage
 * mode, else enrolAddImage mode) with the cross-frame @feature_state.
 * Returns NULL with *reject = NO_FEATURES when no records were found. */
GoodixChicagoRuntimeProbe *goodix_chicago_runtime_probe_new (GoodixChicagoFeatureState       *feature_state,
                                                             const GoodixChicagoRuntimeFrame *frame,
                                                             gboolean                         identify,
                                                             GoodixChicagoRuntimeReject      *reject,
                                                             GError                         **error);

/* enrolAddImage (@identify FALSE, preprocessor purpose 1) or identifyImage
 * (@identify TRUE, purpose 0) feature path: runtime_preprocess(), then
 * runtime_probe_new().  Returns NULL with *reject set when the frame is not
 * usable; @frame (may be NULL) receives the preprocessor outputs. */
GoodixChicagoRuntimeProbe *goodix_chicago_runtime_prepare_probe (GoodixChicagoPreprocessor  *preprocessor,
                                                                 GoodixChicagoFeatureState  *feature_state,
                                                                 const guint16               raw[GOODIX_CHICAGO_PIXELS],
                                                                 gboolean                    identify,
                                                                 GoodixChicagoRuntimeFrame  *frame,
                                                                 GoodixChicagoRuntimeReject *reject,
                                                                 GError                    **error);

void goodix_chicago_runtime_probe_free (GoodixChicagoRuntimeProbe *probe);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixChicagoRuntimeProbe,
                               goodix_chicago_runtime_probe_free)

const GoodixChicagoSubtemplateView *
goodix_chicago_runtime_probe_get_view (const GoodixChicagoRuntimeProbe *probe);

/* identifyImage against a packed template (templatePack TLV), unpacked afresh. */
gboolean goodix_chicago_runtime_match (const GoodixChicagoRuntimeProbe  *probe,
                                       GBytes                           *packed,
                                       GoodixChicagoMatchTemplateResult *result,
                                       GError                          **error);

/* templateStudy after a match (EngineAdapter: score > 0).  *updated is the
 * repacked template, or NULL when the DLL would not update it. */
gboolean goodix_chicago_runtime_study (const GoodixChicagoRuntimeProbe        *probe,
                                       GBytes                                 *packed,
                                       const GoodixChicagoMatchTemplateResult *result,
                                       GBytes                                **updated,
                                       GError                                **error);

G_END_DECLS
