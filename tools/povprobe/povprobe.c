// povprobe: UEFI application that asks the Goodix 27c6:5125 MCU for its state
// before any OS driver touches the sensor.
//
// Purpose: find out whether the MCU keeps a fingerprint frame captured when the
// power button is pressed (Goodix "POV" / "OneKey" image, see README.md). The
// Windows driver logs the reply of query_mcu_state (0xae) as
//   version, isPOVImageValid, isTlsConnected, isLocked, captured_num(%d),
//   EC_Falling_Cnt, WakeupToPovTime ms, WakeupSrc, OneKeyProcedure,
//   FdtTxFailUsbSuspend
// The byte layout below follows decode_mcu_state() in tools/goodix5125/goodix.py
// and is partly a guess; the raw reply is always logged.
//
// After the first query the state is polled every POLL_INTERVAL seconds while
// the POV bit stays set, to measure how long the MCU itself keeps the frame.
//
// Only read-only commands are sent: query_mcu_state (0xae) and firmware
// version (0xa8). No reset, no flash, no PSK.
//
// Output goes to the console and to the NVRAM variable
// PovprobeLog-cbdbd978-a794-476a-b8c2-8592c5b04f79 (last run only). Not to a
// file: on resume from hibernation the ESP is still mounted by the frozen
// kernel, and writing to it behind its back corrupts its view of the FAT.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;
typedef UINTN EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;

#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)

typedef struct {
  UINT32 d1;
  UINT16 d2, d3;
  UINT8  d4[8];
} EFI_GUID;

