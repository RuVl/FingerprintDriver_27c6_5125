"""Emulate the firmware's white-box unwrap (0x08032640) on a PSK blob.

Called at boot and after preset_psk_write from 0x08032938 as
    status = unwrap(blob, blob_len, psk_out, &psk_len /* in: 0x30 */)
"""

import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_MEM_INVALID, UC_MODE_MCLASS,
                     UC_MODE_THUMB, Uc)
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP)

from fwre.fw import BASE, img

UNWRAP = 0x08032640
RAM = 0x20000000
RAM_SIZE = 0x20000
RETURN = 0x08000100
DATA_LOAD, DATA_SIZE = 0x0803eaa0, 0x754
IN_ADDR, OUT_ADDR, LEN_ADDR = 0x2001e000, 0x2001f000, 0x2001f100


def unwrap(blob: bytes):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(0x08000000, 0x100000)
    uc.mem_write(BASE, img)
    uc.mem_write(RETURN, b"\x00\xbf\x00\xbf")  # nop; nop
    uc.mem_map(RAM, RAM_SIZE)
    # .data initialisation, as done by the reset handler at 0x0803c134
    uc.mem_write(RAM, img[DATA_LOAD - BASE:DATA_LOAD - BASE + DATA_SIZE])
    uc.mem_map(0x40000000, 0x100000)  # APB/AHB peripherals (IWDG etc.)
    uc.mem_map(0xe0000000, 0x100000)  # system control space

    def invalid(uc, access, address, size, value, data):
        pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"invalid access {access} @ {address:#x} pc={pc:#x}",
              file=sys.stderr)
        return False

    uc.hook_add(UC_HOOK_MEM_INVALID, invalid)
    uc.mem_write(IN_ADDR, blob)
    uc.mem_write(LEN_ADDR, struct.pack("<I", 0x30))
    uc.reg_write(UC_ARM_REG_SP, RAM + RAM_SIZE - 0x3000)
    uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
    uc.reg_write(UC_ARM_REG_R0, IN_ADDR)
    uc.reg_write(UC_ARM_REG_R1, len(blob))
    uc.reg_write(UC_ARM_REG_R2, OUT_ADDR)
    uc.reg_write(UC_ARM_REG_R3, LEN_ADDR)
    uc.emu_start(UNWRAP | 1, RETURN, count=100_000_000)

    status = uc.reg_read(UC_ARM_REG_R0)
    out_len = struct.unpack("<I", uc.mem_read(LEN_ADDR, 4))[0]
    return status, bytes(uc.mem_read(OUT_ADDR, min(out_len, 0x30)))


if __name__ == "__main__":
    from wbgen.wrap import Emulator
    import os
    for name, psk in [("zero", bytes(32)), ("random", os.urandom(32))]:
        blob = Emulator().wrap(psk)
        status, out = unwrap(blob)
        print(f"{name:6} status={status:#x} out={out.hex()} "
              f"match={out == psk}")
    status, out = unwrap(bytes(102))
    print(f"garbage status={status:#x} out={out.hex()}")
