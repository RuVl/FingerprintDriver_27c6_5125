#!/bin/sh
# Build a core test against a configured libfprint _build directory:
#   build.sh <libfprint _build dir> test-suspend-removed
#   build.sh <libfprint _build dir> test-suspend-removed-fixed
set -e
B=${1:?usage: build.sh <libfprint _build dir> <test name>}
T=${2:?usage: build.sh <libfprint _build dir> <test name>}
SP=$(cd "$(dirname "$0")" && pwd)
cd "$B"
cc -Itests -I../tests -Ilibfprint -I../libfprint -I. -I.. \
  $(pkg-config --cflags gio-unix-2.0 gusb json-glib-1.0 gudev-1.0) \
  -Wall -D_GNU_SOURCE '-DG_LOG_DOMAIN="libfprint"' \
  -o "$SP/$T" "$SP/$T.c" \
  -Wl,--whole-archive tests/libfprint-test-utils.a -Wl,--no-whole-archive \
  -Wl,--start-group libfprint/libfprint-private.a libfprint/libnbis.a \
  libfprint/libfprint-2.so.2.0.0 -Wl,--end-group -Wl,-rpath,"$PWD/libfprint" \
  $(pkg-config --libs gio-unix-2.0 gmodule-2.0 gusb json-glib-1.0 gudev-1.0 libssl libcrypto pixman-1) -lm
