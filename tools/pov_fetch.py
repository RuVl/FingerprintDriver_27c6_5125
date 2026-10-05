"""Fetch the power-button POV frame(s) from the 27c6:5125 MCU.

Order as in the Windows driver (tools/algo/re/notes/90-pov-image.md):
query_mcu_state (0xae) -> TLS handshake (0xd0 ... 0xd4) -> 0xd2 for every
stored frame. Each 0xd2 reply is a TLS record carrying a 7693-byte frame
(8-byte header with the 0xaa POV marker, 7680 pixels, CRC-32/MPEG-2, 0x88).

Nothing else is sent: no reset, no config upload, no image capture (0x20 would
overwrite POV slot 0). Must run within ~180 s of the button press, with the
sensor kept out of USB autosuspend (udev/71-goodix-5125-pov.rules) and before
fprintd opens it. Frames go to dumps/pov-fetch-*/.
"""

import datetime
import json
import sys
import time

import usb

from capture import FRAME_BYTES, HEIGHT, WIDTH, save_pgm
from goodix5125 import goodix, image, protocol
from goodix5125.device import Device5125
from goodix5125.tls import TLSServer
from pov_state import decode
from probe import DUMPS, PRODUCT
from topng import to_png

POV_MARKER = 0xaa
MAX_FETCHES = 3  # OneKeyBootRetryMaxCnt in the Windows driver

ALLOWED_COMMANDS = {
    goodix.COMMAND_QUERY_MCU_STATE,
    goodix.COMMAND_REQUEST_TLS_CONNECTION,
    goodix.COMMAND_TLS_SUCCESSFULLY_ESTABLISHED,
    goodix.COMMAND_MCU_GET_POV_IMAGE,
}


class PovUSBProtocol(protocol.USBProtocol):

    def __init__(self, vendor, product, timeout=5):
        # The kernel has already configured the device; do not send another
        # SET_CONFIGURATION, so the MCU sees nothing but our commands.
        original = usb.core.Device.set_configuration
        usb.core.Device.set_configuration = lambda *args, **kwargs: None
        try:
            super().__init__(vendor, product, timeout)
        finally:
            usb.core.Device.set_configuration = original

    def write(self, data, timeout=5):
        if data[0] == goodix.FLAGS_TRANSPORT_LAYER_SECURITY:
            return super().write(data, timeout)
        if (data[0] != goodix.FLAGS_MESSAGE_PROTOCOL
                or data[4] not in ALLOWED_COMMANDS):
            raise PermissionError(f"Blocked packet {data[:8].hex()}")
        return super().write(data, timeout)


def crc32_mpeg2(data: bytes) -> int:
    crc = 0xffffffff
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1 ^ 0x04c11db7 if crc & 0x80000000 else crc << 1)
            crc &= 0xffffffff
    return crc


def check_frame(plain: bytes) -> dict:
    """Validate a decrypted 0xd2 reply; returns what was found."""
    pixels = plain[8:8 + FRAME_BYTES]
    tail = plain[8 + FRAME_BYTES:8 + FRAME_BYTES + 4]
    stored = (tail[2] << 24 | tail[3] << 16 | tail[0] << 8 | tail[1]
              if len(tail) == 4 else None)
    computed = crc32_mpeg2(pixels)
    return {
        "length": len(plain),
        "command": f"{plain[0]:#04x}" if plain else None,
        "pov_marker": len(plain) > 3 and plain[3] == POV_MARKER,
        "crc_stored": f"{stored:#010x}" if stored is not None else None,
        "crc_computed": f"{computed:#010x}",
        "crc_ok": stored == computed,
    }


def state(device, log, label):
    reply = device.query_mcu_state()
    entry = {"step": label, "t": round(time.clock_gettime(time.CLOCK_BOOTTIME), 1),
             "mcu_state": reply.hex(" "), **decode(reply)}
    log.append(entry)
    print(f"[{label}] {reply.hex(' ')}  POV {entry['pov_image_valid']}  "
          f"captured {entry['captured']}", flush=True)
    return entry


def main():
    out = DUMPS / datetime.datetime.now().strftime("pov-fetch-%Y%m%d-%H%M%S")
    log = []
    device = Device5125(PRODUCT, PovUSBProtocol)
    try:
        first = state(device, log, "start")
        if not first["pov_image_valid"]:
            print("No POV frame in the MCU (flag is 0): nothing to fetch.")
            return 2

        server = TLSServer(bytes(32))
        server.connect(device)
        device.tls_successfully_established()
        print(f"TLS: {server.ssl.version()} {server.ssl.cipher()[0]}")
        if not state(device, log, "after-tls")["pov_image_valid"]:
            print("POV flag dropped during the TLS handshake.")
            return 3

        out.mkdir(parents=True)
        for index in range(MAX_FETCHES):
            record = device.command(
                goodix.COMMAND_MCU_GET_POV_IMAGE, b"\x00\x00",
                reply_flags=(goodix.FLAGS_TRANSPORT_LAYER_SECURITY,
                             goodix.FLAGS_TRANSPORT_LAYER_SECURITY_DATA),
                timeout=3)
            plain = server.decrypt(record)
            info = check_frame(plain)
            log.append({"step": f"pov-{index}", **info})
            print(f"[pov-{index}] {json.dumps(info)}", flush=True)
            (out / f"pov-{index}.bin").write_bytes(plain)
            if len(plain) >= 8 + FRAME_BYTES:
                pgm = out / f"pov-{index}.pgm"
                save_pgm(image.decode_image(plain[8:8 + FRAME_BYTES]), pgm)
                print(f"  saved {to_png(pgm)}")
            if not state(device, log, f"after-pov-{index}")["pov_image_valid"]:
                break
    finally:
        if log:
            out.mkdir(parents=True, exist_ok=True)
            (out / "log.json").write_text(json.dumps(log, indent=1) + "\n")
            print(f"log: {out / 'log.json'}  ({WIDTH}x{HEIGHT})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
