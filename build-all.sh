#!/usr/bin/env bash
# KVMapper cross-compile script.
#
# Builds the x64 exe + DLL pair and the x86 DLL (plus optionally x86 exe).
# Uses `python -m ziglang cc` per windows-native-cicd skill §1 so the same
# command works on Linux, macOS, and Windows.
#
# Required:  pip install ziglang
# Optional:  mingw-w64 windres for embedding the .rc resources. If not
#            available we skip the resource (icon/version info) and the
#            exe still builds and runs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DIST="${ROOT}/dist"
mkdir -p "${DIST}"

ZIG="python3 -m ziglang"
CFLAGS="-O2 -Wall -Wextra -std=c99 -Iinclude -DUNICODE -D_UNICODE"
LD_EXE="-Wl,--subsystem,windows -lcomctl32 -lshell32 -luser32 -lgdi32 -lole32"
LD_DLL="-shared -luser32 -lkernel32"

EXE_SRC="src/main.c src/capture.c src/config.c src/shared_mem.c src/classifier.c src/icon_data.c"
DLL_SRC="src/hook/hook.c src/hook/inject.c"

# ---- Resource file (optional) ----
RES_OBJ=""
if command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
    echo "==> windres: app.rc -> dist/res.o"
    x86_64-w64-mingw32-windres -O coff app.rc -o "${DIST}/res.o"
    RES_OBJ="${DIST}/res.o"
elif command -v windres >/dev/null 2>&1; then
    echo "==> windres: app.rc -> dist/res.o"
    windres -O coff app.rc -o "${DIST}/res.o"
    RES_OBJ="${DIST}/res.o"
else
    echo "==> windres not found - building without embedded icon/version info"
fi

build_exe () {
    local target="$1"
    local out="$2"
    echo "==> ${target}: ${out}"
    # shellcheck disable=SC2086
    ${ZIG} cc -target "${target}" ${CFLAGS} \
        ${EXE_SRC} ${RES_OBJ} \
        -o "${out}" \
        ${LD_EXE}
}

build_dll () {
    local target="$1"
    local out="$2"
    echo "==> ${target}: ${out}"
    # shellcheck disable=SC2086
    ${ZIG} cc -target "${target}" ${CFLAGS} \
        ${DLL_SRC} \
        -o "${out}" \
        ${LD_DLL}
}

build_exe x86_64-windows-gnu "${DIST}/kvmapper.exe"
build_dll x86_64-windows-gnu "${DIST}/kvmapper_hook.dll"
build_dll x86-windows-gnu    "${DIST}/kvmapper_hook_x86.dll"

# 32-bit exe helper (optional but ships in v1 per user request)
build_exe x86-windows-gnu    "${DIST}/kvmapper_x86.exe"

# Manifest sidecar (optional - manifest is also embedded via app.rc)
cp app.manifest "${DIST}/kvmapper.exe.manifest"

echo
echo "==> Build complete"
ls -la "${DIST}"