typedef struct {
  UINT64 Signature;
  UINT32 Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

typedef struct {
  UINT16 ScanCode;
  CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct SIMPLE_INPUT {
  void      *Reset;
  EFI_STATUS (*ReadKeyStroke)(struct SIMPLE_INPUT *, EFI_INPUT_KEY *);
  EFI_EVENT  WaitForKey;
} SIMPLE_INPUT;

typedef struct SIMPLE_OUTPUT {
  void      *Reset;
  EFI_STATUS (*OutputString)(struct SIMPLE_OUTPUT *, CHAR16 *);
} SIMPLE_OUTPUT;

typedef struct {
  UINT16 Year;
  UINT8  Month, Day, Hour, Minute, Second, Pad1;
  UINT32 Nanosecond;
  INT16  TimeZone;
  UINT8  Daylight, Pad2;
} EFI_TIME;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  EFI_STATUS       (*GetTime)(EFI_TIME *, void *);
  void            *SetTime, *GetWakeupTime, *SetWakeupTime;
  void            *SetVirtualAddressMap, *ConvertPointer;
  void            *GetVariable, *GetNextVariableName;
  EFI_STATUS       (*SetVariable)(CHAR16 *, EFI_GUID *, UINT32, UINTN, void *);
} EFI_RUNTIME_SERVICES;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  void            *RaiseTPL, *RestoreTPL;
  void            *AllocatePages, *FreePages, *GetMemoryMap;
  void            *AllocatePool;
  EFI_STATUS       (*FreePool)(void *);
  void            *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent;
  void            *CloseEvent, *CheckEvent;
  void            *InstallProtocolInterface, *ReinstallProtocolInterface;
  void            *UninstallProtocolInterface;
  EFI_STATUS       (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
  void            *Reserved, *RegisterProtocolNotify, *LocateHandle;
  void            *LocateDevicePath, *InstallConfigurationTable;
  void            *LoadImage, *StartImage, *Exit, *UnloadImage;
  void            *ExitBootServices, *GetNextMonotonicCount;
  EFI_STATUS       (*Stall)(UINTN);
  EFI_STATUS       (*SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
  EFI_STATUS       (*ConnectController)(EFI_HANDLE, EFI_HANDLE *, void *, BOOLEAN);
  void            *DisconnectController;
  void            *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
  void            *ProtocolsPerHandle;
  EFI_STATUS       (*LocateHandleBuffer)(UINT32, EFI_GUID *, void *, UINTN *,
                                         EFI_HANDLE **);
} EFI_BOOT_SERVICES;

#define ALL_HANDLES 0
#define BY_PROTOCOL 2

typedef struct {
  EFI_TABLE_HEADER      Hdr;
  CHAR16               *FirmwareVendor;
  UINT32                FirmwareRevision;
  EFI_HANDLE            ConsoleInHandle;
  SIMPLE_INPUT         *ConIn;
  EFI_HANDLE            ConsoleOutHandle;
  SIMPLE_OUTPUT        *ConOut;
  EFI_HANDLE            StandardErrorHandle;
  SIMPLE_OUTPUT        *StdErr;
  EFI_RUNTIME_SERVICES *RuntimeServices;
  EFI_BOOT_SERVICES    *BootServices;
} EFI_SYSTEM_TABLE;

#define VAR_NV_BS_RT 7

#pragma pack(push, 1)
typedef struct {
  UINT8  bLength, bDescriptorType;
  UINT16 bcdUSB;
  UINT8  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
  UINT16 idVendor, idProduct, bcdDevice;
  UINT8  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} USB_DEVICE_DESCRIPTOR;

typedef struct {
  UINT8 bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
  UINT8 bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
  UINT8 iInterface;
} USB_INTERFACE_DESCRIPTOR;

typedef struct {
  UINT8  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
  UINT16 wMaxPacketSize;
  UINT8  bInterval;
} USB_ENDPOINT_DESCRIPTOR;
#pragma pack(pop)

typedef struct USB_IO {
  void      *UsbControlTransfer;
  EFI_STATUS (*UsbBulkTransfer)(struct USB_IO *, UINT8, void *, UINTN *, UINTN,
                                UINT32 *);
  void      *UsbAsyncInterruptTransfer, *UsbSyncInterruptTransfer;
  void      *UsbIsochronousTransfer, *UsbAsyncIsochronousTransfer;
  EFI_STATUS (*UsbGetDeviceDescriptor)(struct USB_IO *, USB_DEVICE_DESCRIPTOR *);
  void      *UsbGetConfigDescriptor;
  EFI_STATUS (*UsbGetInterfaceDescriptor)(struct USB_IO *,
                                          USB_INTERFACE_DESCRIPTOR *);
  EFI_STATUS (*UsbGetEndpointDescriptor)(struct USB_IO *, UINT8,
                                         USB_ENDPOINT_DESCRIPTOR *);
} USB_IO;

static EFI_GUID usb_io_guid = { 0x2b2f68d6, 0x0cd2, 0x44cf,
                                { 0x8e, 0x8b, 0xbb, 0xa2, 0x0b, 0x1b, 0x5b, 0x75 } };
static EFI_GUID log_guid = { 0xcbdbd978, 0xa794, 0x476a,
                             { 0xb8, 0xc2, 0x85, 0x92, 0xc5, 0xb0, 0x4f, 0x79 } };

#define GOODIX_VID 0x27c6
#define GOODIX_PID 0x5125

#define CMD_FIRMWARE_VERSION 0xa8
#define CMD_QUERY_MCU_STATE  0xae
#define CMD_ACK              0xb0
#define FLAGS_MESSAGE        0xa0

#define POLL_INTERVAL 10   /* seconds between queries while POV is valid */
#define POLL_LIMIT    300  /* give up after this many seconds */

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* The compiler may emit calls to these for struct copies and zeroing. */
void *
memset (void *d, int c, size_t n)
{
  volatile UINT8 *p = d;

  while (n--)
    *p++ = (UINT8) c;
  return d;
}

void *
memcpy (void *d, const void *s, size_t n)
{
  volatile UINT8 *p = d;
  const UINT8 *q = s;

  while (n--)
    *p++ = *q++;
  return d;
}

/* ---- output: console and log file ---- */

static char log_buf[8192];
static UINTN log_len;

static void
put_str (char *buf, UINTN size, UINTN *pos, const char *s)
{
  while (*s && *pos + 1 < size)
    buf[(*pos)++] = *s++;
}

static void
put_num (char *buf, UINTN size, UINTN *pos, UINT64 v, int base, int width,
         char pad, int neg)
{
  char tmp[24];
  int n = 0;

  do
    {
      tmp[n++] = "0123456789abcdef"[v % base];
      v /= base;
    }
  while (v);
  if (neg)
    tmp[n++] = '-';
  while (n < width && n < (int) sizeof tmp)
    tmp[n++] = pad;
  while (n && *pos + 1 < size)
    buf[(*pos)++] = tmp[--n];
}

/* Minimal printf: %s %c %d %u %x with optional 0, width and l. */
static void
vformat (char *buf, UINTN size, const char *fmt, va_list ap)
{
  UINTN pos = 0;

  for (; *fmt && pos + 1 < size; fmt++)
    {
      int width = 0, lng = 0;
      char pad = ' ';

      if (*fmt != '%')
        {
          buf[pos++] = *fmt;
          continue;
        }
      fmt++;
      if (*fmt == '0')
        pad = '0', fmt++;
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
      if (*fmt == 'l')
        lng = 1, fmt++;

      switch (*fmt)
        {
        case 's':
          put_str (buf, size, &pos, va_arg (ap, const char *));
          break;

        case 'c':
          buf[pos++] = (char) va_arg (ap, int);
          break;

        case 'd': {
            int64_t v = lng ? va_arg (ap, int64_t) : va_arg (ap, int);
            put_num (buf, size, &pos, v < 0 ? -v : v, 10, width, pad, v < 0);
            break;
          }

        case 'u':
        case 'x': {
            UINT64 v = lng ? va_arg (ap, UINT64) : va_arg (ap, unsigned);
            put_num (buf, size, &pos, v, *fmt == 'x' ? 16 : 10, width, pad, 0);
            break;
          }

        default:
          buf[pos++] = *fmt;
        }
    }
  buf[pos] = 0;
}

static void
out (const char *fmt, ...)
{
  char line[512];
  CHAR16 wide[1100];
  UINTN i, w = 0;
  va_list ap;

  va_start (ap, fmt);
  vformat (line, sizeof line, fmt, ap);
  va_end (ap);

  for (i = 0; line[i]; i++)
    {
      if (log_len + 1 < sizeof log_buf)
        log_buf[log_len++] = line[i];
      if (line[i] == '\n')
        wide[w++] = '\r';
      wide[w++] = (UINT8) line[i];
    }
  wide[w] = 0;
  ST->ConOut->OutputString (ST->ConOut, wide);
}

static void
out_hex (const char *prefix, const UINT8 *data, UINTN len)
{
  UINTN i;

  out ("%s(%u)", prefix, (unsigned) len);
  for (i = 0; i < len; i++)
    out (" %02x", data[i]);
  out ("\n");
}

static const char *
stamp (void)
{
  static char buf[32];
  EFI_TIME t;
  UINTN pos = 0;

  if (EFI_ERROR (ST->RuntimeServices->GetTime (&t, NULL)))
    return "time unknown";
  put_num (buf, sizeof buf, &pos, t.Year, 10, 4, '0', 0);
  put_str (buf, sizeof buf, &pos, "-");
  put_num (buf, sizeof buf, &pos, t.Month, 10, 2, '0', 0);
  put_str (buf, sizeof buf, &pos, "-");
  put_num (buf, sizeof buf, &pos, t.Day, 10, 2, '0', 0);
  put_str (buf, sizeof buf, &pos, " ");
  put_num (buf, sizeof buf, &pos, t.Hour, 10, 2, '0', 0);
  put_str (buf, sizeof buf, &pos, ":");
  put_num (buf, sizeof buf, &pos, t.Minute, 10, 2, '0', 0);
  put_str (buf, sizeof buf, &pos, ":");
  put_num (buf, sizeof buf, &pos, t.Second, 10, 2, '0', 0);
  buf[pos] = 0;
  return buf;
}

/* Store the whole log of this run (it stays under the 8 KiB buffer). */
static void
log_save (void)
{
  EFI_STATUS st = ST->RuntimeServices->SetVariable ((CHAR16 *) u"PovprobeLog",
                                                    &log_guid, VAR_NV_BS_RT,
                                                    log_len, log_buf);

  if (EFI_ERROR (st))
    out ("log: SetVariable failed %lx\n", st);
}

/* ---- sensor ---- */

typedef struct {
  USB_IO *usb;
  UINT8   ep_in, ep_out;
} SENSOR;

static int
find_sensor (SENSOR *sensor)
{
  EFI_HANDLE *handles;
  UINTN count, i;
  int found = 0;

  if (EFI_ERROR (BS->LocateHandleBuffer (BY_PROTOCOL, &usb_io_guid, NULL,
                                         &count, &handles)))
    return 0;

  for (i = 0; i < count && !found; i++)
    {
      USB_IO *usb;
      USB_DEVICE_DESCRIPTOR dev;
      USB_INTERFACE_DESCRIPTOR intf;
      UINT8 e, ep_in = 0, ep_out = 0;

      if (EFI_ERROR (BS->HandleProtocol (handles[i], &usb_io_guid, (void **) &usb)) ||
          EFI_ERROR (usb->UsbGetDeviceDescriptor (usb, &dev)) ||
          dev.idVendor != GOODIX_VID || dev.idProduct != GOODIX_PID ||
          EFI_ERROR (usb->UsbGetInterfaceDescriptor (usb, &intf)))
        continue;

      out ("usb %04x:%04x bcdDevice %04x: interface %u class %02x, %u endpoints\n",
           dev.idVendor, dev.idProduct, dev.bcdDevice, intf.bInterfaceNumber,
           intf.bInterfaceClass, intf.bNumEndpoints);
      if (intf.bInterfaceClass != 0x0a && intf.bInterfaceClass != 0xff)
        continue;

      for (e = 0; e < intf.bNumEndpoints; e++)
        {
          USB_ENDPOINT_DESCRIPTOR ep;

          if (EFI_ERROR (usb->UsbGetEndpointDescriptor (usb, e, &ep)) ||
              (ep.bmAttributes & 3) != 2)
            continue;
          if (ep.bEndpointAddress & 0x80)
            ep_in = ep.bEndpointAddress;
          else
            ep_out = ep.bEndpointAddress;
        }
      if (ep_in && ep_out)
        {
          sensor->usb = usb;
          sensor->ep_in = ep_in;
          sensor->ep_out = ep_out;
          out ("using bulk endpoints in %02x out %02x\n", ep_in, ep_out);
          found = 1;
        }
    }
  BS->FreePool (handles);
  return found;
}

/* Fast boot may leave USB devices unenumerated: connect every controller. */
static void
connect_all (void)
{
  EFI_HANDLE *handles;
  UINTN count, i;

  if (EFI_ERROR (BS->LocateHandleBuffer (ALL_HANDLES, NULL, NULL, &count, &handles)))
    return;
  for (i = 0; i < count; i++)
    BS->ConnectController (handles[i], NULL, NULL, 1);
  BS->FreePool (handles);
}

static UINT8 rx[4096];

static EFI_STATUS
bulk_read (SENSOR *s, UINTN *len, UINTN timeout_ms)
{
  UINT32 usb_status = 0;

  *len = sizeof rx;
  return s->usb->UsbBulkTransfer (s->usb, s->ep_in, rx, len, timeout_ms, &usb_status);
}

/* Read whatever the MCU queued before we started (logged, not parsed). */
static void
drain (SENSOR *s)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      UINTN len;

      if (EFI_ERROR (bulk_read (s, &len, 200)) || !len)
        break;
      out_hex ("pending ", rx, len < 64 ? len : 64);
    }
}

