"""Tiny helpers for reading the ST411 firmware image (Cortex-M, Thumb-2)."""

import pathlib
import struct

import capstone

IMAGE = (pathlib.Path(__file__).resolve().parents[2] / "win-driver" / "fw" /
         "GF_ST411SEC_APP_12512.bin")
BASE = 0x08020000

img = IMAGE.read_bytes()
md = capstone.Cs(capstone.CS_ARCH_ARM,
                 capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
md.detail = True


def u32(addr):
    return struct.unpack_from("<I", img, addr - BASE)[0]


def dis(addr, count=40, stop_at_ret=False):
    out = []
    for ins in md.disasm(img[addr - BASE:addr - BASE + count * 4], addr):
        text = f"{ins.address:#x}: {ins.mnemonic:8} {ins.op_str}"
        lit = literal_target(ins)
        if lit is not None and BASE <= lit < BASE + len(img) - 4:
            text += f"    ; [{lit:#x}] = {u32(lit):#x}"
        out.append(text)
        if len(out) >= count:
            break
        if stop_at_ret and (ins.mnemonic.startswith("pop") and "pc" in
                            ins.op_str or ins.mnemonic == "bx"
                            and ins.op_str == "lr"):
            break
    return "\n".join(out)


def literal_target(ins):
    if ins.mnemonic.startswith("ldr") and "[pc" in ins.op_str:
        disp = ins.operands[1].mem.disp
        return ((ins.address + 4) & ~3) + disp
    return None


def _all_insns():
    """Decode at every halfword; noisy but enough for xref searches."""
    for off in range(0, len(img) - 4, 2):
        for ins in md.disasm(img[off:off + 4], BASE + off, count=1):
            yield ins


_cache = None


def insns():
    global _cache
    if _cache is None:
        _cache = list(_all_insns())
    return _cache


def refs_literal(addr):
    return [i.address for i in insns() if literal_target(i) == addr]


def refs_call(target):
    out = []
    for i in insns():
        if i.mnemonic in ("bl", "b", "b.w", "blx") and i.operands and \
                i.operands[0].type == capstone.arm.ARM_OP_IMM and \
                i.operands[0].imm == target:
            out.append(i.address)
    return out


def func_start(addr, limit=0x400):
    """Walk back to the nearest push {..., lr}."""
    for a in range(addr, addr - limit, -2):
        for ins in md.disasm(img[a - BASE:a - BASE + 4], a, count=1):
            if ins.mnemonic.startswith("push") and "lr" in ins.op_str:
                return a
    return None
