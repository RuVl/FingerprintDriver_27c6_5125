#!/bin/sh
# Build and run the MR648 ChicagoHS port evaluation (tools/algo/port_eval.c).
#   ./port_eval.sh [enroll_n]   env as in port_eval.c (ENROLL, WARM, WARMSET, DUMP_PROC,
#                               INJECT_PROC, VERBOSE)
#   FIXES=1 ./port_eval.sh ...  same, but with port_preprocess_fixes.patch applied to a copy of
#                               goodix-chicago-preprocess.c (upstream/ itself is never modified)
#   MFIXES=1 ./port_eval.sh ... same, with port_match_fixes.patch applied to a copy of
#                               goodix-chicago-match.c (matcher decision as in AlgoChicago.dll,
#                               docs/stage2-match-decision.md, flag re-evaluation 0x180019800:
#                               docs/stage5-match.md); MTRACE=1 prints the per-record trace
#                               to stderr.  FIXES and MFIXES combine.
#   FFIXES=1 ./port_eval.sh ... same, with port_feature_fixes.patch applied to copies of
#                               goodix-chicago-{feature,runtime}.c (getFeature as in AlgoChicago.dll,
#                               docs/stage3-features.md); combines with FIXES/MFIXES.
#   EFIXES=1 ./port_eval.sh ... same, with port_enroll_fixes.patch applied to a copy of
#                               goodix-chicago-enrollment.c (gallery as in AlgoChicago.dll's
#                               enrolAddImage, docs/stage4-enroll.md); combines with the others.
#   SFIXES=1 EFIXES=1 MFIXES=1 FFIXES=1 ...  same, plus port_study_fixes.patch (templateStudy as in
#                               AlgoChicago.dll, docs/stage7-study.md) applied on top of the patched
#                               goodix-chicago-{match,enrollment,runtime}.c and their headers
#                               (.port-sfixes/, first on the include path); use with STUDY=1.
#   OPENPP=1 FFIXES=1 ...       preprocessor from openchicago/src (stage 1, bit-exact) instead of
#                               the port's (FIXES is then ignored).
# OTP: taken from the newest dumps/probe-*.json (not committed) unless OTP is already set.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
src=$root/upstream/libfprint-mr648/libfprint/drivers/goodix5125
pp=$src/chicago/goodix-chicago-preprocess.c
mt=$src/chicago/goodix-chicago-match.c
ft=$src/chicago/goodix-chicago-feature.c
rt=$src/chicago/goodix-chicago-runtime.c
en=$src/chicago/goodix-chicago-enrollment.c
ffinc=
ffdef=
bin=$here/port_eval
# patched copy of one upstream file: fixed_copy <dir> <file> <patch>
fixed_copy() {
  if [ ! -f "$1/$(basename "$2")" ] || [ "$3" -nt "$1/$(basename "$2")" ]; then
    mkdir -p "$1"
    cp "$2" "$1/"
    patch -s -d "$1" -p1 < "$3"
  fi
}
if [ -n "$FIXES" ]; then
  bin=${bin}_fixes
fi
if [ -n "$MFIXES" ]; then
  bin=${bin}_mfixes
fi
if [ -n "$FFIXES" ]; then
  bin=${bin}_ffixes
fi
if [ -n "$EFIXES" ]; then
  bin=${bin}_efixes
fi
if [ -n "$SFIXES" ]; then
  [ -n "$EFIXES" ] && [ -n "$MFIXES" ] && [ -n "$FFIXES" ] ||
    { echo "SFIXES=1 needs EFIXES=1 MFIXES=1 FFIXES=1" >&2; exit 2; }
  bin=${bin}_sfixes
fi
if [ -n "$OPENPP" ]; then
  [ -n "$FFIXES" ] || { echo "OPENPP=1 needs FFIXES=1" >&2; exit 2; }
  bin=${bin}_openpp
fi
if [ -n "$FIXES" ]; then
  fixed_copy "$here/.port-fixes" "$pp" "$here/port_preprocess_fixes.patch"
  pp=$here/.port-fixes/goodix-chicago-preprocess.c
fi
if [ -n "$MFIXES" ]; then
  fixed_copy "$here/.port-mfixes" "$mt" "$here/port_match_fixes.patch"
  mt=$here/.port-mfixes/goodix-chicago-match.c
fi
if [ -n "$EFIXES" ]; then
  fixed_copy "$here/.port-efixes" "$en" "$here/port_enroll_fixes.patch"
  en=$here/.port-efixes/goodix-chicago-enrollment.c
