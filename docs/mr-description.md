# goodix5125: Add support for 27c6:5125

This MR adds `goodix5125`, a host-matching driver for the Goodix **27c6:5125**
fingerprint sensor (ChicagoHS, chip ID `0x2504`). The sensor sits in the power
button of the HONOR MagicBook 16. It returns encrypted 64x80, 12-bit raw
frames, and the host does preprocessing, enrolment, matching and template
updates.

It builds on the 27c6:5125 driver proposed in
[!648](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/648).
Many thanks to its authors, Thomas97460 and Berke Kabagöz (whose ChicagoHS port
and white-box pairing code from
[berkekbgz/libfprint-goodix-spi](https://github.com/berkekbgz/libfprint-goodix-spi)
were integrated there). The existing copyright notices of the derived files
are kept, and both are credited with `Co-authored-by:` in the commits.

## What is supported

- enroll, verify, identify (`FpDevice`, no on-chip storage; prints are stored by
  the caller, e.g. fprintd);
- identify with an empty gallery reports "no match" at once, so fprintd's
  duplicate check before the first enrolment works;
- finger status reporting and retry hints (`FP_DEVICE_RETRY_GENERAL` for
  unusable touches, `FP_DEVICE_RETRY_CENTER_FINGER` when a touch covers an
  area that is already enrolled);
- autosuspend hwdb entry; the device is removed from the unsupported list.

## How it works

The protocol, pairing, state handling and device emulation are documented in
`libfprint/drivers/goodix5125/README.md`. In short:

- **Transport.** USB interface 1, bulk endpoints `0x01`/`0x81`, Goodix outer
  and inner framing. The MCU often drops the ACK when the reply follows at
  once, and late replies of earlier commands can still be queued, so every IN
  transfer is classified against the command in flight. Firmware update and
  flash erase commands are not implemented.
- **Session.** Volatile initialisation on every open (MCU reset, chip ID, OTP
  check, MCU configuration with the OTP calibration), then a TLS 1.2 PSK
  session with the host as the server (OpenSSL, memory BIOs). Images arrive
  as TLS application data.
- **Pairing.** The driver checks the sensor's PSK hash against the host PSK
  and **never writes a PSK on its own**; writing needs
  `GOODIX5125_PROVISION_PSK=random`.
- **Finger detection.** FDT-down for the touch; the lift is detected by
  polling FDT mode `0x0d`, which is reliable on this hardware, where FDT-up
  (used by !648) was not.
- **Matcher.** `chicago/` (openchicago) is a free C implementation of the
  ChicagoHS algorithm (profile 12), based on the port from !648: adaptive
  preprocessing, type-24 features, enrolment merge, identify scoring and
  template study. No binary blobs, nothing loaded at runtime, no globals.
- **Local state.** `/var/lib/fprint/goodix5125` (fprintd's
  `StateDirectory`): the host PSK, the adaptive algorithm state and learned
  templates. Files are bound to the sensor, checksummed and range-checked on
  load.
- **Device emulation.** With `FP_DEVICE_EMULATION=1` the host side is
  deterministic (fixed TLS randomness, all-zero PSK, temporary state), so a
  recorded session replays byte for byte.

## Tests

- `goodix5125` (umockdev, `tests/goodix5125/custom.py`): identify with an
  empty gallery, enrolment with 12 stages, then verify with a deserialized
  print until it matches (the recording has 3 non-matching touches before
  the match). It replays byte for byte, TLS included. It was recorded with a
  finger that is not used for login, since the frames in the capture are
  effectively unencrypted;
- `goodix5125` (unit): framing, RX classification (optional ACK, stale
  messages), OTP-based MCU configuration, image decoding, FDT bases, pairing,
  calibration persistence and rejection of out-of-range state, all on
  synthetic data;
- `goodix5125-tls`: the TLS-PSK memory-BIO transport against an in-process
  OpenSSL client, and the deterministic mode;
- `udev-hwdb`: passes with the new hwdb entry.

`meson test` passes with `-Ddrivers=all`. `ninja` shows no warnings in the new
files, and the sources are formatted with `scripts/uncrustify.cfg`.

## Known limitations

- **PSK provisioning.** A sensor that was never paired with this host (for
  example, one still paired with another operating system) needs a one-time
  PSK write with `GOODIX5125_PROVISION_PSK=random`. That write replaces the
  existing pairing.
- **Learned templates outside of fprintd.** fprintd reloads prints from disk
  for every match and never stores them again, and libfprint has no "print
  updated" signal. The driver therefore keeps template-study results in its
  own store, `/var/lib/fprint/goodix5125/learned/`: one file per print, keyed
  by the SHA-256 of the print's data, bound to the sensor, checksummed, written
  atomically with mode 0600. The driver is not told when a print is deleted, so
  entries unused for 180 days expire and at most 50 are kept (least recently
  used first). A libfprint API for updated prints would make this store
  unnecessary.
- **umockdev replay and OpenSSL.** The replay compares every host transfer
  with the recording, so it depends on the exact bytes OpenSSL produces for
  the TLS handshake with the fixed randomness. A different OpenSSL version on
  CI could change them; this is to be checked on CI. A new recording needs a
  sensor paired with the all-zero PSK.
- **One unit tested.** Only one OEM-integrated unit has been tested; the
  sensor-specific calibration and pairing make testing more units hard.

## Commits

1. `goodix5125: Add the openchicago ChicagoHS matcher`
2. `goodix5125: Add support for Goodix 27c6:5125` (driver, build, hwdb)
3. `tests: Add goodix5125 unit tests` (unit tests and the umockdev recording)
