#!/usr/bin/env bash
# Запись umockdev-теста tests/goodix5125/ (custom.py + custom.pcapng + device)
# для ветки goodix5125-mr, по образцу других драйверов libfprint
# (tests/README.md, tests/create-driver-test.py).
#
# Запускает ПОЛЬЗОВАТЕЛЬ, пальцем, которым НЕ пользуется для входа
# (например, левый мизинец), из корня репо:
#   tools/record_umockdev_5125.sh check    # 1. проверить зависимости
#   tools/record_umockdev_5125.sh build    # 2. собрать libfprint с introspection
#   tools/record_umockdev_5125.sh try      #    (необяз.) тот же сценарий без записи
#   tools/record_umockdev_5125.sh record   # 3. записать (sudo, нужны касания)
#   tools/record_umockdev_5125.sh replay   # 4. попробовать воспроизвести запись
# Без аргумента выполняются все шаги по очереди.
#
# Что меняется в системе (записать в SYSTEM_CHANGES.local.md):
#   - пакеты: sudo pacman -S --needed umockdev wireshark-cli gobject-introspection
#     (откат: sudo pacman -Rs umockdev wireshark-cli gobject-introspection)
#   - sudo modprobe usbmon          (откат: sudo modprobe -r usbmon)
#   - sudo systemctl stop fprintd   (откат: sudo systemctl start fprintd)
#   - create-driver-test.py делает USB-сброс порта сенсора (обычный USB reset,
#     не команда MCU; прошивка и flash не трогаются).
# В сенсор ничего не пишется: в режиме эмуляции (FP_DEVICE_EMULATION=1)
# драйвер берёт нулевой PSK, никогда не записывает ключ, игнорирует
# GOODIX5125_* и держит состояние во временном каталоге (удаляется при
# закрытии), /var/lib/fprint/goodix5125 не трогается. TLS в этом режиме
# детерминирован, поэтому запись воспроизводится побайтно.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$ROOT/upstream/libfprint-mr648}
BUILD=${BUILD:-$SRC/build-umockdev}
DEPS=$ROOT/deps/root/usr
TEST_DIR=$SRC/tests/goodix5125
PY=/usr/bin/python3          # системный python (нужен python-gobject), не .venv

# g-ir-scanner и meson-скрипты зовут `env python3`: активированный .venv без
# distutils/gi ломает сборку introspection -- убираем его из PATH
if [ -n "${VIRTUAL_ENV:-}" ]; then
  PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -vxF "$VIRTUAL_ENV/bin" | paste -sd:)
  export PATH
  unset VIRTUAL_ENV
fi

banner() {
  printf '\n============================================================\n  %s\n============================================================\n' "$1"
}
die() { printf 'ОШИБКА: %s\n' "$1" >&2; exit 1; }

export LD_LIBRARY_PATH="$BUILD/libfprint/:$DEPS/lib"
export GI_TYPELIB_PATH="$BUILD/libfprint/:$DEPS/lib/girepository-1.0"
export GI_GIR_PATH="$DEPS/share/gir-1.0"

