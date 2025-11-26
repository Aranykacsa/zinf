#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <stddef.h>
#include <stdint.h>

//#include "storage.h"
#include "driver.h"

extern driver_t *active_driver;

uint32_t crc32(const uint8_t *data, size_t len);
uint8_t read_sector(uint32_t sector, uint8_t *buffer);
uint8_t write_sector(uint32_t sector, const uint8_t *buffer);

#endif /* HELPER_H */
