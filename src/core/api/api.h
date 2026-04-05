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
