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
