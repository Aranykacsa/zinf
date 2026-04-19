#pragma once
#include "driver.h"

extern driver_t ram_driver;

/* Testing API */
void ram_driver_set_capacity(uint64_t sectors);
void ram_driver_corrupt(uint64_t lba, uint32_t offset, uint8_t val);
void ram_driver_drop_buffer(void);
