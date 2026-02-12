#pragma once
#include <stdint.h>
#include <stddef.h>

/* last-sector metadata stored in log_sector header */
uint8_t log_get_last_sector(uint32_t *last_sector);
uint8_t log_set_last_sector(const uint32_t *last_sector);

/* init log header sectors */
uint8_t log_init_log_sector(void);

/* writers */
uint8_t log_raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
uint8_t log_save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
