#!/usr/bin/env bash
# Collect more data: 30 natural genuine touches, 100 impostor touches.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG=$ROOT/dumps/dataset/collect-$(date +%Y%m%d-%H%M%S).log
mkdir -p "$ROOT/dumps/dataset"
cd "$ROOT/tools" || exit 1

run() { # label, count, prompt
  uv run --project "$ROOT" python -u collect.py "$1" "$2" "$3" 2>&1 | tee -a "$LOG" |
    grep --line-buffered -E "^={10}|^  \[|УБЕРИТЕ|ГОТОВО|Error|error" |
    grep --line-buffered -v "^={10}"
}

echo "Подробный лог: $LOG"
run natural 30 "УКАЗАТЕЛЬНЫЙ ПРАВОЙ: прикладывайте ЕСТЕСТВЕННО, как для разблокировки"
run impostor 100 "ЧУЖОЙ палец: любые пальцы, кроме указательного правой, чередуйте (можно и пальцы других людей)"
echo
echo "Всё. Напишите в чат, что сбор закончен."
