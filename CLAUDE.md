# CLAUDE.md

STM32 NUCLEO-H563ZI + X-NUCLEO-53L9A1 (VL53L9CX ToF sensor) on Ubuntu 20.04,
STM32CubeIDE 1.13.2, X-CUBE-53L9A1 V1.0.0, ST-LINK V3.

---

## Working rules

**When the user says "your code is wrong" or "check your code" — audit my own code. That is an
instruction, not a conversation starter.** Do not respond by handing back something for them to go
check. Read the actual source on both sides of the interface, trace the logic, and find the bug.

**When the user says they have verified something (wiring, config, hardware, environment), it is
verified.** Remove it from the hypothesis space entirely — do not re-raise it later in a different
form. Re-asking costs them real bench time and is usually me avoiding harder work. If I genuinely
exhaust the software side and a physical cause is the only remaining candidate, say so once,
explicitly, and ask before sending them back to the hardware.

**Diagnostic principle learned here the hard way:** if identical code fails *differently* on two
different chips/environments, the bug is in how my code interacts with each environment — not in a
shared physical layer. A common physical fault corrupts both the same way.

**Never delete or overwrite `secrets.h`** (gitignored, holds real WiFi credentials) in this project
or in `~/Arduino/arduino_projects`. It has been destroyed once already. Do not touch it without
asking, even incidentally.

Do not commit or push unless explicitly asked.

---

## Where the real source lives — read before editing

Almost every file in the CubeIDE project is an Eclipse **linked resource**. Editing inside the
workspace project folder does nothing (or fails). **Always edit the real files under the resource
dir:**

```
$BASE = ~/STM32/resource/X-CUBE-53L9A1/STM32CubeExpansion_53L9A1_V1.0.0
$APP  = $BASE/Projects/NUCLEO-H563ZI/Applications/53L9A1/53L9A1_PostprocessSingle
```

Application source is `$APP/Src/*.c` and `$APP/Inc/*.h` — `main.c`, `vl53l9_app.c`,
`stm32h5xx_hal_msp.c`, `stm32h5xx_it.c`.

**There are TWO separate CubeIDE project dirs** for the same app, physically distinct, not symlinked:

| Project dir | Command-line `make` |
|---|---|
| `$APP/STM32CubeIDE/` (bundled, inside the package tree) | **Works** — use this one |
| `~/STM32/STM32CubeIDE/workspace_1.13.2/53L9A1_PostprocessSingle/` | Broken (linked-resource paths resolve outside the package tree) |

Both compile the same real source via linked resources. When checking "was my edit actually built,"
check both `Debug/*.elf` timestamps.

---

## NEVER flash the board myself

**I may edit code and build it. Building exists for exactly one purpose: to verify the code compiles
and links.** That is the entire scope. It is not a step on the way to flashing and it never implies
permission to flash.

**Flashing either board is the user's action alone. Never mine. No exceptions** — not when a script
exists to do it, not mid-debugging, not when it would save the user a step, not when it seems
obviously helpful. I flashed the STM32 three times without asking. The user was clear this must
never happen again.

Do not run `flash_stm32.sh`, `STM32_Programmer_CLI`, `arduino-cli upload`, `esptool`, or anything
else that writes to a device. Print the command; the user runs it.

**This is enforced by the harness, not by my judgment.** A `PreToolUse` hook on `Bash` —
`~/.claude/hooks/block-device-flash.sh`, registered in `~/.claude/settings.json` — inspects every
shell command and denies anything matching a device-programming tool (`flash_stm32.sh`,
`STM32_Programmer_CLI`, `st-flash`, `stm32flash`, `openocd`, `dfu-util`, `avrdude`, `esptool`,
`pyocd`, `JLinkExe`, `arduino-cli upload|burn-bootloader`). It is user-scoped, so it also covers
flashing the XIAO from `~/Arduino`. Deliberately fail-closed: it also blocks `cat`/`grep` on a path
containing `flash_stm32.sh` — use the Read/Grep tools for that instead of loosening the pattern.

## Flashing — the user runs these, not me

```bash
./flash_stm32.sh [marker-string]     # builds + flashes + verifies the marker landed
./capture_logs.sh [seconds]          # records BOTH boards into ~/STM32/{stm32,xiao}.log
```

**Why this matters:** this is a two-board system, and one side can silently be a round behind. A
diagnostic build was once analysed against an STM32 still running the previous round's firmware
(the XIAO had been reflashed, the STM32 had not) — the experiment measured nothing and the logs
still looked plausible. `flash_stm32.sh` always builds and flashes the same binary and **refuses to
flash unless a marker string from the new source is present in the ELF**.

Note: the IDE builds the *bundled* tree — the same `Debug/` that command-line `make` writes to — so
there is no split-tree flashing trap, despite the two project dirs described below.

When a fix seems to have no effect, verify what is actually on the chip before theorizing:
`strings <Debug>/53L9A1_PostprocessSingle.elf | grep <a string only the new code has>`, and check
both `Debug/*.elf` timestamps.

## Building

The toolchain is not on `PATH`. Build the bundled project:

```bash
export PATH="/opt/st/stm32cubeide_1.13.2/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.linux64_1.1.1.202309131626/tools/bin:/opt/st/stm32cubeide_1.13.2/plugins/com.st.stm32cube.ide.mcu.externaltools.make.linux64_2.1.0.202305091550/tools/bin:$PATH"
cd "$APP/STM32CubeIDE/Debug" && make -j4 all
```

