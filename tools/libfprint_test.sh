#!/usr/bin/env bash
# Stage 5 hardware test of the goodixtls5125 libfprint driver from the local
# build (no system install): capture, enroll, verify.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/libfprint/build/examples
OUT=$ROOT/dumps/libfprint-test/$(date +%Y%m%d-%H%M%S)
export LD_LIBRARY_PATH=$ROOT/deps/root/usr/lib
mkdir -p "$OUT" && cd "$OUT" || exit 1

banner() {
  printf '\n%s\n  %s\n%s\n' "============================================================" \
    "$1" "============================================================"
}

run() { # name, stdin answers, program
  G_MESSAGES_DEBUG=all stdbuf -oL "$BUILD/$3" < <(printf '%b' "$2") \
    2> "$1.debug.log" | tee "$1.log" | grep --line-buffered -v "^\s*\[[0-9]\] "
}

banner "1/3 СНИМОК: приложите палец ОДИН раз, когда появится 'finger'"
run capture "" img-capture

banner "2/3 РЕГИСТРАЦИЯ: прикладывайте ОДИН И ТОТ ЖЕ палец (указательный правой руки), \
каждый раз убирая его, пока не будет 'Enroll stage 15 of 15'"
run enroll "6\nn\n" enroll

banner "3/3 ПРОВЕРКА, 5 раз: касания 1-3 — ТЕМ ЖЕ пальцем, касания 4-5 — ДРУГИМ пальцем"
run verify "6\ny\ny\ny\ny\nn\n" verify

banner "ГОТОВО. Результаты в $OUT"
grep -h "MATCH\|Enroll stage\|Enroll complete\|rror" "$OUT"/*.log | grep -v debug
