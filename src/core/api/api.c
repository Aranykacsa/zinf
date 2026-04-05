#include "api.h"
#include "driver.h"
#include "storage.h"
#include "helper.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
   Raw sector I/O
   ----------------------------------------------------------------------- */

int read_sector(zinf_ctx_t *ctx, uint64_t sector, uint8_t *buffer) {
    if (ctx->driver->read_blocks)
        return ctx->driver->read_blocks(ctx->driver, sector, buffer, 1);
    return ctx->driver->read_block(ctx->driver, sector, buffer);
}

int write_sector(zinf_ctx_t *ctx, uint64_t sector, const uint8_t *buffer) {
    if (ctx->driver->write_blocks)
        return ctx->driver->write_blocks(ctx->driver, sector, buffer, 1);
    return ctx->driver->write_block(ctx->driver, sector, buffer);
}

/* -----------------------------------------------------------------------
   Lifecycle
   ----------------------------------------------------------------------- */

uint8_t setup_storage(zinf_ctx_t *ctx) {
    zinf_ctx_init_defaults(ctx);
    int rc = ctx->driver->init(ctx->driver);
    printf("[STORAGE] init: %d\r\n", rc);
    return (rc == DRIVER_OK) ? STORAGE_OK : STORAGE_ERR_DRIVER;
}

uint8_t init_log_sector(zinf_ctx_t *ctx) {
    return log_init_log_sector(ctx);
}

uint8_t get_last_sector(zinf_ctx_t *ctx, uint64_t *last_sector) {
    return log_get_last_sector(ctx, last_sector);
}

/* -----------------------------------------------------------------------
   RAID read with CRC verification + mirror fallback + majority voting
   ----------------------------------------------------------------------- */

uint8_t raid_read(zinf_ctx_t *ctx, uint64_t logical_sector, uint8_t *payload) {
    if (!payload) return STORAGE_ERR_PARAM;

    uint8_t  raw[SECTOR_SIZE];
    uint8_t  candidates[PAYLOAD_SIZE * MAX_MIRRORS];
    uint8_t  valid_count  = 0;
    /* mirror_count is clamped to MAX_MIRRORS by zinf_ctx_init_defaults */
    uint8_t  limit = (ctx->mirror_count < MAX_MIRRORS)
                     ? ctx->mirror_count : (uint8_t)MAX_MIRRORS;

    for (uint8_t m = 0; m < limit; m++) {
        uint64_t phys = logical_sector + (uint64_t)m * ctx->mirror_offset;

        if (read_sector(ctx, phys, raw) != DRIVER_OK) continue;

        uint32_t stored = (uint32_t)raw[ctx->sector_size - 4]
                        | ((uint32_t)raw[ctx->sector_size - 3] << 8)
                        | ((uint32_t)raw[ctx->sector_size - 2] << 16)
                        | ((uint32_t)raw[ctx->sector_size - 1] << 24);
        uint32_t calc = crc32(raw, HEADER_SIZE + PAYLOAD_SIZE);

        if (calc == stored) {
            memcpy(&candidates[valid_count * PAYLOAD_SIZE],
                   &raw[HEADER_SIZE], PAYLOAD_SIZE);
            valid_count++;
        }
    }

    if (valid_count == 0) return STORAGE_ERR_UNRECOVERABLE;

    /* For a single valid mirror just return it */
    if (valid_count == 1 || ctx->mirror_count < 3) {
        memcpy(payload, &candidates[0], PAYLOAD_SIZE);
        return STORAGE_OK;
    }

    /* Majority vote: find the candidate agreed upon by > mirror_count/2 mirrors */
    uint8_t majority_threshold = (uint8_t)(ctx->mirror_count / 2u + 1u);
    for (uint8_t i = 0; i < valid_count; i++) {
        uint8_t votes = 1;
        for (uint8_t j = 0; j < valid_count; j++) {
            if (j != i && memcmp(&candidates[i * PAYLOAD_SIZE],
                                 &candidates[j * PAYLOAD_SIZE],
                                 PAYLOAD_SIZE) == 0) {
                votes++;
            }
        }
        if (votes >= majority_threshold) {
            memcpy(payload, &candidates[i * PAYLOAD_SIZE], PAYLOAD_SIZE);
            return STORAGE_OK;
        }
    }

    /* No majority found — all valid mirrors disagree; data is unrecoverable */
    return STORAGE_ERR_UNRECOVERABLE;
}

