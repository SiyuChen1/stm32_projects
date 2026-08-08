#!/usr/bin/env bash
# Capture BOTH sides of the STM32 <-> XIAO SPI bridge at the same time, into timestamped logs.
#
#   ./capture_logs.sh [seconds]     (default 20)
#
# Both boards must already be flashed and running. Close picocom / the Arduino Serial Monitor
# first -- they hold the ports open and this will get nothing.

set -u
SECS="${1:-20}"
OUT="$HOME/STM32"

STM32_PORT=/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
XIAO_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E0:72:A1:FB:E5:54-if00

# The STM32 UART carries binary visualizer frames interleaved with printf text, so the raw capture
# is deliberately kept and `strings` is used to pull out just the readable diagnostics.
stty -F "$STM32_PORT" 3000000 raw -echo || exit 1
stty -F "$XIAO_PORT"  115200  raw -echo || exit 1

echo "Recording ${SECS}s from both boards..."
timeout "$SECS" cat "$STM32_PORT" > "$OUT/stm32_raw.bin" &
STM32_PID=$!
timeout "$SECS" cat "$XIAO_PORT"  > "$OUT/xiao.log" &
XIAO_PID=$!
wait $STM32_PID $XIAO_PID 2>/dev/null

strings "$OUT/stm32_raw.bin" > "$OUT/stm32.log"

echo "Done:"
echo "  $OUT/stm32.log   ($(wc -l < "$OUT/stm32.log") lines)"
echo "  $OUT/xiao.log    ($(wc -l < "$OUT/xiao.log") lines)"