fi
if [ -n "$FFIXES" ]; then
  ffd=$here/.port-ffixes
  if [ ! -f "$ffd/goodix-chicago-feature.c" ] || [ "$here/port_feature_fixes.patch" -nt "$ffd/goodix-chicago-feature.c" ]; then
    rm -rf "$ffd" && mkdir -p "$ffd"
    cp "$ft" "$rt" "$ffd/"
    patch -s -d "$ffd" -p1 < "$here/port_feature_fixes.patch"
  fi
  ft=$ffd/goodix-chicago-feature.c
  rt=$ffd/goodix-chicago-runtime.c
  ffinc=-I$ffd
  ffdef="-DPORT_FFIXES -Wl,--wrap=goodix_chicago_preprocessor_finalize_metrics -Wl,--wrap=goodix_chicago_feature_preprocessor_context"
fi
if [ -n "$SFIXES" ]; then
  sfd=$here/.port-sfixes
  if [ ! -f "$sfd/.stamp" ] || [ "$here/port_study_fixes.patch" -nt "$sfd/.stamp" ] ||
     [ "$mt" -nt "$sfd/.stamp" ] || [ "$en" -nt "$sfd/.stamp" ] || [ "$rt" -nt "$sfd/.stamp" ]; then
    rm -rf "$sfd" && mkdir -p "$sfd"
    cp "$mt" "$en" "$rt" "$src"/chicago/goodix-chicago-match.h "$src"/chicago/goodix-chicago-enrollment.h \
      "$src"/chicago/goodix-chicago-runtime.h "$sfd/"
    patch -s -d "$sfd" -p1 < "$here/port_study_fixes.patch"
    touch "$sfd/.stamp"
  fi
  mt=$sfd/goodix-chicago-match.c
  en=$sfd/goodix-chicago-enrollment.c
  rt=$sfd/goodix-chicago-runtime.c
  ffinc="-I$sfd $ffinc"
  ffdef="$ffdef -DPORT_SFIXES"
fi
cal=$src/chicago/goodix-chicago-calibration.c
chi=$src/chicago
if [ -n "$OPENPP" ]; then
  # one directory with every chicago source, so no translation unit sees both preprocess.h
  opd=$here/.port-openpp
  rm -rf "$opd" && mkdir -p "$opd"
  cp "$src"/chicago/*.[ch] "$opd/"
  cp "$root"/openchicago/src/goodix-chicago-preprocess.[ch] "$root"/openchicago/src/goodix-chicago-calibration.[ch] "$opd/"
  cp "$ffd"/* "$mt" "$en" "$opd/"
  [ -z "$SFIXES" ] || cp "$sfd"/*.[ch] "$opd/"
  chi=$opd
  pp=$opd/goodix-chicago-preprocess.c
  cal=$opd/goodix-chicago-calibration.c
  mt=$opd/goodix-chicago-match.c
  ft=$opd/goodix-chicago-feature.c
  rt=$opd/goodix-chicago-runtime.c
  en=$opd/goodix-chicago-enrollment.c
  ffinc=-I$opd
  ffdef="$ffdef -DPORT_OPENPP"
fi
if [ ! -x "$bin" ] || [ -n "$REBUILD" ] || [ "$here/port_eval.c" -nt "$bin" ] ||
   [ "$pp" -nt "$bin" ] || [ "$mt" -nt "$bin" ] || [ "$ft" -nt "$bin" ] || [ "$en" -nt "$bin" ] || [ "$here/port_eval.sh" -nt "$bin" ]; then
  others=$(ls "$chi"/*.c | grep -v '/goodix-chicago-\(preprocess\|match\|feature\|runtime\|calibration\|enrollment\)\.c$')
  # shellcheck disable=SC2086
  cc -O2 -g -std=gnu11 -w -o "$bin" "$here/port_eval.c" "$pp" "$cal" "$mt" "$ft" "$rt" "$en" $others \
    "$src"/common/goodix-crc.c "$src"/goodix5125-calib.c "$src"/goodix5125-proto.c \
    $ffinc -I"$src/chicago" -I"$src/common" -I"$src" $ffdef \
    -Wl,--wrap=goodix_chicago_preprocessor_build_enhanced_checked \
    $(pkg-config --cflags --libs glib-2.0 gio-2.0) -lm
fi
if [ -z "$OTP" ]; then
  j=$(ls -t "$root"/dumps/probe-*.json 2>/dev/null | head -1)
  [ -n "$j" ] && OTP=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('otp',''))" "$j")
  export OTP
fi
cd "$here" && exec "$bin" "$@"
