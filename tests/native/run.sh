#!/usr/bin/env bash
# Native pure-logic tests for KVMapper. Runs on Linux/macOS via gcc.
#
# Stage 1: 25 unit tests (test_config.c)
# Stage 2: 10k fuzz iterations under ASan + UBSan (fuzz_config.c)
#
# Builds shim'd win32.h on the fly so config.c compiles without mingw.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
OUT=/tmp/kvmapper_test_native
mkdir -p "${OUT}"
cp "${HERE}/win32_shim.h" "${OUT}/windows.h"

echo "==> Stage 1: unit tests"
gcc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L \
  -I"${OUT}" -I"${ROOT}/include" \
  "${ROOT}/src/config.c" "${HERE}/test_config.c" \
  -o "${OUT}/test_unit"
"${OUT}/test_unit"

echo
echo "==> Stage 2: 10k fuzz iterations (ASan + UBSan)"
gcc -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -D_POSIX_C_SOURCE=200809L \
  -I"${OUT}" -I"${ROOT}/include" \
  "${ROOT}/src/config.c" "${HERE}/fuzz_config.c" \
  -o "${OUT}/test_fuzz"
"${OUT}/test_fuzz" 10000

echo
echo "==> All native tests passed"
