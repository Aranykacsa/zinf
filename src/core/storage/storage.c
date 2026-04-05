#include "storage.h"
#include "driver.h"
#include "config.h"
#include "helper.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
   Internal helpers
   ----------------------------------------------------------------------- */

static int ctx_read_sector(zinf_ctx_t *ctx, uint64_t lba, uint8_t *buf) {
    if (ctx->driver->read_blocks)
        return ctx->driver->read_blocks(ctx->driver, lba, buf, 1);
    return ctx->driver->read_block(ctx->driver, lba, buf);
}

static int ctx_write_sector(zinf_ctx_t *ctx, uint64_t lba, const uint8_t *buf) {
    if (ctx->driver->write_blocks)
        return ctx->driver->write_blocks(ctx->driver, lba, buf, 1);
    return ctx->driver->write_block(ctx->driver, lba, buf);
}

static int write_sectors(zinf_ctx_t *ctx, uint64_t start, const uint8_t *buf, uint32_t count) {
    if (ctx->driver->write_blocks)
        return ctx->driver->write_blocks(ctx->driver, start, buf, count);

    for (uint32_t i = 0; i < count; i++) {
        int rc = ctx->driver->write_block(ctx->driver, start + i,
                                          buf + (i * ctx->sector_size));
        if (rc != DRIVER_OK) return rc;
    }
    return DRIVER_OK;
}

/* -----------------------------------------------------------------------
   Metadata: last-sector pointer with 3 redundant copies + versioning
   Layout of sector ctx->log_sector (format v3 — 64-bit LBA):
     Copy i (i = 0..META_COPIES-1) starts at byte i*META_COPY_STRIDE:
       [+0..+7] last_sector (uint64 LE)
       [+8..+9] version     (uint16 LE, monotonic)
     [META_WRITE_POS_OFF..+1]  write_pos (uint16 LE)
     [META_FLAGS_OFF]           flags     (uint8)
     [META_HDR_SIZE..511]       message log payload
   ----------------------------------------------------------------------- */

