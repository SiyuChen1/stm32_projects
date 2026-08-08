/**
 ******************************************************************************
 * @file    vl53l9_app.c
 * @author  IMD Software Team
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vl53l9.h"
#include "vl53l9_device.h"
#include "vl53l9_interface.h"
#include "vl53l9_transform.h"
#include "vl53l9_utils.h"
#include "runtime_control.h"

/* USER CODE BEGIN Includes */
#include "stm32h5xx_nucleo.h" /* hcom_uart[], COM1, HAL_UART_Transmit - used to stream binary frames to the PC */
#include "main.h" /* SPI4_NSS_Pin/XIAO_READY_Pin etc. - used to stream binary frames to a XIAO over SPI4 */

extern SPI_HandleTypeDef hspi4; /* defined in main.c, initialized by MX_SPI4_Init() */
/* USER CODE END Includes */

/* application customization */
#define CONF_DEVICE_ID   (0) /**< select device entry in platform descriptor array (see vl53l9_device.c) */
#define CONF_PRINT_FRAME (0) /**< enable printing depth frames as ascii art (slows performance) */
#define CONF_USECASE     (VL53L9_USECASE_AR_PRECISION) /**< select ranging profile to be applied (see vl53l9_utils.h) */

/**
 * @brief Enable streaming amplitude+depth frames to the PC as a binary packet over the COM1 UART.
 * Pair with Utilities/vl53l9_visualizer.py on the host.
 *
 * TURNED OFF for throughput. Measured cost: 83ms of a 210ms frame (40%) - 22692 bytes at 3Mbaud is
 * 75.6ms of pure line time plus per-call overhead, and it is a blocking transmit, so it is pure
 * added latency on every frame. The XIAO/WiFi path now carries the same data. Set back to 1 if you
 * need the tethered PC visualizer again, accepting ~5fps. Requires BspCOMInit.BaudRate in main.c
 * to be raised (see comment there) since 115200 bps is far too slow for image-sized payloads.
 */
#define CONF_STREAM_VISUALIZER (0)

/**
 * @brief Enable bridging the same amplitude+depth+ambient frame to a XIAO ESP32 over SPI4 (SCK=PE12,
 * MISO=PE13, MOSI=PE14, software NSS=PE11, XIAO_READY handshake input=PE9), for the XIAO to re-serve
 * as a live webpage over WiFi instead of requiring a PC tethered to the STM32's UART. Independent of
 * CONF_STREAM_VISUALIZER - either or both can be on. See stm32_utility/spi/spi.ino in the separate
 * Arduino repo for the receiving side and the full wire/handshake protocol writeup.
 */
#define CONF_STREAM_SPI (1)

/**
 * @brief Max consecutive retries for one frame's trigger/wait/read sequence before giving up and
 * calling handle_error(). Observed on hardware: this sequence intermittently fails in different ways
 * (stale interrupt flag, IRQ wait timeout, sensor-reported transient fault) on different runs - a
 * transient timing hiccup, not a persistent fault - so it's worth a few retries before treating it as
 * fatal. Worst case adds up to ACQUIRE_MAX_RETRIES seconds of latency to a single frame (each failed
 * attempt can wait up to the 1000ms IRQ timeout).
 */
#define ACQUIRE_MAX_RETRIES (5)

/**
 * @brief Print a per-stage timing breakdown of the acquisition loop once per second.
 *
 * The XIAO receives every frame this board produces with zero errors, so the frame rate is set
 * entirely here, not by the SPI link. Measured 4.7fps = ~213ms/frame, of which UART (75.6ms at
 * 3Mbaud) and SPI (50.3ms at 3.9MHz) only account for ~126ms - so something else owns the other
 * ~87ms. Rather than guess which stage, time them all. Turn off once the pipeline is tuned.
 */
#define CONF_PROFILE_TIMING (1)

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/**
 * @brief Wire format of one visualizer packet, sent as: header, then `width*height` little-endian
 * float32 amplitude samples, then `width*height` little-endian uint16 depth samples (mm, rounded to
 * nearest), then `width*height` little-endian float32 ambient samples.
 *
 * Depth is quantized to uint16 because its real range is known and safe (0-8800mm per
 * MAX_DISTANCE_RANGE/MAX_DISTANCE_PRECISION in the transform library, plus a 12000mm "invalid pixel"
 * sentinel - see distance_check in vl53l9_transform.c - all comfortably under 65535). Amplitude and
 * ambient are the library's raw "signal_rate"/"ambient_rate" photon-count-rate values, whose true
 * dynamic range isn't documented/bounded the same way - quantizing those to uint16 was tried and
 * visibly clipped/saturated real texture (confirmed on hardware), so they're sent as float32 to stay
 * lossless. Kept in sync with the HEADER_FMT/parsing logic in Utilities/vl53l9_visualizer.py.
 */
typedef struct __attribute__((packed)) {
    uint8_t magic[4];    /**< 'V','L','5','9' - lets the host resynchronize on the byte stream */
    uint32_t frame_counter;
    uint8_t width;
    uint8_t height;
    uint16_t crc16;       /**< CRC-16/CCITT (poly 0x1021, init 0x0000) over the amplitude+depth+ambient payload */
} vis_frame_header_t;

/**
 * @brief Max resolution across all binning values this sensor supports (see the resolution LUT in
 * vl53l9_utils.c - the largest entry is 54x42 for binning=2). Sizes the quantization scratch buffers
 * in send_vis_frame() regardless of which CONF_USECASE profile is selected above.
 */
#define VIS_MAX_PLANE_PIXELS (54U * 42U)

static void print_frame(float *p_frame, size_t height, size_t width);
static memory_t allocate_memory(uint16_t size);
static void handle_error_impl(int line);
/**
 * @brief handle_error() used to spin forever with zero UART output, which made every failure
 * indistinguishable from a hang. This macro captures the call site's line number so the printout
 * below tells you exactly which check tripped - grep vl53l9_app.c for that line number.
 */
#define handle_error() handle_error_impl(__LINE__)
#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
/**
 * @brief Shared by both the UART (send_vis_frame) and SPI (send_vis_frame_spi) transports below, so
 * there's exactly one CRC implementation to keep in sync with the receiving ends - not two copies
 * that could silently drift apart. Declared/defined under this combined guard so it's still available
 * if CONF_STREAM_VISUALIZER is ever turned off while CONF_STREAM_SPI stays on (or vice versa).
 */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc);
#endif
#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
static void send_vis_frame(uint32_t frame_counter, uint8_t width, uint8_t height, const float *amplitude,
                            const float *depth, const float *ambient);
#endif
#if CONF_STREAM_SPI
/**
 * @brief Maximum image storage for the all-plane format. The active payload can be smaller when
 * one plane is selected; the negotiated chunk count includes that payload plus the status trailer.
 * Matches SPI_FRAME_BYTES in the XIAO's spi.ino exactly:
 * 12 (header) + 2268 * (4+2+4) = 22692 bytes.
 */
#define SPI_FRAME_BYTES (sizeof(vis_frame_header_t) + VIS_MAX_PLANE_PIXELS * (sizeof(float) + sizeof(uint16_t) + sizeof(float)))
/* A frame goes out as several transactions, each its own CS assert + READY handshake, rather than
 * one 22692-byte transfer. The boot-safe default is 12 x 2048; runtime control negotiates other
 * supported sizes at a frame boundary. Espressif's SPI slave docs note the receiver
 * "cannot recognize or receive data correctly if the clock is too fast", and that hosts should
 * write lengths that are multiples of 4 bytes. Empirically a single full-frame transaction died
 * ~4000 bits in, at bit counts that were NOT multiples of 8 - a master cannot clock a partial
 * byte, so the slave was dropping SCK edges and losing sync mid-transaction. Chunking gives it a
 * clean CS resync point 12x per frame and lets the receiver verify each piece independently.
 * Both peers keep the last stable chunk size and revert independently if verification fails. */
