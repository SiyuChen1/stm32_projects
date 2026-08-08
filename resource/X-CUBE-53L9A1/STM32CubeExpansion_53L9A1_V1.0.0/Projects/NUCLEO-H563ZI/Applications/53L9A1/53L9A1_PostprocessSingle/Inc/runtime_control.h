#ifndef RUNTIME_CONTROL_H
#define RUNTIME_CONTROL_H

#include <stdint.h>

/* Versioned, CRC-protected control plane carried full-duplex alongside each
 * image chunk.  XIAO -> STM32 commands occupy the first bytes of MISO; STM32
 * -> XIAO status is placed immediately after the selected image payload in
 * the padded MOSI frame.  Keep this file byte-for-byte compatible with the
 * XIAO copy. */
#define RC_PROTOCOL_VERSION 2U
#define RC_COMMAND_MAGIC "V9CT"
#define RC_STATUS_MAGIC  "V9ST"

enum {
    RC_OP_IDLE = 0,
    RC_OP_APPLY_SAFE = 1,
    RC_OP_PROPOSE_LINK = 2,
    RC_OP_COMMIT_LINK = 3,
    RC_OP_VERIFY_LINK = 4,
    RC_OP_ABORT_LINK = 5,
};

enum {
    RC_PLANE_AMPLITUDE = 1U << 0,
    RC_PLANE_DEPTH = 1U << 1,
    RC_PLANE_AMBIENT = 1U << 2,
    RC_PLANE_ALL = RC_PLANE_AMPLITUDE | RC_PLANE_DEPTH | RC_PLANE_AMBIENT,
};

enum {
    RC_LINK_STABLE = 0,
    RC_LINK_PREPARED = 1,
    RC_LINK_SWITCH_AFTER_FRAME = 2,
    RC_LINK_VERIFYING = 3,
    RC_LINK_REVERT_AFTER_FRAME = 4,
};

enum {
    RC_FLAG_UART_STREAM = 1U << 0,
    RC_FLAG_PROFILER = 1U << 1,
};

enum {
    RC_ERROR_NONE = 0,
    RC_ERROR_BAD_COMMAND = 1,
    RC_ERROR_BAD_VALUE = 2,
    RC_ERROR_PROFILE_RESTART_REQUIRED = 3,
    RC_ERROR_APPLY_FAILED = 4,
    RC_ERROR_LINK_TIMEOUT = 5,
};

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t op;
    uint16_t flags;
    uint32_t sequence;
    uint32_t uart_baud;
    uint16_t spi_prescaler;
    uint16_t chunk_bytes;
    uint8_t bypass_mask;
    uint8_t profile;
    uint8_t plane_mask;
    uint8_t reserved;
    uint16_t crc16;
} rc_command_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t link_state;
    uint8_t error;
    uint8_t bypass_mask;
    uint16_t flags;
    uint16_t chunk_bytes;
    uint16_t chunk_count;
    uint16_t spi_prescaler;
    uint8_t profile;
    uint8_t binning;
    uint8_t plane_mask;
    uint8_t reserved;
    uint32_t ack_sequence;
    uint32_t frame_counter;
    uint32_t uart_baud;
    uint32_t spi_hz;
    uint16_t fps_x100;
    uint16_t acquire_ms;
    uint16_t transform_ms;
    uint16_t readout_ms;
    uint16_t print_ms;
    uint16_t uart_ms;
    uint16_t spi_ms;
    uint16_t total_ms;
    uint16_t command_crc_errors;
    uint16_t link_rollbacks;
    uint16_t crc16;
} rc_status_t;

#ifdef __cplusplus
static_assert(sizeof(rc_command_t) == 26U, "runtime command layout drifted");
static_assert(sizeof(rc_status_t) == 58U, "runtime status layout drifted");
#else
_Static_assert(sizeof(rc_command_t) == 26U, "runtime command layout drifted");
_Static_assert(sizeof(rc_status_t) == 58U, "runtime status layout drifted");
#endif

#endif /* RUNTIME_CONTROL_H */