/*
 * Send one command and wait for its reply. The 5125 MCU may drop the ACK, so
 * ACKs are logged but not required.
 */
static int
command (SENSOR *s, UINT8 cmd, const UINT8 *payload, UINTN plen,
         UINT8 *reply, UINTN *rlen)
{
  UINT8 pkt[64];
  UINT8 *p = pkt + 4;
  UINTN i, len, mlen = plen + 4;
  UINT8 sum = 0;
  UINT32 usb_status = 0;
  EFI_STATUS st;
  int timeouts = 0;

  memset (pkt, 0, sizeof pkt);
  p[0] = cmd;
  p[1] = (UINT8) ((plen + 1) & 0xff);
  p[2] = (UINT8) ((plen + 1) >> 8);
  memcpy (p + 3, payload, plen);
  for (i = 0; i < plen + 3; i++)
    sum += p[i];
  p[plen + 3] = (UINT8) (0xaa - sum);
  pkt[0] = FLAGS_MESSAGE;
  pkt[1] = (UINT8) (mlen & 0xff);
  pkt[2] = (UINT8) (mlen >> 8);
  pkt[3] = (UINT8) (pkt[0] + pkt[1] + pkt[2]);

  len = sizeof pkt;
  st = s->usb->UsbBulkTransfer (s->usb, s->ep_out, pkt, &len, 1000, &usb_status);
  if (EFI_ERROR (st))
    {
      out ("cmd %02x: write failed %lx usb %x\n", cmd, st, usb_status);
      return -1;
    }

  while (timeouts < 2)
    {
      UINT8 *m;
      UINTN mlen_in;

      st = bulk_read (s, &len, 1000);
      if (EFI_ERROR (st) || !len)
        {
          timeouts++;
          continue;
        }
      if (len < 7 || rx[0] != FLAGS_MESSAGE ||
          (UINT8) (rx[0] + rx[1] + rx[2]) != rx[3])
        {
          out_hex ("unexpected ", rx, len < 64 ? len : 64);
          continue;
        }
      m = rx + 4;
      mlen_in = (UINTN) (m[1] | m[2] << 8);
      if (mlen_in < 1 || mlen_in + 3 > len - 4)
        {
          out_hex ("bad length ", rx, len < 64 ? len : 64);
          continue;
        }
      if (m[0] == CMD_ACK)
        continue;
      if (m[0] != cmd)
        {
          out_hex ("other message ", rx, len < 64 ? len : 64);
          continue;
        }
      *rlen = mlen_in - 1;
      memcpy (reply, m + 3, *rlen);
      return 0;
    }
  out ("cmd %02x: no reply\n", cmd);
  return -1;
}

