/*
 * Expected behaviour with libfprint-suspend-removed.patch: a device removed
 * while suspended can be closed (and so leaves FpContext), and a suspended
 * action of a driver that does not handle the removal is cancelled.
 *
 * Built against libfprint's private test helpers (test-device-fake.c).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <libfprint/fprint.h>

#include "fpi-device.h"
#include "test-device-fake.h"

typedef struct
{
  gboolean done;
  GError  *error;
} Result;

static void
verify_cb (GObject *obj, GAsyncResult *res, gpointer user_data)
{
  Result *r = user_data;

  fp_device_verify_finish (FP_DEVICE (obj), res, NULL, NULL, &r->error);
  r->done = TRUE;
}

static void
suspend_cb (GObject *obj, GAsyncResult *res, gpointer user_data)
{
  Result *r = user_data;

  fp_device_suspend_finish (FP_DEVICE (obj), res, &r->error);
  r->done = TRUE;
}

static void
removed_cb (FpDevice *device, gpointer user_data)
{
  *(gboolean *) user_data = TRUE;
}

/* Verify waits "for a finger" forever, like an MCU with finger detection. */
static void
pending_verify (FpDevice *device)
{
}

/* A cancelled wait completes the verify with the cancellation error. */
static void
cancel_verify (FpDevice *device)
{
  fpi_device_verify_complete (device,
                              g_error_new_literal (G_IO_ERROR,
                                                   G_IO_ERROR_CANCELLED,
                                                   "cancelled"));
}

/* Driver keeps the verify running across suspend (synaptics style). */
static void
keep_suspend (FpDevice *device)
{
  fpi_device_suspend_complete (device, NULL);
}

static void
keep_resume (FpDevice *device)
{
  fpi_device_resume_complete (device, NULL);
}

static void
iterate (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static FpDevice *
open_verifying_device (FpDeviceClass *cls, Result *verify)
{
  FpDevice *device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpPrint) print = fp_print_new (device);

  g_assert_true (fp_device_open_sync (device, NULL, NULL));
  fp_device_verify (device, print, NULL, NULL, NULL, NULL, verify_cb, verify);
  iterate ();
  g_assert_false (verify->done);
  return device;
}

/* Driver without suspend vfunc: the client cannot release the device
 * during the suspend; once the device is removed it can. */
static void
test_removed_device_can_be_closed (void)
{
  FpDeviceClass *cls = g_type_class_ref (FPI_TYPE_DEVICE_FAKE);
  FpDeviceClass saved = *cls;
  Result verify = { 0 }, suspend = { 0 };
  g_autoptr(GError) error = NULL;
  gboolean removed = FALSE;
  FpDevice *device;

  cls->verify = pending_verify;
  cls->cancel = cancel_verify;
  cls->suspend = NULL;
  cls->resume = NULL;
  device = open_verifying_device (cls, &verify);
  g_signal_connect (device, "removed", G_CALLBACK (removed_cb), &removed);

  fp_device_suspend (device, NULL, suspend_cb, &suspend);
  iterate ();
  g_assert_error (verify.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);

  fpi_device_remove (device);
  iterate ();
  g_assert_true (removed);

  g_assert_false (fp_device_close_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_clear_error (&error);
  g_assert_false (fp_device_is_open (device));

  g_assert_false (fp_device_resume_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);

  g_clear_error (&verify.error);
  g_clear_error (&suspend.error);
  *cls = saved;
  g_object_unref (device);
  g_type_class_unref (cls);
}

/* Driver keeping the action over suspend without handling the removal
 * itself: the removal cancels the action; resume, then close work. */
static void
test_removed_suspended_action_is_cancelled (void)
{
  FpDeviceClass *cls = g_type_class_ref (FPI_TYPE_DEVICE_FAKE);
  FpDeviceClass saved = *cls;
  Result verify = { 0 }, suspend = { 0 };
  g_autoptr(GError) error = NULL;
  gboolean removed = FALSE;
  FpDevice *device;

  cls->verify = pending_verify;
  cls->cancel = cancel_verify;
  cls->suspend = keep_suspend;
  cls->resume = keep_resume;
  device = open_verifying_device (cls, &verify);
  g_signal_connect (device, "removed", G_CALLBACK (removed_cb), &removed);

  fp_device_suspend (device, NULL, suspend_cb, &suspend);
  iterate ();
  g_assert_no_error (suspend.error);

  fpi_device_remove (device);
  iterate ();
  g_assert_true (verify.done);
  g_assert_error (verify.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_assert_true (removed);

  g_assert_false (fp_device_resume_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_clear_error (&error);

  g_assert_false (fp_device_close_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
  g_assert_false (fp_device_is_open (device));

  g_clear_error (&verify.error);
  *cls = saved;
  g_object_unref (device);
  g_type_class_unref (cls);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/core/suspend-removed-fixed/close",
                   test_removed_device_can_be_closed);
  g_test_add_func ("/core/suspend-removed-fixed/cancel-action",
                   test_removed_suspended_action_is_cancelled);

  return g_test_run ();
}
