# NUCLEO-H563ZI + X-NUCLEO-53L9A1 on Ubuntu 20.04

## Setup, build, flashing, PC visualization, and debugging reference

This documents a complete, working setup for:

- **STM32 NUCLEO-H563ZI** + **X-NUCLEO-53L9A1** expansion board + **VL53L9CX** direct Time-of-Flight sensor
- **Ubuntu 20.04**, **STM32CubeIDE 1.13.2**, **X-CUBE-53L9A1 V1.0.0**, **ST-LINK V3**
- **picocom** and a custom **Python/matplotlib visualizer** for serial/PC-side output
- Optional future integration with a **Seeed Studio XIAO ESP32-S3** Wi-Fi bridge

### Status at a glance

| What | State |
|---|---|
| Hardware bring-up | **Working.** Required `SW1 = INT` on the X-NUCLEO-53L9A1 (see [Hardware Setup](#4-hardware-setup)). |
| Official prebuilt binary (`Binary/53L9A1_PostprocessSingle.bin`) | **Working**, unmodified, 115200 baud, text output only. |
| Local build (`Src/main.c`, `Src/vl53l9_app.c`) | **Working**, extended with a live PC-side visualizer, 3 acquisition-loop bugs fixed, 3,000,000 baud. |
| PC visualizer (`Utilities/vl53l9_visualizer.py`) | **Working.** Live amplitude/depth/ambient view over the same USB-serial link. |

If you're picking this project back up after a break, read [Quick Start](#1-quick-start) and
[Project Structure](#2-project-structure-read-this-first) first, then jump to whichever part you need.

---

# Table of Contents

1. [Quick Start](#1-quick-start)
2. [Project Structure (read this first)](#2-project-structure-read-this-first)
3. [Environment Setup](#3-environment-setup)
4. [Hardware Setup](#4-hardware-setup)
5. [Build, Flash, and Run](#5-build-flash-and-run)
6. [The PC Visualizer](#6-the-pc-visualizer)
7. [Firmware Bugs Found and Fixed](#7-firmware-bugs-found-and-fixed)
8. [Debugging Methodology: How SW1 Was Found](#8-debugging-methodology-how-sw1-was-found)
9. [Troubleshooting Checklist](#9-troubleshooting-checklist)
10. [Command Reference](#10-command-reference)
11. [STM32 → XIAO Bridge: SPI + Wi-Fi Live Visualizer](#11-stm32--xiao-bridge-spi--wi-fi-live-visualizer)
12. [Key Takeaways](#12-key-takeaways)
13. [Reference Paths](#13-reference-paths)
14. [SPI Bridge Debugging Record & Frame-Rate Budget](#14-spi-bridge-debugging-record--frame-rate-budget)

---

# 1. Quick Start

Two independent paths, depending on what you're trying to do.

## 1A. Just confirm the hardware works (official binary, 115200 baud)

```bash
BASE="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"
BIN_FILE="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Binary/53L9A1_PostprocessSingle.bin"
CUBEPROG_CLI=$(find /opt/st/stm32cubeide_1.13.2/plugins -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)

# power off, verify hardware first (see Section 4): SW1=INT, J6=1V8, J2-J5 installed, shield fully seated

"$CUBEPROG_CLI" -c port=SWD freq=1000 mode=UR reset=HWrst -w "$BIN_FILE" 0x08000000 -v -rst

picocom -b 115200 /dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

Expected:

```text
Processed frame n. 272 @ 16 fps
Processed frame n. 273 @ 16 fps
...
```

## 1B. Run the current local build with the live PC visualizer (3,000,000 baud)

```bash
# 1. Build & flash Src/main.c + Src/vl53l9_app.c from STM32CubeIDE (see Section 5.2 -
#    use the in-package project for command-line builds, see Section 2)

# 2. Run the visualizer (closes picocom first - a serial port has one owner)
python3 ~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle/Utilities/vl53l9_visualizer.py \
  -p /dev/ttyACM0 -b 3000000
```

A 3-panel matplotlib window opens (amplitude / depth / ambient), each with a colorbar, plus a
numeric depth readout. See [Section 6](#6-the-pc-visualizer) for flags and protocol details.

---

# 2. Project Structure (read this first)

## 2.1 The X-CUBE package

The expansion package is extracted at:

```bash
BASE="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"
APP="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle"
```

All commands below assume these two variables. Inside double quotes, don't escape underscores in
these paths - `"$BASE/..._53L9A1_V1.0.0"` is correct, `"$BASE/..._53L9A1\_V1.0.0"` is not.

The example used throughout is `53L9A1_PostprocessSingle`. Its real source lives at `$APP/Src/` -
`main.c`, `vl53l9_app.c`, etc. **Always edit these files directly**, never a copy inside a project
folder (see 2.2).

## 2.2 Two STM32CubeIDE projects exist for the same source - know this before building anything

There are **two separate STM32CubeIDE project directories** on this machine, both named
`53L9A1_PostprocessSingle`, both with identical `.project`/`.cproject` files, both compiling the
same real source via linked resources - but genuinely separate directories on disk, not symlinks:

```text
1. $APP/STM32CubeIDE
   (bundled inside the X-CUBE package itself - IMPORT AND BUILD THIS ONE)

2. ~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle
   (a separate project living outside the package tree)
```

**Why both exist**: the sample project was downloaded and imported a *second* time, separately
from the package extraction in 2.1 - each import correctly used "without copying" (see 2.3), so
Eclipse never literally duplicated files on either import. The second download/import landed at the
workspace path *without* its own sibling `Utilities/`, `Drivers/`, `Middlewares/` folders (those
only exist inside the full package extraction at `$BASE`). Both `.project` files declare identical
linked-resource paths like `PARENT-6-PROJECT_LOC/Utilities/...`, but those resolve *relative to each
project's own location* - from inside `$BASE` that math reaches the real shared folders; from the
standalone workspace location there's nothing there to reach.

**Practical consequences:**

- **STM32CubeIDE's GUI** resolves linked resources correctly for whichever project you open, from
  either location - both build and flash fine from inside the IDE. Building from inside CubeIDE?
  You don't need to think about this further.
- **Command-line/headless builds** (`make` from a terminal) only work from project 1
  (`$APP/STM32CubeIDE`) - confirmed by actually running `make` there (`Utilities/platform_utils.o`
  built successfully). From project 2 the identical `make` fails immediately:
  ```text
  make: *** No rule to make target '/Utilities/vl53l9-common/platform/platform_utils.c', ...
  ```
  Don't try to fix project 2's makefiles by hand - re-import it in place instead.
- **Editing source**: always edit `$APP/Src/*.c` directly (see 2.1). Project 2 has no real files of
  its own for anything covered by a linked resource - only genuinely local files like
  `syscalls.c`/`sysmem.c` physically live inside it.
- Both projects' `Debug/` folders can independently hold build artifacts from whichever was built
  most recently. If you're checking "was my latest edit actually built", check the `.elf`/`.bin`
  timestamp in *both* `Debug/` folders, not just one.

## 2.3 Correct project import method (avoids the problem in 2.2 from recurring)

An early build once failed with:

```text
No rule to make target '/Utilities/vl53l9-common/platform/platform_utils.c'
```

Cause: importing with **"Copy projects into workspace"** enabled. The project relies on relative
paths to shared package directories (`Drivers/`, `Utilities/`, `Middlewares/`) - copying only the
`STM32CubeIDE` subdirectory breaks those paths (this is the same underlying failure mode as 2.2).

**Correct method**: import directly from `$APP/STM32CubeIDE`, leave **"Copy projects into
workspace" unchecked**. Keep the project physically inside the X-CUBE package directory. Don't move
just the `STM32CubeIDE` subdirectory out of the package tree unless you also rewrite its linked
resources.

---

# 3. Environment Setup

## 3.1 Why STM32CubeIDE 1.13.2

A newer STM32CubeIDE version's bundled ST-LINK GDB server could not run on Ubuntu 20.04 - it
required system libraries Ubuntu 20.04 doesn't ship by default:

```text
GLIBC_2.32
GLIBC_2.33
GLIBC_2.34
GLIBCXX_3.4.29
```

STM32CubeIDE 1.13.2, installed at `/opt/st/stm32cubeide_1.13.2`, provides a compatible GDB server
and CubeProgrammer CLI and is the version used throughout.

## 3.2 Verify the Nucleo and ST-LINK

Connect the board via the ST-LINK USB connector. A successful probe looks like:

```text
ST-LINK SN  : 0026002C3235511137333439
ST-LINK FW  : V3J17M11
Board       : NUCLEO-H563ZI
Voltage     : 3.27-3.28V
Device name : STM32H5xx
Device CPU  : Cortex-M33
Flash size  : 2 MBytes
```

A warning like `Connection to AP 0 requested and failed, Connection established with AP 1` does not
prevent correct flashing.

## 3.3 Locate the GDB server and CubeProgrammer CLI

```bash
# ST-LINK GDB server
/opt/st/stm32cubeide_1.13.2/plugins/com.st.stm32cube.ide.mcu.externaltools.stlink-gdb-server.linux64_2.1.0.202305091550/tools/bin/ST-LINK_gdbserver

# STM32CubeProgrammer CLI - locate dynamically rather than hard-coding the plugin version
CUBEPROG_CLI=$(find /opt/st/stm32cubeide_1.13.2/plugins \
  -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)
printf '%s\n' "$CUBEPROG_CLI"
test -x "$CUBEPROG_CLI" && echo "Programmer OK"
```

## 3.4 Serial device

```bash
ls -l /dev/serial/by-id/
```

The stable device symlink used throughout:

```text
/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02  ->  /dev/ttyACM0
```

List all aliases:

```bash
for device in /dev/serial/by-id/*; do
    printf '%s -> %s\n' "$device" "$(readlink -f "$device")"
done
```

If access is denied: `sudo usermod -aG dialout "$USER"`, then log out and back in.

---

# 4. Hardware Setup

Mount the X-NUCLEO-53L9A1 directly on the NUCLEO-H563ZI's Arduino-style headers. **Disconnect USB
power before changing any switch or jumper.**

## 4.1 Required configuration

```text
SW1 = INT   <-- the critical one, see 4.2
J6  = 1V8
J2  = installed
J3  = installed
J4  = installed
J5  = installed
J1  = set consistently with the Nucleo's I/O voltage selection
```

## 4.2 SW1 - critical, and easy to get wrong

SW1 is a small **slide switch** (not a 2-pin jumper), marked approximately `EXT` / `INT` on the PCB.
Slide the actuator toward `INT`, with power disconnected.

**This single switch fixed the entire startup problem** described in [Section 8](#8-debugging-methodology-how-sw1-was-found).
With it in the wrong position, `vl53l9_init()` reliably times out:

```text
_wait_for_state(FSM_STATE_READY_TO_BOOT, 4) -> -5   (VL53L9_ERROR_TIMEOUT)
```

No software change - larger timeout, rewritten I3C code, different binning, a custom sensor patch -
fixes this. Only the switch does.

## 4.3 Physical checklist

- shield fully seated, no connector row shifted by one position, no bent header pin
- board not tilted or partially inserted
- J2-J5 all present, J6 in the correct position
- SW1 really at `INT`

---

# 5. Build, Flash, and Run

Two independent paths - pick based on what you need (see [Quick Start](#1-quick-start) for the
condensed commands).

## 5.1 Path A: Official prebuilt binary (fastest hardware sanity check, 115200 baud)

Useful for isolating "is this a hardware problem or a local build/source problem" - it removes your
compiler/build configuration from the test entirely.

```bash
BIN_FILE="$APP/Binary/53L9A1_PostprocessSingle.bin"
CUBEPROG_CLI=$(find /opt/st/stm32cubeide_1.13.2/plugins -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)

test -x "$CUBEPROG_CLI" && echo "Programmer OK"
test -f "$BIN_FILE" && echo "Official binary OK"

"$CUBEPROG_CLI" -c port=SWD freq=1000 mode=UR reset=HWrst -w "$BIN_FILE" 0x08000000 -v -rst
```

A successful flash looks like:

```text
STM32CubeProgrammer v2.14.0
ST-LINK SN  : 0026002C3235511137333439
...
Memory Programming ...
Download in Progress: [==================================================] 100%
File download complete
Verifying ...
Download verified successfully
MCU Reset
Software reset is performed
```

Then:

```bash
picocom -b 115200 /dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

Expected:

```text
Processed frame n. 272 @ 16 fps
Processed frame n. 273 @ 16 fps
...
```

This binary is untouched by any of the local source changes described in Sections 6-7 - it stays at
115200 baud and text-only output regardless of what's changed in `Src/`.

Note the serial output can appear visually "diagonal" (each line further indented than the last) -
this is the example's ASCII-art rendering using terminal cursor-positioning escapes
(`printf("\033[%d;%dH", 0, 0)`), not evidence of a problem. What matters: the frame counter
increases, fps stays valid, output continues.

## 5.2 Path B: Local build (current source, 3,000,000 baud, PC visualizer)

Build via STM32CubeIDE - use project 1 from [Section 2.2](#22-two-stm32cubeide-projects-exist-for-the-same-source---know-this-before-building-anything)
for anything command-line:

```text
Project -> Clean...
Project -> Build Project
```

A successful build: `Build Finished. 0 errors, 0 warnings.` Output lands at `$APP/STM32CubeIDE/Debug/`
(`53L9A1_PostprocessSingle.elf`/`.bin`) - don't confuse this with the reference binary in `Binary/`.

Flash and start a **Debug** session from CubeIDE: it connects via ST-LINK, resets the target,
downloads and verifies the program, and stops at `main()`. At that point the CPU is paused - press
**F8** (or click Resume). The Debug tree should change from `Suspended` to `Running`. **If you skip
this step, the application never runs.**

The CubeIDE debug console (`Download verified successfully`, `Temporary breakpoint at main`) shows
debugger/programmer messages, not application output - `printf()` is routed through the BSP UART to
the ST-LINK virtual COM port, which you view separately:

```bash
picocom -b 3000000 /dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

3,000,000 baud, not 115200 - the local source was changed for this (`Src/main.c`,
`BspCOMInit.BaudRate = 3000000`, was `115200`) because the PC visualizer's binary payloads (~22.7
KB/frame) are far too slow for 115200. If you rebuild with a different baud, update both picocom
and the visualizer's `-b` flag to match - and if 3,000,000 proves unreliable on your ST-Link VCP
bridge (visible as frequent CRC failures/dropped frames in the visualizer), it's safe to step back
down to 921600 or 115200; just also change `main.c`.

`CONF_PRINT_FRAME` in `vl53l9_app.c` currently reads `(0)` - ASCII-art frame printing is disabled;
only the `Processed frame n. ... @ N fps` text line and the binary visualizer stream are emitted per
frame. `grep -R -n "CONF_PRINT_FRAME" "$APP"` before assuming either value - it's a cheap toggle
that's easy to flip during development.

For the actual live view, see [Section 6](#6-the-pc-visualizer) instead of picocom.

## 5.3 Cleaning up after using Path A to isolate a problem

If you used the official binary (5.1) to confirm hardware works, before returning to local
development: remove any temporary debugging globals/breakpoints you added, then

```bash
rm -rf "$APP/STM32CubeIDE/Debug"
```

and in CubeIDE: `Right-click project -> Refresh`, `Project -> Clean...`, `Project -> Build Project`.
Don't regenerate the project with CubeMX unless you intentionally want to rebuild the peripheral
configuration.

---

# 6. The PC Visualizer

## 6.1 What it does

The stock example only prints ASCII-art depth frames and `Processed frame n. ... @ N fps` text over
the ST-LINK VCP. The local build now also streams binary amplitude+depth+ambient frames over the
same UART for a live PC-side viewer.

```bash
python3 Utilities/vl53l9_visualizer.py -p /dev/ttyACM0 -b 3000000
```

(close picocom first - a serial port has one owner). This opens a 3-panel matplotlib window
(amplitude / depth / ambient), each with its own colorbar, plus a numeric readout (center/min/max
depth in mm) so you can check actual values instead of reading colors.

Useful flags:

- `--raw` - skip frame parsing entirely, dump whatever bytes arrive to stdout unparsed (like
  picocom). Use this when no valid frames are parsing, to see the firmware's plain-text traces
  directly without needing a debugger session.
- `--save-npz FILE` - record every received frame to disk.
- `--depth-cmap` / `--amp-cmap` / `--ambient-cmap` - override the default colormaps.

The script is currently only physically present under the workspace project (see
[Section 13](#13-reference-paths)) - it's a standalone PC tool, not part of the firmware build
graph, so unlike `Src/` it doesn't need to live inside the X-CUBE package tree.

## 6.2 Wire protocol

```text
header (12 bytes, packed, little-endian):
    magic          4s   b"VL59"
    frame_counter  u32
    width          u8
    height         u8
    crc16          u16   CRC-16/CCITT (poly 0x1021, init 0x0000) over the payload below

payload:
    amplitude      width*height x float32   (native AF32 - "signal_rate", raw photon-count-rate)
    depth          width*height x uint16    (ZF32 quantized, round-to-nearest mm)
    ambient        width*height x float32   (native IF32 - "ambient_rate", raw photon-count-rate)
```

Depth alone is quantized to `uint16` because its real range is known and safe:
`MAX_DISTANCE_RANGE` (8500) / `MAX_DISTANCE_PRECISION` (8800) in `vl53l9_transform.c` bound
legitimate readings to 0-8800mm, and the library's own `12000.0f` invalid-pixel sentinel (6.4) also
fits comfortably under 65535. Amplitude and ambient were *also* tried as quantized `uint16` in an
earlier iteration - it visibly clipped/saturated all texture out of the amplitude image on real
hardware, because those are raw, undocumented-range photon-count-rate values
(`_signal_rate`/`_ambient_rate` internally, not bounded like depth-in-mm), so they were reverted to
native `float32` to stay lossless. For the default AR_PRECISION usecase (54x42 = 2268 pixels) that's
~22.7 KB/frame.

Text `printf()` traces (ASCII frames, `Processed frame n. ...` lines) still go out the same UART,
interleaved with the binary frames; the Python parser resyncs on the 4-byte magic and validates
CRC, so it safely ignores anything that isn't a valid `VL59` packet.

## 6.3 Amplitude vs. ambient (physical meaning)

Both render as brightness images but measure physically different things:

- **Amplitude** (`_signal_rate`) = strength of the sensor's *own* reflected laser (VCSEL) pulse per
  zone. Depends on target distance (falls off ~inverse-square), reflectivity, and angle of
  incidence. A quasi-image: bright = close/reflective/facing the sensor, dark = far/absorptive/
  grazing angle.
- **Ambient** (`_ambient_rate`) = background/environmental IR light per zone, independent of the
  sensor's own laser (sunlight, indoor lighting, etc.) - a scene-lighting / SNR indicator. High
  ambient degrades the sensor's ability to pick its own laser return out of the noise floor.

Both are exposed as separate `transform_set_stream_capabilities()` streams (`"amplitude"`, format
`AF32`; `"ambient"`, format `IF32`) alongside `"depth"` (`ZF32`), all at the same resolution -
confirmed against `vl53l9_transform.c` to be first-class supported streams, not gated behind the
library's `VL53L9_TRANSFORM_LIGHT` reduced-feature flag.

## 6.4 The 12000mm "invalid pixel" sentinel

Depth pixels that fail the library's validity checks (flying pixels, low confidence, filtered
edges, etc.) don't come back as an out-of-band flag - they come back as a specific fixed value:

```c
// Middlewares/ST/vl53l9-transform-c/vl53l9-transform-c-lib/src/vl53l9_transform.c
const float invalid_distance = 12000.0f;
```

passed directly into `_process_distance_check()` / `vl53l9_algo_distance_check()`. If you see
`depth == 12000`, that's not a real 12-meter measurement (the VL53L9 doesn't range that far) - it's
the algorithm marking the pixel invalid. The visualizer masks this explicitly
(`INVALID_DEPTH_MM = 12000`, `is_valid_depth = (depth > 0) & (depth < INVALID_DEPTH_MM - 1)`) rather
than a naive `depth > 0` check, which would let the sentinel dominate the colorbar and wash out real
data by comparison.

## 6.5 fps characteristics

Bandwidth is the dominant cost for the binary stream, but not the only one. At the current ~22.7
KB/frame mixed-precision payload and 3,000,000 baud, pure UART transmission time is ~75.6ms/frame
(~13fps ceiling from bandwidth alone). An earlier, smaller all-`uint16` 13.6KB/frame version (since
reverted - see 6.2) measured **9.7 fps** on hardware, well under its ~22fps bandwidth-only estimate
- meaning roughly half the per-frame time was *not* UART transmission. The likely remaining
contributors are the sensor's own native ~33ms frame period (the configured profile targets 30fps)
and `transform_process_stream()`'s on-MCU pipeline cost (TNR, radial-to-perpendicular, sharpener,
rate-normalization, reflectance, flying-pixel filter, distance-check stages, all running on the
Cortex-M33 every frame). This has not been instrumented with hard per-phase timing to confirm the
exact split - an open question, not a solved one. Practical takeaway: once you're past a few Mbps,
further baud increases give diminishing returns, because the bottleneck stops being purely the wire.
The current mixed-precision protocol (6.2) has not yet been independently re-measured for fps on
hardware - the 9.7fps figure above is from the prior, smaller all-`uint16` payload.

---

# 7. Firmware Bugs Found and Fixed

Three real, pre-existing gaps in the acquisition loop, found while adding diagnostics after the
streaming feature initially appeared to make frame acquisition fail - it did not; these bugs are
reachable independently of the streaming feature, just previously silent.

## Bug 1 - discarded return value

The stock loop called:

```c
vl53l9_trigger_frame(p_dev);
if (ret) {              // `ret` is stale from setup, never actually reassigned here
    handle_error();
}
```

`vl53l9_trigger_frame()`'s result was never captured, so any trigger failure was silently ignored.
Fixed by capturing it - now folded into the retry loop in Bug 3.

## Bug 2 - stale interrupt flag before the first wait

`main.c`'s `MX_GPIO_Init()` enables the `EXTI7` interrupt (the sensor's `INTR` pin) at boot, before
`vl53l9_app()` even runs. Any falling edge during the reset/init/calibration/prepare/start sequence
- for any reason, not necessarily "frame ready" - latches a sticky flag
(`g_platform_evt |= PLATFORM_GPIO_IT_EVT` in `platform_utils.c`'s `HAL_GPIO_EXTI_Falling_Callback()`)
that nothing clears before the main loop starts. The *first*
`platform_wait_for_event(PLATFORM_GPIO_IT_EVT, ...)` call could therefore return immediately on
stale state instead of the real post-trigger interrupt, so the "frame" it found was never actually
ready. Fixed with one `platform_acknowledge_event(PLATFORM_GPIO_IT_EVT)` flush right after
`vl53l9_start()` succeeds, before the loop begins.

## Bug 3 - no tolerance for transient acquisition faults

Even with Bug 2 fixed, one raw frame acquisition (trigger -> wait for IRQ -> read) was observed on
real hardware to fail three different ways across separate runs: a stale interrupt flag (Bug 2,
before the fix), a full 1000ms IRQ-wait timeout with nothing arriving, and a sensor-reported
transient fault (`status.error` bit `sof_outside_blanking`, read via `vl53l9_get_status()`). A
different failure point each time is the signature of a genuine transient timing hiccup, not one
deterministic bug - so the stock behavior (kill the whole application on the first failure) was
replaced with a bounded retry:

```c
#define ACQUIRE_MAX_RETRIES (5)
// trigger -> wait -> read wrapped in a for-loop up to ACQUIRE_MAX_RETRIES attempts, flushing
// the stale-flag before each attempt, only calling handle_error() after 5 consecutive
// failures. This also subsumes Bug 1 (trigger's return value is now checked).
```

## Diagnostics added (worth knowing about even if you never hit a bug)

- `handle_error()` used to spin forever with **zero** UART output, making every possible failure
  look identical (indistinguishable from a hang). It's now a macro (`#define handle_error()
  handle_error_impl(__LINE__)`) that prints the exact source line plus `vl53l9_get_status()`'s
  fsm/command/firmware-error/error-bits before spinning - grep `vl53l9_app.c` for the printed line
  number to find the exact failing check.
- `printf()` checkpoints after every major setup stage (`vl53l9_init`, calibration read,
  `transform_initialize`, stream capabilities, `transform_prepare`, `vl53l9_start`, "entering main
  loop") so a hang shows exactly how far setup got even without a clean `handle_error()` call (e.g.
  a crash before reaching it).
- `vl53l9_visualizer.py --raw` (6.1) is the fastest way to read all of the above without needing a
  debugger session.

---

# 8. Debugging Methodology: How SW1 Was Found

This is the original hardware bring-up investigation, kept as a worked example of debugging an
optimized embedded build with GDB - the technique is reusable well beyond this one bug.

## 8.1 The symptom

picocom opened successfully (`Terminal ready`) but showed no application output. Important
distinction: `"Terminal ready"` only means Linux opened the serial device - it does **not** mean the
STM32 is transmitting. Pressing F8 in a Debug session changed the state from `Suspended` to
`Running`, but picocom stayed blank - so the problem was deeper than a paused MCU.

## 8.2 Technique: breakpoints, backtraces, and reading `r0` under `-Ofast`

A breakpoint on `handle_error()`, combined with:

```gdb
bt
frame 1
list
info locals
```

is the reliable pattern for finding which call in `vl53l9_app()` reached the error path.

**Watch out under aggressive optimization.** The project builds with `-Ofast`, and GDB showed many
locals as `<optimized out>`, with instructions sometimes mapped to confusing source lines. One early
lead - a backtrace landing near `handle_error(); /* unsupported binning */` - looked like it
implicated the configured binning, but this was a false lead: under `-Ofast`, a nearby source
comment isn't reliable evidence of which check actually failed.

Two ways around `<optimized out>`:

1. **Temporary volatile globals**, immune to optimization:
   ```c
   volatile int g_dbg_ret = 0;
   volatile uint32_t g_dbg_usecase = 0;
   volatile uint8_t g_dbg_binning = 0;
   volatile uint8_t g_dbg_width = 0;
   volatile uint8_t g_dbg_height = 0;
   ```
   Observed values (`g_dbg_usecase=1, g_dbg_binning=2, g_dbg_ret=0, g_dbg_width=54,
   g_dbg_height=42`) conclusively proved the resolution lookup succeeded - ruling out the binning
   lead. Cross-checked directly against the profile table in `vl53l9_utils.c`:
   ```text
   VL53L9_USECASE_AR_RANGE       binning = 2
   VL53L9_USECASE_AR_PRECISION   binning = 2   <- selected profile
   VL53L9_USECASE_AF_RANGE       binning = 4
   VL53L9_USECASE_AF             binning = 4

   resolution lookup: 2->54x42  4->24x20  6->18x14  8->12x10  12->8x6  24->4x4
   ```
   binning 2 was valid; that wasn't the bug.

2. **Read the Cortex-M return-value register directly.** A simple integer function's return value
   is normally in `r0` immediately after it returns, regardless of whether a local C variable
   holding it got optimized away:
   ```gdb
   finish
   print/d $r0
   print/x $r0
   ```

## 8.3 Root-causing the real failure

A backtrace eventually showed:

```text
#0  handle_error()
#1  vl53l9_app() at vl53l9_app.c:88
#2  main()
```

corresponding to:

```c
ret = vl53l9_init(p_dev);
if (ret) {
    handle_error();
}
```

so the real failure was `vl53l9_init()`, not binning/UART/frame-processing. Breaking on it directly:

```gdb
break vl53l9_init
continue
finish
print/d $r0
```

returned `-5`. The driver's error codes:

```c
#define VL53L9_ERROR_NONE              (0)
#define VL53L9_ERROR_PLATFORM          (-1)
#define VL53L9_ERROR_INVALID_PARAM     (-2)
#define VL53L9_ERROR_INVALID_STATE     (-3)
#define VL53L9_ERROR_INVALID_OPERATION (-4)
#define VL53L9_ERROR_TIMEOUT           (-5)
#define VL53L9_ERROR_INTERNAL          (-6)
```

`-5 = VL53L9_ERROR_TIMEOUT`. `vl53l9_init()`'s very first substantive call is:

```c
int vl53l9_init(void *const p_dev) {
    int ret;
    ...
    CHECK_NULL_PTR(p_dev);
    ret = _wait_for_state(p_dev, FSM_STATE_READY_TO_BOOT, 4);
    CHECK_RET(ret);
    ...
}
```

so the failure was *before* firmware-patch upload, boot command, standby transition, calibration
read, frame acquisition, or post-processing - narrowing it to the sensor's initial startup state.
Breaking directly on `_wait_for_state` (after disabling an earlier breakpoint that intercepted
execution first: `info breakpoints`, `disable 3`, `continue`):

```text
_wait_for_state(p_dev = 0x200009a8, state = FSM_STATE_READY_TO_BOOT, timeout_ms = 4)

#0 _wait_for_state()
#1 vl53l9_init()
#2 vl53l9_app()
#3 main()
```

then `finish` / `print/d $r0` again returned `-5`. Decisive finding: **the VL53L9 sensor did not
reach `READY_TO_BOOT` in the reference startup sequence** - a hardware/config-level fact, not
something more `_wait_for_state()` timeout budget would fix.

## 8.4 Root cause and fix

Physical inspection of the X-NUCLEO board found `SW1`, a two-position slide switch marked `EXT` /
`INT`. Moving it to `INT` and retesting: the system worked immediately, producing continuous
`Processed frame n. ... @ 16 fps` output. See [Section 4.2](#42-sw1---critical-and-easy-to-get-wrong)
for the fix itself; this section is the methodology that found it.

---

# 9. Troubleshooting Checklist

Work through in order.

**A. ST-LINK not detected** - `lsusb`; `"$CUBEPROG_CLI" -c port=SWD`.

**B. CubeIDE GDB server fails on Ubuntu 20.04** (missing `GLIBC_2.32`/`2.33`/`2.34`/`GLIBCXX_3.4.29`)
- use a compatible release like 1.13.2 rather than upgrading system glibc manually.

**C. Build reports missing `Utilities/...` targets** - you're building from the wrong project
location, or imported with "Copy projects into workspace" checked. See
[Section 2](#2-project-structure-read-this-first).

**D. Flash works but application serial is blank** - is the CPU still stopped at `main()`? Press F8.
Then confirm the correct serial device: `ls -l /dev/serial/by-id/`.

**E. `handle_error()` is reached** - `bt`, `frame 1`, `list`. Don't assume a nearby source comment
is the real failing operation under `-Ofast` (8.2). The current build also prints the failing line
number and sensor status directly (Section 7, "Diagnostics added") - check the UART output first.

**F. Local variables show `<optimized out>`** - `finish` / `print/d $r0`, or a temporary
`volatile` global (8.2).

**G. Binning appears unsupported** - check the actual profile and lookup table first (8.2); for
this package, `AR_PRECISION` uses binning 2, which outputs 54x42 - binning itself is very unlikely
to be the real problem.

**H. `vl53l9_init()` returns `-5`** (`VL53L9_ERROR_TIMEOUT`) - break on `_wait_for_state`; if
`FSM_STATE_READY_TO_BOOT` times out, check hardware startup configuration first - specifically
`SW1 = INT` (Section 4.2).

**I. Unsure whether hardware or local build is at fault** - flash `Binary/53L9A1_PostprocessSingle.bin`
(Section 5.1). This removes your local compiler/build configuration from the test entirely.

**J. No valid `VL59` frames parsing in the visualizer** - run `vl53l9_visualizer.py --raw` (6.1) to
see the firmware's raw text traces without a debugger. Check baud match between `main.c` and the
visualizer's `-b` flag (5.2).

---

# 10. Command Reference

## Shell

```bash
BASE="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"
APP="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle"

# frame-printing config
grep -R -n "CONF_PRINT_FRAME" "$APP"

# print/UART redirection
grep -R -nE 'CONF_PRINT_FRAME|printf\(|HAL_UART_Transmit|CDC_Transmit|USBD_CDC|__io_putchar|_write\(' "$APP"

# BSP COM initialization
grep -R -nE 'BSP_COM_Init|BSP_COM_SelectLogPort|COM_InitTypeDef' "$APP/Src" "$APP/Inc"

# ranging profiles
grep -R -n "g_ranging_profiles" "$BASE" --exclude-dir=Debug --exclude='*.list' --exclude='*.map'

# display utility source
nl -ba "$BASE/Utilities/vl53l9-common/vl53l9/vl53l9_utils.c" | sed -n '1,180p'

# display vl53l9_init()
nl -ba "$BASE/Drivers/BSP/Components/vl53l9/vl53l9.c" | sed -n '145,260p'

# search error/status definitions
grep -R -nE 'VL53L9.*(ERROR|STATUS|TIMEOUT|BOOT|COMM)|typedef enum' \
  "$BASE/Drivers/BSP/Components/vl53l9" "$BASE/Utilities/vl53l9-common" | head -n 200

# serial devices
for device in /dev/serial/by-id/*; do printf '%s -> %s\n' "$device" "$(readlink -f "$device")"; done
```

## GDB

```gdb
continue                    # or F8 in CubeIDE
bt                           # backtrace
frame 1                      # change frame
list                         # show nearby source
info locals
info args
info breakpoints
disable 1 2 3
delete
break vl53l9_init            # or: break _wait_for_state
finish                       # run until current function returns
print/d $r0                  # Cortex-M integer return value - crucial under -Ofast
print/x $r0
```

---

# 11. STM32 → XIAO Bridge: SPI + Wi-Fi Live Visualizer

Implemented. Adds a second consumer of the same amplitude/depth/ambient frames the PC visualizer
already streams (see [Section 6](#6-the-pc-visualizer)) - a XIAO ESP32 (S3/C6/C5) that receives them
over SPI instead of UART and re-serves them as a live webpage over Wi-Fi, removing the need for a PC
physically tethered to the STM32's ST-LINK USB port:

```text
VL53L9CX --I3C--> STM32H563 --raw acquisition/post-processing--> depth/amplitude/ambient frame
    --SPI4--> XIAO ESP32 (S3/C6/C5) --Wi-Fi--> any browser on the LAN, http://<xiao-ip>/
```

Both sides use the identical wire format (header + amplitude/depth/ambient planes + CRC-16/CCITT)
already documented in [Section 6.2](#62-wire-protocol) - only the transport changed, not the frame
layout. Code lives in two places: the STM32 side is `MX_SPI4_Init()` / `HAL_SPI_MspInit()` /
`send_vis_frame_spi()` in this project's `vl53l9_app.c`/`main.c`/`stm32h5xx_hal_msp.c` (gated by
`CONF_STREAM_SPI`, independent of `CONF_STREAM_VISUALIZER` - either or both can be on); the XIAO side
is a separate Arduino sketch at `~/Arduino/arduino_projects/seeed_xiao/stm32_utility/spi/spi.ino`
(not part of this STM32 project's build).

An earlier UART-based prototype of this same idea (`stm32_utility/uart/uart.ino` in the Arduino repo)
is still there as a simpler diagnostic - plain USART6 passthrough to USB Serial, no Wi-Fi, no SPI. The
SPI+Wi-Fi version superseded it as "the" bridge; the UART one is kept as a minimal link-level sanity
check if the SPI path is ever misbehaving.

## 11.1 SPI4 wiring

Pins chosen by cross-checking this project's own `.ioc` (for what the X-NUCLEO-53L9A1 shield and the
`uart.ino` prototype already occupy) against ST's own CubeMX pin database (`db/mcu/STM32H563ZITx.xml`,
bundled with STM32CubeIDE) for a real SPI4/AF5 hardware pin group - not guessed, not bit-banged:

```text
NUCLEO-H563ZI (SPI4, master)              XIAO ESP32 S3/C6/C5 (SPI2_HOST, slave)
PE12          (Zio only, no D-number) ->  D8  -- SCK
PE13 / Arduino D3                     ->  D9  -- MISO
PE14 / Arduino D4                     ->  D10 -- MOSI
PE11 / Arduino D5                     ->  D3  -- CS (software-managed NSS on the STM32 side)
PE9  / Arduino D6                     ->  D0  -- PIN_READY (XIAO -> STM32 handshake, see 11.2)
GND                                    ---     GND
```

> ### DO NOT USE THE HEADER PIN LABELLED `SCK`
>
> The NUCLEO-144 Arduino header has a pin silkscreened **`SCK` (D13)**. That pin is **PA5**, which is
> `SPI1_SCK`/`SPI6_SCK` - **SPI4 physically cannot drive it** (verified in ST's CubeMX pin database).
> The clock must come from the pin labelled **PE12** on the Zio connector, which has no Arduino
> D-number at all.
>
> Getting this wrong cost a full day, because the failure looks nothing like a broken wire: CS, MOSI
> and the READY handshake all work perfectly, every transfer reports `HAL_OK`, the handshake never
> times out - and the slave counts **zero clock edges**. At higher clock rates a floating SCK input
> even picks up crosstalk from the neighbouring wires and delivers plausible-looking *aliased* data,
> which is far more misleading than silence. See Section 14.

MISO/MOSI already encode direction, so same-named pin goes to same-named pin (STM32 MISO to XIAO
MISO) - unlike UART's TX/RX, these don't cross. All 5 signals verified free of the shield's I3C
(PB8/PB9), XSHUT (PB6), INTR (PB7), SYNC_IN (PB1), TIM3_CH2 (PB5) and the 3 status LEDs, and free of
`uart.ino`'s USART6 pins (PG14/PG9).

## 11.2 The handshake and framing

SPI has no built-in framing and a slave cannot produce data on demand mid-transaction, so the receive
buffer must be queued *before* the master clocks. Three separate mistakes were made here before it
worked; all three are worth understanding because each produced a different, misleading symptom.

**A frame is sent as 12 chunks of 2048 bytes** (`SPI_CHUNK_BYTES`), padded to 24576, not as one
22692-byte transaction. This is a **wire-protocol constant - change it on both sides and reflash both
together.** (Chunking is not strictly required: with DMA, ESP-IDF bounds slave transfers only by
internal memory. It was introduced while chasing a symptom whose real cause was the SCK wiring, and
it costs ~14ms/frame in handshake round-trips. It does buy per-chunk verification and resync points.)

**READY must be driven from the SPI driver's ISR callbacks, never from `loop()`:**

- `spi_slave_queue_trans()` returns **before** the hardware is armed - it only posts to a FreeRTOS
  queue. Raising READY when it returns tells the master "go" while the slave is still deaf.
  `post_setup_cb` fires when the registers are actually loaded; raise READY there.
- `post_trans_cb` fires the instant a transaction ends; drop READY there. Doing it in `loop()` leaves
  READY **stale-HIGH** after a transfer, and with 12 back-to-back chunks there is no idle gap to hide
  the latency - the master reads the stale level as "armed" and clocks into an unarmed slave.
- Use ISR-safe `gpio_set_level()` with `IRAM_ATTR`, not `digitalWrite()`.
- Correspondingly the master uses an **edge** handshake between chunks: wait for READY LOW (slave saw
  the transaction end), *then* HIGH (slave re-armed). A level-only wait cannot tell stale from fresh.

**The receiver must find the frame boundary itself.** The transport carries no chunk index - a frame
is just 12 identical transactions - so the receiver cannot tell chunk 0 from chunk 7. It almost always
starts listening mid-frame (the STM32 is already streaming when the XIAO boots), and treating the
first chunk seen as chunk 0 locks assembly to a constant wrong offset *forever*: the tail of one frame
reassembled with the head of the next, header landing mid-buffer. Chunk 0 is self-identifying because
the `"VL59"` header sits at its offset 0, so the receiver refuses to assemble until a chunk starts
with the magic, and re-anchors to chunk 0 whenever it sees the magic. That second rule also recovers
from the master abandoning a frame part-way on a READY timeout and restarting at chunk 0.

**Always check `trans_len`.** `spi_slave_get_trans_result() == ESP_OK` means only that CS toggled - it
does **not** mean the requested bytes arrived. Without this check the sketch parses whatever is in the
DMA buffer, which reads as plausible repeating garbage. Zero the buffer between transactions too.

*Generalizable:* any protocol that splits a frame across several equal fixed-size transfers needs
either a chunk index or a self-identifying first chunk. Length alone is not framing.

## 11.3 Build gotcha: SPI wasn't wired into this project before

This project never used SPI, so two things needed a one-time manual fix (both already applied, both
worth knowing if the project is ever regenerated from its `.ioc`, which would silently undo them):

1. `HAL_SPI_MODULE_ENABLED` was commented out in `Inc/stm32h5xx_hal_conf.h` - the umbrella
   `stm32h5xx_hal.h` only pulls in `stm32h5xx_hal_spi.h` (hence `SPI_HandleTypeDef` etc.) when this is
   defined. Symptom if it regresses: `unknown type name 'SPI_HandleTypeDef'`.
2. The bundled project's generated `Debug/Drivers/STM32H5xx_HAL_Driver/subdir.mk` and `objects.list`
   never listed `stm32h5xx_hal_spi.c`/`stm32h5xx_hal_spi_ex.c` at all (CubeIDE only adds a HAL source
   file to these generated lists when a peripheral using it is configured through the `.ioc`/pin view,
   which this hand-edit bypassed). Symptom if it regresses on a command-line `make -C Debug`:
   `undefined reference to HAL_SPI_Init` / `HAL_SPI_Transmit` at the link step, even though the code
   compiles fine. Both generated files were manually patched (mirroring the existing UART entries) so
   `make -C Debug all` builds clean end-to-end - confirmed with a real build, not just a syntax check.
   Opening the project in STM32CubeIDE and re-adding SPI4 through the pin view would regenerate these
   properly instead of relying on the manual patch.

---

# 12. Key Takeaways

1. **Check hardware switches early.** A single physical switch (`SW1 = INT`) caused a low-level
   driver timeout that initially looked like a software bug. The fix was never "increase timeout,
   rewrite I3C, change binning/profile, modify the firmware patch."
2. **Use the official binary as a reference.** Flashing `Binary/*.bin` is one of the fastest ways to
   isolate a hardware problem from a local build problem.
3. **Separate debugger output from UART output.** CubeIDE debug-console messages
   (`Download verified successfully`, etc.) are not application logs; application `printf()` comes
   through the BSP UART / ST-LINK VCP only.
4. **Optimized debugging (`-Ofast`) can mislead you.** GDB may hide locals, show
   `<optimized out>`, and map instructions to confusing source lines - use `bt`/`frame`/`list`/
   `finish`/`print/d $r0` rather than trusting the highlighted source line alone.
5. **Preserve the X-CUBE directory structure.** The project depends on relative paths reaching its
   package's shared `Utilities/`/`Drivers/`/`Middlewares/` folders - see [Section 2](#2-project-structure-read-this-first)
   for what breaks (and why) when a project ends up outside that structure.

---

# 13. Reference Paths

```text
X-CUBE root:
~/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0

Application:
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle

CubeIDE project (in-package - use this one for command-line builds, see Section 2.2):
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE

Second CubeIDE project (outside the package tree - GUI-only, see Section 2.2):
~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle

Official binary:
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Binary/53L9A1_PostprocessSingle.bin

Application source (edit these, not any project-local copy):
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Src/main.c
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Src/vl53l9_app.c

PC-side visualizer (Section 6) - a standalone PC tool, not part of the firmware build graph, so
its exact location doesn't matter the way Src/ does; currently only physically present at:
~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle/Utilities/vl53l9_visualizer.py
(not yet copied into the in-package project's own Utilities/ folder)

VL53L9 driver:
Drivers/BSP/Components/vl53l9/vl53l9.c

VL53L9 profile utilities:
Utilities/vl53l9-common/vl53l9/vl53l9_utils.c

VL53L9 platform/event glue (Section 7, Bug 2):
Utilities/vl53l9-common/platform/platform_utils.c

Depth-processing pipeline (Section 6.3/6.4 - invalid-distance sentinel, amplitude/ambient source):
Middlewares/ST/vl53l9-transform-c/vl53l9-transform-c-lib/src/vl53l9_transform.c
```

---

# 14. SPI Bridge Debugging Record & Frame-Rate Budget

Getting the STM32 -> XIAO SPI link working took a full day and roughly a dozen wrong turns. This
section records what was tried, what each symptom actually meant, and the measured performance
budget, so none of it has to be rediscovered.

## 14.1 Root cause: the wrong pin was called "SCK"

**The clock wire was connected to the header pin silkscreened `SCK` (D13 = PA5) instead of PE12.**
SPI4 cannot drive PA5 (it is `SPI1_SCK`/`SPI6_SCK`), so the slave never received a clock.

This originated in the wiring instructions, which described the pin as "PE12 / SCK" - and the board
has a *different* pin wearing that label. The wiring matched the instructions; the instructions were
wrong.

**Why it took so long to find:** the failure is almost perfectly disguised.

| Observation | What it seemed to mean | What it actually meant |
|---|---|---|
| CS, MOSI, READY all work; handshake never times out | link basically fine, data problem | only the clock was missing |
| Master reports `HAL_OK` on every transfer | data was sent successfully | `HAL_OK` only means the *master's* peripheral finished its side |
| Slave returns plausible-looking garbage | corruption / dropped SCK edges | floating SCK picking up crosstalk from neighbouring wires |
| Bit counts not multiples of 8 | slave dropping clock edges | slave latching noise; later, arming mid-byte |
| Two chips failed *differently* (C5 fixed 5 bits, S3 scattered) | chip or driver bug | different silicon reacting to the same dead line |

**The measurement that cracked it:** bisecting down to one 64-byte transaction at 976 kHz. At large,
fast transfers a disconnected clock yields *believable* garbage; at tiny, slow transfers it yields an
unmistakable `trans_len == 0`. **Shrink the problem before theorizing about corrupted data.**

## 14.2 Hypotheses that were wrong (and why)

Recorded so they are not re-tried:

- **"ESP-IDF caps slave transfers at 4092 bytes."** No - 4092 is only the *default* for
  `max_transfer_sz`. With DMA, transfers are bounded only by available internal memory.
- **"GPIO-matrix routing is too slow."** No - at 80 MHz and below it behaves identically to IO_MUX.
- **"The clock is too fast."** No - ÷128 (1.95 MHz) was verified in the flashed ELF by disassembly,
  and ESP32 slaves handle up to 60 MHz.
- **"HTTP serving starves the SPI loop."** No - measured identical 4.7 fps and zero bad frames with
  the browser open and closed.
- **"The IDE is flashing a stale binary."** No - the workspace tree predated features the chip
  demonstrably had. The IDE builds the *bundled* tree, the same `Debug/` that `make` writes to.
- **Wiring was questioned four times after being confirmed.** It was correct relative to the
  instructions given. When someone says their setup matches the spec, re-audit the spec.

## 14.3 Real bugs found along the way (all fixed, all worth keeping)

Independent of the wiring fault, these were genuine defects:

1. **`trans_len` never checked** - `spi_slave_get_trans_result() == ESP_OK` only means CS toggled.
   Stale DMA buffer contents were being parsed as data.
2. **READY driven from `loop()`** instead of the driver's ISR callbacks - stale-HIGH race (Section 11.2).
3. **Level-based master handshake** - could not distinguish stale READY from fresh; now edge-based.
4. **No frame alignment** - the receiver could not identify chunk 0 (Section 11.2).
5. **`SPI_TRANSFER_TIMEOUT_MS` left at 100 ms** after the clock was slowed 8x, leaving ~7% margin on a
   92.9 ms transfer. *When changing a clock divider, re-check every timeout derived from it.*
6. **Guard-scope bugs** - `crc16_ccitt()`, and later `out_amp_mem`/`out_ambient_mem`, were declared
   under `CONF_STREAM_VISUALIZER` only although the SPI path needs them. Both are now guarded
   `CONF_STREAM_VISUALIZER || CONF_STREAM_SPI`.

## 14.4 Tooling written for this

All in the repo root. See `CLAUDE.md` for the rule that Claude never runs the flashing script.

| Script | Purpose |
|---|---|
| `flash_stm32.sh [marker]` | Builds, **refuses to flash unless a marker string is present in the freshly built ELF**, then flashes. Guards against analysing a chip that is running the previous round's firmware. |
| `capture_logs.sh [secs]` | Records both boards simultaneously into `stm32.log` / `xiao.log` so the two sides line up in time. |
| `capture_stm32.sh [secs] [baud] [out]` | Single-board capture. Baud is a parameter because ST's official binary is 115200 while the local build is 3000000. |

**Verify what is actually on the chip before trusting any log.** One side of a two-board system can
silently be a round behind, and both logs still look plausible. Put a unique marker string in each
build and grep the log for it.

## 14.5 Frame-rate budget (measured, not calculated)

Per-stage timing from `CONF_PROFILE_TIMING` in `vl53l9_app.c`, at 54x42 (binning 2, `AR_PRECISION`):

```text
acquire     18 ms   sensor trigger + IRQ wait
transform   42 ms   ST's 11-stage postprocessing pipeline
readout      1 ms   I3C DMA
print        0 ms
uart        83 ms   22692 B at 3 Mbaud, blocking   <- removed (CONF_STREAM_VISUALIZER = 0)
spi         64 ms   ~50 ms line time + ~14 ms chunk handshakes
            ------
total      210 ms   = 4.76 fps
```

**The 30 fps in the profile table is the *sensor's* acquisition rate, not an end-to-end claim.** ST
does not claim 30 fps end-to-end anywhere in the package.

**Verified baseline: ST's own unmodified prebuilt binary runs at 16 fps** - which matches this
project's `acquire + transform + readout` = 61 ms (16.4 fps) exactly. So the 42 ms transform cost is
simply what ST's pipeline costs on this MCU at this resolution; the local build does not make
processing slower. Everything including the middleware compiles `-Ofast` with hardware FPU, and ST's
code uses no double-precision math - neither is a factor.

**~16 fps is therefore the ceiling** at 54x42 with the full pipeline. Reaching it means driving the
transport cost to approximately nothing:

| Change | Saves | Result |
|---|---|---|
| `CONF_STREAM_VISUALIZER = 0` | 83 ms | ~7.9 fps |
| SPI ÷64 -> ÷16 (15.6 MHz) | ~38 ms | ~11.4 fps |
| Chunks 12 -> 1 (drops handshake round-trips) | ~13 ms | ~13 fps |
| **SPI via DMA instead of blocking** | most of the remainder | **~16 fps** |

The acquisition loop already overlaps `transform` with sensor acquisition, but `HAL_SPI_Transmit` is
blocking, so the SPI transfer is pure dead time. DMA would let it run during acquisition. This is far
simpler with 1 chunk than with 12.

**To exceed 16 fps** the 42 ms `transform` must shrink. ST exposes seven bypass properties -
`bypass-tnr-algo`, `bypass-sharpener-filter`, `bypass-fp-filter`, `bypass-conf-filter`,
`bypass-refl-filter`, `bypass-r2p-algo`, `bypass-r2p-filter` (all default to off, i.e. every stage
runs). Profile the individual stages before disabling any. Dropping to binning 4 (27x21) quarters the
pixel count and would approach 30 fps, at half the spatial resolution.
