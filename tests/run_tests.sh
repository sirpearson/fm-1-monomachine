#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# MNM-1 host tests: the maths, the platform pieces kept from Felucca / X0X / FoMni / FiMba-1, the engine
# (built without libm, as on the device), and the whole firmware app in the simulator
# (tests/scenarios/*.mnm: the grid, locks, keys and recording, patterns and persistence, MIDI), with
# screenshots and audio in build/scenarios/ and a WAV per machine in build/host/wav/.
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FAIL=0
OUT=build/host
mkdir -p "$OUT" "$OUT/wav" build/scenarios
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run uac sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/uac_test tests/host/uac_test.c -lm && $OUT/uac_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
# no -lm: a libm call that crept into the engine fails to link here, as it would on the device
run engine sh -c "$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wdouble-promotion -Werror -DOM_HOST -Ifirmware/src/dsp \
    -o $OUT/mnm_test tests/host/mnm_test.c firmware/src/dsp/mnm.c && $OUT/mnm_test $OUT/wav"
run storage sh -c "$CC -O2 -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
# the update path, against the firmware package (Felucca's tests; needs ./build.sh)
if [ -f build/mnm.fwsc ]; then
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/mnm.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 100000 build/mnm.bin > $OUT/old_app.bin && \
        python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/mnm.fwsc"
    fi
    run installer python3 tests/install_test.py
    # the package taken apart and every CRC checked; against FoMni's own package when one is there
    REF=""
    [ -f build/fomni-ref/build/omni.fwsc ] && REF="--ref build/fomni-ref/build/omni.fwsc"
    run package sh -c "python3 tools/check_fwsc.py build/mnm.fwsc --app build/mnm.bin $REF"
else
    echo "  skip update-path tests (no build/mnm.fwsc: run ./build.sh)"
fi
run host-build sh host/build_host.sh
for s in tests/scenarios/*.mnm; do
    n=$(basename "$s" .mnm)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/mnm_host "$s" "build/scenarios/$n"
done
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