/* -----------------------------------------------------------------------
   Message log
   Uses ctx->log_sector (sector 0) and ctx->log_sector+1 for the log.
   write_pos is stored at META_WRITE_POS_OFF inside the metadata sector.
   ----------------------------------------------------------------------- */

static inline uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void wr_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

uint8_t save_msg(zinf_ctx_t *ctx, uint8_t *msg) {
    uint8_t  buffer[SECTOR_SIZE];
    uint16_t pos;
    uint32_t target_sector;
    uint16_t offset_in_sector;

    if (read_sector(ctx, ctx->log_sector, buffer) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    pos = rd_u16_le(&buffer[META_WRITE_POS_OFF]);

    if (pos >= (uint16_t)MSG_LOG_TOTAL_CAP)
        return STORAGE_ERR_LOG_FULL;

    /* Map logical payload position to physical (sector, offset) */
    if (pos < (uint16_t)MSG_LOG_CAP_S0) {
        target_sector    = ctx->log_sector;
        offset_in_sector = (uint16_t)(META_HDR_SIZE + pos);
    } else {
        target_sector    = ctx->log_sector + 1u;
        offset_in_sector = (uint16_t)(pos - MSG_LOG_CAP_S0);
    }

    if (read_sector(ctx, target_sector, buffer) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    buffer[offset_in_sector] = *msg;

    if (write_sector(ctx, target_sector, buffer) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    /* Persist incremented write cursor back to metadata sector */
    pos++;
    if (read_sector(ctx, ctx->log_sector, buffer) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    wr_u16_le(&buffer[META_WRITE_POS_OFF], pos);

    if (write_sector(ctx, ctx->log_sector, buffer) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    return STORAGE_OK;
}

uint8_t test_save_msg(zinf_ctx_t *ctx) {
    uint8_t msg = 5u;
    for (uint32_t i = 0; i < 1024u; i++) {
        uint8_t err = save_msg(ctx, &msg);
        if (err != STORAGE_OK) return err;
    }
    return STORAGE_OK;
}

/* -----------------------------------------------------------------------
   Typed RAID writes
   ----------------------------------------------------------------------- */

#define SENSOR_WIRE_SIZE 8u  /* 2 × float32 */

static inline void pack_f32_le(uint8_t out[4], float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    out[0] = (uint8_t)(u & 0xFFu);
    out[1] = (uint8_t)((u >> 8)  & 0xFFu);
    out[2] = (uint8_t)((u >> 16) & 0xFFu);
    out[3] = (uint8_t)((u >> 24) & 0xFFu);
}

static inline size_t sensor_to_wire(uint8_t *out, const sensor_t *s) {
    pack_f32_le(&out[0], s->temp);
    pack_f32_le(&out[4], s->humidity);
    return SENSOR_WIRE_SIZE;
}

uint8_t raid_sensor_values(zinf_ctx_t *ctx, sensor_t *buffer, size_t len) {
    if (!buffer || len == 0u) return STORAGE_ERR_PARAM;

    uint8_t  header    = 0x01u;
    size_t   wire_size = len * SENSOR_WIRE_SIZE;
    uint8_t *wire      = (uint8_t *)malloc(wire_size);
    if (!wire) return STORAGE_ERR_PARAM;

    for (size_t i = 0; i < len; i++)
        sensor_to_wire(&wire[i * SENSOR_WIRE_SIZE], &buffer[i]);

    uint8_t rc = log_raid_u8bit_values(ctx, wire, wire_size, &header);
    free(wire);
    return rc;
}
