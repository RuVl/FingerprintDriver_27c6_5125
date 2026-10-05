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
- The libfprint driver is under review upstream as
  [libfprint!669](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/669).
  The Arch package in `packaging/arch/` builds that branch (see
  [Installing on Arch Linux](#installing-on-arch-linux)).

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

## Installing on Arch Linux

`packaging/arch/PKGBUILD` builds `libfprint-goodix5125-git`, a drop-in
replacement for `extra/libfprint` with every upstream driver plus
`goodix5125`. It works with `fprintd` from `extra`.

### 1. Driver source

`makepkg` fetches the driver itself: branch `goodix5125-mr` of
https://gitlab.freedesktop.org/RuVl/libfprint, the branch of the upstream
merge request. To build another clone or branch instead:

```sh
export LIBFPRINT_GOODIX5125_REPO=file:///path/to/libfprint
export LIBFPRINT_GOODIX5125_BRANCH=my-branch
```

### 2. Build and install

```sh
sudo pacman -S --needed base-devel fprintd
cd packaging/arch
makepkg -si -C        # pacman offers to replace libfprint: answer yes
sudo systemctl restart fprintd
pacman -Q libfprint-goodix5125-git
```

`makepkg` runs the libfprint unit tests before packaging (the umockdev driver
tests need introspection, which the package does not build). The version
(`1.94.100.rN.gHASH`) names the commit it was built from.

To update later, run the same commands again in `packaging/arch`: `makepkg`
fetches the latest commit of the branch. Then restart fprintd. Enrolled
prints and the driver state in `/var/lib/fprint` are kept, so there is no
need to enrol again.
Each build leaves its `*.pkg.tar.zst` in `packaging/arch/` under its own
name; keep the previous one to go back with `sudo pacman -U <file>`.

### 3. Pair the sensor (once)

The sensor and the host share a TLS key (PSK). The driver reads it from
`/var/lib/fprint/goodix5125/psk` (64 hex digits, mode 0600) and checks it
against the hash the sensor reports. It never writes a key on its own.

The Windows driver pairs with the all-zero key, and so does
`tools/provision.py`. Try that one first:

```sh
sudo install -d -m 700 /var/lib/fprint/goodix5125
printf '%064d\n' 0 | sudo install -m 600 /dev/stdin /var/lib/fprint/goodix5125/psk
```

If the key does not match, enrolment fails with a pairing error and the
sensor is left untouched. The driver can then write a new random key, which
breaks Windows Hello on a dual-boot machine:

```sh
sudo rm /var/lib/fprint/goodix5125/psk
sudo systemctl set-environment GOODIX5125_PROVISION_PSK=random
sudo systemctl restart fprintd
fprintd-enroll -f right-thumb   # the first open writes and saves the key
sudo systemctl unset-environment GOODIX5125_PROVISION_PSK
sudo systemctl restart fprintd
```

### 4. Enrol and verify

```sh
fprintd-enroll -f right-thumb
fprintd-verify
```

- The first `enroll-stage-passed` appears at once, before any touch. fprintd
  first checks that the finger is not enrolled yet, and with no prints stored
  that check passes immediately.
- Then touch the sensor until `enroll-completed` (13 stages). Use the same
  part of the finger every time and shift it only slightly. Touches spread
  over the whole finger make a template that later matches poorly.
- `enroll-finger-not-centered` means the touch overlaps the template too much
  and adds nothing new: shift the finger a little.

### 5. Fingerprint for sudo (optional)

Add as the first `auth` line of `/etc/pam.d/sudo` (keep a root shell open
while editing):

```
auth sufficient pam_fprintd.so max-tries=3 timeout=15
```

The password still works after three failed touches or the timeout.

### 6. Unlock by pressing the power button (optional, experimental)

The sensor is the power button. When it is pressed with a finger, the sensor
captures the finger by itself and keeps the frames for about 3 minutes; the
first fingerprint check after that (lock screen, login) can then succeed
without a second touch, as on Windows. It works after power-off and after
hibernation. The code is on the branch `goodix5125-pov` (not in the merge
request yet), and it is off unless enabled:

```sh
cd packaging/arch
LIBFPRINT_GOODIX5125_BRANCH=goodix5125-pov makepkg -si -C
cd ../..
sudo install -m644 udev/71-goodix-5125-pov.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo install -Dm644 packaging/fprintd/power-button-unlock.conf \
  /etc/systemd/system/fprintd.service.d/power-button-unlock.conf
sudo systemctl daemon-reload && sudo systemctl restart fprintd
```

The udev rule keeps the sensor out of USB autosuspend, which would drop the
frames. **Security:** the driver cannot tell a login from any other request,
so for about 3 minutes after such a press the first fingerprint request of
any kind (also `sudo` or polkit) is answered without a touch. Disable it by
removing the drop-in and restarting fprintd. How it was found and measured:
`tools/povprobe/README.md`, `tools/algo/re/notes/90-pov-image.md`.

### Uninstall

```sh
sudo pacman -S libfprint                 # replaces the package
sudo systemctl restart fprintd
sudo rm -r /var/lib/fprint/goodix5125    # driver state and the host key
```

Remove the `pam_fprintd` line first if you added it. Prints enrolled with
`fprintd` stay in `/var/lib/fprint/<user>/`; `fprintd-delete <user>` removes
them.

### Troubleshooting

- "Device disabled to prevent overheating": libfprint limits how long a
  sensor stays active unless the driver declares it always-on. The driver
  does that since the MCU waits for the finger on its own, so a lock screen
  can keep the sensor armed indefinitely. If the message still appears after
  10 minutes or so on a lock screen, the installed package predates this
  change: update it as described in step 2.
- Debug log of the driver:

  ```sh
  sudo systemctl set-environment G_MESSAGES_DEBUG=all
  sudo systemctl restart fprintd
  # reproduce, then:
  journalctl -u fprintd --since -5min
  sudo systemctl unset-environment G_MESSAGES_DEBUG
  sudo systemctl restart fprintd
  ```

  `enroll frame: … q=… c=…` lines show quality and coverage of each touch,
  `match: score …` lines the result of each verify attempt.
- The driver keeps its state in `/var/lib/fprint/goodix5125`
  (`openchicago.state`, `learned/`). fprintd lists that directory as if it
  were a user; this is harmless.

The prototype tools in `tools/` need raw USB access. Stop fprintd first and
install the udev rule:
`sudo install -m 644 udev/70-goodix-5125.rules /etc/udev/rules.d/`, then
`sudo udevadm control --reload && sudo udevadm trigger`.

## Layout

| Path | What it is |
|---|---|
| `openchicago/` | ChicagoHS matcher in C: preprocessing, features, enrolment, identify, template study. Meson project with unit tests. |
| `tools/goodix5125/` | Python protocol library: USB framing, MCU commands, TLS-PSK server, image decoding. |
| `tools/*.py`, `tools/*.sh` | Probing (read-only), PSK provisioning, TLS check, capture, finger detection tests, dataset collection, umockdev recording (`record_umockdev_5125.sh`). |
| `tools/wbgen/` | Builds the white-box PSK container the sensor stores, by running the routine from the Windows driver's `gfusb.dll` under Unicorn (reference for the driver's own implementation). |
| `tools/fwre/` | Analysis of the ST411 MCU firmware: reads the image and emulates its PSK unwrap. |
| `tools/tune/` | Offline evaluation of matching approaches on the dataset. |
| `tools/algo/` | Reference harness: loads the vendor Windows DLL natively (no wine) to compare results with openchicago. Notes are in `tools/algo/re/notes/`. |
| `packaging/arch/` | `PKGBUILD` for libfprint with the `goodix5125` driver, a drop-in for `extra/libfprint`. |
| `udev/` | udev rule that gives the logged-in user raw USB access for the prototype tools. |
| `docs/` | Stage-by-stage notes and results (in Russian), and the upstream MR description. |

## Python tools

The tools in `tools/` run in a [uv](https://docs.astral.sh/uv/) environment
described by `pyproject.toml` and `uv.lock`; `uv run` creates it on first use.

```sh
cd tools
uv run python probe.py        # read-only device probe
uv run pytest -q wbgen fwre   # unit tests
```

Talking to the sensor needs the udev rule above and fprintd stopped
(`sudo systemctl stop fprintd`).

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
