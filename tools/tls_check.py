"""Check whether the MCU accepts a TLS-PSK handshake with the given PSK.

Nothing is written to persistent storage on the device: only status
commands, request_tls_connection and TLS records pass the guard.
"""

import argparse
import pathlib
import ssl
import sys
import time

import usb

from goodix5125 import goodix, protocol
from goodix5125.device import Device5125
from goodix5125.tls import TLSServer
from probe import PRODUCT

ALLOWED_COMMANDS = {
    goodix.COMMAND_NOP,
    goodix.COMMAND_ENABLE_CHIP,
    goodix.COMMAND_FIRMWARE_VERSION,
    goodix.COMMAND_QUERY_MCU_STATE,
    goodix.COMMAND_REQUEST_TLS_CONNECTION,
    goodix.COMMAND_TLS_SUCCESSFULLY_ESTABLISHED,
}


class TLSCheckUSBProtocol(protocol.USBProtocol):

    def write(self, data, timeout=5):
        if data[0] == goodix.FLAGS_TRANSPORT_LAYER_SECURITY:
            return super().write(data, timeout)
        if data[0] != goodix.FLAGS_MESSAGE_PROTOCOL:
            raise PermissionError(f"Blocked packet with flags {data[0]:#x}")
        if data[4] not in ALLOWED_COMMANDS:
            raise PermissionError(f"Blocked command {data[4]:#04x}")
        return super().write(data, timeout)


def wake(device: Device5125, attempts=5):
    """Wait until the MCU answers query_mcu_state.

    The first command after opening the device is sometimes dropped, and
    after an interrupted TLS handshake the MCU ignores enable_chip, so poll
    with the always-handled state query instead.
    """
    for attempt in range(attempts):
        try:
            return device.query_mcu_state(b"\x55", True)
        except usb.core.USBTimeoutError:
            print(f"query_mcu_state timed out (attempt {attempt + 1})")
            time.sleep(3)
            device.empty_buffer()
    raise TimeoutError("device does not respond")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--psk-file", type=pathlib.Path)
    args = parser.parse_args()
    psk = args.psk_file.read_bytes() if args.psk_file else bytes(32)

    device = Device5125(PRODUCT, TLSCheckUSBProtocol)
    wake(device)
    print(f"Firmware: {device.firmware_version()}")

    server = TLSServer(psk)
    try:
        server.connect(device)
    except (ssl.SSLError, usb.core.USBError, ValueError) as error:
        print(f"TLS handshake FAILED: {error!r}")
        return 1

    device.tls_successfully_established()
    print(f"TLS handshake OK: {server.ssl.version()} {server.ssl.cipher()}")
    state = device.query_mcu_state(b"\x55", True)
    print(f"MCU state: {state.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
