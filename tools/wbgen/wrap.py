"""Run the PSK white-box wrap routine from gfusb.dll under Unicorn.

gfusb.dll 1.1.125.20 (Goodix FP - Biometric, Microsoft Update Catalog)
exposes the routine at RVA 0x6c20 with the signature

    int wrap(const u8 *psk, u32 psk_len, u8 *out, u32 *out_len)

The produced blob is what the Windows driver writes to the MCU with
preset_psk_write(0xbb010003, blob); the MCU then reports SHA256(blob)
through preset_psk_read(0xbb020003).
"""

import hashlib
import pathlib
import struct
import sys

import pefile
from unicorn import (UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_INVALID,
                     UC_MODE_64, Uc, UcError)
from unicorn.x86_const import (UC_X86_REG_GS_BASE, UC_X86_REG_R8,
                               UC_X86_REG_R9, UC_X86_REG_RAX, UC_X86_REG_RCX,
                               UC_X86_REG_RDX, UC_X86_REG_RIP, UC_X86_REG_RSP)

DLL = (pathlib.Path(__file__).resolve().parents[2] / "win-driver" /
       "gfusb.dll")
WRAP_RVA = 0x6c20
OUT_CAPACITY = 0x7f8

STUB_BASE = 0x7ff000000000
STACK_BASE = 0x7ffe00000000
STACK_SIZE = 0x100000
HEAP_BASE = 0x7ffd00000000
HEAP_SIZE = 0x1000000
TEB_BASE = 0x7ffc00000000
SCRATCH_BASE = 0x7ffb00000000
RETURN_ADDRESS = STUB_BASE + 0xfff0


def align(value, alignment=0x1000):
    return (value + alignment - 1) & ~(alignment - 1)


class Emulator:

    def __init__(self, path=DLL):
        self.pe = pefile.PE(str(path))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image = self.pe.get_memory_mapped_image()
        self.heap_top = HEAP_BASE
        self.calls = []

        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc.mem_map(self.base, align(len(self.image)))
        self.uc.mem_write(self.base, self.image)

        self.stubs = {}
        self.uc.mem_map(STUB_BASE, 0x10000)
        self.uc.mem_write(STUB_BASE, b"\xc3" * 0x10000)
        self._patch_imports()

        self.uc.mem_map(STACK_BASE, STACK_SIZE)
        self.uc.mem_map(HEAP_BASE, HEAP_SIZE)
        self.uc.mem_map(TEB_BASE, 0x10000)
        self.uc.mem_write(TEB_BASE + 0x30, struct.pack("<Q", TEB_BASE))
        self.uc.reg_write(UC_X86_REG_GS_BASE, TEB_BASE)
        self.uc.mem_map(SCRATCH_BASE, 0x10000)

        self.uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUB_BASE,
                         end=STUB_BASE + 0xffff)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._on_invalid)

    def _patch_imports(self):
        index = 0
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else f"ord{imp.ordinal}"
                address = STUB_BASE + index * 0x10
                self.stubs[address] = f"{entry.dll.decode()}!{name}"
                self.uc.mem_write(imp.address, struct.pack("<Q", address))
                index += 1

    def _alloc(self, size):
        address = align(self.heap_top, 0x10)
        self.heap_top = address + max(size, 1)
        if self.heap_top > HEAP_BASE + HEAP_SIZE:
            raise MemoryError("emulator heap exhausted")
        self.uc.mem_write(address, b"\x00" * max(size, 1))
        return address

    def _on_stub(self, uc, address, size, user_data):
        if address == RETURN_ADDRESS:
            uc.emu_stop()
            return

        name = self.stubs.get(address)
        if name is None:
            return

        rcx = uc.reg_read(UC_X86_REG_RCX)
        rdx = uc.reg_read(UC_X86_REG_RDX)
        r8 = uc.reg_read(UC_X86_REG_R8)
        self.calls.append(name)
        short = name.split("!")[1]

        if short == "HeapAlloc":
            result = self._alloc(r8)
        elif short in ("HeapFree", "GetProcessHeap"):
            result = 1 if short == "HeapFree" else 0x1
        elif short == "HeapSize":
            result = 0
        else:
            uc.emu_stop()
            raise NotImplementedError(f"unhandled import {name} "
                                      f"(rcx={rcx:#x} rdx={rdx:#x})")

        uc.reg_write(UC_X86_REG_RAX, result)

    def _on_invalid(self, uc, access, address, size, value, user_data):
        rip = uc.reg_read(UC_X86_REG_RIP)
        print(f"invalid memory access {access} at {address:#x} "
              f"(rip={rip:#x}, rva={rip - self.base:#x})", file=sys.stderr)
        return False

    def wrap(self, psk: bytes) -> bytes:
        self.heap_top = HEAP_BASE
        psk_address = SCRATCH_BASE
        out_address = self._alloc(OUT_CAPACITY)
        out_len_address = SCRATCH_BASE + 0x100

        self.uc.mem_write(psk_address, psk)
        self.uc.mem_write(out_len_address, struct.pack("<I", OUT_CAPACITY))

        rsp = STACK_BASE + STACK_SIZE - 0x1000
        self.uc.mem_write(rsp, struct.pack("<Q", RETURN_ADDRESS))
        self.uc.reg_write(UC_X86_REG_RSP, rsp)
        self.uc.reg_write(UC_X86_REG_RCX, psk_address)
        self.uc.reg_write(UC_X86_REG_RDX, len(psk))
        self.uc.reg_write(UC_X86_REG_R8, out_address)
        self.uc.reg_write(UC_X86_REG_R9, out_len_address)

        self.uc.emu_start(self.base + WRAP_RVA, RETURN_ADDRESS,
                          count=200_000_000)

        result = self.uc.reg_read(UC_X86_REG_RAX) & 0xffffffff
        if result != 0:
            raise RuntimeError(f"wrap returned {result:#x}")

        out_len = struct.unpack("<I", self.uc.mem_read(out_len_address, 4))[0]
        return bytes(self.uc.mem_read(out_address, out_len))


def white_box(psk: bytes) -> bytes:
    return Emulator().wrap(psk)


def main():
    psk = bytes.fromhex(sys.argv[1]) if len(sys.argv) > 1 else bytes(32)
    emulator = Emulator()
    try:
        blob = emulator.wrap(psk)
    except (UcError, NotImplementedError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        print(f"imports called: {emulator.calls}", file=sys.stderr)
        return 1

    print(f"dll sha256: {hashlib.sha256(DLL.read_bytes()).hexdigest()}")
    print(f"imports called: {sorted(set(emulator.calls))}")
    print(f"psk:  {psk.hex()}")
    print(f"blob ({len(blob)} bytes): {blob.hex()}")
    print(f"sha256(blob): {hashlib.sha256(blob).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
