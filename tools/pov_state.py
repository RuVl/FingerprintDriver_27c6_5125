"""Read-only check: does the power-button POV frame survive until Linux?

Sends query_mcu_state (0xae) and nothing else, prints the decoded state and
saves it to dumps/pov-state-*.json. Run it before fprintd or anything else has
opened the sensor (see tools/povprobe/README.md).
"""

import datetime
import json
import pathlib
import time

from goodix5125 import goodix, protocol
from goodix5125.device import Device5125

PRODUCT = 0x5125
DUMPS = pathlib.Path(__file__).resolve().parent.parent / "dumps"


class QueryOnlyUSBProtocol(protocol.USBProtocol):

    def write(self, data, timeout=5):
        if (data[0] != goodix.FLAGS_MESSAGE_PROTOCOL
                or data[4] != goodix.COMMAND_QUERY_MCU_STATE):
            raise PermissionError(f"Blocked packet {data[:8].hex()}")
        super().write(data, timeout)


def decode(d: bytes) -> dict:
    # Layout per tools/algo/re/notes/90-pov-image.md (isLocked is bit 3, not
    # bit 2 as in goodix.decode_mcu_state()).
    return {
        "version": d[0],
        "pov_image_valid": d[1] & 1,
        "tls_connected": d[1] >> 1 & 1,
        "locked": d[1] >> 3 & 1,
        "captured": [d[2] >> 4, d[2] & 0xf],
        "wake_windows": f"{d[5]:#04x}",
        "ec_falling_count": d[9],
        "wakeup_to_pov_ms": d[10] | d[11] << 8,
        "wakeup_src": f"{d[12]:#04x}",
        "onekey_procedure": f"{d[13]:#04x}",
        "fdt_tx_fail_usb_suspend": d[14],
    }


def main():
    uptime = time.clock_gettime(time.CLOCK_BOOTTIME)
    device = Device5125(PRODUCT, QueryOnlyUSBProtocol)
    state = device.query_mcu_state()

    result = {
        "time": datetime.datetime.now().isoformat(timespec="seconds"),
        "seconds_since_kernel_start": round(uptime),
        "mcu_state": state.hex(" "),
        **decode(state),
    }
    print(json.dumps(result, indent=2))

    DUMPS.mkdir(exist_ok=True)
    name = datetime.datetime.now().strftime("pov-state-%Y%m%d-%H%M%S.json")
    (DUMPS / name).write_text(json.dumps(result, indent=2) + "\n")
    print(f"saved dumps/{name}")


if __name__ == "__main__":
    main()
