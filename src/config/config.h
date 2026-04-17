#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================
   Sector geometry
   ========================= */
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512u
#endif

#ifndef HEADER_SIZE
#define HEADER_SIZE 1u
#endif

/* CRC is stored in last 4 bytes of sector */
#ifndef PAYLOAD_SIZE
#define PAYLOAD_SIZE (SECTOR_SIZE - HEADER_SIZE - 4u)
#endif

/* Default mirror count (used as fallback; overridden at runtime via zinf_ctx) */
#ifndef RAID_MIRRORS
#define RAID_MIRRORS 2u
#endif

/* Maximum supported mirror count — raid_read() candidates[] is sized for this */
#define MAX_MIRRORS 5u

/* Bad-sector blacklist capacity in zinf_ctx_t (from zinf.yaml max_bad_sectors) */
#define MAX_BAD_SECTORS 16u

/* =========================
   Metadata sector layout (format v3 — 64-bit LBA)
   ---------------------------------
   Each of META_COPIES copy-slots holds:
     [+0..+7]  last_sector (64-bit LE)
     [+8..+9]  version     (16-bit LE, monotonic)
   Followed by:
     [30..31]  write_pos   (16-bit LE)
     [32]      flags       (1 byte)
     [33..511] message log payload
   ========================= */
#define META_COPY_STRIDE    10u                              /* bytes per copy slot */
#define META_COPIES          3u                              /* redundant copies    */
#define META_WRITE_POS_OFF  (META_COPIES * META_COPY_STRIDE) /* = 30               */
#define META_FLAGS_OFF      (META_WRITE_POS_OFF + 2u)        /* = 32               */
#define META_HDR_SIZE       (META_FLAGS_OFF + 1u)            /* = 33               */

/* Message log capacity */
#define MSG_LOG_CAP_S0      (SECTOR_SIZE - META_HDR_SIZE)           /* 479 bytes  */
#define MSG_LOG_CAP_S1      SECTOR_SIZE                              /* 512 bytes  */
#define MSG_LOG_TOTAL_CAP   (MSG_LOG_CAP_S0 + MSG_LOG_CAP_S1)       /* 991 bytes  */

/* =========================
   Storage return codes
   ========================= */
#define STORAGE_OK                0u
#define STORAGE_ERR_PARAM         1u
#define STORAGE_ERR_DRIVER        2u
#define STORAGE_ERR_LOG_FULL      3u
#define STORAGE_ERR_UNRECOVERABLE 4u
/* Non-fatal: write committed to >=1 mirror but fewer than mirror_count */
#define STORAGE_WARN_DEGRADED     5u

/* Driver return codes */
#define DRIVER_OK        0
#define DRIVER_ERR_IO    1
#define DRIVER_ERR_INIT  2
#define DRIVER_ERR_PARAM 3

/* =========================
   Data types
   ========================= */
typedef struct sensor_t {
    float temp;
    float humidity;
} sensor_t;

/* =========================
   Runtime context
   ========================= */
struct driver_t;

typedef struct zinf_ctx_t {
    struct driver_t *driver;
    uint32_t         sector_size;      /* bytes per sector (default SECTOR_SIZE) */
    uint8_t          mirror_count;     /* number of RAID mirrors                 */
    uint8_t          metadata_sectors; /* sectors reserved for metadata          */
    uint64_t         mirror_offset;    /* sectors between mirror copies          */
    uint64_t         log_sector;       /* LBA of metadata sector (default 0)     */
    uint64_t         raid_offset;      /* runtime-computed mirror spacing        */
    /* bad-sector blacklist — RAM only; rebuilt via zinf_scrub() at startup */
    uint64_t         bad_sectors[MAX_BAD_SECTORS];
    uint8_t          bad_sector_count;
} zinf_ctx_t;

/* Default global instance (set by platform file) */
extern zinf_ctx_t *zinf_ctx;

/* Initialise *ctx from compile-time defaults.
   Caller must set ctx->driver before calling this. */
void zinf_ctx_init_defaults(zinf_ctx_t *ctx);

/* =========================
   Bad-sector blacklist helpers (static inline — available everywhere config.h is included)
   ========================= */
static inline bool zinf_is_bad_sector(const zinf_ctx_t *ctx, uint64_t lba) {
    for (uint8_t _i = 0; _i < ctx->bad_sector_count; _i++)
        if (ctx->bad_sectors[_i] == lba) return true;
    return false;
}

static inline uint8_t zinf_mark_bad_sector(zinf_ctx_t *ctx, uint64_t lba) {
    if (zinf_is_bad_sector(ctx, lba))             return STORAGE_OK;
    if (ctx->bad_sector_count >= MAX_BAD_SECTORS) return STORAGE_ERR_PARAM;
    ctx->bad_sectors[ctx->bad_sector_count++] = lba;
    return STORAGE_OK;
}

static inline void zinf_clear_bad_sectors(zinf_ctx_t *ctx) {
    ctx->bad_sector_count = 0u;
}
