# goodix: Add support for 27c6:5125

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
were integrated there). The copyright notices of !648 are kept in every
derived file, and both are credited with `Co-authored-by:` in the commits.

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

**Transport.** USB interface 1, bulk endpoints `0x01`/`0x81`, Goodix outer
(`0xa0` command, `0xb0`/`0xb2` TLS) and inner command framing. The MCU often
drops the ACK when the reply comes right after it, and a late reply to an
earlier command (usually FDT-down) can still be queued. Every IN transfer is
therefore classified against the command in flight: its ACK, its reply, or a
stale message that is skipped. The ACK is optional. Firmware update and flash
erase commands are not implemented.

**Session.** Initialisation runs on every open and is volatile. It does an MCU
reset, checks the chip ID, validates the OTP and sends the MCU configuration
with the OTP calibration applied. Then the host acts as a **TLS 1.2 PSK server**
(`PSK-AES128-GCM-SHA256`, OpenSSL with memory BIOs) and the sensor is the
client. Finger images arrive as TLS application data.

**Pairing / PSK.** The sensor stores a write-only white-box container of the
PSK and reports its SHA-256. The driver rebuilds that container from the host
PSK (`psk` in the state directory) and uses the key only if the hashes match.
**The driver never writes a PSK on its own.** Writing needs
`GOODIX5125_PROVISION_PSK=random` and can invalidate the pairing of another
operating system on the same machine.

**Finger detection (FDT).** After a frame, FDT mode `0x0d` is armed, followed by
an FDT-down probe (no reply expected within 2 s), an MCU state query and
FDT-down `0x0c`. The lift is detected by polling FDT mode `0x0d` until the touch
mask is clear. This sequence is reliable on 27c6:5125 hardware, where FDT-up
(used by !648 for the lift) was not.

**Matcher: openchicago.** `libfprint/drivers/goodix5125/chicago/` is a free C
implementation of the ChicagoHS algorithm (profile 12), based on the port from
!648. It contains no binary blobs and loads nothing at runtime. It covers:

- preprocessing with an adaptive calibration (gain/kr/multiplier planes),
  quality and coverage, the capture policy and the per-frame context;
- type-24 feature extraction;
- enrolment merge (relation graph, groups, packed tagged template);
- identify scoring (two-pass geometry, flag re-evaluation, late rejection,
  fallback);
- template study, which lets a matched template learn from new touches.

All state lives in an `OcSession`, with no globals. Enrolment follows the
ChicagoHS enrolment protocol: 12 counted touches, coverage >= 65, quality >= 25,
overlay "tips". By default it also takes a second frame of the same touch and
keeps the better one (`GOODIX5125_ENROLL_PAIR=0` disables this).

**Local state.** The driver keeps its state in `/var/lib/fprint/goodix5125`
(fprintd's `StateDirectory`), or `$XDG_STATE_HOME/libfprint/goodix5125` when that
is not writable, or `GOODIX5125_STATE_DIR`:

- `psk`: host PSK, mode 0600;
- `openchicago.state`: the complete adaptive algorithm state, bound to the
  sensor's OTP ID and rebased onto a fresh no-finger ImageBase on each
  activation;
- `learned/`: learned templates (see limitations).

Files that belong to another sensor, and corrupt files, are rejected, never
overwritten. The state and the templates are only checksummed, so every
counter and index read from them is range-checked on load.

**Device emulation.** With `FP_DEVICE_EMULATION=1` (umockdev tests and their
recording) the host side is deterministic, so that a recorded session replays
byte for byte: the TLS server gets its own OpenSSL library context with a small
built-in RAND provider that returns a fixed pattern (ServerHello random,
session ID and AES-GCM nonces are fixed; no session tickets), the all-zero PSK
is used and never written, the `GOODIX5125_*` environment is ignored, and the
state lives in a temporary directory that is removed on close. umockdev
replays a read that timed out on the device as an empty completion, so an
IN transfer without data is treated as a timeout in every mode.

## Tests

- `goodix5125` (umockdev, `tests/goodix5125/custom.py`): identify with an
  empty gallery, enrolment with 12 stages, then verify with a deserialized
  print until it matches (the recording has 3 non-matching touches before
  the match). It replays byte for byte, TLS included. It was recorded with
  a finger that is not used for login, since the frames in the capture are
  effectively unencrypted;
- `goodix5125` (unit): framing, RX classification (optional ACK, stale
  messages), OTP-based MCU configuration, image decoding, FDT bases, pairing
  and calibration persistence, rejection of out-of-range state counters, all
  on synthetic data;
- `goodix5125-tls`: the TLS-PSK memory-BIO transport against an in-process
  OpenSSL client, and the deterministic mode: the client side of a handshake
  replayed against a second server gets the same bytes back, and the recorded
  application data decrypts;
- `goodix5125-algo`: replays recorded raw frames through the driver's
  algorithm layer and through the openchicago API in parallel. It covers
  enrolment (plain protocol and engine protocol, frame pairs),
  verify/identify, state save/restore/rebase and the learned-template store,
  and checks that every decision, score and saved state is identical. The
  frames are **not** in the repository (they are biometric data); the test
  reads them from `GOODIX5125_TEST_FRAMES` and is skipped (exit code 77)
  without them;
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
3. `tests: Add goodix5125 unit tests`

🤖 Generated with [Claude Code](https://claude.com/claude-code)
