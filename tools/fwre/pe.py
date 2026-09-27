"""Helpers for reading x64 PE files (Goodix Windows driver DLLs)."""

import pathlib

import capstone
import pefile

WIN = pathlib.Path(__file__).resolve().parents[2] / "win-driver"


class Pe:

    def __init__(self, name):
        self.pe = pefile.PE(str(WIN / name))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        text = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.text = (text.VirtualAddress, text.VirtualAddress +
                     text.Misc_VirtualSize)
        self._insns = None
        self.funcs = sorted(
            (self.base + f.struct.BeginAddress, self.base + f.struct.EndAddress)
            for f in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", []))
        self.exports = {}
        if hasattr(self.pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.name:
                    self.exports[e.name.decode()] = self.base + e.address
        self.imports = {}
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            for imp in entry.imports:
                if imp.name:
                    self.imports[imp.address] = imp.name.decode()

    def find_string(self, text, wide=False):
        needle = text.encode("utf-16-le" if wide else "ascii")
        out, i = [], self.img.find(needle)
        while i >= 0:
            out.append(self.base + i)
            i = self.img.find(needle, i + 1)
        return out

    def insns(self):
        """All instructions, decoded function by function (from .pdata)."""
        if self._insns is None:
            self._insns = []
            for lo, hi in self.funcs:
                self._insns += list(self.md.disasm(
                    self.img[lo - self.base:hi - self.base], lo))
        return self._insns

    @staticmethod
    def rip_target(ins):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and \
                    op.mem.base == capstone.x86.X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp
        return None

    def xrefs(self, addr):
        return [i.address for i in self.insns() if self.rip_target(i) == addr]

    def func_of(self, addr):
        for lo, hi in self.funcs:
            if lo <= addr < hi:
                return lo, hi
        return None

    def string_at(self, addr, limit=120):
        off = addr - self.base
        if not 0 <= off < len(self.img):
            return None
        raw = self.img[off:off + limit]
        s = raw.split(b"\0")[0]
        if len(s) >= 4 and all(32 <= c < 127 or c in (9, 10) for c in s):
            return s.decode()
        w = raw.decode("utf-16-le", "ignore").split("\0")[0]
        if len(w) >= 4 and w.isprintable():
            return "L" + repr(w)
        return None

    def dis(self, lo, hi, names=None):
        names = names or {}
        lines = []
        for ins in self.md.disasm(self.img[lo - self.base:hi - self.base], lo):
            text = f"{ins.address:#x}: {ins.mnemonic:6} {ins.op_str}"
            t = self.rip_target(ins)
            if t is not None:
                if t in names:
                    text += f"   ; <{names[t]}>"
                elif t in self.imports:
                    text += f"   ; {self.imports[t]}"
                else:
                    s = self.string_at(t)
                    if s:
                        text += f"   ; {s!r}"
            if ins.mnemonic == "call" and ins.operands[0].type == \
                    capstone.x86.X86_OP_IMM:
                name = {v: k for k, v in self.exports.items()}.get(
                    ins.operands[0].imm)
                if name:
                    text += f"   ; {name}"
            lines.append(text)
        return "\n".join(lines)
