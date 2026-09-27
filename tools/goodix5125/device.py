"""Goodix 5125 command layer that tolerates lost ACKs.

The 5125 MCU sends an ACK (command 0xb0) followed by the reply. When both
are produced back to back the ACK is regularly lost on the IN endpoint, so
the strict "ACK, then reply" parsing of goodix.Device times out or trips on
the reply. Here the ACK is optional and replies are matched by command.
"""

import struct

import usb

from . import goodix

ACK_ONLY_TIMEOUT = 1.0


class Device5125(goodix.Device):

    def command(self, command: int, payload: bytes, reply: bool = True,
                checksum: bool = True, reply_flags: tuple[int, ...] = (),
                reply_checksum: bool = True, timeout: float | None = 5):
        """Send one command and return its reply payload (or None).

        reply_flags: return the raw pack payload of the first message with
        one of these flags (TLS records use 0xb0/0xb2) instead of a 0xa0
        reply.
        """
        self.protocol.write(
            goodix.encode_message_pack(
                goodix.encode_message_protocol(payload, command,
                                               checksum=checksum)))

        wait = timeout if reply else ACK_ONLY_TIMEOUT
        while True:
            try:
                message = self.protocol.read(timeout=wait)
            except usb.core.USBTimeoutError:
                if not reply:
                    return None  # ACK lost; ACK-only command is done
                raise

            data, flags, _ = goodix.decode_message_pack(message)
            if flags in reply_flags:
                return data
            if flags != goodix.FLAGS_MESSAGE_PROTOCOL:
                print(f"ignoring pack with flags {flags:#x}")
                continue

            body, received, _ = goodix.decode_message_protocol(
                data, checksum=True if data[0] == goodix.COMMAND_ACK else
                reply_checksum)
            if received == goodix.COMMAND_ACK:
                acked, _ = goodix.decode_ack(body)
                if acked != command:
                    print(f"ignoring stale ACK for {acked:#x}")
                    continue
                if not reply:
                    return None
                continue
            if received != command:
                print(f"ignoring stale reply for {received:#x}")
                continue
            return body

    def enable_chip(self, enable: bool):
        self.command(goodix.COMMAND_ENABLE_CHIP,
                     struct.pack("<B", 1 if enable else 0) + b"\x00",
                     reply=False)

    def query_mcu_state(self, payload: bytes = b"\x55", reply: bool = True):
        return self.command(goodix.COMMAND_QUERY_MCU_STATE, payload, reply)

    def firmware_version(self):
        body = self.command(goodix.COMMAND_FIRMWARE_VERSION, b"\x00\x00")
        return body.split(b"\x00")[0].decode()

    def preset_psk_read(self, flags: int):
        body = self.command(goodix.COMMAND_PRESET_PSK_READ_R,
                            struct.pack("<II", flags, 0))
        if body[0] != 0x00:
            return False, None, None
        length = struct.unpack("<I", body[5:9])[0]
        return True, struct.unpack("<I", body[1:5])[0], body[9:9 + length]

    def preset_psk_write_raw(self, data: bytes) -> int:
        """Returns the MCU status byte (0 on success)."""
        return self.command(goodix.COMMAND_PRESET_PSK_WRITE_R, data)[0]

    def reset(self, reset_sensor: bool, soft_reset_mcu: bool,
              sleep_time: int):
        mode = ((1 if reset_sensor else 0) | (1 if soft_reset_mcu else 0) << 1
                | (1 if reset_sensor else 0) << 2)
        body = self.command(goodix.COMMAND_RESET,
                            struct.pack("<BB", mode, sleep_time),
                            reply=not soft_reset_mcu)
        if body is None:
            return None
        if body[0] != 0x01:
            return False, None
        return True, struct.unpack("<H", body[1:3])[0]

    def read_sensor_register(self, address: int, length: int):
        return self.command(goodix.COMMAND_READ_SENSOR_REGISTER,
                            struct.pack("<BHB", 0, address, length))

    def read_otp(self):
        return self.command(goodix.COMMAND_READ_OTP, b"\x00\x00")

    def request_tls_connection(self):
        return self.command(goodix.COMMAND_REQUEST_TLS_CONNECTION,
                            b"\x00\x00",
                            reply_flags=(goodix.FLAGS_TRANSPORT_LAYER_SECURITY,))

    def tls_successfully_established(self):
        self.command(goodix.COMMAND_TLS_SUCCESSFULLY_ESTABLISHED, b"\x00\x00",
                     reply=False)

    def mcu_switch_to_idle_mode(self, sleep_time: int):
        self.command(goodix.COMMAND_MCU_SWITCH_TO_IDLE_MODE,
                     struct.pack("<BB", sleep_time, 0), reply=False)

    def write_sensor_register(self, address: int, value: bytes):
        self.command(goodix.COMMAND_WRITE_SENSOR_REGISTER,
                     b"\x00" + struct.pack("<H", address) + value,
                     reply=False)

    def upload_config_mcu(self, config: bytes) -> bool:
        return self.command(goodix.COMMAND_UPLOAD_CONFIG_MCU,
                            config)[0] == 0x01

    def set_powerdown_scan_frequency(self, frequency: int) -> bool:
        return self.command(goodix.COMMAND_SET_POWERDOWN_SCAN_FREQUENCY,
                            struct.pack("<H", frequency))[0] == 0x01

    def mcu_switch_to_fdt_mode(self, mode: bytes):
        return self.command(goodix.COMMAND_MCU_SWITCH_TO_FDT_MODE, mode)

    def mcu_switch_to_fdt_down(self, mode: bytes, reply: bool = True,
                               timeout: float | None = None):
        """With reply=True this blocks until a finger touches the sensor."""
        return self.command(goodix.COMMAND_MCU_SWITCH_TO_FDT_DOWN, mode,
                            reply=reply, timeout=timeout)

    def mcu_switch_to_fdt_up(self, mode: bytes, timeout: float | None = None):
        """Blocks until the finger is lifted."""
        return self.command(goodix.COMMAND_MCU_SWITCH_TO_FDT_UP, mode,
                            timeout=timeout)

    def nav(self):
        return self.command(goodix.COMMAND_NAV, b"\x01\x00",
                            reply_checksum=False)

    def mcu_get_image(self, payload: bytes = b"\x01\x00") -> bytes:
        """Returns the TLS application data record(s) carrying the frame."""
        return self.command(
            goodix.COMMAND_MCU_GET_IMAGE, payload,
            reply_flags=(goodix.FLAGS_TRANSPORT_LAYER_SECURITY,
                         goodix.FLAGS_TRANSPORT_LAYER_SECURITY_DATA))
