"""Stage 3: write a known PSK white box to the Goodix 27c6:5125.

This overwrites the current white box in slot 0xbb010003. The old one cannot
be read back, so the step is irreversible (the Windows driver would simply
provision a new key if it ever runs on this device again). Firmware is never
touched: only preset_psk_write/read plus harmless status commands pass the
guard below.

Usage:
    provision.py [--psk-file FILE] --yes-overwrite-psk

Without --psk-file the PSK is 32 zero bytes, the same key the Windows driver
writes.
"""

import argparse
import datetime
import hashlib
import json
import pathlib
import struct
import sys

from goodix5125 import goodix, protocol
from goodix5125.device import Device5125
from probe import (DUMPS, EXPECTED_FIRMWARE_PREFIX, PRODUCT, PSK_HASH_FLAGS)
from tls_check import wake
from wbgen.wrap import Emulator

PSK_WHITE_BOX_FLAGS = 0xbb010003


def record_padding(blob: bytes) -> bytes:
    """Zero padding so the stored record length is a multiple of 4.

    The firmware stores the payload in flash sector 4 as
    [u32 len][payload][u32 crc] and programs the CRC as a 32-bit word at
    0x08010000 + len + 4 (store routine at 0x080277d8). An unaligned len
    makes that program fail: the sector is left erased and the command
    answers status 3. The Windows driver pads its payload the same way.
    """
    return b"\x00" * (-(8 + len(blob)) % 4)


ALLOWED_COMMANDS = {
    goodix.COMMAND_NOP,
    goodix.COMMAND_ENABLE_CHIP,
    goodix.COMMAND_FIRMWARE_VERSION,
    goodix.COMMAND_PRESET_PSK_READ_R,
    goodix.COMMAND_PRESET_PSK_WRITE_R,
    goodix.COMMAND_QUERY_MCU_STATE,
}


class ProvisionUSBProtocol(protocol.USBProtocol):
    expected_blob: bytes | None = None

    def write(self, data, timeout=5):
        if data[0] != goodix.FLAGS_MESSAGE_PROTOCOL:
            raise PermissionError(f"Blocked packet with flags {data[0]:#x}")

        command = data[4]
        if command not in ALLOWED_COMMANDS:
            raise PermissionError(f"Blocked command {command:#04x}")

        if command == goodix.COMMAND_PRESET_PSK_READ_R:
            flags = int.from_bytes(data[7:11], "little")
            if flags != PSK_HASH_FLAGS:
                raise PermissionError(f"Blocked PSK read flags {flags:#x}")

        if command == goodix.COMMAND_PRESET_PSK_WRITE_R:
            flags = int.from_bytes(data[7:11], "little")
            length = int.from_bytes(data[11:15], "little")
            payload = bytes(data[15:15 + length])
            padding = bytes(data[15 + length:15 + length +
                                 len(record_padding(payload))])
            record_length = int.from_bytes(data[5:7], "little") - 1
            if (flags != PSK_WHITE_BOX_FLAGS
                    or payload != self.expected_blob
                    or record_length % 4 != 0
                    or any(padding)):
                raise PermissionError("Blocked unexpected PSK write")

        super().write(data, timeout)


def write_white_box(device, blob):
    """preset_psk_write with the record padded to a multiple of 4 bytes."""
    data = (struct.pack("<II", PSK_WHITE_BOX_FLAGS, len(blob)) + blob +
            record_padding(blob))
    status = device.preset_psk_write_raw(data)
    print(f"write status: {status:#x}")
    return status == 0x00


def read_hash(device):
    """SHA256 of the stored white box, or None if the record is invalid."""
    success, flags, psk_hash = device.preset_psk_read(PSK_HASH_FLAGS)
    if not success or flags != PSK_HASH_FLAGS:
        return None
    return psk_hash


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--psk-file", type=pathlib.Path)
    parser.add_argument("--yes-overwrite-psk", action="store_true")
    args = parser.parse_args()

    psk = args.psk_file.read_bytes() if args.psk_file else bytes(32)
    if len(psk) != 32:
        raise ValueError("PSK must be 32 bytes")

    blob = Emulator().wrap(psk)
    blob_hash = hashlib.sha256(blob).digest()
    print(f"white box ({len(blob)} bytes), sha256 {blob_hash.hex()}")

    device = Device5125(PRODUCT, ProvisionUSBProtocol)
    device.protocol.expected_blob = blob
    wake(device)

    firmware = device.firmware_version()
    if not firmware.startswith(EXPECTED_FIRMWARE_PREFIX):
        print(f"STOP: unexpected firmware {firmware}")
        return 1

    before = read_hash(device)
    print(f"current PSK hash: {before.hex() if before else 'invalid record'}")
    if before == blob_hash:
        print("Device already has this PSK, nothing to do")
        return 0

    if not args.yes_overwrite_psk:
        print("Dry run: pass --yes-overwrite-psk to write")
        return 0

    record = {
        "time": datetime.datetime.now().isoformat(),
        "firmware": firmware,
        "hash_before": before.hex() if before else None,
        "blob": blob.hex(),
        "expected_hash": blob_hash.hex(),
        "psk_is_zero": psk == bytes(32),
    }
    DUMPS.mkdir(exist_ok=True)
    path = DUMPS / f"provision-{datetime.datetime.now():%Y%m%d-%H%M%S}.json"
    path.write_text(json.dumps(record, indent=2) + "\n")

    written = write_white_box(device, blob)
    after = read_hash(device)
    record.update(write_status_ok=written,
                  hash_after=after.hex() if after else None)
    path.write_text(json.dumps(record, indent=2) + "\n")

    print(f"write status ok: {written}")
    print(f"PSK hash after:  {after.hex() if after else 'invalid record'}")
    print(f"Saved {path}")
    if after != blob_hash:
        print("FAIL: hash does not match the written white box")
        return 1

    print("OK: device now uses the new PSK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
