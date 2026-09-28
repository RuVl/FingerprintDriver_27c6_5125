#!/bin/sh
# Stage 1 check: openchicago preprocessor vs AlgoChicago.dll, byte by byte.
#   openchicago/tests/run.sh            build, run the DLL oracle (scenarios A and B), compare
#   VERBOSE=1 openchicago/tests/run.sh  one line per frame
# Needs win-driver/AlgoChicago.dll and tools/algo/{frames_raw,bg_raw}.bin, meta_raw.txt.
# Then test_context_fuzz: the cbuf[0..5] context (0x180043c70) against the DLL's internal
# functions on perturbed frames, for branches the dataset does not reach.
# Exit status is non-zero if any call differs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
algo=$root/tools/algo
out=$here/build
mkdir -p "$out"

oracle=$algo/oracle_pp
if [ ! -x "$oracle" ] || [ "$algo/oracle_pp.c" -nt "$oracle" ]; then
  cc -O2 -g -o "$oracle" "$algo/oracle_pp.c" "$algo/winpe.c" "$algo/algolog.c"
fi
# shellcheck disable=SC2046
cc -O2 -g -std=gnu11 -Wall -Wno-unused-function -o "$out/test_preprocess" \
  "$here/test_preprocess.c" "$here/../src/goodix-chicago-preprocess.c" \
  "$here/../src/goodix-chicago-calibration.c" -I"$here/../src" \
  $(pkg-config --cflags --libs glib-2.0 gio-2.0) -lm

cc -O2 -g -std=gnu11 -Wall -Wno-unused-function -o "$out/test_context_fuzz" \
  "$here/test_context_fuzz.c" "$algo/winpe.c" "$here/../src/goodix-chicago-calibration.c" \
  -I"$here/../src" -I"$algo" $(pkg-config --cflags --libs glib-2.0 gio-2.0) -lm

status=0
for s in A B; do
  if [ ! -f "$out/oracle_$s.bin" ] || [ "$oracle" -nt "$out/oracle_$s.bin" ]; then
    (cd "$algo" && ./oracle_pp "$s" "$out/oracle_$s.bin")
  fi
  "$out/test_preprocess" "$s" "$out/oracle_$s.bin" "$algo" || status=1
done
"$out/test_context_fuzz" "$algo" "$root/win-driver/AlgoChicago.dll" || status=1
exit $status
