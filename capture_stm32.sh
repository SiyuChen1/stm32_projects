#!/usr/bin/env bash
# Capture the STM32's serial output to a file. Baud is a parameter because the two firmwares differ:
#   115200   ST's official prebuilt Binary/53L9A1_PostprocessSingle.bin (plain text)
#   3000000  our local build (text interleaved with binary visualizer frames)
#
#   ./capture_stm32.sh [seconds] [baud] [outfile]
#   ./capture_stm32.sh 20 115200 ~/STM32/stm32_official.log
#
# Defaults: 20s, 115200, ~/STM32/stm32_official.log
# Close picocom / any serial monitor first -- they hold the port and this will capture nothing.

set -uo pipefail

SECS="${1:-20}"
BAUD="${2:-115200}"
OUT="${3:-$HOME/STM32/stm32_official.log}"
PORT=/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02

if [ ! -e "$PORT" ]; then
    echo "ERROR: $PORT not found -- is the ST-LINK plugged in?" >&2
    exit 1
fi
if ! stty -F "$PORT" "$BAUD" raw -echo 2>/dev/null; then
    echo "ERROR: could not configure $PORT at $BAUD (is picocom or a serial monitor holding it open?)" >&2
    exit 1
fi

echo "Recording ${SECS}s from the STM32 at ${BAUD} baud..."
timeout "$SECS" cat "$PORT" > "$OUT.raw"

# At 3000000 the stream carries binary frames mixed with text, so extract the readable runs. At
# 115200 (the official binary) it is already plain text, but `strings` is harmless either way.
strings "$OUT.raw" > "$OUT"

echo "Done:"
echo "  $OUT      ($(wc -l < "$OUT") text lines)"
echo "  $OUT.raw  (raw bytes, kept in case the text filter hid something)"
echo
echo "Reported frame rate:"
grep -oE "Processed frame n\. [0-9]+ @ [0-9]+ fps" "$OUT" | tail -5 || echo "  (no 'Processed frame' lines found)"