/* Returns the POV-valid bit, or -1 on error; prints the full state if verbose. */
static int
query_state (SENSOR *s, int verbose)
{
  static const UINT8 payload[] = { 0x55 };
  UINT8 d[256];
  UINTN n;

  if (command (s, CMD_QUERY_MCU_STATE, payload, sizeof payload, d, &n) || n < 2)
    return -1;
  if (!verbose)
    return d[1] & 1;
  out ("[%s] ", stamp ());
  out_hex ("mcu_state ", d, n);
  if (n < 15)
    return d[1] & 1;
  out ("  version %u  POV image valid %u  tls %u  locked %u  captured %u(%u)\n",
       d[0], d[1] & 1, (d[1] >> 1) & 1, (d[1] >> 2) & 1, d[2] >> 4, d[2] & 0xf);
  out ("  EC falling %u  wakeup->POV %u ms  wakeup src %02x  onekey %02x  fdt tx fail %u\n",
       d[9], d[10] | d[11] << 8, d[12], d[13], d[14]);
  return d[1] & 1;
}

static void
query_firmware (SENSOR *s)
{
  static const UINT8 payload[] = { 0x00, 0x00 };
  UINT8 d[256];
  UINTN n, i;

  if (command (s, CMD_FIRMWARE_VERSION, payload, sizeof payload, d, &n))
    return;
  for (i = 0; i < n && d[i]; i++)
    if (d[i] < 0x20 || d[i] > 0x7e)
      d[i] = '?';
  d[i < sizeof d ? i : sizeof d - 1] = 0;
  out ("firmware: %s\n", (const char *) d);
}

