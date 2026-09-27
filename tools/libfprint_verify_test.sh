#!/usr/bin/env bash
# Verify accuracy test of the goodixtls5125 driver from the local build:
# 10 attempts against an existing enrollment (5 genuine, 5 impostor).
# Usage: libfprint_verify_test.sh [DIR_WITH_test-storage.variant]
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/libfprint/build/examples
SRC=${1:-$(ls -d "$ROOT"/dumps/libfprint-test/2*/ | tail -1)}
OUT=$ROOT/dumps/libfprint-test/verify-$(date +%Y%m%d-%H%M%S)
export LD_LIBRARY_PATH=$ROOT/deps/root/usr/lib
mkdir -p "$OUT" && cp "$SRC/test-storage.variant" "$OUT/" && cd "$OUT" || exit 1

echo "Отпечаток из: $SRC"
echo "Лог: $OUT/verify.log"

# 6 = right index; then "y" for attempts 2..10 and "n" at the end
G_MESSAGES_DEBUG=all stdbuf -oL "$BUILD/verify" \
  < <(printf '6\ny\ny\ny\ny\ny\ny\ny\ny\ny\nn\n') > verify.log 2>&1 &
PID=$!

tail -n +1 -f verify.log --pid=$PID | awk '
  /Time to verify/ {
    n++
    who = (n <= 5) ? "УКАЗАТЕЛЬНЫЙ ПРАВОЙ (зарегистрированный)" : "ДРУГОЙ палец (например, большой)"
    printf "\n>>> Попытка %d из 10: приложите %s\n", n, who; fflush()
  }
  /sigfm score [1-9]/ { sub(/.*sigfm score /, "    оценка "); print; fflush() }
  /^MATCH!|^NO MATCH!|Finger not matched, retry|rror/ { print "    " $0; fflush() }
'
wait $PID
echo
echo "ИТОГ:"
awk '/Time to verify/{n++} /^MATCH!/{print "  попытка " n ": MATCH"} /^NO MATCH!/{print "  попытка " n ": NO MATCH"} /retry error/{print "  попытка " n ": ПОВТОР (плохой снимок)"}' verify.log
