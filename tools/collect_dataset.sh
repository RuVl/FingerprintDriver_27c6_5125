#!/usr/bin/env bash
# Collect the tuning dataset: 25 touches of the right index finger, then 10
# of other fingers. Only the prompts are shown; details go to the log.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG=$ROOT/dumps/dataset/collect-$(date +%Y%m%d-%H%M%S).log
mkdir -p "$ROOT/dumps/dataset"
cd "$ROOT/tools" || exit 1

run() { # label, count, prompt
  "$ROOT/.venv/bin/python" -u collect.py "$1" "$2" "$3" 2>&1 | tee -a "$LOG" |
    grep --line-buffered -E "^={10}|^  \[|УБЕРИТЕ|ГОТОВО|Error|error" |
    grep --line-buffered -v "^={10}"
}

echo "Подробный лог: $LOG"
run genuine 25 "УКАЗАТЕЛЬНЫЙ ПРАВОЙ: приложите, каждый раз чуть иначе (центр, выше, ниже, левее, правее, под углом)"
run impostor 10 "ДРУГОЙ палец: каждый раз любой, кроме указательного правой"
echo
echo "Всё. Напишите в чат, что сбор закончен."
