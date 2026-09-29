#!/usr/bin/env bash
# Запись umockdev-теста tests/goodix5125/ (custom.py + custom.pcapng + device)
# для ветки goodix5125-mr, по образцу других драйверов libfprint
# (tests/README.md, tests/create-driver-test.py).
#
# Запускает ПОЛЬЗОВАТЕЛЬ, с пальцем (или другим участком кожи), из корня репо:
#   tools/record_umockdev_5125.sh check    # 1. проверить зависимости
#   tools/record_umockdev_5125.sh build    # 2. собрать libfprint с introspection
#   tools/record_umockdev_5125.sh record   # 3. записать (sudo, нужны касания)
#   tools/record_umockdev_5125.sh replay   # 4. попробовать воспроизвести запись
# Без аргумента выполняются все шаги по очереди.
#
# Что меняется в системе (записать в SYSTEM_CHANGES.local.md):
#   - sudo modprobe usbmon          (откат: sudo modprobe -r usbmon)
#   - sudo systemctl stop fprintd   (откат: sudo systemctl start fprintd)
#   - create-driver-test.py делает USB-сброс порта сенсора (обычный USB reset,
#     не команда MCU; прошивка и flash не трогаются).
# В сенсор ничего не пишется: PSK не записывается (GOODIX5125_PROVISION_PSK
# не задаётся), используется уже записанный нулевой PSK.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$ROOT/upstream/libfprint-mr648}
BUILD=${BUILD:-$SRC/build-umockdev}
DEPS=$ROOT/deps/root/usr
TEST_DIR=$SRC/tests/goodix5125
PY=/usr/bin/python3          # системный python (нужен python-gobject), не .venv

banner() {
  printf '\n============================================================\n  %s\n============================================================\n' "$1"
}
die() { printf 'ОШИБКА: %s\n' "$1" >&2; exit 1; }

export LD_LIBRARY_PATH="$BUILD/libfprint/:$DEPS/lib"
export GI_TYPELIB_PATH="$BUILD/libfprint/:$DEPS/lib/girepository-1.0"
export GI_GIR_PATH="$DEPS/share/gir-1.0"

step_check() {
  banner "1. Проверка зависимостей"
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

write_custom_py() {
  mkdir -p "$TEST_DIR"
  [[ -f $TEST_DIR/custom.py ]] && return
  cat > "$TEST_DIR/custom.py" <<'EOF'
#!/usr/bin/env python3

import os
import sys
import tempfile
import traceback

import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, included those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

# The driver keeps its PSK and adaptive state in a state directory; use a
# fresh one with the all-zero PSK the recorded sensor was paired with.
state_dir = tempfile.mkdtemp(prefix='goodix5125-test-')
psk_path = os.path.join(state_dir, 'psk')
with open(os.open(psk_path, os.O_WRONLY | os.O_CREAT, 0o600), 'w') as psk:
    psk.write('0' * 64)
os.environ['GOODIX5125_STATE_DIR'] = state_dir
os.environ['GOODIX5125_ENROLL_PAIR'] = '0'

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

assert d.get_driver() == 'goodix5125'
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)

d.open_sync()

template = FPrint.Print.new(d)


def enroll_progress(*args):
    print('enroll progress: ' + str(args))


print('enrolling')
p = d.enroll_sync(template, None, enroll_progress, None)
print('enroll done')

print('verifying')
verify_res, verify_print = d.verify_sync(p)
print('verify done')
assert verify_res

print('identifying')
identify_match, identify_print = d.identify_sync([p])
print('identify done')
assert identify_match.equal(p)

d.close_sync()

del d
del c
EOF
  chmod +x "$TEST_DIR/custom.py"
  echo "Создан $TEST_DIR/custom.py"
}

step_record() {
  banner "3. Запись теста"
  write_custom_py
  [[ -x $BUILD/tests/create-driver-test.py ]] || die "сначала: $0 build"

  cat <<EOF

НУЖНО ВАШЕ УЧАСТИЕ
  1. Запись пойдёт через sudo: usbmon, остановка fprintd, create-driver-test.py.
  2. Когда появится "enrolling" — прикладывайте ОДИН И ТОТ ЖЕ участок кожи
     и каждый раз убирайте, пока не будет "enroll done" (12 засчитанных касаний,
     повторы при плохом касании не считаются).
  3. "verifying" — одно касание тем же участком; "identifying" — ещё одно.
  4. Запись содержит OTP сенсора и зашифрованные нулевым PSK кадры, то есть
     фактически изображения отпечатка. Чтобы не публиковать свой отпечаток,
     используйте, например, боковую сторону пальца или костяшку (как советует
     tests/README.md). Публиковать запись только осознанно.
EOF
  read -r -p "Продолжить? [y/N] " answer
  [[ $answer == [yY] ]] || exit 0

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
  - файлы tests/goodix5125/ НЕ коммитить без решения о публикации.
EOF
}

step_replay() {
  banner "4. Воспроизведение записи (без устройства)"
  [[ -f $TEST_DIR/custom.pcapng ]] || die "нет $TEST_DIR/custom.pcapng: сначала $0 record"
  env FP_DEVICE_EMULATION=1 FP_DRIVERS_ALLOWLIST=goodix5125 G_MESSAGES_DEBUG=all \
    G_DEBUG=fatal-warnings "$PY" "$SRC/tests/umockdev-test.py" --test custom "$TEST_DIR" \
    && echo "REPLAY OK: можно добавить 'goodix5125': {} в drivers_tests (tests/meson.build)" \
    || cat <<EOF
REPLAY FAIL. Ожидаемая причина: TLS-рукопожатие недетерминировано (случайный
ServerHello.random хоста), поэтому записанный Finished сенсора не совпадает при
воспроизведении. Нужна доработка драйвера: при FP_DEVICE_EMULATION=1
использовать детерминированный RAND для TLS (см. docs/stage10-mr.md).
EOF
}

case "${1:-all}" in
  check)  step_check ;;
  build)  step_check; step_build ;;
  record) step_record ;;
  replay) step_replay ;;
  all)    step_check; step_build; step_record; step_replay ;;
  *)      die "неизвестный шаг: $1 (check|build|record|replay)" ;;
esac
