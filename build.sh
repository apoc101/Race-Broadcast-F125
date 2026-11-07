#!/bin/sh
# Builds native/race_broadcast.dll for the addon (64-bit Windows) with clang and lld-link; no C runtime is used.
# Needs: clang and lld (LLVM 15 or later). Output: build/race_broadcast.dll
set -e
cd "$(dirname "$0")"
mkdir -p build
for d in kernel32 user32 ws2_32; do
  [ -f build/$d.lib ] || lld-link /lib /machine:x64 /def:native/lib/$d.def /out:build/$d.lib >/dev/null
done
clang --target=x86_64-pc-windows-msvc -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
  -Wall -Wno-unused-function -Inative/include -c native/src/race_broadcast.c -o build/rb.obj
lld-link /dll /Brepro /noentry /nodefaultlib /machine:x64 /out:build/race_broadcast.dll build/rb.obj build/kernel32.lib build/user32.lib build/ws2_32.lib
echo "built build/race_broadcast.dll"