uint8_t log_get_last_sector(zinf_ctx_t *ctx, uint64_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t buf[SECTOR_SIZE];
    if (ctx_read_sector(ctx, ctx->log_sector, buf) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    /* Pick the copy with the newest version counter.
       Comparison uses modular arithmetic so the counter wraps correctly:
       ver_a is newer than ver_b iff (uint16_t)(ver_a - ver_b) < 0x8000u */
    uint16_t best_ver = 0;
    uint64_t best_val = 0;

    for (uint8_t i = 0; i < META_COPIES; i++) {
        uint16_t off = (uint16_t)(i * META_COPY_STRIDE);
        uint64_t val = (uint64_t)buf[off]
                     | ((uint64_t)buf[off + 1] << 8)
                     | ((uint64_t)buf[off + 2] << 16)
                     | ((uint64_t)buf[off + 3] << 24)
                     | ((uint64_t)buf[off + 4] << 32)
                     | ((uint64_t)buf[off + 5] << 40)
                     | ((uint64_t)buf[off + 6] << 48)
                     | ((uint64_t)buf[off + 7] << 56);
        uint16_t ver = (uint16_t)buf[off + 8]
                     | ((uint16_t)buf[off + 9] << 8);

        /* i==0: unconditional first assignment; otherwise pick newer version */
        if (i == 0 || (uint16_t)(ver - best_ver) < 0x8000u) {
            best_ver = ver;
            best_val = val;
        }
    }

    *last_sector = best_val;
    return STORAGE_OK;
}

uint8_t log_set_last_sector(zinf_ctx_t *ctx, const uint64_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t buf[SECTOR_SIZE];
    if (ctx_read_sector(ctx, ctx->log_sector, buf) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

    /* Find the slot with the newest version (modular comparison) */
    uint16_t max_ver  = 0;
    uint8_t  max_slot = 0;

    for (uint8_t i = 0; i < META_COPIES; i++) {
        uint16_t off = (uint16_t)(i * META_COPY_STRIDE);
        uint16_t ver = (uint16_t)buf[off + 8]
                     | ((uint16_t)buf[off + 9] << 8);
        if (i == 0 || (uint16_t)(ver - max_ver) < 0x8000u) {
            max_ver  = ver;
            max_slot = i;
        }
    }

    /* Write to the NEXT slot (round-robin) with version = max+1. */
    uint8_t  next_slot = (uint8_t)((max_slot + 1u) % META_COPIES);
    uint16_t next_ver  = (uint16_t)(max_ver + 1u);
    uint16_t off       = (uint16_t)(next_slot * META_COPY_STRIDE);

    buf[off]     = (uint8_t)(*last_sector & 0xFFu);
    buf[off + 1] = (uint8_t)((*last_sector >> 8)  & 0xFFu);
    buf[off + 2] = (uint8_t)((*last_sector >> 16) & 0xFFu);
    buf[off + 3] = (uint8_t)((*last_sector >> 24) & 0xFFu);
    buf[off + 4] = (uint8_t)((*last_sector >> 32) & 0xFFu);
    buf[off + 5] = (uint8_t)((*last_sector >> 40) & 0xFFu);
    buf[off + 6] = (uint8_t)((*last_sector >> 48) & 0xFFu);
    buf[off + 7] = (uint8_t)((*last_sector >> 56) & 0xFFu);
    buf[off + 8] = (uint8_t)(next_ver & 0xFFu);
    buf[off + 9] = (uint8_t)((next_ver >> 8) & 0xFFu);

    if (ctx_write_sector(ctx, ctx->log_sector, buf) != DRIVER_OK)
        return STORAGE_ERR_DRIVER;
    return STORAGE_OK;
}

uint8_t log_init_log_sector(zinf_ctx_t *ctx) {
    uint8_t buf[SECTOR_SIZE];
    memset(buf, 0, sizeof(buf));

    uint8_t meta_sects = (ctx->metadata_sectors > 0) ? ctx->metadata_sectors : 2u;

    /* Zero all reserved metadata sectors */
    for (uint8_t s = 0; s < meta_sects; s++) {
        if (ctx_write_sector(ctx, ctx->log_sector + s, buf) != DRIVER_OK)
            return STORAGE_ERR_DRIVER;
    }
    return STORAGE_OK;
}

/* -----------------------------------------------------------------------
   RAID write
   ----------------------------------------------------------------------- */

uint8_t log_raid_u8bit_values(zinf_ctx_t *ctx, uint8_t *buffer, size_t len, uint8_t *header) {
    if (!buffer || !header) return STORAGE_ERR_PARAM;

    uint64_t last_log_index = 0;
    uint8_t  rc = log_get_last_sector(ctx, &last_log_index);
    if (rc != STORAGE_OK) return rc;

    /* Ceiling division: a partial tail fills one extra sector with zero padding */
    uint32_t num_chunks        = (len > 0u)
                                 ? (uint32_t)((len + PAYLOAD_SIZE - 1u) / PAYLOAD_SIZE)
                                 : 0u;
    if (num_chunks == 0u) return STORAGE_OK;

    uint64_t base_write_cursor = last_log_index + 1u;

    size_t   total_size  = (size_t)num_chunks * ctx->sector_size;
    uint8_t *bulk_buffer = (uint8_t *)malloc(total_size);

    if (bulk_buffer != NULL) {
        /* Build all sectors in one allocation */
        for (uint32_t i = 0; i < num_chunks; i++) {
            uint8_t *sp = &bulk_buffer[i * ctx->sector_size];
            memset(sp, 0, ctx->sector_size);
            sp[0] = *header;
            size_t src_off   = (size_t)i * PAYLOAD_SIZE;
            size_t chunk_len = (src_off + PAYLOAD_SIZE <= len)
                               ? PAYLOAD_SIZE : (len - src_off);
            memcpy(&sp[1], &buffer[src_off], chunk_len);
            uint32_t crc = crc32(sp, HEADER_SIZE + PAYLOAD_SIZE);
            sp[ctx->sector_size - 4] = (uint8_t)(crc & 0xFFu);
            sp[ctx->sector_size - 3] = (uint8_t)((crc >> 8)  & 0xFFu);
            sp[ctx->sector_size - 2] = (uint8_t)((crc >> 16) & 0xFFu);
            sp[ctx->sector_size - 1] = (uint8_t)((crc >> 24) & 0xFFu);
        }

        for (uint8_t m = 0; m < ctx->mirror_count; m++) {
            uint64_t phys = base_write_cursor + (uint64_t)m * ctx->mirror_offset;
            if (write_sectors(ctx, phys, bulk_buffer, num_chunks) != DRIVER_OK) {
                free(bulk_buffer);
                return STORAGE_ERR_DRIVER;
            }
        }
        free(bulk_buffer);
    } else {
        /* Low-RAM fallback: sector-by-sector */
        uint8_t  sector_buf[SECTOR_SIZE];
        uint64_t cursor = base_write_cursor;

        for (uint32_t i = 0; i < num_chunks; i++) {
            memset(sector_buf, 0, sizeof(sector_buf));
            sector_buf[0] = *header;
            size_t src_off   = (size_t)i * PAYLOAD_SIZE;
            size_t chunk_len = (src_off + PAYLOAD_SIZE <= len)
                               ? PAYLOAD_SIZE : (len - src_off);
            memcpy(&sector_buf[1], &buffer[src_off], chunk_len);
            uint32_t crc = crc32(sector_buf, HEADER_SIZE + PAYLOAD_SIZE);
            sector_buf[ctx->sector_size - 4] = (uint8_t)(crc & 0xFFu);
            sector_buf[ctx->sector_size - 3] = (uint8_t)((crc >> 8)  & 0xFFu);
            sector_buf[ctx->sector_size - 2] = (uint8_t)((crc >> 16) & 0xFFu);
            sector_buf[ctx->sector_size - 1] = (uint8_t)((crc >> 24) & 0xFFu);

            for (uint8_t m = 0; m < ctx->mirror_count; m++) {
                uint64_t phys = cursor + (uint64_t)m * ctx->mirror_offset;
                if (ctx_write_sector(ctx, phys, sector_buf) != DRIVER_OK)
                    return STORAGE_ERR_DRIVER;
            }
            cursor++;
        }
        uint64_t new_last = cursor - 1u;
        return log_set_last_sector(ctx, &new_last);
    }

    uint64_t new_last = base_write_cursor + num_chunks - 1u;
    return log_set_last_sector(ctx, &new_last);
}
