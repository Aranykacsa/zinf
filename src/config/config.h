#ifndef ZINF_CONFIG_H
#define ZINF_CONFIG_H

#include <stdint.h>

/* --- Globális, mindenhol ugyanaz lesz --- */
/*#define SECTOR_SIZE   512U
#define CRC_SIZE      4U
#define HEADER_SIZE   1U
#define PAYLOAD_SIZE  (SECTOR_SIZE - CRC_SIZE - HEADER_SIZE)
#define RAID_MIRRORS  3U
extern uint32_t RAID_OFFSET;*/

typedef struct {
    uint16_t sector;
    uint32_t crc_key;
    uint8_t mirror_count;
    uint32_t mirror_offset;
} config_t;

config_t config;

extern config_t* get_config(void);

typedef struct {
    uint8_t type;
} environment_header_t;

typedef struct {
    uint32_t temperature;
    uint32_t humidity;
    uint32_t pressure;
    uint32_t altitude;
} environment_payload_t;

typedef struct {
    environment_header_t header;
    environment_payload_t* payload[512];
} environment_t;

#endif /* ZINF_CONFIG_H */
