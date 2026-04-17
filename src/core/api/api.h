#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

/* Storage lifecycle */
uint8_t setup_storage(zinf_ctx_t *ctx);
uint8_t init_log_sector(zinf_ctx_t *ctx);

/* Raw sector I/O (bypass RAID) */
int read_sector(zinf_ctx_t *ctx, uint64_t sector, uint8_t *buffer);
int write_sector(zinf_ctx_t *ctx, uint64_t sector, const uint8_t *buffer);

/* RAID read: verifies CRC, falls back through mirrors, majority votes when M>=3 */
uint8_t raid_read(zinf_ctx_t *ctx, uint64_t logical_sector, uint8_t *payload);

/* Message log */
uint8_t save_msg(zinf_ctx_t *ctx, uint8_t *msg);
uint8_t test_save_msg(zinf_ctx_t *ctx);

/* Typed RAID writes */
uint8_t raid_sensor_values(zinf_ctx_t *ctx, sensor_t *buffer, size_t len);
uint8_t get_last_sector(zinf_ctx_t *ctx, uint64_t *last_sector);

/* -----------------------------------------------------------------------
   Fault detection and recovery
   ----------------------------------------------------------------------- */

/* Per-mirror status codes used in zinf_sector_health_t */
#define ZINF_MIRROR_OK        0u  /* CRC verified                          */
#define ZINF_MIRROR_IO_ERR    1u  /* read_sector failed                    */
#define ZINF_MIRROR_CRC_FAIL  2u  /* CRC mismatch                          */
#define ZINF_MIRROR_BLACKLIST 3u  /* LBA is in bad-sector list — skipped   */

/* Per-sector health snapshot returned by zinf_check_sector() */
typedef struct {
    uint8_t status[MAX_MIRRORS]; /* per-mirror status (ZINF_MIRROR_*)     */
    uint8_t valid_count;         /* mirrors with status == ZINF_MIRROR_OK */
} zinf_sector_health_t;

/* Scrub statistics returned by zinf_scrub() */
typedef struct {
    uint32_t checked;       /* logical sectors examined                   */
    uint32_t healthy;       /* all mirrors OK, no repair needed           */
    uint32_t repaired;      /* >=1 mirror was repaired successfully       */
    uint32_t unrecoverable; /* no valid mirror found                      */
} zinf_scrub_report_t;

/*
 * zinf_check_sector — read all mirrors for logical_sector and verify CRC.
 * Fills *out with per-mirror status and valid_count.
 * Pure read: no writes, no blacklist mutations, no side effects.
 * Returns STORAGE_ERR_PARAM if ctx or out is NULL, STORAGE_OK otherwise.
 */
uint8_t zinf_check_sector(zinf_ctx_t *ctx, uint64_t logical_sector,
                           zinf_sector_health_t *out);

/*
 * zinf_recover_sector — repair bad mirrors by copying from the best valid mirror.
 * For each mirror whose CRC fails or I/O errors:
 *   1. Write the good copy to that physical LBA.
 *   2. Re-read and re-verify CRC.
 *   3. If re-verify fails: zinf_mark_bad_sector(ctx, physical_lba).
 * Returns STORAGE_OK if >=1 mirror was valid (repair may be partial).
 * Returns STORAGE_ERR_UNRECOVERABLE if no valid mirror was found.
 * Returns STORAGE_ERR_PARAM if ctx is NULL.
 */
uint8_t zinf_recover_sector(zinf_ctx_t *ctx, uint64_t logical_sector);

/*
 * zinf_scrub — iterate logical sectors [start..end] inclusive, check and
 * recover each. Continues past unrecoverable sectors; does not stop early.
 * report may be NULL (statistics discarded).
 * Returns STORAGE_ERR_PARAM if ctx is NULL, STORAGE_OK otherwise.
 */
uint8_t zinf_scrub(zinf_ctx_t *ctx, uint64_t start, uint64_t end,
                   zinf_scrub_report_t *report);
