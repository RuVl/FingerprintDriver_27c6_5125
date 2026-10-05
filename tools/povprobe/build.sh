#!/bin/sh
# Build povprobe.efi with clang and the lld-link bundled with rustup (no gnu-efi needed).
set -e
cd "$(dirname "$0")"
LLD=${LLD:-$(find ~/.rustup/toolchains -name rust-lld -path '*x86_64-unknown-linux-gnu*' | head -n1)}
clang --target=x86_64-unknown-windows -ffreestanding -fno-stack-protector \
  -fshort-wchar -mno-red-zone -mgeneral-regs-only -O2 -Wall -Wextra \
  -c povprobe.c -o povprobe.o
"$LLD" -flavor link /nologo /subsystem:efi_application /entry:efi_main \
  /nodefaultlib /machine:x64 /out:povprobe.efi povprobe.o
rm -f povprobe.o