/* DIAGNOSTIC MODE. Set to 1 to send a known byte ramp instead of real sensor data: every chunk is
 * filled so that byte k of the whole padded frame == (k & 0xFF). The XIAO has a matching flag and
 * checks each received chunk against that ramp, reporting the first mismatching offset. This takes
 * frame layout, CRC and chunk assembly entirely out of the picture so the raw link can be measured
 * on its own - specifically it distinguishes "no data at all" from "data arrives but shifted by N
 * bytes" (which means the slave armed mid-transfer) from "data arrives but corrupted" (electrical
 * or SPI mode). Set back to 0 once the link is proven. Must match SPI_TEST_PATTERN in spi.ino. */
/* RAW LINK TEST. Set to 1 to bypass SPI entirely and drive SCK/MOSI/NSS as plain GPIO outputs at
 * three different slow rates, so the XIAO (matching flag in spi.ino) can count transitions on each
 * wire independently of any SPI configuration on either side.
 *
 * Why this exists: with a 64-byte transfer at 976kHz the master reports every frame sent OK and the
 * handshake never errors, yet the slave counts ZERO clock edges - even during all-FF, where MOSI is
 * held high the whole transfer. CS demonstrably works (transactions start and end, and the READY
 * edge handshake succeeds 37/37), so the slave is seeing CS but not SCK. Everything checkable in
 * software on the STM32 side has been verified against primary sources, so the next thing to
 * measure is the wires themselves - one at a time, at a speed where nothing subtle can matter.
 *
 * NSS/CS is the built-in control: we already know that path works, so if the XIAO counts NSS
 * transitions but not SCK transitions, that is a measured per-wire result with a working reference,
 * not a guess. Set back to 0 to resume normal operation. */
#define SPI_LINK_GPIO_TEST (0)

#define SPI_TEST_PATTERN (0)
/* In test mode, shrink the problem to the smallest thing that could possibly work: ONE transaction
 * of SPI_TEST_BYTES per frame, at the slowest clock the prescaler offers (/256, ~976kHz - see
 * MX_SPI4_Init). Everything on the STM32 side has been verified correct against primary sources
 * (pin + AF5 against ST's CubeMX database, the clock chain against SystemClock_Config and the CMSIS
 * headers, HAL_SPI_Transmit against the HAL source), and the master reports all chunks sent, yet
 * the slave still captures ~25% of the bits with a beating/aliased pattern. So rather than keep
 * inspecting a 2048-byte 12-chunk transfer, bisect: if 64 bytes at 976kHz arrives perfectly, the
 * link is sound and the fault is size/timing dependent, and the size can be walked back up to find
 * the breaking point. If even this fails, the fault is fundamental and independent of both. */
#define SPI_TEST_BYTES (64)
#define SPI_DEFAULT_CHUNK_BYTES (2048U)
#define SPI_MAX_PADDED_FRAME_BYTES (24576U)
#define SPI_READY_TIMEOUT_MS (200)   /**< how long to wait for the XIAO's READY handshake before skipping this frame */
/* ~92.9ms of actual transfer at the current 1.95MHz (SPI_BAUDRATEPRESCALER_128) SCK - was 100ms
 * (a ~7% margin, not "generous") until this got caught: I dropped the clock 8x when testing a
 * crosstalk hypothesis but forgot this constant assumed the old 15.625MHz timing. 500ms gives
 * real headroom against interrupt latency (I3C/TIM3/USB) stretching the transfer. Re-check this
 * value again if the clock is ever raised back toward /16. */
#define SPI_TRANSFER_TIMEOUT_MS (500)
/* Guard delays around the SPI handshake - see the long comment in send_vis_frame_spi(). These
 * cover the gap between the XIAO's spi_slave_queue_trans() returning (which is when it raises
 * READY) and its SPI slave hardware actually being armed by the driver's own ISR/task context.
 * 200us is deliberately generous: at 4fps this costs 0.08% of the frame budget, so there is no
 * reason to trim it close. Only worth revisiting if the frame rate ever climbs high enough that
 * a fifth of a millisecond per frame actually matters. */
#define SPI_READY_TO_CS_DELAY_US (200)
#define SPI_CS_SETUP_DELAY_US (10)
#define SPI_CS_HOLD_DELAY_US (10)

/**
 * @brief Busy-wait for approximately @p us microseconds.
 *
 * HAL_Delay()'s 1ms granularity is far too coarse for the sub-millisecond guard delays the SPI
 * handshake needs, and its tick-phase rounding means HAL_Delay(1) can return almost immediately.
 * This is only ever called with small values around a single SPI transaction, so a NOP loop is
 * simpler than standing up DWT's cycle counter. `volatile` is what keeps -Ofast (this project's
 * optimization level) from deleting the loop outright as having no observable effect.
 */
#if SPI_LINK_GPIO_TEST
/**
 * @brief Drive SCK/MOSI/NSS as plain GPIO outputs at three distinct rates, forever. Never returns.
 *
 * Each wire gets its own toggle rate so the receiving side can tell them apart by transition count
 * alone, with no shared clock, no framing and no SPI peripheral involved on either end:
 *   SCK  (PE12) toggles every  50ms -> ~20 transitions/sec
 *   MOSI (PE14) toggles every 100ms -> ~10 transitions/sec
 *   NSS  (PE11) toggles every 200ms -> ~5  transitions/sec
 */
