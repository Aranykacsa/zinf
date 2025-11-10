#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

extern const uint32_t SECTOR_SIZE;
extern const uint32_t CRC_SIZE;
extern const uint32_t HEADER_SIZE;
extern const uint32_t PAYLOAD_SIZE;
extern const uint32_t RAID_MIRRORS;
extern uint32_t RAID_OFFSET;

#endif /* CONFIG_H */
