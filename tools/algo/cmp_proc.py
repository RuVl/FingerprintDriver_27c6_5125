"""Compare 8-bit preprocessor outputs of AlgoChicago.dll and the MR648 port.

Inputs: two DUMP_PROC files (algo_eval4.c / port_eval.c), records of
{int32 rec, status, quality, coverage} + 5120 bytes (64 rows x 80), in call order.
Both programs must have been run with the same protocol (same N, WARM, WARMSET),
so that call k saw the same raw frame with the same preprocessor history.

Usage: python3 cmp_proc.py dll.bin port.bin [--calls 0,1,2,...] [--all]
"""
import sys

import numpy as np

REC = np.dtype([("hdr", "<i4", 4), ("img", "u1", 5120)])


def load(path):
    return np.fromfile(path, dtype=REC)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    a, b = load(args[0]), load(args[1])
    n = min(len(a), len(b))
    if len(a) != len(b):
        print(f"call count differs: dll {len(a)} port {len(b)} (comparing first {n})")
    calls = range(n)
    for opt in sys.argv[1:]:
        if opt.startswith("--calls="):
            calls = [int(x) for x in opt.split("=", 1)[1].split(",")]
    show_all = "--all" in sys.argv
    rows = []
    for k in calls:
        ha, hb = a[k]["hdr"], b[k]["hdr"]
        if ha[0] != hb[0]:
            print(f"call {k}: record mismatch dll={ha[0]} port={hb[0]} — protocols diverged")
            break
        ia = a[k]["img"].astype(np.int16)
        ib = b[k]["img"].astype(np.int16)
        d = np.abs(ia - ib)
        # Pearson correlation of the two images (robust to a global offset/scale)
        corr = np.corrcoef(ia.ravel(), ib.ravel())[0, 1] if ia.std() and ib.std() else float("nan")
        rows.append((k, ha[0], ha[1], hb[1], ha[2], hb[2], ha[3], hb[3],
                     (d == 0).mean() * 100, (d <= 2).mean() * 100, d.max(), d.mean(), corr))
    print("call rec  rc_dll rc_port  q_dll q_port  c_dll c_port  exact%  |d|<=2%  max|d|  mean|d|  corr")
    for r in rows if show_all else rows[:10]:
        print("%4d %3d  %6x %7x  %5d %6d  %5d %6d  %6.2f  %7.2f  %6d  %7.2f  %5.3f" % r)
    ok = [r for r in rows if r[2] == 0 and r[3] == 0]
    if ok:
        ex = np.array([r[8] for r in ok]); le2 = np.array([r[9] for r in ok])
        mx = np.array([r[10] for r in ok]); co = np.array([r[12] for r in ok])
        print(f"summary over {len(ok)} calls accepted by both: exact {ex.mean():.2f}% "
              f"(min {ex.min():.2f}, max {ex.max():.2f}), |d|<=2 {le2.mean():.2f}%, "
              f"max|d| median {int(np.median(mx))} / max {mx.max()}, corr mean {np.nanmean(co):.3f}")
        identical = sum(1 for r in ok if r[10] == 0)
        print(f"bit-identical images: {identical}/{len(ok)}")
    rc_a = np.array([r[2] != 0 for r in rows]); rc_b = np.array([r[3] != 0 for r in rows])
    print(f"rejects: dll {rc_a.sum()}, port {rc_b.sum()}, both {(rc_a & rc_b).sum()}")
    qa = np.array([r[4] for r in ok]); qb = np.array([r[5] for r in ok])
    if len(ok):
        print(f"quality: dll mean {qa.mean():.1f}, port mean {qb.mean():.1f}, "
              f"equal {np.mean(qa == qb) * 100:.1f}%, corr {np.corrcoef(qa, qb)[0, 1]:.3f}")


if __name__ == "__main__":
    main()