#define SCAN_ESC 0x17

/*
 * Wait up to `seconds`; returns 1 if Esc was pressed. Other keystrokes are
 * logged and ignored, so a stray key does not end the measurement.
 */
static int
wait_key (UINTN seconds)
{
  EFI_INPUT_KEY key;
  UINTN i;

  for (i = 0; i < seconds * 10; i++)
    {
      if (!EFI_ERROR (ST->ConIn->ReadKeyStroke (ST->ConIn, &key)))
        {
          if (key.ScanCode == SCAN_ESC)
            return 1;
          out ("key ignored: scan %04x char %04x\n", key.ScanCode, key.UnicodeChar);
        }
      BS->Stall (100000);
    }
  return 0;
}

EFI_STATUS
efi_main (EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
  SENSOR sensor;
  UINTN t;
  int pov;

  ST = st;
  BS = st->BootServices;
  BS->SetWatchdogTimer (0, 0, 0, NULL);

  (void) image;
  out ("=== povprobe %s ===\n", stamp ());

  if (!find_sensor (&sensor))
    {
      out ("sensor not found, connecting all controllers\n");
      connect_all ();
      if (!find_sensor (&sensor))
        {
          out ("sensor not found\n");
          goto done;
        }
    }

  drain (&sensor);
  pov = query_state (&sensor, 1);
  query_firmware (&sensor);
  log_save ();

  /* How long does the MCU itself keep the frame? */
  if (pov == 1)
    {
      out ("polling every %u s for up to %u s (Esc: stop)\n",
           POLL_INTERVAL, POLL_LIMIT);
      for (t = POLL_INTERVAL; t <= POLL_LIMIT; t += POLL_INTERVAL)
        {
          if (wait_key (POLL_INTERVAL))
            {
              out ("stopped by Esc after %u s\n", (unsigned) (t - POLL_INTERVAL));
              break;
            }
          pov = query_state (&sensor, 0);
          out ("[%s] +%u s: POV %d\n", stamp (), (unsigned) t, pov);
          if (pov != 1)
            {
              out ("POV cleared between +%u and +%u s:\n",
                   (unsigned) (t - POLL_INTERVAL), (unsigned) t);
              query_state (&sensor, 1);
              break;
            }
        }
      if (pov == 1 && t > POLL_LIMIT)
        out ("POV still valid after %u s\n", POLL_LIMIT);
    }

done:
  out ("done, continuing boot in 15 s (Esc: now)\n");
  log_save ();
  wait_key (15);
  return 0;
}
