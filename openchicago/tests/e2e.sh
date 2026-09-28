#!/bin/sh
# End-to-end check of the openchicago library against AlgoChicago.dll (docs/stage-lib.md).
#   openchicago/tests/e2e.sh [path/to/test_e2e]      (default: openchicago/build/tests/test_e2e)
# DLL side (cached in tests/build/e2e): tools/algo/oracle_feat.sh with STUDY=1 (decisions,
# study blobs, gallery TPL_OUT) and dll_match_trace.py under gdb (selected subtemplate).
# openchicago side: test_e2e (public API only, no DLL).  Scenarios:
#   n12   N=12                              w12   N=12, WARM=1 WARMSET=impostor
#   n12r  N=12, session saved/restored after frames 20,60,100,140 (RESTORE_AT)
#   w12r  WARM, restored after frames 1,50,100,130,200
#   neg   copies of the n12 run with one bit of one study blob, or one score, changed:
#         the comparison must fail
#   enrolment results (OVTRACE: frame count, overlay, preoverlay after every enrolAddImage,
#   and the final template), no study:  ov20 N=20;  ov30w N=30 WARM;  del* N=20 with
#   enrolDeleteImage after the listed records (EDEL, the backup path of OC_ENROLL_ENGINE)
# Exit status non-zero if any scenario differs (or the negative check does not).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
algo=$root/tools/algo
bin=${1:-$root/openchicago/build/tests/test_e2e}
out=$here/build/e2e
mkdir -p "$out"
[ -f "$algo/frames_raw.bin" ] && [ -f "$root/win-driver/AlgoChicago.dll" ] ||
  { echo "e2e: needs tools/algo/frames_raw.bin and win-driver/AlgoChicago.dll"; exit 77; }

reference() {   # reference <name> <env...>
  name=$1; shift
  r=$out/$name
  if [ ! -f "$r.done" ] || [ "$algo/oracle_feat.c" -nt "$r.done" ] || [ "$algo/algo_eval4.c" -nt "$r.done" ]; then
    rm -rf "$r.dll" && mkdir -p "$r.dll"
    (cd "$algo" && env "$@" STUDY=1 STRACE="$r.st" STUDY_DIR="$r.dll" TPL_OUT="$r.tpl" \
       ./oracle_feat.sh "$r.d" 12 > /dev/null 2>&1)
    (cd "$algo" && env "$@" ALGO=chicago VERBOSE=1 STUDY=1 TRACE_OUT="$r.trace" \
       gdb --batch -x dll_match_trace.py --args ./algo_eval4 12 > /dev/null 2>&1)
    touch "$r.done"
  fi
}

run() {   # run <scenario> <reference> <env...>
  sc=$1 ref=$2; shift 2
  p=$out/$sc.oc
  rm -rf "$p" && mkdir -p "$p"
  env "$@" STUDY=1 STRACE="$p.st" STUDY_DIR="$p" TPL_OUT="$p.tpl" "$bin" "$algo" 12 > "$p.txt"
  printf '%-5s ' "$sc"
  "$here/e2e_cmp.py" "$out/$ref" "$p" | tail -1
  "$here/e2e_cmp.py" "$out/$ref" "$p" > /dev/null
}

reference n12 X=
reference w12 WARM=1 WARMSET=impostor
ovrun() {   # ovrun <name> <n> <env...>: enrolment trace + template, DLL vs openchicago
  name=$1 n=$2; shift 2
  r=$out/$name
  if [ ! -f "$r.done" ] || [ "$algo/oracle_feat.c" -nt "$r.done" ] || [ "$algo/algo_eval4.c" -nt "$r.done" ]; then
    (cd "$algo" && env "$@" SESSCAP=50 OVTRACE="$r.ov" TPL_OUT="$r.tpl" ./oracle_feat.sh "$r.d" "$n" > /dev/null 2>&1)
    touch "$r.done"
  fi
  env "$@" OVTRACE="$r.oc.ov" TPL_OUT="$r.oc.tpl" "$bin" "$algo" "$n" > "$r.oc.txt"
  # DLL rc 0x83 lines: frames without features, which openchicago rejects before enrolAddImage
  grep -v ' rc 0x83 ' "$r.ov" | cut -d' ' -f1-3,7,11,13 > "$r.ov.cmp"
  cut -d' ' -f1-3,7,11,13 "$r.oc.ov" > "$r.oc.ov.cmp"
  d=$(diff "$r.ov.cmp" "$r.oc.ov.cmp" | grep -c '^[<>]')
  if cmp -s "$r.tpl" "$r.oc.tpl"; then t=same; else t=DIFFERS; fi
  printf '%-5s enrolment steps %d (dll %d): count/overlay/preoverlay differ %d; template %s -> %s\n' \
    "$name" "$(wc -l < "$r.oc.ov.cmp")" "$(wc -l < "$r.ov.cmp")" "$d" "$t" \
    "$( [ "$d" = 0 ] && [ $t = same ] && echo OK || echo FAIL)"
  [ "$d" = 0 ] && [ $t = same ]
}

status=0
run n12 n12 X= || status=1
run w12 w12 WARM=1 WARMSET=impostor || status=1
run n12r n12 RESTORE_AT=20,60,100,140 || status=1
run w12r w12 WARM=1 WARMSET=impostor RESTORE_AT=1,50,100,130,200 || status=1
ovrun ov20 20 X= || status=1
ovrun ov30w 30 WARM=1 WARMSET=impostor || status=1
ovrun del1 20 EDEL=25,26,38,39,40 || status=1
ovrun del2 20 EDEL=28,34,35,42,43,44 || status=1
ovrun del3 20 EDEL=36,37,38,39 WARM=1 WARMSET=impostor || status=1
for what in blob score; do
  n=$out/neg-$what.oc
  rm -rf "$n" && cp -r "$out/n12.oc" "$n" && cp "$out/n12.oc.st" "$n.st" && cp "$out/n12.oc.tpl" "$n.tpl"
  if [ $what = blob ]; then
    f=$(ls "$n"/*.tpl | head -1)
    python3 -c "import sys; b=bytearray(open(sys.argv[1],'rb').read()); b[len(b)//2]^=1; open(sys.argv[1],'wb').write(b)" "$f"
    cp "$out/n12.oc.txt" "$n.txt"
  else
    sed '0,/score=  *\([0-9]\)/s//score=9\1/' "$out/n12.oc.txt" > "$n.txt"
  fi
  if "$here/e2e_cmp.py" "$out/n12" "$n" > "$n.cmp"; then
    echo "neg   $what change NOT detected"; status=1
  else
    echo "neg   $what change detected (expected): $(head -1 "$n.cmp")"
  fi
done
exit $status
