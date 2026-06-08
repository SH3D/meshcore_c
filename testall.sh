#!/usr/bin/env bash
#
# testall.sh -- run every test suite available on this machine.
#
#   * CMake + ctest      (host build of the C core + the codec unit test)
#   * pio test -e native (the same unit test via PlatformIO's native env)
#   * pio ci             (compile an Arduino example for an ESP32-S3 board)
#
# Each suite is optional: if a tool is missing the suite is skipped with a
# warning, so this works on a cmake-only box, a PlatformIO-only box, or both.
# Exit status is non-zero if any suite that DID run failed (or if nothing could
# run at all).
#
#   ./testall.sh
#   ARDUINO_BOARD=esp32dev ./testall.sh     # override the board
#
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")" || exit 2

ARDUINO_BOARD="${ARDUINO_BOARD:-esp32-s3-devkitc-1}"
ARDUINO_EXAMPLE="examples/SensorChannelBridge/SensorChannelBridge.ino"

ran=0 failed=0 skipped=0
results=()

have()    { command -v "$1" >/dev/null 2>&1; }
section() { printf '\n\033[1m==== %s ====\033[0m\n' "$1"; }
warn()    { printf '\033[33mWARN:\033[0m %s\n' "$*" >&2; }

# run_suite <label> <function-name>
run_suite() {
    local label="$1" fn="$2"
    section "$label"
    if "$fn"; then
        results+=("PASS  $label"); ran=$((ran + 1))
    else
        results+=("FAIL  $label"); ran=$((ran + 1)); failed=$((failed + 1))
    fi
}
skip_suite() {
    results+=("SKIP  $1 ($2)"); skipped=$((skipped + 1))
    warn "$1 skipped: $2"
}

# Pick whichever PlatformIO CLI name exists.
PIO=""
if   have pio;        then PIO=pio
elif have platformio; then PIO=platformio
fi

# ---- Suite definitions -----------------------------------------------------

suite_cmake() {
    cmake -B build -S .          || return 1
    cmake --build build          || return 1
    if have ctest; then
        ctest --test-dir build --output-on-failure || return 1
    else
        warn "ctest not found -- built the core but did not run the unit test"
    fi
}

suite_pio_native() {
    "$PIO" test -e native
}

suite_pio_arduino() {
    "$PIO" ci --board="$ARDUINO_BOARD" \
              --project-option="framework=arduino" \
              --lib="." "$ARDUINO_EXAMPLE"
}

# ---- Run what's available --------------------------------------------------

if have cmake; then
    run_suite "CMake build + ctest" suite_cmake
else
    skip_suite "CMake build + ctest" "cmake not installed"
fi

if [ -n "$PIO" ]; then
    run_suite "PlatformIO native test"               suite_pio_native
    run_suite "Arduino compile ($ARDUINO_BOARD)"     suite_pio_arduino
else
    skip_suite "PlatformIO native test"          "pio/platformio not installed"
    skip_suite "Arduino compile"                 "pio/platformio not installed"
fi

# ---- Summary ---------------------------------------------------------------

section "Summary"
for r in "${results[@]}"; do
    case "$r" in
        PASS*) printf '\033[32m%s\033[0m\n' "$r" ;;
        FAIL*) printf '\033[31m%s\033[0m\n' "$r" ;;
        *)     printf '\033[33m%s\033[0m\n' "$r" ;;
    esac
done
printf '%d ran, %d failed, %d skipped\n' "$ran" "$failed" "$skipped"

if [ "$ran" -eq 0 ]; then
    warn "no test tools found (install cmake and/or platformio)"
    exit 2
fi
[ "$failed" -eq 0 ] || exit 1
exit 0
