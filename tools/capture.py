"""Stage 4: capture fingerprint frames from the Goodix 27c6:5125.

Sequence follows goodix-fp-dump driver_51x7.run_driver and the libfprint
goodix511 driver (same 80x88 silicon), with the 5125 MCU config captured
from the Windows driver and calibration registers taken from this unit's OTP.

Usage:
    capture.py            # background frame only (no finger needed)
    capture.py --finger   # also wait for a finger (FDT) and capture it
    capture.py --poll 20  # grab frames for 20 s, keep the finger frame
"""

import argparse
import datetime
import json
import pathlib
import struct
import sys
import time

from goodix5125 import goodix, image, protocol
from goodix5125.config import build_config, otp_register_values
from goodix5125.device import Device5125
from goodix5125.tls import TLSServer
from probe import DUMPS, EXPECTED_CHIP_ID, PRODUCT
from tls_check import wake

# The 5125 sends 64x80 12-bit pixels (8-byte header + 7680 + 5-byte trailer);
# the 5110 sends 88x80 and libfprint's goodix511 crops that to 64x80.
WIDTH, HEIGHT = 64, 80
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2
# Allowlist: everything capture.py, fdt_test.py and collect.py send.  Firmware
# erase/write/check/read and PSK writes are never on it (see CLAUDE.md).
ALLOWED_COMMANDS = {
    goodix.COMMAND_NOP,
    goodix.COMMAND_MCU_GET_IMAGE,
    goodix.COMMAND_MCU_SWITCH_TO_FDT_DOWN,
    goodix.COMMAND_MCU_SWITCH_TO_FDT_UP,
    goodix.COMMAND_MCU_SWITCH_TO_FDT_MODE,
    goodix.COMMAND_NAV,
    goodix.COMMAND_MCU_SWITCH_TO_IDLE_MODE,
    goodix.COMMAND_WRITE_SENSOR_REGISTER,
    goodix.COMMAND_READ_SENSOR_REGISTER,
    goodix.COMMAND_UPLOAD_CONFIG_MCU,
    goodix.COMMAND_SET_POWERDOWN_SCAN_FREQUENCY,
    goodix.COMMAND_ENABLE_CHIP,
    goodix.COMMAND_RESET,
    goodix.COMMAND_READ_OTP,
    goodix.COMMAND_FIRMWARE_VERSION,
    goodix.COMMAND_QUERY_MCU_STATE,
    goodix.COMMAND_REQUEST_TLS_CONNECTION,
    goodix.COMMAND_TLS_SUCCESSFULLY_ESTABLISHED,
    goodix.COMMAND_PRESET_PSK_READ_R,
}
# mcu_switch_to_fdt_mode payload the Windows driver sends first on the 5125
# (available314/goodix-27c6-5125, MCU_SWITCH_TO_FDT_MODE_BUF)
FDT_MODE_INITIAL = bytes.fromhex("0901b3b3c3c3a8a8b5b5a8a8b7b7")


class CaptureUSBProtocol(protocol.USBProtocol):

    def write(self, data, timeout=5):
        if data[0] == goodix.FLAGS_TRANSPORT_LAYER_SECURITY:
            return super().write(data, timeout)
        if data[0] != goodix.FLAGS_MESSAGE_PROTOCOL:
            raise PermissionError(f"Blocked packet with flags {data[0]:#x}")
        if data[4] not in ALLOWED_COMMANDS:
            raise PermissionError(f"Blocked command {data[4]:#04x}")
        if data[4] == goodix.COMMAND_RESET and data[7] & 0x2:
            raise PermissionError("Blocked MCU reset")
        return super().write(data, timeout)


def fdt_payload(mode: int, reply: bytes) -> bytes:
    """Next FDT payload from the six 16-bit base values in an FDT reply.

    Mirrors fdt_mode_construct_payload in available314/goodix-27c6-5125 and
    the 0x80,value>>1 pairs hardcoded in driver_51x7.
    """
    values = struct.unpack("<6H", reply[4:16])
    return bytes([mode, 0x01]) + b"".join(
        bytes([0x80, (v >> 1) & 0xff]) for v in values)


def read_frame(device, server):
    record = device.mcu_get_image()
    plain = server.decrypt(record)
    if len(plain) < FRAME_BYTES:
        raise ValueError(f"short frame: {len(plain)} bytes")
    return plain, image.decode_image(plain[8:8 + FRAME_BYTES])


