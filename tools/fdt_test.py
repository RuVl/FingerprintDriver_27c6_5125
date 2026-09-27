"""Find an FDT (finger detect) sequence that reports a touch on the 5125.

Tries the sequences of the two working drivers for the same silicon, one
after another, printing clear prompts. Every reply is logged with a
timestamp to dumps/fdt-test-*/log.json.

    A: driver_51x7  - fdt_mode, probe fdt_down, query state, fdt_down (wait)
    B: goodix511    - fdt_up, nav, calibration frame, fdt_down (wait)
"""

import datetime
import json
import sys
import time

import usb

from capture import (CaptureUSBProtocol, fdt_payload, read_frame, save_pgm)
from goodix5125.config import build_config, otp_register_values
from goodix5125.device import Device5125
from goodix5125.tls import TLSServer
from probe import DUMPS, PRODUCT
from tls_check import wake

import struct

WAIT_SECONDS = 25
START = time.time()
LOG = []


def note(event, data=None):
    entry = {"t": round(time.time() - START, 2), "event": event}
    if data is not None:
        entry["data"] = data.hex() if isinstance(data, bytes) else data
    LOG.append(entry)
    print(f"[{entry['t']:6.2f}] {event}"
          + (f": {entry['data']}" if data is not None else ""), flush=True)


def banner(text):
    print("\n" + "=" * 60 + f"\n  {text}\n" + "=" * 60, flush=True)


def init(device):
    wake(device)
    device.reset(True, False, 20)
    otp = device.read_otp()
    device.reset(True, False, 20)
    device.mcu_switch_to_idle_mode(20)
    for address, value in otp_register_values(otp).items():
        device.write_sensor_register(address, struct.pack("<H", value))
    note("config", device.upload_config_mcu(build_config(otp)))
    note("powerdown", device.set_powerdown_scan_frequency(100))
    server = TLSServer(bytes(32))
    server.connect(device)
    device.tls_successfully_established()
    note("mcu_state", device.query_mcu_state())
    return server


def wait_touch(device, payload, out, server, name):
    banner(f"[{name}] ПРИЛОЖИТЕ ПАЛЕЦ сейчас (жду {WAIT_SECONDS} с)")
    try:
        down = device.mcu_switch_to_fdt_down(payload, timeout=WAIT_SECONDS)
    except usb.core.USBTimeoutError:
        note(f"{name}: no touch event")
        banner(f"[{name}] касание НЕ обнаружено. Уберите палец.")
        time.sleep(3)
        return False
    note(f"{name}: TOUCH EVENT", down)
    _, frame = read_frame(device, server)
    save_pgm(frame, out / f"{name}-finger.pgm")
    banner(f"[{name}] касание обнаружено! Уберите палец.")
    try:
        up = device.mcu_switch_to_fdt_up(fdt_payload(0x0e, down), timeout=15)
        note(f"{name}: lift event", up)
    except usb.core.USBTimeoutError:
        note(f"{name}: no lift event")
    return True


def variant_a(device, server, out):
    banner("[A] Подготовка. НЕ трогайте сенсор.")
    reply = device.mcu_switch_to_fdt_mode(
        bytes.fromhex("0901b3b3c3c3a8a8b5b5a8a8b7b7"))
    note("A: fdt_mode 1", reply)
    note("A: nav bytes", len(device.nav()))
    reply = device.mcu_switch_to_fdt_mode(fdt_payload(0x0d, reply))
    note("A: fdt_mode 2", reply)
    _, frame = read_frame(device, server)
    save_pgm(frame, out / "A-background.pgm")
    reply = device.mcu_switch_to_fdt_mode(fdt_payload(0x0d, reply))
    note("A: fdt_mode 3", reply)
    try:
        probe = device.mcu_switch_to_fdt_down(fdt_payload(0x0c, reply),
                                              timeout=2)
        note("A: probe fdt_down replied", probe)
        reply = probe
    except usb.core.USBTimeoutError:
        note("A: probe fdt_down: no reply in 2 s")
    note("A: mcu_state", device.query_mcu_state())
    return wait_touch(device, fdt_payload(0x0c, reply), out, server, "A")


def variant_b(device, server, out):
    banner("[B] Подготовка. НЕ трогайте сенсор.")
    reply = device.mcu_switch_to_fdt_mode(
        bytes.fromhex("0901b3b3c3c3a8a8b5b5a8a8b7b7"))
    note("B: fdt_mode", reply)
    try:
        up = device.mcu_switch_to_fdt_up(fdt_payload(0x0e, reply), timeout=2)
        note("B: fdt_up replied", up)
    except usb.core.USBTimeoutError:
        note("B: fdt_up: no reply in 2 s")
    note("B: nav bytes", len(device.nav()))
    _, frame = read_frame(device, server)
    save_pgm(frame, out / "B-background.pgm")
    return wait_touch(device, fdt_payload(0x0c, reply), out, server, "B")


def main():
    out = DUMPS / f"fdt-test-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    out.mkdir(parents=True)
    results = {}
    try:
        device = Device5125(PRODUCT, CaptureUSBProtocol)
        server = init(device)
        results["A"] = variant_a(device, server, out)
        results["B"] = variant_b(device, server, out)
    finally:
        (out / "log.json").write_text(
            json.dumps({"results": results, "log": LOG}, indent=2) + "\n")
        banner(f"ГОТОВО. Результаты: {results}. Лог: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