static void spi_link_gpio_test(void) {
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* Take SCK and MOSI back from SPI4's alternate function - MX_SPI4_Init/HAL_SPI_MspInit set them
     * to AF5 at startup, and this test needs them as plain outputs. NSS is already a plain output. */
    gpio.Pin = GPIO_PIN_12 | GPIO_PIN_14 | SPI4_NSS_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &gpio);

    printf("SPI_LINK_GPIO_TEST: driving SCK(PE12)=20 transitions/s, MOSI(PE14)=10/s, NSS(PE11)=5/s\n");
    printf("SPI_LINK_GPIO_TEST: SPI is NOT running in this mode - this measures the wires only.\n");

    uint32_t last_report = HAL_GetTick();
    while (1) {
        uint32_t t = HAL_GetTick();
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_12, ((t / 50U) & 1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_14, ((t / 100U) & 1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOE, SPI4_NSS_Pin, ((t / 200U) & 1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        if ((t - last_report) >= 1000U) {
            last_report = t;
            printf("SPI_LINK_GPIO_TEST: still driving (t=%lums) READY(PE9) reads %d\n", (unsigned long)t,
                   (int)HAL_GPIO_ReadPin(XIAO_READY_GPIO_Port, XIAO_READY_Pin));
        }
    }
}
#endif /* SPI_LINK_GPIO_TEST */

static void spi_bridge_delay_us(uint32_t us) {
    /* ~4 core cycles per iteration of this loop; SystemCoreClock is 250MHz on this board. */
    volatile uint32_t cycles = (SystemCoreClock / 1000000UL) * us / 4UL;
    while (cycles-- > 0UL) {
        __NOP();
    }
}

/**
 * @brief Block until the XIAO's READY line reaches @p level, or SPI_READY_TIMEOUT_MS elapses.
 * @return true if the level was reached, false on timeout.
 *
 * The handshake is edge-based, not level-based, and that distinction is the whole point: after a
 * chunk is clocked out, READY is still HIGH from that same chunk until the XIAO's ISR drops it. A
 * master that only waits for "READY == HIGH" would read that stale HIGH as "armed for the next
 * chunk" and start clocking into an unarmed slave. So between chunks we wait for LOW (the slave
 * acknowledged the previous transaction ended) and only then for HIGH (it has re-armed).
 */
static bool spi_wait_ready(GPIO_PinState level) {
    uint32_t wait_start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(XIAO_READY_GPIO_Port, XIAO_READY_Pin) != level) {
        if ((HAL_GetTick() - wait_start) > SPI_READY_TIMEOUT_MS) {
            return false;
        }
    }
    return true;
}

static void send_vis_frame_spi(uint32_t frame_counter, uint8_t width, uint8_t height, const float *amplitude,
                                const float *depth, const float *ambient);
#endif

/* Runtime state is deliberately initialized to the exact production settings that were already
 * proven stable.  The control plane can therefore be flashed on one side at a time without silently
 * changing clock speed, frame layout, UART traffic, or transform output. */
static rc_status_t g_rc_status = {
    .version = RC_PROTOCOL_VERSION,
    .link_state = RC_LINK_STABLE,
    .flags = (CONF_STREAM_VISUALIZER ? RC_FLAG_UART_STREAM : 0U) |
             (CONF_PROFILE_TIMING ? RC_FLAG_PROFILER : 0U),
    .chunk_bytes = SPI_DEFAULT_CHUNK_BYTES,
    .chunk_count = 12U,
    .spi_prescaler = 16U,
    .profile = CONF_USECASE,
    .binning = 2U,
    .plane_mask = RC_PLANE_ALL,
    .uart_baud = 3000000U,
    .spi_hz = 15625000U,
};
static rc_command_t g_pending_safe;
static bool g_have_pending_safe = false;
static uint16_t g_proposed_chunk_bytes = SPI_DEFAULT_CHUNK_BYTES;
static uint16_t g_proposed_prescaler = 16U;
static uint8_t g_proposed_plane_mask = RC_PLANE_ALL;
static uint16_t g_stable_chunk_bytes = SPI_DEFAULT_CHUNK_BYTES;
static uint16_t g_stable_prescaler = 16U;
static uint8_t g_stable_plane_mask = RC_PLANE_ALL;
static uint32_t g_verify_deadline_frame = 0U;

static bool valid_plane_mask(uint8_t mask) {
    return (mask == RC_PLANE_ALL) || (mask == RC_PLANE_AMPLITUDE) ||
           (mask == RC_PLANE_DEPTH) || (mask == RC_PLANE_AMBIENT);
}

static size_t spi_payload_bytes(uint8_t plane_mask) {
    size_t bytes = sizeof(vis_frame_header_t);
    if ((plane_mask & RC_PLANE_AMPLITUDE) != 0U) {
        bytes += VIS_MAX_PLANE_PIXELS * sizeof(float);
    }
    if ((plane_mask & RC_PLANE_DEPTH) != 0U) {
        bytes += VIS_MAX_PLANE_PIXELS * sizeof(uint16_t);
    }
    if ((plane_mask & RC_PLANE_AMBIENT) != 0U) {
        bytes += VIS_MAX_PLANE_PIXELS * sizeof(float);
    }
    return bytes;
}

static uint16_t spi_chunk_count(uint16_t chunk_bytes, uint8_t plane_mask) {
    size_t controlled_bytes = spi_payload_bytes(plane_mask) + sizeof(rc_status_t);
    return (uint16_t)((controlled_bytes + chunk_bytes - 1U) / chunk_bytes);
}

static bool valid_chunk_bytes(uint16_t bytes) {
    return (bytes == 512U) || (bytes == 1024U) || (bytes == 2048U) || (bytes == 4092U);
}

static uint32_t hal_prescaler_value(uint16_t divider) {
    switch (divider) {
        case 2U: return SPI_BAUDRATEPRESCALER_2;
        case 4U: return SPI_BAUDRATEPRESCALER_4;
        case 8U: return SPI_BAUDRATEPRESCALER_8;
        case 16U: return SPI_BAUDRATEPRESCALER_16;
        case 32U: return SPI_BAUDRATEPRESCALER_32;
        case 64U: return SPI_BAUDRATEPRESCALER_64;
        case 128U: return SPI_BAUDRATEPRESCALER_128;
        case 256U: return SPI_BAUDRATEPRESCALER_256;
        default: return 0U;
    }
}

static bool valid_spi_prescaler(uint16_t divider) {
    /* SPI4 runs from 250 MHz. /16 (15.625 MHz) is the fastest setting validated on the complete
     * full-duplex XIAO/jumper-wire path. /8 failed link verification on this hardware, while /4
     * and /2 approach or exceed the ESP32-S3 slave's specified maximum. */
    return (divider == 16U) || (divider == 32U) || (divider == 64U) ||
           (divider == 128U) || (divider == 256U);
}

static bool apply_spi_link(uint16_t chunk_bytes, uint16_t prescaler, uint8_t plane_mask) {
    uint32_t hal_prescaler = hal_prescaler_value(prescaler);
    if (!valid_chunk_bytes(chunk_bytes) || !valid_plane_mask(plane_mask) ||
        !valid_spi_prescaler(prescaler) || (hal_prescaler == 0U)) {
        return false;
    }
    if (HAL_SPI_DeInit(&hspi4) != HAL_OK) {
        return false;
    }
    hspi4.Init.BaudRatePrescaler = hal_prescaler;
    if (HAL_SPI_Init(&hspi4) != HAL_OK) {
        return false;
    }
    g_rc_status.chunk_bytes = chunk_bytes;
    g_rc_status.chunk_count = spi_chunk_count(chunk_bytes, plane_mask);
    g_rc_status.spi_prescaler = prescaler;
    g_rc_status.spi_hz = 250000000U / prescaler;
    g_rc_status.plane_mask = plane_mask;
    return true;
}

static bool valid_uart_baud(uint32_t baud) {
    return (baud == 115200U) || (baud == 460800U) || (baud == 921600U) ||
           (baud == 2000000U) || (baud == 3000000U);
}

static void apply_pending_safe_controls(transform_t *p_transform, vl53l9_device_t *p_dev) {
    if (!g_have_pending_safe) {
        return;
    }
    rc_command_t command = g_pending_safe;
    g_have_pending_safe = false;
    g_rc_status.error = RC_ERROR_NONE;

    if (!valid_uart_baud(command.uart_baud) || command.profile >= VL53L9_NB_USECASES) {
        g_rc_status.error = RC_ERROR_BAD_VALUE;
        return;
    }

    /* Cross-binning changes alter both the raw input size and every prepared output capability.
     * Reject them explicitly; applying them to the live pipeline would overrun/misinterpret buffers. */
    vl53l9_profile_t *requested_profile = &g_ranging_profiles[command.profile];
    if (requested_profile->binning != g_rc_status.binning) {
        g_rc_status.error = RC_ERROR_PROFILE_RESTART_REQUIRED;
        return;
    }

    static const char *const bypass_names[7] = {
        "bypass-r2p-algo", "bypass-tnr-algo", "bypass-r2p-filter", "bypass-conf-filter",
        "bypass-refl-filter", "bypass-sharpener-filter", "bypass-fp-filter",
    };
    for (uint8_t i = 0; i < 7U; i++) {
        int ret = transform_set_control(p_transform, bypass_names[i],
                                        (value_t){ .val.v_bool = ((command.bypass_mask >> i) & 1U) != 0U,
                                                   .tid = VTID_BOOL });
        if (ret != 0) {
            g_rc_status.error = RC_ERROR_APPLY_FAILED;
            return;
        }
    }

    if (command.profile != g_rc_status.profile) {
        if ((vl53l9_stop(p_dev) != 0) || (vl53l9_utils_set_profile(p_dev, requested_profile) != 0) ||
            (vl53l9_set_sync_mode(p_dev, VL53L9_SYNC_MANUAL) != 0) || (vl53l9_start(p_dev) != 0)) {
            g_rc_status.error = RC_ERROR_APPLY_FAILED;
            return;
        }
        platform_acknowledge_event(PLATFORM_GPIO_IT_EVT);
        g_rc_status.profile = command.profile;
    }

    if (command.uart_baud != g_rc_status.uart_baud) {
        if (HAL_UART_DeInit(&hcom_uart[COM1]) != HAL_OK) {
            g_rc_status.error = RC_ERROR_APPLY_FAILED;
            return;
        }
        hcom_uart[COM1].Init.BaudRate = command.uart_baud;
        if (HAL_UART_Init(&hcom_uart[COM1]) != HAL_OK) {
            g_rc_status.error = RC_ERROR_APPLY_FAILED;
            return;
        }
        g_rc_status.uart_baud = command.uart_baud;
    }

    g_rc_status.flags = command.flags & (RC_FLAG_UART_STREAM | RC_FLAG_PROFILER);
    g_rc_status.bypass_mask = command.bypass_mask & 0x7FU;
}

static void process_runtime_command(const uint8_t *rx, size_t rx_len) {
    static uint32_t last_sequence = 0U;
    if (rx_len < sizeof(rc_command_t)) {
        return;
    }
    rc_command_t command;
    memcpy(&command, rx, sizeof(command));
    if (memcmp(command.magic, RC_COMMAND_MAGIC, 4U) != 0 || command.version != RC_PROTOCOL_VERSION ||
        crc16_ccitt((const uint8_t *)&command, offsetof(rc_command_t, crc16), 0U) != command.crc16) {
        if (memcmp(command.magic, RC_COMMAND_MAGIC, 4U) == 0 && g_rc_status.command_crc_errors != UINT16_MAX) {
            g_rc_status.command_crc_errors++;
        }
        return;
    }
    if (command.op == RC_OP_IDLE || command.sequence == last_sequence) {
        return;
    }
    last_sequence = command.sequence;
    g_rc_status.ack_sequence = command.sequence;
    g_rc_status.error = RC_ERROR_NONE;

    switch (command.op) {
        case RC_OP_APPLY_SAFE:
            g_pending_safe = command;
            g_have_pending_safe = true;
            break;
        case RC_OP_PROPOSE_LINK:
            if (!valid_chunk_bytes(command.chunk_bytes) || !valid_plane_mask(command.plane_mask) ||
                !valid_spi_prescaler(command.spi_prescaler) ||
                hal_prescaler_value(command.spi_prescaler) == 0U) {
                g_rc_status.error = RC_ERROR_BAD_VALUE;
                break;
            }
            g_proposed_chunk_bytes = command.chunk_bytes;
            g_proposed_prescaler = command.spi_prescaler;
            g_proposed_plane_mask = command.plane_mask;
            g_rc_status.link_state = RC_LINK_PREPARED;
            break;
        case RC_OP_COMMIT_LINK:
            if (g_rc_status.link_state != RC_LINK_PREPARED) {
                g_rc_status.error = RC_ERROR_BAD_COMMAND;
                break;
            }
            /* This state is advertised in the next old-layout frame. Both peers switch only after
             * that complete frame, so the XIAO can resize its next queued transaction in time. */
            g_rc_status.link_state = RC_LINK_SWITCH_AFTER_FRAME;
            break;
        case RC_OP_VERIFY_LINK:
            if (g_rc_status.link_state != RC_LINK_VERIFYING) {
                g_rc_status.error = RC_ERROR_BAD_COMMAND;
                break;
            }
            g_stable_chunk_bytes = g_rc_status.chunk_bytes;
            g_stable_prescaler = g_rc_status.spi_prescaler;
            g_stable_plane_mask = g_rc_status.plane_mask;
            g_rc_status.link_state = RC_LINK_STABLE;
            break;
        case RC_OP_ABORT_LINK:
            if (g_rc_status.link_state == RC_LINK_VERIFYING) {
                g_rc_status.link_state = RC_LINK_REVERT_AFTER_FRAME;
            } else {
                g_rc_status.link_state = RC_LINK_STABLE;
                g_proposed_chunk_bytes = g_stable_chunk_bytes;
                g_proposed_prescaler = g_stable_prescaler;
                g_proposed_plane_mask = g_stable_plane_mask;
            }
            break;
        default:
            g_rc_status.error = RC_ERROR_BAD_COMMAND;
            break;
    }
}

void vl53l9_app() {

#if CONF_STREAM_SPI && SPI_LINK_GPIO_TEST
    /* Raw wire measurement mode - never returns. Runs before the sensor is touched so nothing else
     * can interfere with, or be blamed for, what the pins are doing. */
    spi_link_gpio_test();
#endif

    /* checkpoint prints: pinpoint exactly how far setup gets before a silent hang/crash */
    printf("vl53l9_app: starting (CONF_STREAM_VISUALIZER=%d)\n", CONF_STREAM_VISUALIZER);

    int ret;
    transform_t *p_transform = vl53l9_transform_create();
    vl53l9_device_t *p_dev = &device[CONF_DEVICE_ID];
    vl53l9_profile_t *p_profile = &g_ranging_profiles[CONF_USECASE];

    uint16_t raw_buffer_size = 0, frame_buffer_size = 0; /* bytes */
    uint32_t in_width = 0, in_height = 0;                /* pixels */
    uint8_t out_width = 0, out_height = 0;               /* pixels */
    vl53l9_get_raw_buffer_size(p_profile->binning, &raw_buffer_size);
    vl53l9_utils_get_resolution(p_profile->binning, &out_width, &out_height);
    frame_buffer_size = out_width * out_height * sizeof(float);

    if (p_profile->binning == 2) {
        in_width = 14842;
        in_height = 1;
    } else if (p_profile->binning == 4) {
        in_width = 3844;
        in_height = 1;
    } else {
        handle_error(); /* unsupported binning */
    }

    /* sensor reset */
    platform_power_reset(CONF_DEVICE_ID);
    if (p_dev->bus_type & PLATFORM_BUS_I3C) {
        platform_assign_dynamic_address();
    }

    /* initialize sensor and retrieve calibration data */
    ret = vl53l9_init(p_dev);
    if (ret) {
        handle_error();
    }
    printf("vl53l9_app: vl53l9_init OK\n");

    uint8_t calib_data[VL53L9_CALIB_DATA_SIZE];
    ret = vl53l9_get_calib_data(p_dev, calib_data);
    if (ret) {
        handle_error();
    }
    printf("vl53l9_app: calibration data OK\n");

    vl53l9_utils_set_profile(p_dev, p_profile);

    /* initialize processing pipeline */
    ret = transform_initialize(p_transform);
    if (ret) {
        handle_error();
    }
    printf("vl53l9_app: transform_initialize OK\n");

    /* inspect available streams and controls */
    const streams_t *stream_list;
    transform_get_streams(p_transform, &stream_list);
    streams_inspect(stream_list, printf);

    const controls_t *control_list;
    transform_get_controls(p_transform, &control_list);
    controls_inspect(control_list, printf);

    /* set capabilities */

    /**
     * NOTE:
     * setting capabilities is a mandatory step:
     *  - at least one input and one output stream must be set
     *  - input stream must be configured before output ones
     *  - there are no default capabilities, they must be explicitly set
     */

    /* build raw stream capabilities */
    property_t raw_format = { "format", { .val.v_string = "3DMD", .tid = VTID_STRING } };
    property_t raw_width = { "width", { .val.v_uint32 = in_width, .tid = VTID_UINT32 } };
    property_t raw_height = { "height", { .val.v_uint32 = in_height, .tid = VTID_UINT32 } };

    properties_t *raw_props = properties_new(3); /* format, width, height */
    properties_add(raw_props, &raw_format);
    properties_add(raw_props, &raw_width);
    properties_add(raw_props, &raw_height);
    capabilities_t *raw_caps = capabilities_new_simple(&raw_props);

    /* build depth stream capabilities */
    property_t depth_format = { "format", { .val.v_string = "ZF32", .tid = VTID_STRING } };
    property_t depth_width = { "width", { .val.v_uint32 = out_width, .tid = VTID_UINT32 } };
    property_t depth_height = { "height", { .val.v_uint32 = out_height, .tid = VTID_UINT32 } };

    properties_t *depth_props = properties_new(3); /* format, width, height */
    properties_add(depth_props, &depth_format);
    properties_add(depth_props, &depth_width);
    properties_add(depth_props, &depth_height);
    capabilities_t *depth_caps = capabilities_new_simple(&depth_props);

#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
    /* build amplitude stream capabilities (same resolution as depth, used as the "image" for the PC visualizer) */
    property_t amp_format = { "format", { .val.v_string = "AF32", .tid = VTID_STRING } };
    property_t amp_width = { "width", { .val.v_uint32 = out_width, .tid = VTID_UINT32 } };
    property_t amp_height = { "height", { .val.v_uint32 = out_height, .tid = VTID_UINT32 } };

    properties_t *amp_props = properties_new(3); /* format, width, height */
    properties_add(amp_props, &amp_format);
    properties_add(amp_props, &amp_width);
    properties_add(amp_props, &amp_height);
    capabilities_t *amp_caps = capabilities_new_simple(&amp_props);

    /* build ambient stream capabilities (same resolution as depth - background/environmental IR light
     * per zone, independent of the sensor's own laser; distinct from amplitude, see chat explanation) */
    property_t ambient_format = { "format", { .val.v_string = "IF32", .tid = VTID_STRING } };
    property_t ambient_width = { "width", { .val.v_uint32 = out_width, .tid = VTID_UINT32 } };
    property_t ambient_height = { "height", { .val.v_uint32 = out_height, .tid = VTID_UINT32 } };

    properties_t *ambient_props = properties_new(3); /* format, width, height */
    properties_add(ambient_props, &ambient_format);
    properties_add(ambient_props, &ambient_width);
    properties_add(ambient_props, &ambient_height);
    capabilities_t *ambient_caps = capabilities_new_simple(&ambient_props);
#endif

    /* set stream capabilities */
    ret = transform_set_stream_capabilities(p_transform, "raw", raw_caps);
    if (ret) {
        handle_error();
    }

    ret = transform_set_stream_capabilities(p_transform, "depth", depth_caps);
    if (ret) {
        handle_error();
    }

#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
    ret = transform_set_stream_capabilities(p_transform, "amplitude", amp_caps);
    if (ret) {
        handle_error();
    }

    ret = transform_set_stream_capabilities(p_transform, "ambient", ambient_caps);
    if (ret) {
        handle_error();
    }
#endif

    /* free properties and capabilities (TODO: improve using free functions) */
    properties_free(raw_props, NULL);
    properties_free(depth_props, NULL);
    capabilities_free(raw_caps, NULL);
    capabilities_free(depth_caps, NULL);
#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
    properties_free(amp_props, NULL);
    capabilities_free(amp_caps, NULL);
    properties_free(ambient_props, NULL);
    capabilities_free(ambient_caps, NULL);
#endif
    printf("vl53l9_app: stream capabilities set OK\n");

    /* set controls */

    /* NOTE: the following control is mandatory and must be set before calling prepare() */
    ret = transform_set_control(p_transform, "calib-buffer", (value_t){ .val.v_ptr = calib_data, .tid = VTID_POINTER });
    if (ret) {
        handle_error();
    }

    /* check pipeline configuration and compute internal parameters required for processing */
    ret = transform_prepare(p_transform);
    if (ret) {
        handle_error();
    }
    printf("vl53l9_app: transform_prepare OK\n");

    /* allocate memory and initialize buffers (raw data is double buffered) */
    uint8_t raw_mem_index = 0;
    memory_t in_raw_mem[2] = { allocate_memory(raw_buffer_size), allocate_memory(raw_buffer_size) };
    memory_t out_depth_mem = allocate_memory(frame_buffer_size);

    memories_t in_raw_mems = { .items = &in_raw_mem, .size = 1, .capacity = 1, .item_size = sizeof(memory_t) };
    memories_t out_depth_mems = { .items = &out_depth_mem, .size = 1, .capacity = 1, .item_size = sizeof(memory_t) };

    stream_buffer_t in_raw_stream_buffer = { .name = "raw", .buffer = { .memories = &in_raw_mems, .nb = 1 } };
    stream_buffer_t out_depth_stream_buffer = { .name = "depth", .buffer = { .memories = &out_depth_mems, .nb = 1 } };

#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
    /* amplitude/ambient outputs, same resolution/size as depth (AF32/IF32 are float32 like ZF32) */
    memory_t out_amp_mem = allocate_memory(frame_buffer_size);
    memories_t out_amp_mems = { .items = &out_amp_mem, .size = 1, .capacity = 1, .item_size = sizeof(memory_t) };
    stream_buffer_t out_amp_stream_buffer = { .name = "amplitude", .buffer = { .memories = &out_amp_mems, .nb = 1 } };

    memory_t out_ambient_mem = allocate_memory(frame_buffer_size);
    memories_t out_ambient_mems = { .items = &out_ambient_mem, .size = 1, .capacity = 1, .item_size = sizeof(memory_t) };
    stream_buffer_t out_ambient_stream_buffer = { .name = "ambient",
                                                  .buffer = { .memories = &out_ambient_mems, .nb = 1 } };
#endif

    /* build stream buffers container */
    stream_buffers_t stream_buffers = { .items =
                                            (stream_buffer_t[]){
                                                in_raw_stream_buffer,
                                                out_depth_stream_buffer,
#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
                                                out_amp_stream_buffer,
                                                out_ambient_stream_buffer,
#endif
                                            },
#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
                                        .size = 4,
                                        .capacity = 4,
#else
                                        .size = 2,
                                        .capacity = 2,
#endif
                                        .item_size = sizeof(stream_buffer_t) };

    ret = vl53l9_set_sync_mode(p_dev, VL53L9_SYNC_MANUAL);
    if (ret) {
        handle_error();
    }

    ret = vl53l9_start(p_dev);
    if (ret) {
        handle_error();
    }

    /* BUGFIX: EXTI7 (the sensor's INTR pin) is enabled way back in main.c's MX_GPIO_Init(), before
     * vl53l9_app() even runs, so any falling edge during the reset/init/calib/prepare/start sequence
     * above (unrelated to an actual "frame ready" condition) can latch PLATFORM_GPIO_IT_EVT in the
     * platform layer's sticky g_platform_evt flag - nothing before this point ever clears it. Flush it
     * here so the first acquisition attempt below starts clean (each retry attempt also re-flushes it,
     * see ACQUIRE_MAX_RETRIES below). Pre-existing gap in the unmodified ST example, not introduced for
     * the visualizer streaming. */
    platform_acknowledge_event(PLATFORM_GPIO_IT_EVT);

    printf("vl53l9_app: buffers allocated, vl53l9_start OK, entering main loop\n");

    platform_profiler_enable();
    uint32_t start_time = platform_profiler_get_timestamp();
    uint32_t stop_time;
    float frame_rate;

    bool is_first_frame = true;
    bool depth_ready = false; /* out_depth_mem/out_amp_mem only hold valid data once processed once */

    while (1) {

        /* BUGFIX: acquiring one raw frame (trigger -> wait for IRQ -> read) has been observed to fail
         * intermittently in three different ways across repeated runs on this board: a stale/leftover
         * interrupt flag, a full 1000ms timeout with no interrupt at all, and a sensor-reported
         * transient fault (status.error bit sof_outside_blanking). Different failure point every time
         * is the signature of a real transient timing hiccup, not a single deterministic bug - so
         * instead of killing the whole application on the first one (the original ST example's
         * behavior), retry the whole trigger/wait/read sequence a bounded number of times. This also
         * subsumes the vl53l9_trigger_frame() discarded-return-value bug from the unmodified example
         * (its result is now checked, as part of this retry loop). */
#if CONF_PROFILE_TIMING
        uint32_t t_loop_start = platform_profiler_get_timestamp();
#endif
        int acquire_attempt;
        for (acquire_attempt = 0; acquire_attempt < ACQUIRE_MAX_RETRIES; acquire_attempt++) {
            platform_acknowledge_event(PLATFORM_GPIO_IT_EVT); /* flush any stale flag before this attempt */

            ret = vl53l9_trigger_frame(p_dev);
            if (ret) {
                printf("vl53l9_app: trigger_frame failed (ret=%d), retrying (%d/%d)\n", ret, acquire_attempt + 1,
                       ACQUIRE_MAX_RETRIES);
                continue;
            }

            ret = platform_wait_for_event(PLATFORM_GPIO_IT_EVT, 1000);
            if (ret) {
                printf("vl53l9_app: wait for frame IRQ timed out, retrying (%d/%d)\n", acquire_attempt + 1,
                       ACQUIRE_MAX_RETRIES);
                continue;
            }

            platform_acknowledge_event(PLATFORM_GPIO_IT_EVT);

            /* grab raw data from sensor and fill input buffer */
            ret = vl53l9_get_frame_async(p_dev, in_raw_mem[raw_mem_index].data, in_raw_mem[raw_mem_index].size);
            if (ret == 0) {
                break; /* got a frame - proceed */
            }
            printf("vl53l9_app: get_frame_async failed (ret=%d), retrying (%d/%d)\n", ret, acquire_attempt + 1,
                   ACQUIRE_MAX_RETRIES);
        }
        if (acquire_attempt == ACQUIRE_MAX_RETRIES) {
            printf("vl53l9_app: giving up after %d consecutive failed acquisition attempts\n", ACQUIRE_MAX_RETRIES);
            handle_error();
        }

#if CONF_PROFILE_TIMING
        uint32_t t_after_acquire = platform_profiler_get_timestamp();
#endif
        /* process the previous frame while the sensor is acquiring the next one */
        if (is_first_frame) {
            is_first_frame = false;
        } else {
            /* TODO: find a better way to handle this, maybe leveraging mems list */
            in_raw_mems.items = &in_raw_mem[(raw_mem_index + 1) % 2];
            ret = transform_process_stream(p_transform, &stream_buffers);
            if (ret) {
                handle_error();
            }
            depth_ready = true;
        }

#if CONF_PROFILE_TIMING
        uint32_t t_after_transform = platform_profiler_get_timestamp();
#endif
        ret = platform_wait_for_event(PLATFORM_I3C_DMA_RX_EVT, 1000);
        if (ret) {
            handle_error();
        }
        platform_acknowledge_event(PLATFORM_I3C_DMA_RX_EVT);

        ret = vl53l9_get_frame_async_ack(p_dev, in_raw_mem[raw_mem_index].data, in_raw_mem[raw_mem_index].size);
        if (ret) {
            handle_error();
        }

        /* TODO: to be moved below but avoid printing for first frame */
        vl53l9_frame_t frame = { 0 };
        ret = vl53l9_utils_parse_frame(in_raw_mem[raw_mem_index].data, in_raw_mem[raw_mem_index].size, &frame);
        if (ret) {
            handle_error();
        }

#if CONF_PROFILE_TIMING
        uint32_t t_after_readout = platform_profiler_get_timestamp();
#endif
        /* measure frame rate */
        stop_time = platform_profiler_get_timestamp();
        frame_rate = (1.0f / (float)(platform_profiler_convert_to_us(stop_time - start_time))) * 1000000;
        start_time = stop_time;

        if (depth_ready) {
            print_frame((float *)out_depth_mem.data, out_height, out_width);
            printf("Processed frame n. %lu @ %u fps\n", (unsigned long)frame.p_metadata->frame_counter,
                   (unsigned int)frame_rate);
#if CONF_PROFILE_TIMING
            uint32_t t_before_uart = platform_profiler_get_timestamp();
#endif
#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
            if ((g_rc_status.flags & RC_FLAG_UART_STREAM) != 0U) {
                send_vis_frame(frame.p_metadata->frame_counter, out_width, out_height, (const float *)out_amp_mem.data,
                               (const float *)out_depth_mem.data, (const float *)out_ambient_mem.data);
            }
#endif
#if CONF_PROFILE_TIMING
            uint32_t t_after_uart = platform_profiler_get_timestamp();
#endif
#if CONF_STREAM_SPI
            send_vis_frame_spi(frame.p_metadata->frame_counter, out_width, out_height, (const float *)out_amp_mem.data,
                                (const float *)out_depth_mem.data, (const float *)out_ambient_mem.data);
            apply_pending_safe_controls(p_transform, p_dev);
#endif
#if CONF_PROFILE_TIMING
            {
                /* Rate-limited to one line per second: the point is the steady-state split between
                 * stages, not every frame. All values in milliseconds. "print" is the ascii-art +
                 * "Processed frame" printf above, which also goes out over the same 3Mbaud UART. */
                static uint32_t last_profile_ms = 0;
                uint32_t now_ms = HAL_GetTick();
                uint32_t t_end = platform_profiler_get_timestamp();
                g_rc_status.fps_x100 = (uint16_t)MIN(MAX(frame_rate * 100.0f, 0.0f), 65535.0f);
                g_rc_status.acquire_ms = (uint16_t)(platform_profiler_convert_to_us(t_after_acquire - t_loop_start) / 1000U);
                g_rc_status.transform_ms = (uint16_t)(platform_profiler_convert_to_us(t_after_transform - t_after_acquire) / 1000U);
                g_rc_status.readout_ms = (uint16_t)(platform_profiler_convert_to_us(t_after_readout - t_after_transform) / 1000U);
                g_rc_status.print_ms = (uint16_t)(platform_profiler_convert_to_us(t_before_uart - t_after_readout) / 1000U);
                g_rc_status.uart_ms = (uint16_t)(platform_profiler_convert_to_us(t_after_uart - t_before_uart) / 1000U);
                g_rc_status.spi_ms = (uint16_t)(platform_profiler_convert_to_us(t_end - t_after_uart) / 1000U);
                g_rc_status.total_ms = (uint16_t)(platform_profiler_convert_to_us(t_end - t_loop_start) / 1000U);
                if (((g_rc_status.flags & RC_FLAG_PROFILER) != 0U) && (now_ms - last_profile_ms) >= 1000U) {
                    last_profile_ms = now_ms;
                    printf("PROFILE ms: acquire=%lu transform=%lu readout=%lu print=%lu uart=%lu spi=%lu | total=%lu\n",
                           (unsigned long)g_rc_status.acquire_ms, (unsigned long)g_rc_status.transform_ms,
                           (unsigned long)g_rc_status.readout_ms, (unsigned long)g_rc_status.print_ms,
                           (unsigned long)g_rc_status.uart_ms, (unsigned long)g_rc_status.spi_ms,
                           (unsigned long)g_rc_status.total_ms);
                }
            }
#endif
        }

        /* swap raw buffer index for next frame acquisition */
        raw_mem_index = (raw_mem_index + 1) % 2;
    }

    /* NOTE: free memory and pipeline resources to avoid leaks */
    /* free(in_raw_mem[0].data); */
    /* free(in_raw_mem[1].data); */
    /* free(out_depth_mem.data); */
    /* transform_finalize(p_transform); */
    /* transform_release(p_transform); */
    /* vl53l9_transform_destroy(p_transform); */
}

static void print_frame(float *p_frame, size_t height, size_t width) {
#if CONF_PRINT_FRAME
    static const char ASCII_CHARS[] = "@%#*+=-:. ";

    printf("\033[%d;%dH", 0, 0); /* set cursor to the top of the screen */
    int pixel_step = 1;
    uint32_t min = UINT32_MAX;
    uint32_t max = 0;

    for (uint32_t i = 0; i < (height * width); i++) {
        uint32_t value = (uint32_t)p_frame[i];
        min = MIN(value, min);
        max = MAX(value, max);
    }

    uint32_t average = (uint32_t)((max - min) * 0.05f);
    min = MAX(min - average, 0);
    max = MIN(max + average, UINT32_MAX);

    for (uint32_t y = 0; y < height; y += pixel_step) {
        for (uint32_t x = 0; x < width; x += pixel_step) {
            uint32_t pixel_index = (y * width + x);
            uint32_t value = (uint32_t)p_frame[pixel_index];

            uint32_t ascii_index = (value - min) * (sizeof(ASCII_CHARS) - 1) / (max - min);
            ascii_index = MIN(ascii_index, sizeof(ASCII_CHARS) - 1);

            printf("%c", ASCII_CHARS[ascii_index]);
        }
        printf("\n");
    }
#endif /* CONF_PRINT_FRAME */
    return;
}

static memory_t allocate_memory(uint16_t size) {
    memory_t memory;
    memory.size = size;
    memory.data = malloc(size);
    if (memory.data == NULL) {
        handle_error();
    }
    return memory;
}

static void handle_error_impl(int line) {
    vl53l9_status_t status = { 0 };
    int status_ret = vl53l9_get_status(&device[CONF_DEVICE_ID], &status);
    /* NOTE: printf()/HAL_UART_Transmit are blocking, so this is flushed out before we spin forever below,
     * even though the loop that follows never returns. */
    printf("\n*** handle_error() at vl53l9_app.c:%d - vl53l9_get_status() %s "
           "(fsm=0x%02x command=0x%02x firmware_err=0x%04x error=0x%02x) ***\n",
           line, status_ret ? "FAILED (bus/sensor likely unreachable)" : "ok", status.fsm, status.command,
           status.firmware, *(uint8_t *)&status.error);
    while (1)
        ;
}

#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
/**
 * @brief CRC-16/CCITT (poly 0x1021, no reflection). Must match the Python-side implementation in
 * Utilities/vl53l9_visualizer.py and the XIAO-side implementation in stm32_utility/spi/spi.ino so
 * every receiving end can validate/resync on the byte stream. Shared by both send_vis_frame() (UART)
 * and send_vis_frame_spi() (SPI) below - see the combined guard note at this function's declaration.
 */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
#endif /* CONF_STREAM_VISUALIZER || CONF_STREAM_SPI */

#if CONF_STREAM_VISUALIZER || CONF_STREAM_SPI
/**
 * @brief Send one amplitude+depth+ambient frame to the PC over the COM1 UART as a binary packet, for
 * live visualization by Utilities/vl53l9_visualizer.py. Blocking (uses HAL_UART_Transmit), so this
 * adds latency to the acquisition loop proportional to payload size / baudrate.
 *
 * depth is quantized to uint16 (round-to-nearest, clamped) - its real range (0-8800mm, plus the
 * library's 12000mm invalid-pixel sentinel) is known-safe. amplitude/ambient are sent as the
 * library's native float32 - they are raw photon-count-rate values ("signal_rate"/"ambient_rate")
 * without a documented safe uint16 range; quantizing those was tried and visibly clipped/saturated
 * real texture on hardware, so they stay lossless float32. See the vis_frame_header_t comment above.
 */
static void send_vis_frame(uint32_t frame_counter, uint8_t width, uint8_t height, const float *amplitude,
                            const float *depth, const float *ambient) {
    static uint16_t depth_q[VIS_MAX_PLANE_PIXELS];

    size_t pixels = (size_t)width * height;
    if (pixels > VIS_MAX_PLANE_PIXELS) {
        handle_error(); /* should never happen for a supported binning value, see VIS_MAX_PLANE_PIXELS */
    }

    for (size_t i = 0; i < pixels; i++) {
        depth_q[i] = (uint16_t)MIN(MAX(depth[i] + 0.5f, 0.0f), 65535.0f);
    }

    vis_frame_header_t header;
    memcpy(header.magic, "VL59", sizeof(header.magic));
    header.frame_counter = frame_counter;
    header.width = width;
    header.height = height;

    size_t amp_bytes = pixels * sizeof(float);
    size_t depth_bytes = pixels * sizeof(uint16_t);
    size_t ambient_bytes = pixels * sizeof(float);

    uint16_t crc = crc16_ccitt((const uint8_t *)amplitude, amp_bytes, 0);
    crc = crc16_ccitt((const uint8_t *)depth_q, depth_bytes, crc);
    crc = crc16_ccitt((const uint8_t *)ambient, ambient_bytes, crc);
    header.crc16 = crc;

    if (HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)&header, sizeof(header), 1000) != HAL_OK) {
        handle_error();
    }
    if (HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)amplitude, (uint16_t)amp_bytes, 1000) != HAL_OK) {
        handle_error();
    }
    if (HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)depth_q, (uint16_t)depth_bytes, 1000) != HAL_OK) {
        handle_error();
    }
    if (HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)ambient, (uint16_t)ambient_bytes, 1000) != HAL_OK) {
        handle_error();
    }
}
#endif /* CONF_STREAM_VISUALIZER || CONF_STREAM_SPI */

