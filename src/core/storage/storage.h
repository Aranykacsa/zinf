#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

/* Metadata management */
uint8_t log_get_last_sector(zinf_ctx_t *ctx, uint64_t *last_sector);
uint8_t log_set_last_sector(zinf_ctx_t *ctx, const uint64_t *last_sector);
uint8_t log_init_log_sector(zinf_ctx_t *ctx);

/* RAID write */
uint8_t log_raid_u8bit_values(zinf_ctx_t *ctx, uint8_t *buffer, size_t len, uint8_t *header);
