"""Collect a fingerprint dataset from the 5125 for offline tuning.

For every touch it stores the background frame and three frames of the
finger: right at the FDT touch event, once all six FDT channels report
contact, and 150 ms after that. Everything goes to
dumps/dataset-*/<label>.npz with the FDT replies.

Usage: collect.py LABEL COUNT "prompt text"
"""

import datetime
import os
import json
import sys
import time

import numpy as np
import usb

from capture import CaptureUSBProtocol, fdt_payload, read_frame
from fdt_test import init
from goodix5125.device import Device5125
from probe import DUMPS, PRODUCT

FDT_INITIAL = bytes.fromhex("0901b3b3c3c3a8a8b5b5a8a8b7b7")
FULL_CONTACT = 0x3f
TOUCH_TIMEOUT = float(os.environ.get("COLLECT_TOUCH_TIMEOUT", 120))


def touch_mask(reply):
    return reply[2]


def banner(text):
    print("\n" + "=" * 60 + f"\n  {text}\n" + "=" * 60, flush=True)


def wait_no_finger(device):
    while True:
        reply = device.mcu_switch_to_fdt_mode(FDT_INITIAL)
        if touch_mask(reply) == 0:
            return reply
        time.sleep(0.1)


def one_touch(device, server, index, count, prompt, log):
    reply = wait_no_finger(device)
    device.nav()
    reply = device.mcu_switch_to_fdt_mode(fdt_payload(0x0d, reply))
    _, background = read_frame(device, server)
    reply = device.mcu_switch_to_fdt_mode(fdt_payload(0x0d, reply))
    base = reply
    try:
        reply = device.mcu_switch_to_fdt_down(fdt_payload(0x0c, reply),
                                              timeout=2)
    except usb.core.USBTimeoutError:
        pass
    device.query_mcu_state()

    banner(f"[{index + 1}/{count}] {prompt}")
    down = device.mcu_switch_to_fdt_down(fdt_payload(0x0c, reply),
                                         timeout=TOUCH_TIMEOUT)
    _, first = read_frame(device, server)

    masks = [touch_mask(down)]
    full = None
    deadline = time.time() + 0.6
    while time.time() < deadline:
        r = device.mcu_switch_to_fdt_mode(fdt_payload(0x0d, base))
        masks.append(touch_mask(r))
        if touch_mask(r) == FULL_CONTACT:
            _, full = read_frame(device, server)
            break
        time.sleep(0.03)
    if full is None:
        _, full = read_frame(device, server)
    time.sleep(0.15)
    _, late = read_frame(device, server)

    print("  снято, УБЕРИТЕ палец", flush=True)
    while touch_mask(device.mcu_switch_to_fdt_mode(
            fdt_payload(0x0d, base))) != 0:
        time.sleep(0.1)

    log.append({"down": down.hex(), "base": base.hex(), "masks": masks})
    return [background, first, full, late]


def main():
    label, count, prompt = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    out = DUMPS / "dataset"
    out.mkdir(parents=True, exist_ok=True)

    device = Device5125(PRODUCT, CaptureUSBProtocol)
    server = init(device)
    frames, log = [], []
    try:
        for i in range(count):
            frames.append(one_touch(device, server, i, count, prompt, log))
    finally:
        stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
        if frames:
            np.savez_compressed(out / f"{label}-{stamp}.npz",
                                frames=np.array(frames, dtype=np.uint16))
            (out / f"{label}-{stamp}.json").write_text(
                json.dumps(log, indent=1))
        banner(f"ГОТОВО: {len(frames)} касаний сохранено ({label})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