#if CONF_STREAM_SPI
/**
 * @brief Send all three images, or one runtime-selected image, to a XIAO ESP32 over SPI4 for the XIAO
 * to re-serve as a live webpage over WiFi (stm32_utility/spi/spi.ino in the separate Arduino repo).
 * The all-plane layout retains the same mixed-precision quantization and CRC as send_vis_frame()
 * above (depth is uint16 while amplitude/ambient stay float32); single-plane magic identifies the
 * shorter layouts.
 *
 * Unlike UART, SPI has no built-in framing: the transfer length must be agreed by both sides ahead of
 * the transaction, and the receiving XIAO can't produce a receive buffer on demand mid-transaction. So
 * the frame is padded to the active negotiated chunk boundary and sent as fixed-size chunks (the
 * receiver only reads width*height pixels per plane from the header), each chunk
 * waiting on the XIAO's PIN_READY handshake before asserting the software NSS and clocking it out. If
 * the XIAO never raises READY (not powered, not flashed, not wired, or still processing) the frame is
 * abandoned mid-way rather than blocking the whole acquisition loop; the XIAO resyncs to chunk 0 on its
 * side. Failures here are independent of, and must not affect, the UART path above.
 */
static void send_vis_frame_spi(uint32_t frame_counter, uint8_t width, uint8_t height, const float *amplitude,
                                const float *depth, const float *ambient) {
    static uint8_t tx_buf[SPI_MAX_PADDED_FRAME_BYTES];
    static uint8_t rx_buf[4092U];
    static uint16_t depth_q[VIS_MAX_PLANE_PIXELS];

    size_t pixels = (size_t)width * height;
    if (pixels > VIS_MAX_PLANE_PIXELS) {
        handle_error(); /* should never happen for a supported binning value, see VIS_MAX_PLANE_PIXELS */
    }

    uint8_t plane_mask = g_rc_status.plane_mask;
    if ((plane_mask & RC_PLANE_DEPTH) != 0U) {
        for (size_t i = 0; i < pixels; i++) {
            depth_q[i] = (uint16_t)MIN(MAX(depth[i] + 0.5f, 0.0f), 65535.0f);
        }
    }
    vis_frame_header_t header;
    if (plane_mask == RC_PLANE_AMPLITUDE) {
        memcpy(header.magic, "VL5A", sizeof(header.magic));
    } else if (plane_mask == RC_PLANE_DEPTH) {
        memcpy(header.magic, "VL5D", sizeof(header.magic));
    } else if (plane_mask == RC_PLANE_AMBIENT) {
        memcpy(header.magic, "VL5I", sizeof(header.magic));
    } else {
        memcpy(header.magic, "VL59", sizeof(header.magic));
    }
    header.frame_counter = frame_counter;
    header.width = width;
    header.height = height;

    size_t amp_bytes = pixels * sizeof(float);
    size_t depth_bytes = pixels * sizeof(uint16_t);
    size_t ambient_bytes = pixels * sizeof(float);
    size_t total = sizeof(header);

    uint16_t crc = 0U;
    if ((plane_mask & RC_PLANE_AMPLITUDE) != 0U) {
        crc = crc16_ccitt((const uint8_t *)amplitude, amp_bytes, crc);
        total += amp_bytes;
    }
    if ((plane_mask & RC_PLANE_DEPTH) != 0U) {
        crc = crc16_ccitt((const uint8_t *)depth_q, depth_bytes, crc);
        total += depth_bytes;
    }
    if ((plane_mask & RC_PLANE_AMBIENT) != 0U) {
        crc = crc16_ccitt((const uint8_t *)ambient, ambient_bytes, crc);
        total += ambient_bytes;
    }
    header.crc16 = crc;

    uint16_t chunk_bytes = g_rc_status.chunk_bytes;
    uint16_t chunk_count = spi_chunk_count(chunk_bytes, plane_mask);
    size_t padded_bytes = (size_t)chunk_bytes * chunk_count;
    if (padded_bytes > sizeof(tx_buf)) {
        g_rc_status.error = RC_ERROR_BAD_VALUE;
        return;
    }

    /* Both peers independently time out VERIFYING and return to the last stable layout. This check
     * runs before the status trailer is built, so a still-readable link sees REVERT_AFTER_FRAME. */
    if (g_rc_status.link_state == RC_LINK_VERIFYING && frame_counter >= g_verify_deadline_frame) {
        g_rc_status.link_state = RC_LINK_REVERT_AFTER_FRAME;
        g_rc_status.error = RC_ERROR_LINK_TIMEOUT;
    }
    bool switch_after_frame = (g_rc_status.link_state == RC_LINK_SWITCH_AFTER_FRAME);
    bool revert_after_frame = (g_rc_status.link_state == RC_LINK_REVERT_AFTER_FRAME);

    size_t offset = 0U;
    memcpy(tx_buf + offset, &header, sizeof(header));
    offset += sizeof(header);
    if ((plane_mask & RC_PLANE_AMPLITUDE) != 0U) {
        memcpy(tx_buf + offset, amplitude, amp_bytes);
        offset += amp_bytes;
    }
    if ((plane_mask & RC_PLANE_DEPTH) != 0U) {
        memcpy(tx_buf + offset, depth_q, depth_bytes);
        offset += depth_bytes;
    }
    if ((plane_mask & RC_PLANE_AMBIENT) != 0U) {
        memcpy(tx_buf + offset, ambient, ambient_bytes);
        offset += ambient_bytes;
    }
    memcpy(g_rc_status.magic, RC_STATUS_MAGIC, 4U);
    g_rc_status.version = RC_PROTOCOL_VERSION;
    g_rc_status.frame_counter = frame_counter;
    g_rc_status.chunk_count = chunk_count;
    g_rc_status.crc16 = crc16_ccitt((const uint8_t *)&g_rc_status, offsetof(rc_status_t, crc16), 0U);
    memcpy(tx_buf + total, &g_rc_status, sizeof(g_rc_status));

#if SPI_TEST_PATTERN
    /* Cycle through four diagnostic patterns, switching every 5 seconds so each one spans several
     * of the once-per-second log lines on both sides and the transitions are obvious when the two
     * logs are lined up.
     *
     * The constant patterns are the point. A ramp cannot tell "MOSI carries no real data" apart
     * from "MOSI is fine but sampling timing is wrong" - both look like garbage. A CONSTANT must
     * survive any timing error whatsoever: if the master holds MOSI low for the whole transfer,
     * every sample must read 0x00 no matter when it is taken. So:
     *   all-00 reads 00 AND all-FF reads FF -> the MOSI data path works; the fault is timing/framing
     *   all-FF does not read back as FF     -> MOSI is not carrying the master's data at all
     *   all-AA (01010101) reading as 55/33/0F -> sampling at the wrong rate or edge */
    uint32_t test_phase = (HAL_GetTick() / 5000U) % 4U;
    uint8_t fill = 0x00;
    const char *pattern_name = "ramp";
    switch (test_phase) {
        case 1: fill = 0x00; pattern_name = "all-00"; break;
        case 2: fill = 0xFF; pattern_name = "all-FF"; break;
        case 3: fill = 0xAA; pattern_name = "all-AA"; break;
        default: break;
    }
    for (size_t i = 0; i < padded_bytes; i++) {
        tx_buf[i] = (test_phase == 0U) ? (uint8_t)(i & 0xFF) : fill;
    }

    static uint32_t last_pattern_log_ms = 0;
    uint32_t pattern_now = HAL_GetTick();
    if ((pattern_now - last_pattern_log_ms) >= 1000U) {
        last_pattern_log_ms = pattern_now;
        printf("TESTTX: pattern=%s first16: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
               "%02X %02X\n",
               pattern_name, tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3], tx_buf[4], tx_buf[5], tx_buf[6], tx_buf[7],
               tx_buf[8], tx_buf[9], tx_buf[10], tx_buf[11], tx_buf[12], tx_buf[13], tx_buf[14], tx_buf[15]);
    }
