#!/bin/sh
# Stage 5: build and run reeval_difftest (docs/stage5-match.md) against the MFIXES match.c.
#   ./reeval_difftest.sh [n=1000000] [dll_reeval_trace.py log with real records]
#   MATCH_C=<file> overrides the patched goodix-chicago-match.c (e.g. a deliberately broken copy).
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
MFIXES=1 FFIXES=1 ./port_eval.sh 1 > /dev/null 2>&1 || true   # refreshes .port-mfixes/
mc=${MATCH_C:-$here/.port-mfixes/goodix-chicago-match.c}
up=$here/../../upstream/libfprint-mr648/libfprint/drivers/goodix5125/chicago/goodix-chicago-match.c
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
{
  awk '/^goodix_chicago_match_scheduler_evidence_type24 \($/{p=1; print "static void"} p{print} p&&/^}$/{exit}' "$up"
  awk '/MFIXES stage 5 .docs.stage5-match.md.: flag re-evaluation/{p=1} p{print} /END reeval/{exit}' "$mc"
} > "$tmp/reeval.c"
cc -O1 -w -DTREES_C="\"$tmp/reeval.c\"" -o "$tmp/reeval_difftest" reeval_difftest.c winpe.c algolog.c
"$tmp/reeval_difftest" "${1:-1000000}" ${2:+"$2"}
