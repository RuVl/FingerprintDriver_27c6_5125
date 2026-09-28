#!/bin/sh
# Stage-3 feature oracle (docs/stage3-features.md): build oracle_feat and dump AlgoChicago.dll's
# getFeature output for the algo_eval4 protocol, plus the preprocessor outputs for INJECT_PROC.
#   ./oracle_feat.sh <out_prefix> [enroll_n=12]      (env WARM/WARMSET as in algo_eval4)
# writes <out_prefix>.feat (FEAT_OUT), <out_prefix>.proc (+ .proc.cbuf, DUMP_PROC), <out_prefix>.txt
# Then:  FEAT_DUMP=/tmp/p.feat MFIXES=1 FFIXES=1 INJECT_PROC=<out_prefix>.proc VERBOSE=1 ./port_eval.sh 12
#        ./cmp_feat.py <out_prefix>.feat /tmp/p.feat ;  ./cmp_match.py <out_prefix>.txt <port.txt>
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
if [ ! -x oracle_feat ] || [ oracle_feat.c -nt oracle_feat ] || [ algo_eval4.c -nt oracle_feat ]; then
  cc -O2 -g -o oracle_feat oracle_feat.c winpe.c algolog.c
fi
out=${1:?usage: oracle_feat.sh <out_prefix> [enroll_n]}
FEAT_OUT=$out.feat DUMP_PROC=$out.proc VERBOSE=1 ./oracle_feat "${2:-12}" > "$out.txt"
