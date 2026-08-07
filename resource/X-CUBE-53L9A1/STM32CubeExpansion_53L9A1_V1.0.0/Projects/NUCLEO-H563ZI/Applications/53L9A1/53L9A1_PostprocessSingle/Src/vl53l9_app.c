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

/* USER CODE BEGIN Includes */
#include "stm32h5xx_nucleo.h" /* hcom_uart[], COM1, HAL_UART_Transmit - used to stream binary frames to the PC */
/* USER CODE END Includes */

/* application customization */
#define CONF_DEVICE_ID   (0) /**< select device entry in platform descriptor array (see vl53l9_device.c) */
#define CONF_PRINT_FRAME (0) /**< enable printing depth frames as ascii art (slows performance) */
#define CONF_USECASE     (VL53L9_USECASE_AR_PRECISION) /**< select ranging profile to be applied (see vl53l9_utils.h) */

/**
 * @brief Enable streaming amplitude+depth frames to the PC as a binary packet over the COM1 UART.
 * Pair with Utilities/vl53l9_visualizer.py on the host. Requires BspCOMInit.BaudRate in main.c
 * to be raised (see comment there) since 115200 bps is far too slow for image-sized payloads.
 */
#define CONF_STREAM_VISUALIZER (1)

/**
 * @brief Max consecutive retries for one frame's trigger/wait/read sequence before giving up and
 * calling handle_error(). Observed on hardware: this sequence intermittently fails in different ways
 * (stale interrupt flag, IRQ wait timeout, sensor-reported transient fault) on different runs - a
 * transient timing hiccup, not a persistent fault - so it's worth a few retries before treating it as
 * fatal. Worst case adds up to ACQUIRE_MAX_RETRIES seconds of latency to a single frame (each failed
 * attempt can wait up to the 1000ms IRQ timeout).
 */
#define ACQUIRE_MAX_RETRIES (5)

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
#if CONF_STREAM_VISUALIZER
static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc);
static void send_vis_frame(uint32_t frame_counter, uint8_t width, uint8_t height, const float *amplitude,
                            const float *depth, const float *ambient);
#endif

void vl53l9_app() {

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

#if CONF_STREAM_VISUALIZER
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

#if CONF_STREAM_VISUALIZER
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
#if CONF_STREAM_VISUALIZER
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

#if CONF_STREAM_VISUALIZER
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
#if CONF_STREAM_VISUALIZER
                                                out_amp_stream_buffer,
                                                out_ambient_stream_buffer,
#endif
                                            },
#if CONF_STREAM_VISUALIZER
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

        /* measure frame rate */
        stop_time = platform_profiler_get_timestamp();
        frame_rate = (1.0f / (float)(platform_profiler_convert_to_us(stop_time - start_time))) * 1000000;
        start_time = stop_time;

        if (depth_ready) {
            print_frame((float *)out_depth_mem.data, out_height, out_width);
            printf("Processed frame n. %lu @ %u fps\n", (unsigned long)frame.p_metadata->frame_counter,
                   (unsigned int)frame_rate);
#if CONF_STREAM_VISUALIZER
            send_vis_frame(frame.p_metadata->frame_counter, out_width, out_height, (const float *)out_amp_mem.data,
                            (const float *)out_depth_mem.data, (const float *)out_ambient_mem.data);
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

#if CONF_STREAM_VISUALIZER
/**
 * @brief CRC-16/CCITT (poly 0x1021, no reflection). Must match the Python-side implementation
 * in Utilities/vl53l9_visualizer.py so the host can validate/resync on the byte stream.
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
#endif /* CONF_STREAM_VISUALIZER */