step_check() {
  banner "1. Проверка зависимостей"
  [ -d "/lib/modules/$(uname -r)" ] ||
    die "нет модулей для работающего ядра $(uname -r) (ядро обновлено) -- перезагрузитесь, потом повторите"
  local missing=()
  command -v umockdev-record >/dev/null || missing+=(umockdev)
  command -v umockdev-run    >/dev/null || missing+=(umockdev)
  command -v tshark          >/dev/null || missing+=(wireshark-cli)
  command -v g-ir-scanner    >/dev/null || missing+=(gobject-introspection)
  "$PY" -c 'import gi' 2>/dev/null     || missing+=(python-gobject)
  if ((${#missing[@]})); then
    printf 'Не хватает пакетов: %s\n' "$(printf '%s\n' "${missing[@]}" | sort -u | tr '\n' ' ')"
    printf 'Установить (и записать в SYSTEM_CHANGES.local.md):\n  sudo pacman -S --needed %s\n' \
      "$(printf '%s\n' "${missing[@]}" | sort -u | tr '\n' ' ')"
    exit 1
  fi
  [[ $(git -C "$SRC" branch --show-current) == goodix5125-mr ]] ||
    die "в $SRC должна быть выбрана ветка goodix5125-mr"
  check_custom_py
  echo "OK: umockdev $(umockdev-run --version), $(tshark --version | head -1)"
}

step_build() {
  banner "2. Сборка libfprint (goodix5125 + introspection) в $BUILD"
  if [[ ! -f $BUILD/build.ninja ]]; then
    PKG_CONFIG_PATH=$DEPS/lib/pkgconfig meson setup "$BUILD" "$SRC" \
      -Ddrivers=goodix5125 -Dintrospection=true -Ddoc=false \
      -Dgtk-examples=false -Dudev_rules=disabled -Dudev_hwdb=disabled
  fi
  ninja -C "$BUILD"
  [[ -f $BUILD/libfprint/FPrint-2.0.typelib ]] || die "нет FPrint-2.0.typelib"
  "$PY" -c 'import gi; gi.require_version("FPrint", "2.0"); from gi.repository import FPrint' ||
    die "python не находит FPrint (GI_TYPELIB_PATH=$GI_TYPELIB_PATH)"
}

check_custom_py() {
  # Сценарий лежит в ветке goodix5125-mr (коммит "tests: Add goodix5125 unit tests")
  [[ -f $TEST_DIR/custom.py ]] ||
    die "нет $TEST_DIR/custom.py (ветка goodix5125-mr устарела?)"
}

step_record() {
  banner "3. Запись теста"
  check_custom_py
  [[ -x $BUILD/tests/create-driver-test.py ]] || die "сначала: $0 build"

  cat <<EOF

НУЖНО ВАШЕ УЧАСТИЕ
  Используйте ОДИН палец, которым вы НЕ входите в систему (например, левый
  мизинец): запись содержит его изображения (зашифрованы нулевым PSK, то есть
  фактически открыты) и попадёт в MR.
  1. Сейчас sudo спросит пароль (или палец, если настроен pam_fprintd, — тогда
     лучше дождаться таймаута и ввести пароль). Дальше sudo без вопросов:
     usbmon, остановка fprintd, create-driver-test.py.
  2. "identifying against an empty gallery" — ничего не делать (без касания).
  3. "enrolling, touch the sensor 12 times" — прикладывайте выбранный палец
     и каждый раз убирайте, чуть меняя положение, пока не будет
     "enroll done" (12 засчитанных касаний; строки "enroll progress" с
     ошибкой — повтор, они не засчитываются).
  4. "verifying" — касание тем же пальцем, центром подушечки, как при регистрации;
     при "verify done: no match" будет ещё попытка (всего до 5).
  5. В конце должно быть "Saving USB capture as test case goodix5125" и
     "Done!". Если verify не совпал (AssertionError) — запустить record ещё раз.
EOF
  read -r -p "Продолжить? [y/N] " answer
  [[ $answer == [yY] ]] || exit 0

  sudo -v
  if [[ ! -e /dev/usbmon0 ]]; then
    echo "+ sudo modprobe usbmon"
    sudo modprobe usbmon
  fi
  if systemctl is-active --quiet fprintd; then
    echo "+ sudo systemctl stop fprintd"
    sudo systemctl stop fprintd
  fi

  mkdir -p "$ROOT/dumps"
  # create-driver-test.py relaunches itself unless LD_LIBRARY_PATH already
  # contains $BUILD/libfprint/; keep ours so that the local libgusb is found.
  sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" GI_TYPELIB_PATH="$GI_TYPELIB_PATH" \
    FP_DEVICE_EMULATION=1 G_MESSAGES_DEBUG=all \
    "$PY" "$BUILD/tests/create-driver-test.py" --test custom goodix5125 \
    2>&1 | tee "$ROOT/dumps/umockdev-record-$(date +%Y%m%d-%H%M%S).log"

  sudo chown -R "$(id -u):$(id -g)" "$TEST_DIR"
  ls -l "$TEST_DIR"
  cat <<EOF

Готово. Дальше:
  - $0 replay  — проверить воспроизведение;
  - fprintd снова запустить: sudo systemctl start fprintd;
  - проверить $TEST_DIR/device (серийный номер и т.п. из sysfs);
  - дальше агент добавит 'goodix5125' в drivers_tests (tests/meson.build)
    и закоммитит device + custom.pcapng в коммит тестов.
EOF
}

step_try() {
  banner "Пробный прогон сценария на устройстве, без записи"
  check_custom_py
  cat <<EOF

НУЖНО ВАШЕ УЧАСТИЕ
  Тот же сценарий, что при записи, но USB не записывается и ничего не
  сохраняется: состояние драйвера во временном каталоге (удаляется при
  закрытии), в лог попадают только оценки. Можно своим рабочим пальцем.
  Касания — как при записи (регистрация 12, потом до 5 проверок).
EOF
  read -r -p "Продолжить? [y/N] " answer
  [[ $answer == [yY] ]] || exit 0

  sudo -v
  if systemctl is-active --quiet fprintd; then
    echo "+ sudo systemctl stop fprintd"
    sudo systemctl stop fprintd
  fi
  mkdir -p "$ROOT/dumps"
  local log
  log="$ROOT/dumps/umockdev-try-$(date +%Y%m%d-%H%M%S).log"
  sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" GI_TYPELIB_PATH="$GI_TYPELIB_PATH" \
    FP_DEVICE_EMULATION=1 FP_DRIVERS_ALLOWLIST=goodix5125 G_MESSAGES_DEBUG=all \
    "$PY" -u "$TEST_DIR/custom.py" > "$log" 2>&1 &
  local pid=$!
  # показывать только подсказки сценария и итоги кадров
  tail -f "$log" --pid=$pid | grep --line-buffered -E \
    '^(identifying|enrolling|enroll |verif)|enroll frame:|match: score|Error|assert'
  wait $pid && echo "TRY OK" || echo "TRY FAIL (лог: $log)"
}

step_replay() {
  banner "4. Воспроизведение записи (без устройства)"
  [[ -f $TEST_DIR/custom.pcapng ]] || die "нет $TEST_DIR/custom.pcapng: сначала $0 record"
  env FP_DEVICE_EMULATION=1 FP_DRIVERS_ALLOWLIST=goodix5125 G_MESSAGES_DEBUG=all \
    G_DEBUG=fatal-warnings "$PY" "$SRC/tests/umockdev-test.py" --test custom "$TEST_DIR" \
    && echo "REPLAY OK: можно добавить 'goodix5125': { 'timeout': 120 } в drivers_tests (tests/meson.build)" \
    || cat <<EOF
REPLAY FAIL. Смотреть вывод umockdev: "buffer mismatch" значит, что хост
отправил не те байты, что в записи (недетерминизм в драйвере, другая версия
OpenSSL/кода, чем при записи), "Replay may be stuck" — расхождение порядка
команд (например, другое решение алгоритма при регистрации).
EOF
}

case "${1:-all}" in
  check)  step_check ;;
  build)  step_check; step_build ;;
  record) step_record ;;
  try)    step_try ;;
  replay) step_replay ;;
  all)    step_check; step_build; step_record; step_replay ;;
  *)      die "неизвестный шаг: $1 (check|build|try|record|replay)" ;;
esac
