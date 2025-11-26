#include "helper.h"

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}


uint8_t read_sector(uint32_t sector, uint8_t *buffer) {
  if (!active_driver || !buffer)
    return DRIVER_ERR_INIT;
  return active_driver->read_block(active_driver, sector, buffer);
}

uint8_t write_sector(uint32_t sector, const uint8_t *buffer) {
  if (!active_driver || !buffer)
    return DRIVER_ERR_INIT;
  return active_driver->write_block(active_driver, sector, buffer);
}
