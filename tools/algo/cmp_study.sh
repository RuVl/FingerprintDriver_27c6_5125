#!/bin/sh
# Stage 7 (docs/stage7-study.md): templateStudy of the port against AlgoChicago.dll.
#   ./cmp_study.sh <out_prefix> [enroll_n=12]     (env WARM/WARMSET as in algo_eval4)
# Runs the DLL oracle (oracle_feat = algo_eval4 protocol, STUDY=1) and the port
# (SFIXES+EFIXES+MFIXES+FFIXES, INJECT_PROC = the DLL's preprocessor outputs, STUDY=1),
# both writing STRACE (one line per templateStudy call) and STUDY_DIR (blob after it);
# then compares every study step (record, score, update, blob via cmp_tpl.py) and the
# per-probe decisions / score / selected subtemplate (cmp_match.py --score, gdb trace).
# Exit 0 only if everything is identical.  NEGATIVE=1 corrupts one port blob (must fail).
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
out=${1:?usage: cmp_study.sh <out_prefix> [enroll_n]}
n=${2:-12}
rm -rf "$out.dll" "$out.port"
mkdir -p "$out.dll" "$out.port"
STUDY=1 STRACE=$out.dll.st STUDY_DIR=$out.dll ./oracle_feat.sh "$out.d" "$n" > /dev/null 2>&1
ALGO=chicago VERBOSE=1 STUDY=1 TRACE_OUT=$out.trace \
  gdb --batch -x dll_match_trace.py --args ./algo_eval4 "$n" > /dev/null 2>&1
STUDY=1 STRACE=$out.port.st STUDY_DIR=$out.port SFIXES=1 EFIXES=1 MFIXES=1 FFIXES=1 \
  INJECT_PROC=$out.d.proc VERBOSE=1 ./port_eval.sh "$n" > "$out.p.txt"
set +e
if [ -n "$NEGATIVE" ]; then   # NEGATIVE=1: flip one bit of the port's first study blob
  f=$(ls "$out.port"/*.tpl | head -1)
  python3 -c "import sys; b=bytearray(open(sys.argv[1],'rb').read()); b[len(b)//2]^=1; open(sys.argv[1],'wb').write(b)" "$f"
fi
bad=0
nd=$(wc -l < "$out.dll.st"); np=$(wc -l < "$out.port.st")
[ "$nd" = "$np" ] || { echo "study calls: dll $nd port $np"; bad=1; }
# fields: study rec R score S upd U rc C len L fnv F
paste -d' ' "$out.dll.st" "$out.port.st" | while read -r _ _ r _ s _ u _ _ _ _ _ _ _ _ r2 _ s2 _ u2 _ _ _ _ _ _; do
  d=0; [ "$u" -gt 0 ] && d=1
  p=0; [ "$u2" -gt 0 ] && p=1
  if [ "$r" != "$r2" ] || [ "$s" != "$s2" ] || [ "$d" != "$p" ]; then
    echo "study rec $r: dll score $s upd $u, port rec $r2 score $s2 upd $u2"; echo x
  elif ! ./cmp_tpl.py "$out.dll/s$(printf %03d "$r").tpl" "$out.port/s$(printf %03d "$r").tpl" > /dev/null; then
    echo "study rec $r: blob differs (./cmp_tpl.py $out.dll/s$(printf %03d "$r").tpl $out.port/...)"; echo x
  fi
done > "$out.cmp"
grep -v '^x$' "$out.cmp"
grep -q '^x$' "$out.cmp" && bad=1
echo "study steps: $nd, differing: $(grep -c '^x$' "$out.cmp")"
./cmp_match.py "$out.d.txt" "$out.p.txt" | tail -1
./cmp_match.py "$out.d.txt" "$out.p.txt" --score "$out.trace" | tail -1
./cmp_match.py "$out.d.txt" "$out.p.txt" --score "$out.trace" > /dev/null || bad=1
exit $bad
