#!/usr/bin/env python3
"""CLI disassembler for the Goodix Windows DLLs (uses fwre.pe.Pe).

Usage:
  python -m fwre.dump <dll> func <addr>         # disasm one .pdata function
  python -m fwre.dump <dll> range <lo> <hi>     # disasm an address range
  python -m fwre.dump <dll> xref <addr>         # who references addr (rip-rel)
  python -m fwre.dump <dll> calls <addr>        # call targets inside function
  python -m fwre.dump <dll> callers <addr>      # functions that call addr
  python -m fwre.dump <dll> str <substr>        # find ascii+wide strings
  python -m fwre.dump <dll> exports             # list exports
  python -m fwre.dump <dll> all                 # disasm every .pdata function (grep it)

<dll> is e.g. AlgoMilan.dll or EngineAdapter.dll. <addr> accepts 0x-hex.
Slot/export names from tools/algo/re/slots.json are used as annotations.
"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from fwre.pe import Pe  # noqa: E402
import capstone  # noqa: E402

SLOTS = pathlib.Path(__file__).resolve().parents[1] / "algo" / "re" / "slots.json"


def load_names(pe):
    names = {v: k for k, v in pe.exports.items()}
    try:
        for a, n in json.loads(SLOTS.read_text()).items():
            names[int(a, 16)] = n
    except Exception:
        pass
    return names


def h(s):
    return int(s, 16) if s.startswith("0x") else int(s, 0)


def call_targets(pe, lo, hi):
    out = []
    for ins in pe.md.disasm(pe.img[lo - pe.base:hi - pe.base], lo):
        if ins.mnemonic == "call" and ins.operands[0].type == \
                capstone.x86.X86_OP_IMM:
            out.append((ins.address, ins.operands[0].imm))
    return out


def main():
    dll, cmd = sys.argv[1], sys.argv[2]
    pe = Pe(dll)
    names = load_names(pe)
    if cmd == "func":
        addr = h(sys.argv[3])
        rng = pe.func_of(addr)
        if not rng:
            print("no .pdata function covering", hex(addr)); return
        lo, hi = rng
        print(f"; func {hex(lo)}..{hex(hi)}  ({names.get(lo,'')})")
        print(pe.dis(lo, hi, names))
    elif cmd == "range":
        lo, hi = h(sys.argv[3]), h(sys.argv[4])
        print(pe.dis(lo, hi, names))
    elif cmd == "xref":
        for a in pe.xrefs(h(sys.argv[3])):
            fn = pe.func_of(a)
            tag = names.get(fn[0], hex(fn[0])) if fn else "?"
            print(hex(a), "in", tag)
    elif cmd == "calls":
        rng = pe.func_of(h(sys.argv[3])); lo, hi = rng
        for site, tgt in call_targets(pe, lo, hi):
            print(hex(site), "-> ", hex(tgt), names.get(tgt, ""))
    elif cmd == "callers":
        target = h(sys.argv[3])
        for lo, hi in pe.funcs:
            for site, tgt in call_targets(pe, lo, hi):
                if tgt == target:
                    print("called from", names.get(lo, hex(lo)), "@", hex(site))
    elif cmd == "str":
        needle = sys.argv[3]
        for a in pe.find_string(needle):
            print(hex(a), "ascii", repr(pe.string_at(a)))
        for a in pe.find_string(needle, wide=True):
            print(hex(a), "wide", repr(pe.string_at(a)))
    elif cmd == "exports":
        for n, a in sorted(pe.exports.items(), key=lambda kv: kv[1]):
            print(hex(a), n)
    elif cmd == "all":
        for lo, hi in sorted(set(pe.funcs)):
            print(f"; func {hex(lo)}..{hex(hi)}  ({names.get(lo,'')})")
            print(pe.dis(lo, hi, names))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
