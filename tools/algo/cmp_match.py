#!/usr/bin/env python3
"""Compare per-probe match decisions: algo_eval4 (DLL) vs port_eval (MR648 port).

  VERBOSE=1 ALGO=chicago ./algo_eval4 12 > dll.txt
  VERBOSE=1 INJECT_PROC=... [MFIXES=1] ./port_eval.sh 12 > port.txt
  ./cmp_match.py dll.txt port.txt [--all]
  ./cmp_match.py dll.txt port.txt --score dll_trace.txt     (exit 1 on any score/idx difference)

Both print '  <label> <rec> ... score=<s> idx=<i> ... MATCH|-' lines; unusable probes
('reject=' / rc!=0) are skipped.  Prints the confusion per group and every probe whose
decision differs (with --all: every probe).

--score <trace>: bit-exact mode.  Also compares the score value and the selected subtemplate
index of every probe usable on both sides.  The txt 'idx' of algo_eval4 is the matched *template*
(always 0), so the DLL's subtemplate index is taken from a dll_match_trace.py trace of the same
run (last "SEL i" of the probe's PROBE block = result+0x648, -1 if none); the port prints its
selected_index.  Prints every probe with a difference and exits 1 if there is any.
"""
import re
import sys

LINE = re.compile(r"^\s+(natural|genuine|impostor)\s+(\d+)\s.*score=\s*(-?\d+)\s+idx=\s*(-?\d+)(.*)$")


def load(path):
    out = {}
    for line in open(path):
        m = LINE.match(line)
        if not m:
            continue
        label, rec, score, idx, tail = m.groups()
        rc = re.search(r"rc=(0x[0-9a-f]+)", tail)
        if rc and int(rc.group(1), 16) != 0:
            continue
        out[int(rec)] = (label, int(score), int(idx), tail.strip().endswith("MATCH"))
    return out


def load_sel(path):
    sel, cur = {}, None
    for line in open(path):
        if line.startswith("PROBE "):
            cur = int(line.split()[1])
            sel[cur] = -1
        elif line.startswith("  SEL ") and cur is not None:
            sel[cur] = int(line.split()[1])
    return sel


def score_mode(dll, port, trace):
    sel = load_sel(trace)
    common = sorted(set(dll) & set(port))
    bad = 0
    for rec in common:
        label, ds, _, _ = dll[rec]
        _, ps, pi, _ = port[rec]
        di = sel.get(rec)
        if di is None:
            print("  %-8s %3d  not in trace" % (label, rec))
            bad += 1
        elif ds != ps or di != pi:
            print("  %-8s %3d  dll score=%4d sub=%2d   port score=%4d sub=%2d%s%s"
                  % (label, rec, ds, di, ps, pi, "  SCORE" if ds != ps else "", "  IDX" if di != pi else ""))
            bad += 1
    print("score/idx differ: %d of %d" % (bad, len(common)))
    return 1 if bad else 0


def main():
    dll, port = load(sys.argv[1]), load(sys.argv[2])
    if "--score" in sys.argv:
        sys.exit(score_mode(dll, port, sys.argv[sys.argv.index("--score") + 1]))
    show_all = "--all" in sys.argv
    groups = {}
    diffs = []
    for rec in sorted(set(dll) & set(port)):
        label, ds, di, dm = dll[rec]
        _, ps, pi, pm = port[rec]
        g = groups.setdefault(label, [0, 0, 0, 0])   # both, dll only, port only, neither
        g[0 if dm and pm else 1 if dm else 2 if pm else 3] += 1
        if dm != pm or show_all:
            diffs.append((label, rec, ds, di, dm, ps, pi, pm))
    print("group      both dll-only port-only neither")
    for label in ("natural", "genuine", "impostor"):
        if label in groups:
            print("%-9s %5d %8d %9d %7d" % ((label,) + tuple(groups[label])))
    only = sorted(set(dll) ^ set(port))
    if only:
        print("usable on one side only:", only)
    for label, rec, ds, di, dm, ps, pi, pm in diffs:
        print("  %-8s %3d  dll score=%4d idx=%2d %-5s  port score=%4d idx=%2d %s"
              % (label, rec, ds, di, "MATCH" if dm else "-", ps, pi, "MATCH" if pm else "-"))
    print("decisions differ: %d of %d" % (sum(1 for d in diffs if d[4] != d[7]), len(set(dll) & set(port))))


if __name__ == "__main__":
    main()
