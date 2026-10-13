#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# MNM-1 in the browser: the firmware app compiled to WebAssembly, and its page.
#   [OM_VERSION=0.1] web/emu/build.sh
#     -> build/emu/mnm.wasm
#        build/emu/index.html   the page with the WebAssembly inside (a fragment: published as is)
#        build/emu/MNM-1.html   the same as a whole document: opens straight from disk (file://)
# clang with the wasm32 target (no C library needed: the firmware's own libc.c), or Emscripten's emcc.
# Same sources and -ffp-contract=off as host/build_host.sh and the device.
set -e
cd "$(dirname "$0")/../.."
PY="${PYTHON:-python3}"
mkdir -p build/gen build/emu
[ -f build/gen/felucca_font.h ] || "$PY" tools/gen_font.py build/gen/felucca_font.h >/dev/null
VER="$(printf %s "${OM_VERSION:-DEV}" | tr a-z A-Z)"
FLAGS="-O2 -ffp-contract=off -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable
       -DOM_HOST -DOM_WEB -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen"
if command -v emcc >/dev/null 2>&1 && [ -z "${MNM_CLANG:-}" ]; then
    emcc $FLAGS "-DOM_VERSION=\"$VER\"" --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 \
        -sFILESYSTEM=0 -o build/emu/mnm.wasm web/emu/mnm_web.c firmware/src/dsp/mnm.c
else
    "${CLANG:-clang}" --target=wasm32 $FLAGS "-DOM_VERSION=\"$VER\"" -ffreestanding -fno-builtin -nostdlib \
        -Wno-incompatible-library-redeclaration -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=1048576 \
        -Wl,--initial-memory=33554432 -o build/emu/mnm.wasm web/emu/mnm_web.c firmware/src/dsp/mnm.c
fi
"$PY" web/emu/make_page.py build/emu/mnm.wasm web/emu/index.html build/emu
