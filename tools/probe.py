"""Stage 1: read-only probe of the Goodix 27c6:5125.

Every outgoing packet is checked against an allowlist before it reaches the
wire, so nothing here can touch the PSK slots or the firmware.
"""

import datetime
import json
import pathlib
import sys

from goodix5125 import goodix, protocol
from goodix5125.device import Device5125

PRODUCT = 0x5125
EXPECTED_CHIP_ID = b"\xa2\x04\x25\x00"
EXPECTED_FIRMWARE_PREFIX = "GF_ST411SEC_APP_125"
PSK_HASH_FLAGS = 0xbb020003
# SHA256 of the zero-PSK white box, see wbgen/test_wrap.py
ZERO_PSK_HASH = bytes.fromhex(
    "b5e0beeb94c84eb99b883abd5c251073c56b91035c562a91a46c7f3349c36c89")

ALLOWED_COMMANDS = {
    goodix.COMMAND_NOP,
    goodix.COMMAND_ENABLE_CHIP,
    goodix.COMMAND_FIRMWARE_VERSION,
    goodix.COMMAND_PRESET_PSK_READ_R,
    goodix.COMMAND_READ_SENSOR_REGISTER,
    goodix.COMMAND_READ_OTP,
    goodix.COMMAND_QUERY_MCU_STATE,
    goodix.COMMAND_RESET,
}

DUMPS = pathlib.Path(__file__).resolve().parent.parent / "dumps"


class ReadOnlyUSBProtocol(protocol.USBProtocol):

    def write(self, data, timeout=5):
        if data[0] != goodix.FLAGS_MESSAGE_PROTOCOL:
            raise PermissionError(f"Blocked packet with flags {data[0]:#x}")

        command = data[4]
        if command not in ALLOWED_COMMANDS:
            raise PermissionError(f"Blocked command {command:#04x}")

        # reset(): bit 1 of the first payload byte is a soft MCU reset
        if command == goodix.COMMAND_RESET and data[7] & 0x2:
            raise PermissionError("Blocked MCU reset")

        if command == goodix.COMMAND_PRESET_PSK_READ_R:
            flags = int.from_bytes(data[7:11], "little")
            if flags != PSK_HASH_FLAGS:
                raise PermissionError(f"Blocked PSK read flags {flags:#x}")

        super().write(data, timeout)


def main():
    device = Device5125(PRODUCT, ReadOnlyUSBProtocol)
    result = {"time": datetime.datetime.now().isoformat()}

    device.nop()
    device.enable_chip(True)
    device.nop()

    firmware = device.firmware_version()
    result["firmware"] = firmware
    print(f"Firmware: {firmware}")

    success, flags, psk_hash = device.preset_psk_read(PSK_HASH_FLAGS)
    result["psk_read_ok"] = success
    if success:
        result["psk_flags"] = f"{flags:#x}"
        result["psk_hash"] = psk_hash.hex()
        print(f"PSK hash ({flags:#x}): {psk_hash.hex()}")
        result["psk_is_zero"] = psk_hash == ZERO_PSK_HASH
        print("PSK is zero (Windows default): "
              f"{result['psk_is_zero']}")
    else:
        print("PSK read failed")

    result["mcu_state"] = device.query_mcu_state(b"\x55", True).hex()

    success, number = device.reset(True, False, 20)
    result["reset"] = [success, number]

    chip_id = device.read_sensor_register(0x0000, 4)
    result["chip_id"] = chip_id.hex()
    print(f"Chip ID: {chip_id.hex()}")

    otp = device.read_otp()
    result["otp"] = otp.hex()
    print(f"OTP ({len(otp)} bytes): {otp.hex()}")

    DUMPS.mkdir(exist_ok=True)
    path = DUMPS / f"probe-{datetime.datetime.now():%Y%m%d-%H%M%S}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Saved {path}")

    problems = []
    if not firmware.startswith(EXPECTED_FIRMWARE_PREFIX):
        problems.append(f"unexpected firmware {firmware}")
    if chip_id != EXPECTED_CHIP_ID:
        problems.append(f"unexpected chip id {chip_id.hex()}")
    if problems:
        print("STOP: " + "; ".join(problems))
        return 1

    print("Device matches the expected 5125 profile")
    return 0


if __name__ == "__main__":
    sys.exit(main())
