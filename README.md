# Goodix 27c6:5125 fingerprint sensor on Linux

Research and tooling to make the fingerprint sensor in the power button of the
**HONOR MagicBook 16** (USB `27c6:5125`, Goodix ChicagoHS, chip ID `0x2504`)
work on Linux through libfprint and fprintd.

The sensor sends encrypted 64×80, 12-bit raw frames over a TLS-PSK session.
Matching runs on the host. This repository contains the protocol tools, the
investigation notes and **openchicago**, a free C implementation of the
ChicagoHS matching algorithm. The libfprint driver built on it is being
submitted upstream as `goodix5125`.

## Status

- On my laptop, enrolment and verification work through fprintd,
  and so does `sudo` login through `pam_fprintd`: the enrolled finger matched
  27 of 30 times, another finger 0 of 70 times.
- A single touch is not always enough. On the recorded dataset about half of
  the genuine single touches match, and no impostor touch has matched.
- Only one unit has been tested.
- The libfprint driver is on its way upstream. Until the branch is public,
  the Arch package in `packaging/arch/` builds only from a local clone of that
  branch.

## ⚠️ Safety

- **Never erase or flash the sensor MCU.** Another 27c6:5125 was bricked by
  reflashing. The firmware commands (`0xa4`, `0xf0`, `0xf2`, `0xf4`) have been
  removed from the tools on purpose, and the tools only send commands from an
  allowlist.
- The sensor is paired with the host through a PSK. Writing a new PSK can break
  the pairing with another operating system on the same machine, e.g. Windows
  Hello. The driver never writes one unless `GOODIX5125_PROVISION_PSK=random`
  is set.
- Frames and templates are biometric data. Keep captures (`dumps/`) out of
  git.

## Layout

| Path | What it is |
|---|---|
| `openchicago/` | ChicagoHS matcher in C: preprocessing, features, enrolment, identify, template study. Meson project with unit tests. |
| `tools/goodix5125/` | Python protocol library: USB framing, MCU commands, TLS-PSK server, image decoding. |
| `tools/*.py`, `tools/*.sh` | Probing (read-only), PSK provisioning, TLS check, capture, finger detection tests, dataset collection, libfprint test helpers, umockdev recording (`record_umockdev_5125.sh`). |
| `tools/wbgen/` | Builds the white-box PSK container the sensor stores, by running the routine from the Windows driver's `gfusb.dll` under Unicorn (reference for the driver's own implementation). |
| `tools/fwre/` | Analysis of the ST411 MCU firmware: reads the image and emulates its PSK unwrap. |
| `tools/tune/` | Offline evaluation of matching approaches on the dataset. |
| `tools/algo/` | Reference harness: loads the vendor Windows DLL natively (no wine) to compare results with openchicago. Notes are in `tools/algo/re/notes/`. |
| `packaging/arch/` | `PKGBUILD` for libfprint with the `goodix5125` driver, a drop-in for `extra/libfprint`. |
| `udev/` | udev rule that gives the logged-in user raw USB access for the prototype tools. |
| `docs/` | Stage-by-stage notes and results (in Russian), and the upstream MR description. |

## Building openchicago

```sh
cd openchicago
meson setup build
ninja -C build
meson test -C build --suite unit   # needs the exported dataset, skipped without it
```

## Not included

No proprietary files are in this repository: no Windows driver, DLLs,
firmware, disassembly or biometric captures. `tools/algo/` needs
`AlgoChicago.dll`, `tools/wbgen/` needs `gfusb.dll`, and `tools/fwre/` needs the
firmware image. All of them come from the Windows driver package, which you
have to supply yourself (in `win-driver/`, ignored by git). openchicago and the libfprint
driver neither contain nor load any vendor code.

## Credits

- The first 27c6:5125 libfprint driver,
  [libfprint!648](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/648),
  by Thomas97460.
- The ChicagoHS port and the white-box pairing code,
  [berkekbgz/libfprint-goodix-spi](https://github.com/berkekbgz/libfprint-goodix-spi),
  by Berke Kabagöz (LGPL-2.1-or-later). openchicago is derived from it.
- [goodix-fp-dump](https://github.com/goodix-fp-linux-dev/goodix-fp-dump)
  (MIT, see `tools/goodix5125/LICENSE.goodix-fp-dump`), the base of the Python
  protocol library.

## License

openchicago is licensed under LGPL-2.1-or-later (see the SPDX headers).
`tools/goodix5125/` is adapted from goodix-fp-dump under MIT. Other files do
not yet carry an explicit license.
