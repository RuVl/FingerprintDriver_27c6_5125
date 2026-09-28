#!/usr/bin/env python3
"""Field-by-field comparison of probe features: AlgoChicago.dll getFeature (oracle_feat.c,
FEAT_OUT) against the MR !648 port (port_eval.c, FEAT_DUMP), docs/stage3-features.md.

  ./cmp_feat.py <dll_feat.bin> <port_feat.bin> [--mode identify|enroll|all] [-v]

Prints, for every probe usable on both sides whose features differ, the first differing field
and index; then per-field mismatch counts and the share of fully identical probes.
Blocks are matched by record number; for the port the last block of a record is used (WARM
passes also extract features there).

Normalized fields (DLL feature object F -> port GoodixChicagoSubtemplateView):
  QUAL/COVR  F+0x10c / F+0x110          quality / coverage
  RECS       F+0xf8 (F+0xf0 x 0x3c)     records
  ACTV       F+0x108                    active_count (class-0 records after partition)
  PRES       F+0x140                    metric_data.packed_resolution
  DENS       F+0x158, +0x15c, +0x160    density_positive_percent, density_class, inactive_count
  LAUX       F+0x148[0..5]              live_auxiliary.values
  PRIM SECO VALI  F+0x08/+0x10/+0x18 (40x32 bit planes)  metric_data.primary/secondary/validity
  COAR       F+0x28[0..39] (20x16 bits) metric_data.coarse_mask
  POSM       F+0x130 (40x32 bits)       metric_data.position_map
"""
import struct
import sys

FIELDS = ["QUAL", "COVR", "RECS", "ACTV", "PRES", "DENS", "LAUX",
          "PRIM", "SECO", "VALI", "COAR", "POSM"]


def blocks(path):
    d = open(path, "rb").read()
    o = 0
    while o < len(d):
        if d[o:o + 4] != b"FEAT":
            raise SystemExit("%s: bad block at %d" % (path, o))
        n = struct.unpack_from("<I", d, o + 4)[0]
        b = d[o + 8:o + 8 + n]
        o += 8 + n
        s = {}
        p = 0
        while p < len(b):
            t = b[p:p + 4].decode()
            ln = struct.unpack_from("<I", b, p + 4)[0]
            s[t] = b[p + 8:p + 8 + ln]
            p += 8 + ln
        yield s


def i32(b, off):
    return struct.unpack_from("<i", b, off)[0]


def norm_dll(s):
    rec, mode, rc, q, c, w, h = struct.unpack("<7i", s["HDR "])
    if rc != 0 or "OBJ " not in s:
        return rec, mode, None
    F = s["OBJ "]
    f = {
        "QUAL": i32(F, 0x10c), "COVR": i32(F, 0x110),
        "RECS": s["RECS"], "ACTV": i32(F, 0x108), "PRES": i32(F, 0x140),
        "DENS": (i32(F, 0x158), i32(F, 0x15c), i32(F, 0x160)),
        "LAUX": s["B148"][:6] if "B148" in s else b"\0" * 6,
        "PRIM": s["I008"][0x20:], "SECO": s["I010"][0x20:], "VALI": s["I018"][0x20:],
        "COAR": F[0x28:0x28 + 40], "POSM": s["I130"][0x20:],
        "_cbuf": s.get("CBUF", b""),
    }
    return rec, mode, f


def norm_port(s):
    rec, mode, rc, q, c, w, h = struct.unpack("<7i", s["HDR "])
    f = {
        "QUAL": q, "COVR": c, "RECS": s["RECS"],
        "ACTV": struct.unpack("<I", s["ACTV"])[0],
        "PRES": struct.unpack("<I", s["PRES"])[0] if "PRES" in s else None,
        "DENS": struct.unpack("<3I", s["DENS"]),
        "LAUX": s["LAUX"],
        "PRIM": s.get("PRIM"), "SECO": s.get("SECO"), "VALI": s.get("VALI"),
        "COAR": s.get("COAR"), "POSM": s.get("POSM"),
    }
    return rec, f


def first_diff(field, a, b):
    """None if equal, else a short description of the first difference."""
    if a == b:
        return None
    if field == "RECS":
        na, nb = len(a) // 0x3c, len(b) // 0x3c
        for k in range(min(na, nb)):
            ra, rb = a[k * 0x3c:(k + 1) * 0x3c], b[k * 0x3c:(k + 1) * 0x3c]
            if ra != rb:
                j = next(j for j in range(0x3c) if ra[j] != rb[j])
                return "rec %d byte 0x%x: dll %s port %s (count %d/%d)" % (
                    k, j, ra.hex(), rb.hex(), na, nb)
        return "count dll %d port %d" % (na, nb)
    if isinstance(a, (bytes, bytearray)) and isinstance(b, (bytes, bytearray)):
        if len(a) != len(b):
            return "length dll %d port %d" % (len(a), len(b))
        j = next(j for j in range(len(a)) if a[j] != b[j])
        nd = sum(1 for x, y in zip(a, b) if x != y)
        return "byte %d: dll %02x port %02x (%d bytes differ)" % (j, a[j], b[j], nd)
    return "dll %s port %s" % (a, b)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    opts = [a for a in sys.argv[1:] if a.startswith("-")]
    mode = "identify"
    for k, a in enumerate(sys.argv):
        if a == "--mode" and k + 1 < len(sys.argv):
            mode = sys.argv[k + 1]
            args = [x for x in args if x != mode]
    verbose = "-v" in opts
    if len(args) != 2:
        raise SystemExit(__doc__)
    dll = {}
    for s in blocks(args[0]):
        rec, m, f = norm_dll(s)
        if mode == "all" or (m == 1) == (mode == "identify"):
            dll[rec] = f
    port = {}
    for s in blocks(args[1]):
        rec, f = norm_port(s)
        port[rec] = f
    both = sorted(r for r in dll if dll[r] is not None and r in port)
    only_dll = sorted(r for r in dll if dll[r] is not None and r not in port)
    counts = {k: 0 for k in FIELDS}
    same = 0
    for r in both:
        d, p = dll[r], port[r]
        diffs = [(k, first_diff(k, d[k], p[k])) for k in FIELDS]
        diffs = [(k, x) for k, x in diffs if x]
        for k, _ in diffs:
            counts[k] += 1
        if not diffs:
            same += 1
            continue
        k, x = diffs[0]
        print("rec %3d: %-4s %s%s" % (r, k, x,
                                      "  [+%s]" % ",".join(k2 for k2, _ in diffs[1:]) if len(diffs) > 1 else ""))
        if verbose:
            for k2, x2 in diffs[1:]:
                print("          %-4s %s" % (k2, x2))
    print("probes (%s) compared: %d, identical: %d (%.1f%%)%s" % (
        mode, len(both), same, 100.0 * same / len(both) if both else 0,
        ", missing in port: %s" % only_dll if only_dll else ""))
    print("mismatches per field: " + " ".join("%s=%d" % (k, counts[k]) for k in FIELDS))
    return 0 if same == len(both) else 1


if __name__ == "__main__":
    sys.exit(main())
