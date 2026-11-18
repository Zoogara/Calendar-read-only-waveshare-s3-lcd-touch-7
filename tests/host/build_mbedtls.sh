#!/bin/sh
# Builds ESP-IDF's copy of mbedTLS into build-mbedtls/libmbedcrypto.a, for the host tests when the machine
# has no mbedTLS of its own (see the Makefile). Usage: build_mbedtls.sh <components/mbedtls/mbedtls> <cc>
set -e
src="$1"
cc="${2:-cc}"
mkdir -p build-mbedtls
cd build-mbedtls
for f in "$src"/library/*.c; do
    "$cc" -std=gnu11 -O2 -w -I"$src/include" -I"$src/library" -c "$f" -o "$(basename "$f" .c).o" &
done
wait
ar rcs libmbedcrypto.a ./*.o
