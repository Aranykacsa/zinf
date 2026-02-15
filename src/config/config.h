#pragma once
#include <stdint.h>
#include <stddef.h>

/* =========================
   Host-tool constants (bench/reader)
   ========================= */
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512u
#endif

#ifndef HEADER_SIZE
#define HEADER_SIZE 1u
#endif

/* CRC is stored in last 4 bytes of sector */
#ifndef PAYLOAD_SIZE
#define PAYLOAD_SIZE (SECTOR_SIZE - HEADER_SIZE - 4u)
#endif

#ifndef RAID_MIRRORS
#define RAID_MIRRORS 2u
#endif

/* Host tools compute this (main.c / reader.c) */
extern uint32_t RAID_OFFSET;

/* =========================
   Runtime config for core
   ========================= */
typedef struct config_t {
    uint32_t sector_size;     /* bytes */
    uint8_t  mirror_count;    /* number of mirrors */
    uint32_t mirror_offset;   /* sectors between mirrors */
} config_t;

/* Global pointer used by core */
extern config_t *config;

/* Initialize config defaults (and sync with RAID_OFFSET) */
void config_init_defaults(void);

/* Call this after changing RAID_OFFSET to keep config->mirror_offset in sync */
static inline void config_sync_raid_offset(void) {
    if (config) config->mirror_offset = RAID_OFFSET;
}

typedef struct sensor_t {
    float temp;
    float humidity;
} sensor_t;

#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- Storage return codes ---- */
#define STORAGE_OK             0
#define STORAGE_ERR_PARAM      1
#define STORAGE_ERR_DRIVER     2
#define STORAGE_ERR_LOG_FULL   3

/* Driver return codes (must match your driver implementation) */
#define DRIVER_OK              0

/* ---- External state (defined in main.c / platform) ---- */
struct driver_t;
extern struct driver_t *active_driver;
extern uint32_t log_sector;

/* ---- Storage low-level I/O (implemented in core/data/data.c) ---- */
int read_sector(uint32_t sector, uint8_t *buffer);
int write_sector(uint32_t sector, const uint8_t *buffer);

/* ---- Public API (implemented in core/data/data.c) ---- */
uint8_t setup_storage(void);

/* “msg log” API */
uint8_t save_msg(uint8_t *msg);
uint8_t test_save_msg(void);

/* Log init + writers (compatible names) */
uint8_t init_log_sector(void);
uint8_t raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header);