Recipes assume cwd is `Debug/` (relative `-I../../Inc`) — do not `cd` into subdirectories.
Always do a real `make all` link, not just `-fsyntax-only`; several bugs here only surfaced at link.

**Adding a HAL peripheral this project has never used** (e.g. SPI4, added by hand rather than via
CubeMX) needs three separate things, and missing any one produces a confusing failure:

1. `#define HAL_..._MODULE_ENABLED` in `$APP/Inc/stm32h5xx_hal_conf.h` (else unknown types)
2. A `<link>` entry in **both** `.project` files for each new HAL `.c` (else undefined references
   at link — CubeIDE derives its generated `subdir.mk`/`objects.list` from `.project`'s
   `<linkedResources>`, *not* from a folder scan, and regenerates over any hand-patch)
3. Refresh (F5) the project in the IDE after editing `.project` externally — Eclipse caches its
   resource tree

---

## Data paths out of the sensor

Both are enabled together in `$APP/Src/vl53l9_app.c` (`CONF_STREAM_VISUALIZER`, `CONF_STREAM_SPI`)
and share the same frame layout and `crc16_ccitt()`:

1. **UART → PC** — 3,000,000 baud over the ST-LINK VCP → `Utilities/vl53l9_visualizer.py`.
   Note this is *binary frames interleaved with printf text*, so raw `picocom` looks like garbage —
   that is expected. To read just the text: `stty -F <port> 3000000 raw -echo && timeout N cat <port> > f.log && strings f.log`
2. **SPI4 → XIAO ESP32 → WiFi webpage** — XIAO side lives in
   `~/Arduino/arduino_projects/seeed_xiao/stm32_utility/spi/`. Verify with
   `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32{S3,C6,C5}` (must build for all three).

### ESP32 SPI slave gotchas (cost a full debugging session)

Authoritative reference — **read it before theorizing**:
<https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_slave.html>

- `spi_slave_queue_trans()` returns **before** the slave hardware is armed — it only posts to a
  FreeRTOS queue.
- **Drive the READY handshake from the driver's ISR callbacks, never from `loop()`.**
  `post_setup_cb` = hardware genuinely armed → raise READY. `post_trans_cb` = transaction ended →
  drop READY immediately. Doing it in `loop()` leaves READY stale-HIGH after a transfer, and the
  master reads that as "armed" and clocks into a deaf slave. Use ISR-safe `gpio_set_level()` +
  `IRAM_ATTR`. The master must use an **edge** handshake (wait LOW, then HIGH), not a level one.
- `spi_slave_get_trans_result() == ESP_OK` means only that CS toggled; it does **not** mean the
  requested bytes arrived. **Always check `trans_len`**, and zero the RX buffer between transactions
  so a short transfer can't be mistaken for real data.
- **A `trans_len` that is not a multiple of 8 means the slave is dropping SCK edges** (clock too
  fast / bad duty cycle) — a master cannot clock a partial byte. A byte-aligned short count means
  CS/framing timing instead. This distinction is the fastest way to split those two causes.
- The frame is sent in **12 chunks of 2048 B** (`SPI_CHUNK_BYTES`), not one 22692-byte transaction.
  That constant is a **wire-protocol** value — change it on both sides and reflash both together.
- DMA RX buffers must be word-aligned *and* a multiple of 4 bytes long.
- Not a limit, despite appearances: with DMA enabled, slave transfer size is bounded only by
  internal memory (4092 is just the `max_transfer_sz` default), and GPIO-matrix vs IO_MUX routing
  makes no difference below 80 MHz.

When changing an SCK divider, re-check every timeout constant derived from the old clock rate.

---

## Authoritative reference

`$BASE/Documentation/NUCLEO_H563ZI_XNUCLEO_53L9A1_Ubuntu20_Detailed_Tutorial.md` — 13 purpose-based
sections covering bring-up, build, flashing, visualizer, the SPI bridge, and troubleshooting. It is
kept up to date. **Check it before re-deriving anything about this project**, and update it when
project-level facts change.

Hardware note that blocks everything if wrong: **`SW1 = INT`** on the X-NUCLEO-53L9A1.

## SPI4 wiring — the SCK pin trap

| Signal | STM32 pin | Header location |
|---|---|---|
| SPI4_SCK | **PE12** | **Zio connector only — no Arduino D-number** |
| SPI4_MISO | PE13 | Arduino D3 |
| SPI4_MOSI | PE14 | Arduino D4 |
| SPI4_NSS (soft CS) | PE11 | Arduino D5 |
| XIAO_READY (input) | PE9 | Arduino D6 |

**Never wire the clock to the header pin silkscreened `SCK` (D13).** That pin is **PA5 =
SPI1_SCK/SPI6_SCK**; SPI4 physically cannot drive it (verified in ST's CubeMX pin database).

This exact mistake cost a full day. Its signature is diabolical: CS, MOSI and the READY handshake
all work, the master reports every transfer `HAL_OK`, and the slave still reports `trans_len == 0`
— or, at higher clocks/longer transfers, plausible-looking *aliased garbage* from a floating input
picking up crosstalk off the neighbouring wires. **When describing this wiring, always identify SCK
as "the pin labelled PE12", never as "SCK"** — the board has a differently-named pin wearing that
label.
