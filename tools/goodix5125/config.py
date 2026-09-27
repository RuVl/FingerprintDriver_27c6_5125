"""MCU config for the 5125 (command 0x90 upload_config_mcu).

config_5125.hex is the 224-byte config the Windows driver uploads,
captured by available314/goodix-27c6-5125 (MCU_DOWNLOAD_CHIP_CONFIG_BUF).
It embeds per-unit calibration for registers 0x220/0x236/0x238/0x23a that
comes from the sensor OTP, so those entries are rewritten for this unit.
"""

import pathlib
import struct

CONFIG_HEX = pathlib.Path(__file__).with_name("config_5125.hex")
OTP_REGISTERS = (0x0220, 0x0236, 0x0238, 0x023a)


def otp_register_values(otp: bytes) -> dict[int, int]:
    """Same derivation as goodix511.c:otp_write_run."""
    return {
        0x0220: otp[46] << 4 | 8,
        0x0236: otp[47],
        0x0238: otp[48],
        0x023a: otp[49],
    }


def fix_checksum(config: bytearray):
    checksum = 0xa5a5
    for (word,) in struct.iter_unpack("<H", config[:-2]):
        checksum = (checksum + word) & 0xffff
    config[-2:] = struct.pack("<H", (0x10000 - checksum) & 0xffff)


def build_config(otp: bytes) -> bytes:
    config = bytearray.fromhex(CONFIG_HEX.read_text().strip())
    for address, value in otp_register_values(otp).items():
        tag = struct.pack("<H", address)
        offsets = [i for i in range(len(config) - 4)
                   if config[i:i + 2] == tag]
        if len(offsets) != 1:
            raise ValueError(f"register {address:#x} found {len(offsets)}x")
        config[offsets[0] + 2:offsets[0] + 4] = struct.pack("<H", value)
    fix_checksum(config)
    return bytes(config)
