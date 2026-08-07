# NUCLEO-H563ZI + X-NUCLEO-53L9A1 on Ubuntu 20.04
## Detailed setup, build, flashing, serial output, debugging, and root-cause analysis

> **Read this first if you're picking the project back up.** Sections 1-36 below are the original
> tutorial: they document the hardware bring-up (the `SW1 = INT` fix) using the **stock, unmodified**
> example and the **official prebuilt binary** at 115200 baud. That work is still accurate and still
> the right first step on a fresh board. Since then the local source (`Src/main.c`,
> `Src/vl53l9_app.c`) has been substantially extended with a PC-side live visualizer, three real bugs
> found and fixed in the acquisition loop, and a much higher baud rate - none of that is reflected in
> sections 1-36, and a few specific facts recorded there (baud rate, `CONF_PRINT_FRAME`) no longer
> match the current source. **See [Section 37](#37-post-tutorial-update-pc-visualizer-wire-protocol-and-acquisition-loop-fixes)
> onward for the current state of the project**, and [Section 39](#39-post-tutorial-update-corrections-to-sections-above)
> for a list of exactly what changed vs. what's written below.

This tutorial documents a complete working setup for the following hardware and software stack:

- **STM32 NUCLEO-H563ZI**
- **X-NUCLEO-53L9A1** expansion board
- **VL53L9CX** direct Time-of-Flight sensor
- **Ubuntu 20.04**
- **STM32CubeIDE 1.13.2**
- **X-CUBE-53L9A1 V1.0.0**
- **ST-LINK V3**
- **picocom** for serial output
- Optional later integration with a **Seeed Studio XIAO ESP32-S3** over UART

The key final result was simple but important:

> **The official example worked after setting SW1 on the X-NUCLEO-53L9A1 to `INT`.**

With SW1 in the non-working position, the driver timed out in the very first sensor startup check:

```text
_wait_for_state(FSM_STATE_READY_TO_BOOT, 4) -> -5
```

The driver defines:

```c
#define VL53L9_ERROR_TIMEOUT (-5)
```

After switching SW1 to `INT`, the official package binary immediately produced continuous processed frames such as:

```text
Processed frame n. 272 @ 16 fps
Processed frame n. 273 @ 16 fps
Processed frame n. 274 @ 16 fps
...
```

That proved the sensor, I3C link, firmware patch, processing pipeline, UART output, and ST-LINK virtual COM port were all functioning.

---

# Table of Contents

1. [Final Working Configuration](#1-final-working-configuration)
2. [Why STM32CubeIDE 1.13.2 Was Used](#2-why-stm32cubeide-1132-was-used)
3. [Verify the Nucleo and ST-LINK](#3-verify-the-nucleo-and-st-link)
4. [Locate CubeProgrammer and GDB Server](#4-locate-cubeprogrammer-and-gdb-server)
5. [Install and Locate X-CUBE-53L9A1](#5-install-and-locate-x-cube-53l9a1)
6. [Open the Correct Example Project](#6-open-the-correct-example-project)
7. [Avoid the Project Import Path Problem](#7-avoid-the-project-import-path-problem)
8. [Build the Example](#8-build-the-example)
9. [Hardware Configuration](#9-hardware-configuration)
10. [Flash and Debug from CubeIDE](#10-flash-and-debug-from-cubeide)
11. [GDB Console vs Application Serial Output](#11-gdb-console-vs-application-serial-output)
12. [Open the ST-LINK Virtual COM Port](#12-open-the-st-link-virtual-com-port)
13. [Confirm Frame Printing Is Enabled](#13-confirm-frame-printing-is-enabled)
14. [Confirm BSP COM/UART Redirection](#14-confirm-bsp-comuart-redirection)
15. [The Initial Symptom: Blank picocom](#15-the-initial-symptom-blank-picocom)
16. [First Debugging Step: Confirm the MCU Is Running](#16-first-debugging-step-confirm-the-mcu-is-running)
17. [Using Breakpoints and Backtraces](#17-using-breakpoints-and-backtraces)
18. [Why the First “Unsupported Binning” Diagnosis Was Misleading](#18-why-the-first-unsupported-binning-diagnosis-was-misleading)
19. [Verify the Ranging Profile and Resolution](#19-verify-the-ranging-profile-and-resolution)
20. [Dealing with `<optimized out>`](#20-dealing-with-optimized-out)
21. [Finding the Real Failure in `vl53l9_init()`](#21-finding-the-real-failure-in-vl53l9_init)
22. [Understanding `VL53L9_ERROR_TIMEOUT`](#22-understanding-vl53l9_error_timeout)
23. [Breaking on `_wait_for_state()`](#23-breaking-on-_wait_for_state)
24. [The Root Cause: SW1](#24-the-root-cause-sw1)
25. [Testing with the Official Prebuilt Binary](#25-testing-with-the-official-prebuilt-binary)
26. [Expected Successful Flashing Output](#26-expected-successful-flashing-output)
27. [Expected Successful Serial Output](#27-expected-successful-serial-output)
28. [Why the Serial Output Can Look Diagonal](#28-why-the-serial-output-can-look-diagonal)
29. [Return to the Source-Built CubeIDE Project](#29-return-to-the-source-built-cubeide-project)
30. [Useful Shell Commands](#30-useful-shell-commands)
31. [Useful GDB Commands](#31-useful-gdb-commands)
32. [Optional XIAO ESP32-S3 UART Connection](#32-optional-xiao-esp32-s3-uart-connection)
33. [Recommended Next Architecture](#33-recommended-next-architecture)
34. [Troubleshooting Checklist](#34-troubleshooting-checklist)
35. [Lessons Learned](#35-lessons-learned)
36. [Quick Start: Known-Good Procedure](#36-quick-start-known-good-procedure)
37. [POST-TUTORIAL UPDATE: PC Visualizer, Wire Protocol, and Acquisition-Loop Fixes](#37-post-tutorial-update-pc-visualizer-wire-protocol-and-acquisition-loop-fixes)
38. [POST-TUTORIAL UPDATE: Two Project Locations for the Same Source](#38-post-tutorial-update-two-project-locations-for-the-same-source)
39. [POST-TUTORIAL UPDATE: Corrections to Sections Above](#39-post-tutorial-update-corrections-to-sections-above)

---

# 1. Final Working Configuration

> This describes the **official prebuilt binary** (`Binary/53L9A1_PostprocessSingle.bin`), which is
> still accurate as-is. If you're running **locally built** firmware instead, the serial baud rate
> below no longer applies as-is - see [Section 37.2](#37-post-tutorial-update-pc-visualizer-wire-protocol-and-acquisition-loop-fixes).

The working configuration was:

```text
Operating system:
  Ubuntu 20.04

MCU board:
  NUCLEO-H563ZI

Sensor expansion:
  X-NUCLEO-53L9A1

Sensor:
  VL53L9CX

IDE:
  STM32CubeIDE 1.13.2

X-CUBE package:
  STM32CubeExpansion_53L9A1_V1.0.0

Sensor-board switch/jumper state:
  SW1 = INT
  J6  = 1V8
  J2  = installed
  J3  = installed
  J4  = installed
  J5  = installed
  J1  = set consistently with Nucleo I/O voltage

Serial:
  ST-LINK V3 Virtual COM Port
  115200 baud
  8 data bits
  no parity
  1 stop bit
  no hardware flow control
```

The final working application continuously produced:

```text
Processed frame n. ... @ 16 fps
```

---

# 2. Why STM32CubeIDE 1.13.2 Was Used

A newer STM32CubeIDE version was initially installed, but its bundled ST-LINK GDB server could not run on Ubuntu 20.04 because it required newer system libraries.

The missing versions included requirements such as:

```text
GLIBC_2.32
GLIBC_2.33
GLIBC_2.34
GLIBCXX_3.4.29
```

Ubuntu 20.04 does not provide these versions by default.

The working IDE version was therefore:

```text
STM32CubeIDE 1.13.2
```

installed under:

```bash
/opt/st/stm32cubeide_1.13.2
```

This version provided a compatible ST-LINK GDB server and CubeProgrammer CLI.

---

# 3. Verify the Nucleo and ST-LINK

Before touching the ToF expansion board, confirm that the Nucleo itself can be reached.

The board should be connected through the ST-LINK USB connector.

A successful probe/programmer session showed values similar to:

```text
ST-LINK SN  : 0026002C3235511137333439
ST-LINK FW  : V3J17M11
Board       : NUCLEO-H563ZI
Voltage     : 3.27-3.28V
Device name : STM32H5xx
Device CPU  : Cortex-M33
Flash size  : 2 MBytes
```

A message such as:

```text
Warning: Connection to AP 0 requested and failed, Connection established with AP 1
```

did not prevent correct flashing in this setup.

---

# 4. Locate CubeProgrammer and GDB Server

## ST-LINK GDB server

The compatible GDB server was located at:

```bash
/opt/st/stm32cubeide_1.13.2/plugins/com.st.stm32cube.ide.mcu.externaltools.stlink-gdb-server.linux64_2.1.0.202305091550/tools/bin/ST-LINK_gdbserver
```

## STM32CubeProgrammer CLI

The compatible programmer was:

```bash
/opt/st/stm32cubeide_1.13.2/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_2.1.0.202305091550/tools/bin/STM32_Programmer_CLI
```

Instead of hard-coding the full plugin directory, locate it dynamically:

```bash
CUBEPROG_CLI=$(find /opt/st/stm32cubeide_1.13.2/plugins \
  -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)
```

Check:

```bash
printf '%s\n' "$CUBEPROG_CLI"
```

You can also test that it is executable:

```bash
test -x "$CUBEPROG_CLI" && echo "Programmer OK"
```

---

# 5. Install and Locate X-CUBE-53L9A1

The expansion package was extracted under:

```bash
$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0
```

Define a convenient variable:

```bash
BASE="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"
```

## Shell-path note

Inside double quotes, do not escape underscores.

Use:

```bash
"$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"
```

not:

```bash
"$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion\_53L9A1\_V1.0.0"
```

---

# 6. Open the Correct Example Project

The reference example used throughout this tutorial is:

```text
53L9A1_PostprocessSingle
```

The STM32CubeIDE project directory is:

```bash
$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE
```

Full path:

```bash
/home/siyuchen/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE
```

---

# 7. Avoid the Project Import Path Problem

An early build failed with an error like:

```text
No rule to make target '/Utilities/vl53l9-common/platform/platform_utils.c'
```

The cause was importing the project with:

```text
Copy projects into workspace
```

enabled.

The example relies on relative paths to shared package directories such as:

```text
Drivers/
Utilities/
Middlewares/
```

Copying only the `STM32CubeIDE` project into a workspace breaks those paths.

## Correct import method

Import the project directly from:

```bash
$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE
```

and leave:

```text
Copy projects into workspace
```

unchecked.

Keep the project physically inside the X-CUBE package directory.

---

# 8. Build the Example

In STM32CubeIDE:

```text
Project -> Clean...
Project -> Build Project
```

A successful build produced:

```text
Build Finished. 0 errors, 0 warnings.
```

The local outputs are under:

```bash
$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE/Debug
```

Typical files:

```text
53L9A1_PostprocessSingle.elf
53L9A1_PostprocessSingle.bin
```

The X-CUBE package also contains an official reference binary:

```bash
$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Binary/53L9A1_PostprocessSingle.bin
```

This distinction becomes very useful later:

```text
STM32CubeIDE/Debug/*.bin
    your locally compiled result

Binary/*.bin
    package-supplied reference image
```

---

# 9. Hardware Configuration

Mount the X-NUCLEO-53L9A1 directly on top of the NUCLEO-H563ZI Arduino-style headers.

Before changing any switch or jumper:

```text
Disconnect USB power.
```

The final working configuration was:

```text
SW1 = INT
J6  = 1V8
J2  = installed
J3  = installed
J4  = installed
J5  = installed
J1  = matched to the Nucleo I/O voltage selection
```

## SW1 is not a jumper

SW1 is a small **slide switch**.

It is not removed or repositioned like a 2-pin jumper.

The PCB marks the switch positions approximately:

```text
EXT        INT
```

Slide the actuator toward:

```text
INT
```

with power disconnected.

This single change ultimately fixed the entire startup problem.

## Physical checks

Verify:

- the shield is fully seated;
- no connector row is shifted by one position;
- no header pin is bent;
- the board is not tilted or partially inserted;
- J2-J5 are all present;
- J6 is in the correct position;
- SW1 is really at `INT`.

---

# 10. Flash and Debug from CubeIDE

When starting a **Debug** session, CubeIDE usually:

1. connects through ST-LINK;
2. resets the target;
3. downloads the program;
4. verifies it;
5. stops at `main()`.

Typical GDB output:

```text
Temporary breakpoint, main()
HAL_Init();
```

At this point the CPU is still paused.

Press:

```text
F8
```

or click:

```text
Resume
```

The Debug tree should change from:

```text
Suspended
```

to:

```text
Running
```

If you forget this step, the sensor application will not continue running.

---

# 11. GDB Console vs Application Serial Output

The CubeIDE debug console may show:

```text
ST-LINK GDB server
Download verified successfully
Temporary breakpoint at main
```

Those messages come from the debugger/programmer.

They are **not** application `printf()` output.

The application output is routed through the Nucleo BSP COM implementation and must be viewed through the ST-LINK virtual serial port.

---

# 12. Open the ST-LINK Virtual COM Port

Find the stable serial-device symlink:

```bash
ls -l /dev/serial/by-id/
```

The working device was:

```text
/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

which mapped to:

```text
/dev/ttyACM0
```

To list all stable serial aliases:

```bash
for device in /dev/serial/by-id/*; do
    printf '%s -> %s\n' \
        "$device" \
        "$(readlink -f "$device")"
done
```

Open it with picocom:

```bash
picocom -b 115200 \
  /dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

Expected parameters:

```text
baudrate    : 115200
flowcontrol : none
parity      : none
databits    : 8
stopbits    : 1
```

If access is denied:

```bash
sudo usermod -aG dialout "$USER"
```

Then log out and back in.

---

# 13. Confirm Frame Printing Is Enabled

Search the project:

```bash
APP="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle"

grep -R -n "CONF_PRINT_FRAME" "$APP"
```

At the time this was written, the source showed:

```c
#define CONF_PRINT_FRAME (1)
```

> **Correction (see Section 39):** the current source reads `#define CONF_PRINT_FRAME (0)` -
> verified directly against `Src/vl53l9_app.c`. ASCII-art frame printing is currently **disabled**;
> `grep` for it yourself before assuming either value, since this is exactly the kind of flag that's
> easy to toggle back and forth during development. Flip it to `1` if you want the ASCII art back -
> it's a cheap, harmless, fully independent toggle from everything else in this document.

The frame-processing code also contained:

```c
printf("Processed frame n. %lu @ %u fps\n", ...);
```

and ASCII-frame drawing code.

A broader search is useful:

```bash
grep -R -nE \
'CONF_PRINT_FRAME|printf\(|HAL_UART_Transmit|CDC_Transmit|USBD_CDC|__io_putchar|_write\(' \
"$APP"
```

---

# 14. Confirm BSP COM/UART Redirection

Search:

```bash
grep -R -nE \
'BSP_COM_Init|BSP_COM_SelectLogPort|COM_InitTypeDef' \
"$APP/Src" "$APP/Inc"
```

The example contained:

```c
COM_InitTypeDef BspCOMInit;
```

and:

```c
BSP_COM_Init(COM1, &BspCOMInit)
```

The linked code showed that `printf()` eventually reaches:

```c
HAL_UART_Transmit(&hcom_uart[COM_ActiveLogPort], ...)
```

Therefore, the ST-LINK VCP is the correct place to monitor the application.

---

# 15. The Initial Symptom: Blank picocom

At first, picocom successfully opened:

```text
Terminal ready
```

but displayed no application output.

Important distinction:

```text
"Terminal ready"
```

only means:

```text
Linux opened the serial device successfully.
```

It does **not** mean:

```text
the STM32 is transmitting.
```

The next task was therefore to determine whether:

- the MCU was still paused;
- the program reached the frame loop;
- the UART was initialized;
- the sensor initialization had failed.

---

# 16. First Debugging Step: Confirm the MCU Is Running

The initial GDB session stopped at:

```text
main()
HAL_Init();
```

Pressing F8 changed the debug state to:

```text
Running
```

but picocom remained blank.

Therefore, the problem was deeper than merely a suspended MCU.

---

# 17. Using Breakpoints and Backtraces

A breakpoint was placed in:

```c
handle_error()
```

When it stopped, the most useful GDB command was:

```gdb
bt
```

Example:

```text
#0  handle_error()
#1  vl53l9_app()
#2  main()
```

Then:

```gdb
frame 1
list
```

reveals which call in `vl53l9_app()` caused the error path.

This is a very reliable debugging pattern:

```gdb
bt
frame 1
list
info locals
```

---

# 18. Why the First “Unsupported Binning” Diagnosis Was Misleading

At one point GDB reported a source location near:

```c
handle_error(); /* unsupported binning */
```

This suggested the configured binning was invalid.

However, the project was compiled using aggressive optimization:

```text
-Ofast
```

and several variables appeared as:

```text
<optimized out>
```

Optimized code can make source-line mapping confusing.

Therefore, the apparent source-line comment was not enough evidence.

---

# 19. Verify the Ranging Profile and Resolution

The ranging profile table was found at:

```bash
$BASE/Utilities/vl53l9-common/vl53l9/vl53l9_utils.c
```

The profiles included:

```text
VL53L9_USECASE_AR_RANGE
  binning = 2

VL53L9_USECASE_AR_PRECISION
  binning = 2

VL53L9_USECASE_AF_RANGE
  binning = 4

VL53L9_USECASE_AF
  binning = 4
```

The selected profile was:

```text
VL53L9_USECASE_AR_PRECISION
```

with:

```text
binning = 2
```

The resolution lookup explicitly supported binning 2:

```text
2  -> 54 x 42
4  -> 24 x 20
6  -> 18 x 14
8  -> 12 x 10
12 -> 8 x 6
24 -> 4 x 4
```

Therefore:

```text
binning 2 was valid.
```

---

# 20. Dealing with `<optimized out>`

Because `ret` and other locals were optimized away, temporary volatile globals were added:

```c
volatile int g_dbg_ret = 0;
volatile uint32_t g_dbg_usecase = 0;
volatile uint8_t g_dbg_binning = 0;
volatile uint8_t g_dbg_width = 0;
volatile uint8_t g_dbg_height = 0;
```

The observed values were:

```text
g_dbg_usecase = 1
g_dbg_binning = 2
g_dbg_ret     = 0
g_dbg_width   = 54
g_dbg_height  = 42
```

This conclusively proved the resolution lookup succeeded.

## Another powerful trick: read `r0`

On Cortex-M, a simple integer function return is normally placed in:

```text
r0
```

So immediately after a function returns:

```gdb
print/d $r0
print/x $r0
```

can reveal the return value even if a local C variable is optimized out.

---

# 21. Finding the Real Failure in `vl53l9_init()`

A later backtrace showed:

```text
#0  handle_error()
#1  vl53l9_app() at vl53l9_app.c:88
#2  main()
```

The corresponding code was:

```c
ret = vl53l9_init(p_dev);

if (ret) {
    handle_error();
}
```

So the real failure was:

```text
vl53l9_init()
```

not binning, UART, or frame processing.

A breakpoint was set:

```gdb
break vl53l9_init
continue
```

Then:

```gdb
finish
```

GDB reported:

```text
Value returned is ... = -5
```

and:

```gdb
print/d $r0
```

confirmed:

```text
-5
```

---

# 22. Understanding `VL53L9_ERROR_TIMEOUT`

The driver header defined:

```c
#define VL53L9_ERROR_NONE              (0)
#define VL53L9_ERROR_PLATFORM          (-1)
#define VL53L9_ERROR_INVALID_PARAM     (-2)
#define VL53L9_ERROR_INVALID_STATE     (-3)
#define VL53L9_ERROR_INVALID_OPERATION (-4)
#define VL53L9_ERROR_TIMEOUT           (-5)
#define VL53L9_ERROR_INTERNAL          (-6)
```

So:

```text
-5 = timeout
```

The beginning of `vl53l9_init()` was:

```c
int vl53l9_init(void *const p_dev) {
    int ret;
    vl53l9_vddio_t voltage_vddio;
    vl53l9_vdda_t voltage_vdda;
    uint32_t ext_clock;

    CHECK_NULL_PTR(p_dev);

    ret = _wait_for_state(
        p_dev,
        FSM_STATE_READY_TO_BOOT,
        4
    );
    CHECK_RET(ret);

    ...
}
```

The failure was therefore before:

```text
firmware patch upload
boot command
standby transition
calibration read
frame acquisition
post-processing
```

That narrowed the problem to the sensor's initial startup state.

---

# 23. Breaking on `_wait_for_state()`

A breakpoint was placed on:

```gdb
break _wait_for_state
```

After disabling older breakpoints that intercepted execution first:

```gdb
info breakpoints
disable 3
continue
```

the debugger stopped at:

```text
_wait_for_state(
    p_dev = 0x200009a8,
    state = FSM_STATE_READY_TO_BOOT,
    timeout_ms = 4
)
```

The stack showed:

```text
#0 _wait_for_state()
#1 vl53l9_init()
#2 vl53l9_app()
#3 main()
```

Then:

```gdb
finish
print/d $r0
```

returned:

```text
-5
```

This was the decisive software-level finding:

> The VL53L9 sensor did not reach `READY_TO_BOOT` in the reference startup sequence.

---

# 24. The Root Cause: SW1

The physical X-NUCLEO board was inspected.

The important switch is:

```text
SW1
```

This is a two-position slide switch marked:

```text
EXT / INT
```

After moving it to:

```text
INT
```

and testing again, the system worked.

The important point is that the software did **not** need a larger timeout, modified binning, rewritten I3C code, or a custom sensor patch.

The hardware setup needed to match the reference configuration.

## Final rule

For this working setup:

```text
SW1 = INT
```

should be checked before deep software debugging.

---

# 25. Testing with the Official Prebuilt Binary

To distinguish source/build problems from hardware problems, the package-supplied binary was used.

Find all matching outputs:

```bash
find "$BASE" -type f \
  \( -iname '*PostprocessSingle*.bin' \
     -o -iname '*PostprocessSingle*.hex' \
     -o -iname '*PostprocessSingle*.elf' \)
```

Two relevant `.bin` files appeared:

```text
STM32CubeIDE/Debug/53L9A1_PostprocessSingle.bin
Binary/53L9A1_PostprocessSingle.bin
```

For a clean hardware test, explicitly choose the package binary:

```bash
BIN_FILE="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Binary/53L9A1_PostprocessSingle.bin"
```

Define the programmer:

```bash
CUBEPROG_CLI=$(find /opt/st/stm32cubeide_1.13.2/plugins \
  -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)
```

Verify:

```bash
printf 'Programmer: %s\nBinary: %s\n' \
  "$CUBEPROG_CLI" "$BIN_FILE"

test -x "$CUBEPROG_CLI" && echo "Programmer OK"
test -f "$BIN_FILE" && echo "Official binary OK"
```

Flash:

```bash
"$CUBEPROG_CLI" \
  -c port=SWD freq=1000 mode=UR reset=HWrst \
  -w "$BIN_FILE" 0x08000000 \
  -v \
  -rst
```

---

# 26. Expected Successful Flashing Output

A successful flash looked like:

```text
STM32CubeProgrammer v2.14.0

ST-LINK SN  : 0026002C3235511137333439
ST-LINK FW  : V3J17M11
Board       : NUCLEO-H563ZI
Voltage     : 3.28V

SWD freq    : 1000 KHz
Connect mode: Under Reset
Reset mode  : Hardware reset

Memory Programming ...
Opening and parsing file: 53L9A1_PostprocessSingle.bin

Download in Progress:
[==================================================] 100%

File download complete

Verifying ...

Read progress:
[==================================================] 100%

Download verified successfully

MCU Reset

Software reset is performed
```

At that point the firmware was definitely present and verified in STM32 flash.

---

# 27. Expected Successful Serial Output

Open:

```bash
picocom -b 115200 \
  /dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

A working run produced:

```text
Processed frame n. 272 @ 16 fps
Processed frame n. 273 @ 16 fps
Processed frame n. 274 @ 16 fps
Processed frame n. 275 @ 16 fps
...
Processed frame n. 351 @ 16 fps
```

This was the final proof that setting:

```text
SW1 = INT
```

fixed the startup issue.

---

# 28. Why the Serial Output Can Look Diagonal

Some output appeared visually shifted:

```text
Processed frame n. 272 @ 16 fps
                               Processed frame n. 273 @ 16 fps
                                                              Processed frame n. 274 @ 16 fps
```

This is related to terminal cursor/control behavior and the example's ASCII rendering.

The source includes terminal escape sequences such as:

```c
printf("\033[%d;%dH", 0, 0);
```

The example tries to update a screen-like ASCII display rather than only printing plain lines.

Therefore, unusual cursor positioning in picocom is not evidence of a sensor failure.

The important indicators are:

```text
frame counter increases
fps remains valid
output continues
```

---

# 29. Return to the Source-Built CubeIDE Project

Once the package-supplied binary proves the hardware works:

1. remove temporary debugging variables;
2. remove temporary UART test prints;
3. remove/disable debugging breakpoints;
4. clean the project;
5. rebuild;
6. flash the local project;
7. press F8 if using Debug;
8. monitor the same ST-LINK VCP.

Useful cleanup:

```bash
APP="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle"

rm -rf "$APP/STM32CubeIDE/Debug"
```

Then in CubeIDE:

```text
Right-click project -> Refresh
Project -> Clean...
Project -> Build Project
```

Do not regenerate the project with CubeMX unless you intentionally want to rebuild the peripheral configuration.

---

# 30. Useful Shell Commands

## Define paths

```bash
BASE="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"

APP="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle"
```

## Find frame printing

```bash
grep -R -n "CONF_PRINT_FRAME" "$APP"
```

## Find print/UART redirection

```bash
grep -R -nE \
'CONF_PRINT_FRAME|printf\(|HAL_UART_Transmit|CDC_Transmit|USBD_CDC|__io_putchar|_write\(' \
"$APP"
```

## Find BSP COM initialization

```bash
grep -R -nE \
'BSP_COM_Init|BSP_COM_SelectLogPort|COM_InitTypeDef' \
"$APP/Src" "$APP/Inc"
```

## Find ranging profiles

```bash
grep -R -n "g_ranging_profiles" "$BASE" \
  --exclude-dir=Debug \
  --exclude='*.list' \
  --exclude='*.map'
```

## Display utility source

```bash
PROFILE_FILE="$BASE/Utilities/vl53l9-common/vl53l9/vl53l9_utils.c"

nl -ba "$PROFILE_FILE" | sed -n '1,180p'
```

## Display `vl53l9_init()`

```bash
VL53_SRC="$BASE/Drivers/BSP/Components/vl53l9/vl53l9.c"

nl -ba "$VL53_SRC" | sed -n '145,260p'
```

## Search error definitions

```bash
grep -R -nE \
'VL53L9.*(ERROR|STATUS|TIMEOUT|BOOT|COMM)|typedef enum' \
"$BASE/Drivers/BSP/Components/vl53l9" \
"$BASE/Utilities/vl53l9-common" \
| head -n 200
```

## Show serial devices

```bash
for device in /dev/serial/by-id/*; do
    printf '%s -> %s\n' \
        "$device" \
        "$(readlink -f "$device")"
done
```

---

# 31. Useful GDB Commands

## Continue execution

```gdb
continue
```

or use CubeIDE:

```text
F8
```

## Backtrace

```gdb
bt
```

## Change frame

```gdb
frame 1
```

## Show nearby source

```gdb
list
```

## Show local variables

```gdb
info locals
```

## Show function arguments

```gdb
info args
```

## Show breakpoints

```gdb
info breakpoints
```

## Disable breakpoints

```gdb
disable 1 2 3
```

## Delete breakpoints

```gdb
delete
```

## Break on a function

```gdb
break vl53l9_init
```

or:

```gdb
break _wait_for_state
```

## Run until the current function returns

```gdb
finish
```

## Read Cortex-M integer return value

```gdb
print/d $r0
print/x $r0
```

This was crucial for optimized code.

---

# 32. Optional XIAO ESP32-S3 UART Connection

A later planned integration is to use a XIAO ESP32-S3 as a Wi-Fi bridge.

A UART pair was selected that avoids the X-NUCLEO-53L9A1 pins.

## Wiring

```text
NUCLEO-H563ZI                         XIAO ESP32-S3

PG14 / Arduino D2 / USART6_TX   ->   D7 / GPIO44 / RX
PG9  / Arduino D12 / USART6_RX  <-   D6 / GPIO43 / TX
GND                              ---  GND
```

The UART lines must be crossed:

```text
STM32 TX -> ESP32 RX
STM32 RX <- ESP32 TX
```

Initially connect only:

```text
TX
RX
GND
```

When both boards are USB-powered, do not connect their 5V or 3.3V rails together.

## XIAO Arduino code

With the official Espressif Arduino core:

```cpp
Serial1.begin(115200, SERIAL_8N1, 44, 43);
```

This configures:

```text
RX = GPIO44
TX = GPIO43
```

Use USB `Serial` separately for debugging the XIAO.

---

# 33. Recommended Next Architecture

> **Update:** the "avoid full-frame JSON, use a binary packet" recommendation below was implemented -
> just over the *existing* ST-LINK USB-serial link directly to a PC, without adding the XIAO ESP32-S3
> Wi-Fi hop this section originally proposed. See [Section 37](#37-post-tutorial-update-pc-visualizer-wire-protocol-and-acquisition-loop-fixes)
> for the actual wire protocol shipped (`Utilities/vl53l9_visualizer.py` / `send_vis_frame()` in
> `vl53l9_app.c`) - it differs in detail from the sketch below (mixed uint16/float32 fields, a 4-byte
> string magic, CRC-16 rather than CRC32) but is the same core idea. The XIAO ESP32-S3 Wi-Fi bridge
> below is still a reasonable option if/when wireless (rather than USB-tethered) access is actually
> needed - it just wasn't the first thing built.

Now that the sensor works, a useful project architecture is:

```text
VL53L9CX
   |
   | I3C
   v
STM32H563
   |
   | raw acquisition
   | post-processing
   v
depth/amplitude frame
   |
   | USART6
   v
XIAO ESP32-S3
   |
   | Wi-Fi
   v
PC / Python / Web UI / Server
```

## Avoid full-frame JSON for production

A 54 x 42 frame contains:

```text
2268 pixels
```

Sending every value as decimal JSON wastes significant bandwidth.

For early testing, JSON is useful:

```json
{
  "frame": 123,
  "fps": 16,
  "width": 54,
  "height": 42
}
```

For actual depth-frame streaming, use a binary packet.

Example design:

```text
Header
  magic        2 bytes
  version      1 byte
  frame_id     4 bytes
  width        2 bytes
  height       2 bytes
  payload_len  4 bytes

Payload
  depth values
  optional amplitude values

Trailer
  CRC16 or CRC32
```

---

# 34. Troubleshooting Checklist

Use this order.

## A. ST-LINK not detected

Check USB:

```bash
lsusb
```

Check programmer:

```bash
"$CUBEPROG_CLI" -c port=SWD
```

---

## B. CubeIDE GDB server fails on Ubuntu 20.04

If the executable reports missing:

```text
GLIBC_2.32
GLIBC_2.33
GLIBC_2.34
GLIBCXX_3.4.29
```

use a compatible CubeIDE release such as 1.13.2 rather than upgrading system glibc manually.

---

## C. Build reports missing `Utilities/...` targets

Cause:

```text
project copied into workspace
```

Fix:

```text
import the project in-place
```

with:

```text
Copy projects into workspace
```

unchecked.

---

## D. Flash works but application serial is blank

Check:

```text
Is the CPU still stopped at main()?
```

Press:

```text
F8
```

Then confirm the correct serial device:

```bash
ls -l /dev/serial/by-id/
```

---

## E. `handle_error()` is reached

Use:

```gdb
bt
frame 1
list
```

Do not assume the nearby source comment is the real failing operation when using `-Ofast`.

---

## F. Local variables show `<optimized out>`

Use:

```gdb
finish
print/d $r0
```

or save the result in a temporary global:

```c
volatile int g_dbg_result;
```

---

## G. Binning appears unsupported

Check the actual profile and lookup table first.

For the tested package:

```text
AR_PRECISION:
  binning = 2

binning 2:
  output = 54 x 42
```

So binning 2 itself was not the problem.

---

## H. `vl53l9_init()` returns `-5`

Meaning:

```text
VL53L9_ERROR_TIMEOUT
```

Break on:

```gdb
break _wait_for_state
```

If:

```text
FSM_STATE_READY_TO_BOOT
```

times out, check hardware startup configuration first.

The actual fix here was:

```text
SW1 -> INT
```

---

## I. Unsure whether hardware or local build is wrong

Flash:

```text
Binary/53L9A1_PostprocessSingle.bin
```

from the X-CUBE package.

This removes the local compiler/build configuration from the test.

---

# 35. Lessons Learned

## 1. Check hardware switches early

A single physical switch caused a low-level driver timeout that initially looked like a software bug.

The real fix was:

```text
SW1 = INT
```

not:

```text
increase timeout
rewrite I3C
change binning
change ranging profile
modify firmware patch
```

## 2. Use the official binary as a reference

The package-supplied binary is one of the fastest ways to isolate:

```text
hardware problem
```

versus:

```text
local build problem
```

## 3. Separate debugger output from UART output

Debugger messages:

```text
Download verified successfully
Temporary breakpoint at main
```

are not application logs.

Application messages:

```text
Processed frame n. 317 @ 16 fps
```

come through the BSP UART and ST-LINK VCP.

## 4. Optimized debugging can mislead you

With:

```text
-Ofast
```

GDB may:

- hide locals;
- display `<optimized out>`;
- associate instructions with confusing source lines.

Use:

```gdb
bt
frame
list
finish
print/d $r0
```

instead of trusting only the highlighted source line.

## 5. Preserve the original X-CUBE directory structure

The project depends on relative paths.

Do not move only the `STM32CubeIDE` subdirectory out of the package tree unless you also rewrite its linked resources.

---

# 36. Quick Start: Known-Good Procedure

## Step 1: define paths

```bash
BASE="$HOME/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0"

BIN_FILE="$BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Binary/53L9A1_PostprocessSingle.bin"

CUBEPROG_CLI=$(find /opt/st/stm32cubeide_1.13.2/plugins \
  -type f -name STM32_Programmer_CLI 2>/dev/null | head -n1)
```

## Step 2: power off and verify hardware

```text
SW1 = INT
J6  = 1V8
J2-J5 installed
shield fully seated
```

## Step 3: flash the official binary

```bash
"$CUBEPROG_CLI" \
  -c port=SWD freq=1000 mode=UR reset=HWrst \
  -w "$BIN_FILE" 0x08000000 \
  -v \
  -rst
```

## Step 4: open the ST-LINK VCP

```bash
picocom -b 115200 \
  /dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_0026002C3235511137333439-if02
```

## Step 5: expected result

```text
Processed frame n. ... @ 16 fps
```

At this point, the NUCLEO-H563ZI + X-NUCLEO-53L9A1 stack is operational.

---

# 37. POST-TUTORIAL UPDATE: PC Visualizer, Wire Protocol, and Acquisition-Loop Fixes

Everything above this point documents the original hardware bring-up (the `SW1 = INT` fix) using the
stock example and the official prebuilt binary. This section documents what was built on top of that,
directly in the local source (`Src/main.c`, `Src/vl53l9_app.c` under `$APP`), and three real bugs
found and fixed along the way. All of it is currently live in the source tree, not a proposal.

## 37.1 What was added

The stock example only prints ASCII-art depth frames and `Processed frame n. ... @ N fps` text over
the ST-LINK VCP. It now also streams binary amplitude+depth+ambient frames over the same UART for a
live PC-side viewer:

```text
Utilities/vl53l9_visualizer.py
```

(a real, physical file, not a linked resource - it's a standalone PC-side tool, not part of the
firmware build graph, so unlike `Src/`, it doesn't need to live in the package tree. Currently only
present under the workspace project - see the Reference Paths section at the end for its exact path,
and Section 38 for why two project locations exist at all).

Run it (after closing picocom - a serial port can only have one owner):

```bash
python3 Utilities/vl53l9_visualizer.py -p /dev/ttyACM0 -b 3000000
```

It opens a 3-panel matplotlib window (amplitude / depth / ambient), each with its own colorbar, plus
a numeric readout (center/min/max depth in mm) so you can check actual values instead of reading
colors. `--raw` mode dumps whatever bytes arrive to stdout unparsed (like picocom) - useful when no
valid frames are parsing, to see the firmware's plain-text traces directly. `--save-npz FILE` also
records every received frame to disk.

## 37.2 Baud rate change

```c
// Src/main.c
BspCOMInit.BaudRate = 3000000;  // was 115200
```

This only affects **locally built firmware**. The official `Binary/*.bin` referenced in Section 25
is untouched and still expects 115200, exactly as documented above. If you rebuild locally, both
picocom and the Python visualizer's `--baud` must match whatever is in `main.c` at the time -
`vl53l9_visualizer.py`'s own default is `3000000` to match the current source. 115200 is too slow for
the image-sized binary payloads described below (was tested and works if you need to fall back to
it - just also drop `main.c`'s baud back down).

## 37.3 Wire protocol

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

Depth alone is quantized to `uint16` because its real range is known and safe: `MAX_DISTANCE_RANGE`
(8500) / `MAX_DISTANCE_PRECISION` (8800) in `vl53l9_transform.c` bound legitimate readings to
0-8800mm, and the library's own `12000.0f` "invalid pixel" sentinel (see 37.4) also fits comfortably
under 65535. Amplitude and ambient were *also* tried as quantized `uint16` in an earlier iteration -
it visibly clipped/saturated all texture out of the amplitude image on real hardware, because those
are raw, undocumented-range photon-count-rate values (`_signal_rate`/`_ambient_rate` internally, not
bounded like depth-in-mm), so they were reverted to native `float32` to stay lossless. For the
default AR_PRECISION usecase (54x42 = 2268 pixels) that's ~22.7 KB/frame.

Text `printf()` traces (the ASCII frames, the `Processed frame n. ...` lines) still go out the same
UART and are interleaved with the binary frames; the Python parser resyncs on the 4-byte magic and
validates CRC, so it safely ignores anything that isn't a valid `VL59` packet.

## 37.4 The 12000mm "invalid pixel" sentinel

Depth pixels that fail the library's validity checks (flying pixels, low confidence, filtered edges,
etc.) don't come back as some out-of-band flag - they come back as a specific fixed value:

```c
// Middlewares/ST/vl53l9-transform-c/vl53l9-transform-c-lib/src/vl53l9_transform.c
const float invalid_distance = 12000.0f;
```

passed directly into `_process_distance_check()` / `vl53l9_algo_distance_check()`. If you see
`depth == 12000` (or, after uint16 quantization, exactly `12000`), that pixel is not a real 12-meter
measurement - the VL53L9 doesn't range that far - it's the algorithm marking the pixel invalid. The
Python visualizer masks this explicitly (`INVALID_DEPTH_MM = 12000`, `is_valid_depth = (depth > 0) &
(depth < INVALID_DEPTH_MM - 1)`) rather than the naive `depth > 0` check the first iteration used,
which let the sentinel dominate the colorbar and wash out real data by comparison.

## 37.5 Amplitude vs. ambient (physical meaning)

Both render as brightness images but measure physically different things:

- **Amplitude** (`_signal_rate`) = strength of the sensor's *own* reflected laser (VCSEL) pulse per
  zone. Depends on target distance (falls off ~inverse-square), reflectivity, and angle of incidence.
  A quasi-image: bright = close/reflective/facing the sensor, dark = far/absorptive/grazing angle.
- **Ambient** (`_ambient_rate`) = background/environmental IR light per zone, independent of the
  sensor's own laser (sunlight, indoor lighting, etc.) - a scene-lighting / SNR indicator. High
  ambient degrades the sensor's ability to pick its own laser return out of the noise floor.

Both are exposed as separate `transform_set_stream_capabilities()` streams (`"amplitude"`, format
`AF32`; `"ambient"`, format `IF32`) alongside `"depth"` (`ZF32`), all at the same resolution, verified
against `vl53l9_transform.c` to be first-class supported streams (not gated behind the library's
`VL53L9_TRANSFORM_LIGHT` reduced-feature flag).

## 37.6 Three real bugs found and fixed in the acquisition loop

All three were found by adding diagnostics (see 37.7) after the streaming feature above initially
appeared to make frame acquisition fail - it did not; these were real, pre-existing gaps in the
acquisition loop, reachable independently of the streaming feature, just previously silent.

**Bug 1 - discarded return value.** The stock loop called:

```c
vl53l9_trigger_frame(p_dev);
if (ret) {              // `ret` is stale from setup, never actually reassigned here
    handle_error();
}
```

`vl53l9_trigger_frame()`'s result was never captured, so any trigger failure was silently ignored.
Fixed by capturing it (now folded into the retry loop in Bug 3).

**Bug 2 - stale interrupt flag before the first wait.** `main.c`'s `MX_GPIO_Init()` enables the
`EXTI7` interrupt (the sensor's `INTR` pin) at boot, before `vl53l9_app()` even runs. Any falling
edge during the reset/init/calibration/prepare/start sequence - for any reason, not necessarily
"frame ready" - latches a sticky flag (`g_platform_evt |= PLATFORM_GPIO_IT_EVT` in
`platform_utils.c`'s `HAL_GPIO_EXTI_Falling_Callback()`) that nothing clears before the main loop
starts. The *first* `platform_wait_for_event(PLATFORM_GPIO_IT_EVT, ...)` call could therefore return
immediately on stale state instead of the real post-trigger interrupt, so the "frame" it found was
never actually ready. Fixed with one `platform_acknowledge_event(PLATFORM_GPIO_IT_EVT)` flush right
after `vl53l9_start()` succeeds, before the loop begins.

**Bug 3 - no tolerance for transient acquisition faults.** Even with Bug 2 fixed, one raw frame
acquisition (trigger -> wait for IRQ -> read) was observed on real hardware to fail three different
ways across separate runs: a stale interrupt flag (Bug 2, before the fix), a full 1000ms IRQ-wait
timeout with nothing arriving, and a sensor-reported transient fault (`status.error` bit
`sof_outside_blanking`, read via `vl53l9_get_status()`). A different failure point each time is the
signature of a genuine transient timing hiccup, not one deterministic bug - so the stock behavior
(kill the whole application on the first failure) was replaced with a bounded retry:

```c
#define ACQUIRE_MAX_RETRIES (5)
// ... trigger -> wait -> read wrapped in a for-loop up to ACQUIRE_MAX_RETRIES attempts,
// flushing the stale-flag before each attempt, only calling handle_error() after 5
// consecutive failures. This also subsumes Bug 1 (trigger's return value is now checked).
```

## 37.7 Diagnostics added (worth knowing about even if you never hit a bug)

- `handle_error()` used to spin forever with **zero** UART output, making every possible failure look
  identical (indistinguishable from a hang). It's now a macro (`#define handle_error()
  handle_error_impl(__LINE__)`) that prints the exact source line plus `vl53l9_get_status()`'s fsm/
  command/firmware-error/error-bits before spinning - grep `vl53l9_app.c` for the printed line number
  to find the exact failing check.
- `printf()` checkpoints after every major setup stage (`vl53l9_init`, calibration read,
  `transform_initialize`, stream capabilities, `transform_prepare`, `vl53l9_start`, "entering main
  loop") so a hang shows exactly how far setup got even without a clean `handle_error()` call (e.g. a
  crash before reaching it).
- `vl53l9_visualizer.py --raw` (see 37.1) is the fastest way to read all of the above without
  needing a debugger session - just picocom-style raw serial output.

## 37.8 fps characteristics

Bandwidth is the dominant cost for the binary stream, but not the only one. At the current ~22.7
KB/frame mixed-precision payload and 3,000,000 baud, pure UART transmission time is ~75.6ms/frame
(~13fps ceiling from bandwidth alone). An earlier, smaller all-`uint16` 13.6KB/frame version (since
reverted - see 37.3) measured **9.7 fps** on hardware, well under its ~22fps bandwidth-only estimate -
meaning roughly half the per-frame time was *not* UART transmission. The likely remaining
contributors are the sensor's own native ~33ms frame period (the configured profile targets 30fps)
and `transform_process_stream()`'s on-MCU pipeline cost (TNR, radial-to-perpendicular, sharpener,
rate-normalization, reflectance, flying-pixel filter, distance-check stages, all running on the
Cortex-M33 every frame). This has not been instrumented with hard per-phase timing to confirm the
exact split - noted here as a known open question, not a solved one. Practical takeaway: once you're
past a few Mbps, further baud increases give diminishing returns, because the bottleneck stops being
purely the wire.

---

# 38. POST-TUTORIAL UPDATE: Two Project Locations for the Same Source

There are two separate STM32CubeIDE project directories on this machine, both named
`53L9A1_PostprocessSingle`, both with **identical** `.project`/`.cproject` files, both compiling the
same real source (`$APP/Src/main.c`, `$APP/Src/vl53l9_app.c`, etc. via linked resources) - but they
are genuinely separate directories on disk, not symlinks of each other:

```text
1. $BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE
   (the project bundled inside the X-CUBE package itself - what Section 6/7 above tells you to import)

2. ~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle
   (a separate project living outside the package tree, in a default Eclipse workspace folder)
```

**They are not interchangeable for command-line builds.** Both use Eclipse linked-resource path
variables like `PARENT-6-PROJECT_LOC` in their generated `subdir.mk` files, which resolve *relative
to each project's own physical location*. For project 1 (inside the package tree), that math lands
correctly on the real `Utilities/`, `Drivers/`, `Middlewares/` folders - confirmed by actually running
`make` from `$BASE/.../STM32CubeIDE/Debug`, which built successfully. For project 2 (outside the
package tree), the identical relative-path math lands on nonexistent paths like
`/home/siyuchen/Utilities/...` - confirmed by running `make` there, which fails immediately with:

```text
make: *** No rule to make target '/Utilities/vl53l9-common/platform/platform_utils.c', ...
```

**Confirmed root cause**: project 2 exists because the sample project was downloaded and imported a
*second* time, separately from the full X-CUBE-53L9A1 package extraction described in Section 5 -
imported "without copying" each time (per Section 7's correct procedure), so Eclipse never literally
duplicated files on either import. The breakage isn't a copy-paste mistake; it's that this second
download/import landed at `~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle` without
its own sibling `Utilities/`, `Drivers/`, `Middlewares/` folders alongside it (those only exist
inside the full package extraction from Section 5, at `$BASE`). The `.project` file's linked
resources are identical either way (both imports came from the same upstream sample), but
`PARENT-N-PROJECT_LOC` paths resolve relative to *each project's own location* - from inside the
complete `$BASE` package tree that math reaches the real shared folders; from the standalone
workspace location there's nothing for it to reach. Same underlying lesson as Section 7's original
warning (the project depends on sitting inside its package's full directory structure), just via
a second download rather than a copy checkbox.

**Practical implications:**
- **STM32CubeIDE's GUI** resolves linked resources correctly for whichever project you open,
  regardless of location - both projects build and flash fine from inside the IDE. If you build from
  inside CubeIDE, this whole section is not something you need to worry about day to day.
- **Command-line/headless builds** (`make` from a terminal, useful for scripting or for an assistant
  without GUI access) only work from project 1 (`$BASE/.../STM32CubeIDE`). Don't try `make` from
  project 2 - fix the broken paths would mean re-importing it correctly rather than patching the
  generated makefiles.
- **Editing source**: always edit the real files under `$BASE/Projects/.../53L9A1_PostprocessSingle/Src/`
  (and `$BASE/Utilities/`, `$BASE/Middlewares/`, etc. for the shared libraries) - never a copy that
  might exist under project 2's own folder structure, since project 2 has no real files of its own
  for anything covered by a linked resource (only genuinely local files like `syscalls.c`/`sysmem.c`
  live physically inside it).
- Both projects' `Debug/` output directories can independently contain build artifacts from whichever
  one was built most recently - if you're checking "was my latest change actually built", check the
  `.elf`/`.bin` timestamp in *both* `Debug/` folders, not just one.

---

# 39. POST-TUTORIAL UPDATE: Corrections to Sections Above

Specific factual corrections found while verifying sections 1-36 against the current source (not
speculative - each was checked directly against the file in question):

| Section | Claim | Correction |
|---|---|---|
| 1, 12, 27, 36 | Serial baud rate is `115200` | Still correct for the **official prebuilt binary**. The **locally built** firmware now uses `3000000` (see 37.2) - match whichever binary you actually flashed. |
| 13 | `#define CONF_PRINT_FRAME (1)` | Current source reads `#define CONF_PRINT_FRAME (0)` (`Src/vl53l9_app.c`, verified directly). ASCII-art frame printing is currently **disabled**; only the `Processed frame n. ... @ N fps` text line and (now) the binary visualizer stream are emitted per frame. If you need the ASCII art back, flip this back to `1` - it's a cheap, harmless toggle. |
| 6, 7 | Only one project location is discussed | See Section 38 - a second, non-interchangeable project directory now also exists. |

Everything else checked (the SW1 hardware fix, the ranging profile/resolution table in Section 19,
the `VL53L9_ERROR_*` codes in Section 22, the GDB debugging workflow, the official-binary flashing
procedure) still matches the current source/hardware and needed no correction.

---

# Reference Paths

```text
X-CUBE root:
~/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0

Application:
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle

CubeIDE project (in-package - use this one for command-line builds, see Section 38):
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/STM32CubeIDE

Second CubeIDE project (outside the package tree - GUI-only, see Section 38):
~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle

Official binary:
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Binary/53L9A1_PostprocessSingle.bin

Application source (edit these, not any project-local copy):
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Src/main.c
Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle/Src/vl53l9_app.c

PC-side visualizer (Section 37) - a standalone PC tool, not part of the firmware build graph, so
its exact location doesn't matter the way Src/ does; currently only physically present at:
~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle/Utilities/vl53l9_visualizer.py
(not (yet) copied into the in-package project's own Utilities/ folder)

VL53L9 driver:
Drivers/BSP/Components/vl53l9/vl53l9.c

VL53L9 profile utilities:
Utilities/vl53l9-common/vl53l9/vl53l9_utils.c

VL53L9 platform/event glue (Section 37.6, Bug 2):
Utilities/vl53l9-common/platform/platform_utils.c

Depth-processing pipeline (Section 37.4/37.5 - invalid-distance sentinel, amplitude/ambient source):
Middlewares/ST/vl53l9-transform-c/vl53l9-transform-c-lib/src/vl53l9_transform.c
```

---

# Final Verified Result

The successful final state was:

```text
SW1: INT
J6 : 1V8
```

The package-supplied binary was flashed and verified successfully.

The ST-LINK virtual COM port then continuously reported:

```text
Processed frame n. 272 @ 16 fps
Processed frame n. 273 @ 16 fps
Processed frame n. 274 @ 16 fps
...
Processed frame n. 351 @ 16 fps
```

The most important diagnostic signature to remember is:

```text
_wait_for_state(FSM_STATE_READY_TO_BOOT, ...)
```

returning:

```text
VL53L9_ERROR_TIMEOUT (-5)
```

For this hardware setup, that symptom was resolved by correcting the X-NUCLEO-53L9A1 switch configuration:

```text
SW1 = INT
```

## Update: current milestone (see Section 37)

Since the result above, the locally-built firmware (not the official binary - that remains as
described above, untouched) reached a second milestone: continuous binary amplitude+depth+ambient
streaming to a live PC-side viewer, at 3,000,000 baud, with three real acquisition-loop bugs found
and fixed along the way (Section 37.6) and the transform library's `12000.0f` invalid-depth sentinel
correctly identified and masked (Section 37.4). As of this update, real depth/amplitude/ambient data
displays correctly on the PC (verified: amplitude/ambient preserve full dynamic range as float32,
depth correctly excludes invalid sentinel pixels); the mixed-precision wire format in Section 37.3
has been implemented and unit-tested on the PC side but not yet independently re-confirmed with a
fresh on-hardware fps measurement (the 9.7fps figure in Section 37.8 is from the prior, smaller
all-`uint16` payload, since superseded).