#endif

    /* Send the frame using the negotiated transaction count. Each one re-waits for READY, because
     * the XIAO re-arms its SPI slave between every chunk (spi_slave_queue_trans() only posts to a
     * FreeRTOS queue - the driver's task/ISR context programs the DMA and arms the peripheral some
     * tens of microseconds later, and this loop polls READY at 250MHz, so without the guard delay
     * below the master would start clocking into a slave that isn't listening yet). */
    HAL_StatusTypeDef status = HAL_OK;
#if SPI_TEST_PATTERN
    /* Bisection mode: one small transaction per frame - see SPI_TEST_BYTES. */
    chunk_bytes = SPI_TEST_BYTES;
    chunk_count = 1;
#else
    /* Runtime values captured above. They remain fixed for every chunk in this frame. */
#endif

    if (!spi_wait_ready(GPIO_PIN_SET)) {
        printf("send_vis_frame_spi: XIAO not ready at frame start (timeout)\n");
        return;
    }

    for (size_t chunk = 0; chunk < chunk_count; chunk++) {
        spi_bridge_delay_us(SPI_READY_TO_CS_DELAY_US);

        HAL_GPIO_WritePin(SPI4_NSS_GPIO_Port, SPI4_NSS_Pin, GPIO_PIN_RESET); /* select (active low) */
        /* CS setup: let the slave's CS-detect logic latch the transaction start before the first
         * SCK edge arrives. */
        spi_bridge_delay_us(SPI_CS_SETUP_DELAY_US);

        status = HAL_SPI_TransmitReceive(&hspi4, tx_buf + chunk * chunk_bytes, rx_buf, chunk_bytes,
                                         SPI_TRANSFER_TIMEOUT_MS);

        /* CS hold: HAL_SPI_Transmit returns on EOT, but let the last bits settle at the slave before
         * releasing CS so the tail of the chunk isn't truncated by an early deassert. */
        spi_bridge_delay_us(SPI_CS_HOLD_DELAY_US);
        HAL_GPIO_WritePin(SPI4_NSS_GPIO_Port, SPI4_NSS_Pin, GPIO_PIN_SET); /* deselect */

        if (status != HAL_OK) {
            printf("send_vis_frame_spi: HAL_SPI_TransmitReceive failed at chunk %u/%u (status=%d)\n", (unsigned)chunk,
                   (unsigned)chunk_count, (int)status);
            return;
        }
        process_runtime_command(rx_buf, chunk_bytes);

        /* Edge handshake before the next chunk: LOW confirms the slave saw this transaction end,
         * HIGH confirms it has re-armed. Skipped after the final chunk - the slave re-arms during
         * the idle gap before the next frame, which the frame-start wait above covers. */
        if ((chunk + 1) < chunk_count) {
            if (!spi_wait_ready(GPIO_PIN_RESET)) {
                printf("send_vis_frame_spi: READY stuck high after chunk %u/%u\n", (unsigned)chunk,
                       (unsigned)chunk_count);
                return; /* abandon frame; the XIAO resyncs to chunk 0 on its side */
            }
            if (!spi_wait_ready(GPIO_PIN_SET)) {
                printf("send_vis_frame_spi: XIAO did not re-arm after chunk %u/%u\n", (unsigned)chunk,
                       (unsigned)chunk_count);
                return;
            }
        }
    }

    /* Every chunk went out (the loop returns early on any failure, so reaching here means all of
     * them). Rate-limited rather than silent - lets a bare picocom session confirm sends really are
     * happening and at what rate, without flooding the console. Correlate the frame_counter/crc16
     * printed here against what the XIAO logs to tell "STM32 isn't really sending" apart from "it's
     * sending, but the XIAO can't validate it". */
    static uint32_t last_ok_log_ms = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_ok_log_ms) >= 1000) {
        last_ok_log_ms = now;
        printf("send_vis_frame_spi: sent frame %lu OK (crc=0x%04X, %u chunks x %u bytes)\n",
               (unsigned long)frame_counter, header.crc16, (unsigned)chunk_count, (unsigned)chunk_bytes);
    }

    if (switch_after_frame) {
        if (apply_spi_link(g_proposed_chunk_bytes, g_proposed_prescaler, g_proposed_plane_mask)) {
            g_rc_status.link_state = RC_LINK_VERIFYING;
            g_verify_deadline_frame = frame_counter + 10U;
        } else {
            g_rc_status.error = RC_ERROR_APPLY_FAILED;
            g_rc_status.link_state = RC_LINK_STABLE;
        }
    } else if (revert_after_frame) {
        if (apply_spi_link(g_stable_chunk_bytes, g_stable_prescaler, g_stable_plane_mask)) {
            if (g_rc_status.link_rollbacks != UINT16_MAX) {
                g_rc_status.link_rollbacks++;
            }
        } else {
            g_rc_status.error = RC_ERROR_APPLY_FAILED;
        }
        g_rc_status.link_state = RC_LINK_STABLE;
        g_proposed_chunk_bytes = g_stable_chunk_bytes;
        g_proposed_prescaler = g_stable_prescaler;
        g_proposed_plane_mask = g_stable_plane_mask;
    }
}
#endif /* CONF_STREAM_SPI */
