#include "config.h"

const uint32_t SECTOR_SIZE = 512;
const uint32_t CRC_SIZE = 4;
const uint32_t HEADER_SIZE = 1;
const uint32_t PAYLOAD_SIZE = SECTOR_SIZE - CRC_SIZE - HEADER_SIZE;
const uint32_t RAID_MIRRORS = 3;
uint32_t RAID_OFFSET = 0;