def poll_frames(device, server, background, seconds, out, log):
    print(f"PUT YOUR FINGER ON THE SENSOR (polling {seconds:.0f} s)",
          flush=True)
    best, best_diff, diffs = None, -1.0, []
    end = time.time() + seconds
    while time.time() < end:
        _, frame = read_frame(device, server)
        diff = sum(abs(b - f) for b, f in zip(background, frame)) / len(frame)
        diffs.append(round(diff, 1))
        print(f"  frame {len(diffs):3d}: mean |frame - background| = "
              f"{diff:7.1f}", flush=True)
        if diff > best_diff:
            best, best_diff = frame, diff
    log["poll_diffs"] = diffs
    save_pgm(best, out / "poll-best.pgm")
    delta = [max(0, b - f) for b, f in zip(background, best)]
    top = max(delta) or 1
    save_pgm([d * 4095 // top for d in delta], out / "poll-best-minus-bg.pgm")
    print(f"best frame diff {best_diff:.1f}")


def save_pgm(pixels, path):
    """Binary 16-bit PGM, HEIGHT rows of WIDTH pixels."""
    header = f"P5\n{WIDTH} {HEIGHT}\n4095\n".encode()
    path.write_bytes(header + b"".join(struct.pack(">H", p) for p in pixels))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--finger", action="store_true")
    parser.add_argument("--finger-timeout", type=float, default=90)
    parser.add_argument("--poll", type=float, metavar="SECONDS",
                        help="grab frames continuously (no FDT) and keep "
                        "the one that differs most from the background")
    args = parser.parse_args()

    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
    out = DUMPS / f"capture-{stamp}"
    out.mkdir(parents=True)
    log = {}

    try:
        return capture(args, out, log)
    finally:
        (out / "log.json").write_text(json.dumps(log, indent=2) + "\n")
        print(f"Saved {out}")


def capture(args, out, log):
    device = Device5125(PRODUCT, CaptureUSBProtocol)
    wake(device)
    log["firmware"] = device.firmware_version()

    log["reset"] = device.reset(True, False, 20)
    chip_id = device.read_sensor_register(0x0000, 4)
    if chip_id != EXPECTED_CHIP_ID:
        raise SystemError(f"unexpected chip id {chip_id.hex()}")
    otp = device.read_otp()
    log["otp_registers"] = {f"{k:#x}": f"{v:#x}"
                            for k, v in otp_register_values(otp).items()}
    log["reset2"] = device.reset(True, False, 20)
    device.mcu_switch_to_idle_mode(20)
    for address, value in otp_register_values(otp).items():
        device.write_sensor_register(address, struct.pack("<H", value))
    log["config_ok"] = device.upload_config_mcu(build_config(otp))
    log["powerdown_ok"] = device.set_powerdown_scan_frequency(100)
    print(f"config ok: {log['config_ok']}, "
          f"powerdown ok: {log['powerdown_ok']}")

    server = TLSServer(bytes(32))
    server.connect(device)
    device.tls_successfully_established()
    log["mcu_state"] = device.query_mcu_state().hex()

    reply = device.mcu_switch_to_fdt_mode(FDT_MODE_INITIAL)
    log["fdt_mode_reply_1"] = reply.hex()
    print(f"fdt_mode reply: {reply.hex()}")
    log["nav"] = device.nav().hex()
    reply = device.mcu_switch_to_fdt_mode(fdt_payload(0x0d, reply))
    log["fdt_mode_reply_2"] = reply.hex()

    raw, background = read_frame(device, server)
    (out / "background.bin").write_bytes(raw)
    save_pgm(background, out / "background.pgm")
    log["background_min_max"] = [min(background), max(background)]
    print(f"background frame: min {min(background)} max {max(background)}")

    if args.poll:
        poll_frames(device, server, background, args.poll, out, log)

    if args.finger:
        fdt = fdt_payload(0x0c, reply)
        print("PUT YOUR FINGER ON THE SENSOR", flush=True)
        down = device.mcu_switch_to_fdt_down(fdt,
                                             timeout=args.finger_timeout)
        log["fdt_down_reply"] = down.hex()
        raw, finger = read_frame(device, server)
        (out / "finger.bin").write_bytes(raw)
        save_pgm(finger, out / "finger.pgm")
        diff = [max(0, b - f) for b, f in zip(background, finger)]
        top = max(diff) or 1
        save_pgm([d * 4095 // top for d in diff], out / "finger-minus-bg.pgm")
        log["finger_min_max"] = [min(finger), max(finger)]
        print(f"finger frame: min {min(finger)} max {max(finger)}")
        print("Lift your finger", flush=True)
        log["fdt_up_reply"] = device.mcu_switch_to_fdt_up(
            fdt_payload(0x0e, down), timeout=30).hex()

    return 0


if __name__ == "__main__":
    sys.exit(main())
