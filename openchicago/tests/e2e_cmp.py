#!/usr/bin/env python3
"""Compare an openchicago test_e2e run with the AlgoChicago.dll oracle (same protocol).

  e2e_cmp.py <ref_prefix> <run_prefix>
ref (tools/algo, e2e.sh):  <ref>.d.txt (oracle_feat VERBOSE=1 STUDY=1), <ref>.trace
  (dll_match_trace.py: selected subtemplate), <ref>.st + <ref>.dll/sNNN.tpl (STRACE,
  STUDY_DIR), <ref>.tpl (TPL_OUT, gallery)
run (test_e2e):  <run>.txt, <run>.st, <run>/sNNN.tpl, <run>.tpl
Checks: same usable probes; per probe quality, coverage, score, subtemplate, decision;
gallery blob; every templateStudy step (record, score, update, blob); no impostor match.
Exit 1 on any difference.
"""
import os
import re
import sys

LINE = re.compile(r"^\s+(natural|genuine|impostor)\s+(\d+)\s+q=\s*(\d+)\s+c=\s*(\d+)\s.*"
                  r"score=\s*(-?\d+)\s+idx=\s*(-?\d+)(.*)$")


def probes(path):
    out = {}
    for line in open(path):
        m = LINE.match(line)
        if not m:
            continue
        label, rec, q, c, score, idx, tail = m.groups()
        rc = re.search(r"rc=(0x[0-9a-f]+)", tail)
        if rc and int(rc.group(1), 16) != 0:
            continue
        out[int(rec)] = [label, int(q), int(c), int(score), int(idx), tail.strip().endswith("MATCH")]
    return out


def sel(path):
    s, cur = {}, None
    for line in open(path):
        if line.startswith("PROBE "):
            cur = int(line.split()[1])
            s[cur] = -1
        elif line.startswith("  SEL ") and cur is not None:
            s[cur] = int(line.split()[1])
    return s


def study(path):
    out = []
    for line in open(path) if os.path.exists(path) else []:
        f = line.split()
        out.append((int(f[2]), int(f[4]), int(f[6]) > 0))
    return out


def same_file(a, b):
    return os.path.exists(a) and os.path.exists(b) and open(a, "rb").read() == open(b, "rb").read()


def main():
    ref, run = sys.argv[1], sys.argv[2]
    d, p = probes(ref + ".d.txt"), probes(run + ".txt")
    s = sel(ref + ".trace")
    bad = 0
    if set(d) != set(p):
        print("usable probes differ: dll only %s, openchicago only %s"
              % (sorted(set(d) - set(p)), sorted(set(p) - set(d))))
        bad += 1
    nd = 0
    for rec in sorted(set(d) & set(p)):
        dl, dq, dc, ds, _, dm = d[rec]
        _, pq, pc, ps, pi, pm = p[rec]
        di = s.get(rec, -99)
        if (dq, dc, ds, di, dm) != (pq, pc, ps, pi, pm):
            print("  %-8s %3d dll q=%d c=%d score=%d sub=%d %s | openchicago q=%d c=%d score=%d sub=%d %s"
                  % (dl, rec, dq, dc, ds, di, dm, pq, pc, ps, pi, pm))
            nd += 1
    bad += nd
    imp = sum(1 for v in p.values() if v[0] == "impostor" and v[5])
    bad += imp
    gal = same_file(ref + ".tpl", run + ".tpl")
    bad += not gal
    sd, sp = study(ref + ".st"), study(run + ".st")
    ns = 0
    for k in range(max(len(sd), len(sp))):
        a = sd[k] if k < len(sd) else None
        b = sp[k] if k < len(sp) else None
        blob_ok = a and b and same_file("%s.dll/s%03d.tpl" % (ref, a[0]), "%s/s%03d.tpl" % (run, b[0]))
        if a != b or not blob_ok:
            print("  study step %d: dll %s openchicago %s blob %s" % (k, a, b, "same" if blob_ok else "DIFFERS"))
            ns += 1
    bad += ns
    print("probes %d usable (dll %d): q/c/score/subtemplate/decision differ %d; impostor matches %d; "
          "gallery blob %s; study steps %d (dll %d), differ %d -> %s"
          % (len(p), len(d), nd, imp, "same" if gal else "DIFFERS", len(sp), len(sd), ns,
             "OK" if not bad else "FAIL"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
