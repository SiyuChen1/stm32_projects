#!/usr/bin/env bash
# Build and flash the STM32 from the command line, bypassing the STM32CubeIDE GUI entirely.
#
# Why this exists: there are TWO project trees for 53L9A1_PostprocessSingle (see CLAUDE.md). The
# bundled one is what command-line `make` builds; the workspace one had a 2-day-stale Debug/ that
# silently got flashed instead, so source changes appeared to have no effect. This script always
# builds and flashes the SAME binary, and prints a marker check so you can confirm what went on the
# chip rather than assuming.
#
#   ./flash_stm32.sh [marker-string]
#
# The optional marker is a string expected in the new firmware (default: TESTTX). The script
# refuses to flash if the freshly built ELF does not contain it.

set -euo pipefail

MARKER="${1:-TESTTX}"
IDE=/opt/st/stm32cubeide_1.13.2
APP="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle"
DEBUG="$APP/STM32CubeIDE/Debug"

GCC_BIN=$(find "$IDE/plugins" -maxdepth 1 -type d -name "*gnu-tools-for-stm32*" | head -n1)/tools/bin
MAKE_BIN=$(find "$IDE/plugins" -maxdepth 1 -type d -name "*externaltools.make*" | head -n1)/tools/bin
export PATH="$GCC_BIN:$MAKE_BIN:$PATH"

echo "== Building (bundled project) =="
make -C "$DEBUG" -j4 all

ELF="$DEBUG/53L9A1_PostprocessSingle.elf"
BIN="$DEBUG/53L9A1_PostprocessSingle.bin"

echo "== Verifying the build contains marker: $MARKER =="
# NOTE: deliberately `grep -c`, not `grep -q`. Under `set -o pipefail`, `grep -q` exits on the first
# match, `strings` then dies of SIGPIPE (141), and the pipeline reports failure even though the
# marker WAS found -- which made an earlier version of this script abort on a perfectly good build.
# `grep -c` consumes all input, so there is no SIGPIPE. `|| true` keeps a legitimate no-match (exit
# 1) from tripping `set -e` before the count can be tested.
MARKER_COUNT=$(strings "$ELF" | grep -c -- "$MARKER" || true)
if [ "${MARKER_COUNT:-0}" -eq 0 ]; then
    echo "ABORT: '$MARKER' not found in $ELF -- the build did not pick up the expected source." >&2
    exit 1
fi
echo "OK: marker present ($MARKER_COUNT occurrence(s))."

CUBEPROG=$(find "$IDE/plugins" -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)
if [ -z "$CUBEPROG" ]; then
    echo "ABORT: STM32_Programmer_CLI not found under $IDE/plugins" >&2
    exit 1
fi

echo "== Flashing $BIN =="
"$CUBEPROG" -c port=SWD freq=1000 mode=UR reset=HWrst -w "$BIN" 0x08000000 -v -rst

echo
echo "Flashed. $(stat -c '%y' "$BIN" | cut -d. -f1)  marker=$MARKER"
