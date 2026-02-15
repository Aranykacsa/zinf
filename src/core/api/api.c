#include "api.h"

#include "driver.h"
#include "storage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern driver_t *active_driver;
extern uint32_t log_sector;

/* sector I/O wrappers used by log.c and others */
int read_sector(uint32_t sector, uint8_t *buffer) {
    if (active_driver->read_blocks) {
        return active_driver->read_blocks(active_driver, sector, buffer, 1);
    }
    return active_driver->read_block(active_driver, sector, buffer);
}

int write_sector(uint32_t sector, const uint8_t *buffer) {
    if (active_driver->write_blocks) {
        return active_driver->write_blocks(active_driver, sector, buffer, 1);
    }
    return active_driver->write_block(active_driver, sector, buffer);
}

uint8_t setup_storage(void) {
    /* ensure config exists */
    config_init_defaults();

    int rc = active_driver->init(active_driver);
    printf("[STORAGE] init: %d\r\n", rc);
    return (rc == DRIVER_OK) ? STORAGE_OK : STORAGE_ERR_DRIVER;
}

/* === message logger that uses log_sector and log_sector+1 as before === */
uint8_t test_save_msg(void) {
    uint8_t err;
    uint8_t msg = 5;
    for (uint32_t i = 0; i < 1024; i++) {
        err = save_msg(&msg);
        if (err != STORAGE_OK) return err;
    }
    return STORAGE_OK;
}

#define LOG_HDR_SIZE    6u
#define LOG_DATA0_CAP   (SECTOR_SIZE - LOG_HDR_SIZE)
#define LOG_TOTAL_CAP   (LOG_DATA0_CAP + SECTOR_SIZE)  // payload bytes across 2 sectors

static inline uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void wr_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

uint8_t save_msg(uint8_t *msg) {
    uint8_t buffer[SECTOR_SIZE];
    uint16_t pos;               // NEXT write position in payload stream
    uint32_t target_sector;
    uint16_t offset_in_sector;

    int rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    pos = rd_u16_le(&buffer[3]);

    if (pos >= (uint16_t)LOG_TOTAL_CAP) {
        return STORAGE_ERR_LOG_FULL;
    }

    // Map payload pos to physical location (skip header in first sector)
    if (pos < LOG_DATA0_CAP) {
        target_sector = log_sector;
        offset_in_sector = (uint16_t)(LOG_HDR_SIZE + pos);
    } else {
        target_sector = log_sector + 1;
        offset_in_sector = (uint16_t)(pos - LOG_DATA0_CAP);
    }

    rc = read_sector(target_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    buffer[offset_in_sector] = *msg;

    rc = write_sector(target_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    // Persist incremented pos
    pos++;

    rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    wr_u16_le(&buffer[3], pos);

    rc = write_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    return STORAGE_OK;
}


/* === compatibility wrappers (so main.c stays unchanged) === */
uint8_t init_log_sector(void) {
    return log_init_log_sector();
}

#ifndef SENSOR_WIRE_SIZE
#define SENSOR_WIRE_SIZE 8u  // 2x float32
#endif

// If you don't want to add a new error code, just reuse an existing one.
// Pick something that already exists in storage.h; here I'll use STORAGE_ERR_DRIVER as "bad arg".
#ifndef STORAGE_ERR_INVALID_ARG
#define STORAGE_ERR_INVALID_ARG STORAGE_ERR_DRIVER
#endif

static inline void pack_u32_le(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    out[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void pack_f32_le(uint8_t out[4], float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    pack_u32_le(out, u);
}

// write N bytes by repeatedly calling save_msg()
static uint8_t save_bytes(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = p[i];
        uint8_t err = save_msg(&b);
        if (err != STORAGE_OK) return err;
    }
    return STORAGE_OK;
}

static inline size_t sensor_to_u8(uint8_t *out, size_t cap, const sensor_t *s) {
    if (cap < SENSOR_WIRE_SIZE) return 0;
    pack_f32_le(&out[0], s->temp);
    pack_f32_le(&out[4], s->humidity);
    return SENSOR_WIRE_SIZE;
}

uint8_t save_sensor(const sensor_t *s) {
    uint8_t payload[SENSOR_WIRE_SIZE];
    size_t n = sensor_to_u8(payload, sizeof(payload), s);
    if (n == 0) return STORAGE_ERR_INVALID_ARG;
    return save_bytes(payload, n);
}



uint8_t raid_sensor_values(sensor_t *buffer, size_t len) {
    uint8_t header = 0x1;  // NOT const, because log expects uint8_t*
    uint8_t payload[SENSOR_WIRE_SIZE];

    for (size_t i = 0; i < len; i++) {
        if (sensor_to_u8(payload, sizeof(payload), &buffer[i]) == 0) {
            return STORAGE_ERR_INVALID_ARG;
        }
        uint8_t err = log_raid_u8bit_values(payload, sizeof(payload), &header);
        if (err != STORAGE_OK) return err;
    }
    return STORAGE_OK;
}
