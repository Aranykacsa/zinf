#pragma once
#include <stdint.h>
#include <stddef.h>

#include "config.h"

uint8_t setup_storage(void);

uint8_t save_msg(uint8_t *msg);
uint8_t test_save_msg(void);

/* compatibility exports (same names as original) */
uint8_t init_log_sector(void);
uint8_t raid_sensor_values(sensor_t *buffer, size_t len);
uint8_t get_last_sector(uint32_t *last_sector);
//uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
