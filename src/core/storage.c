#include "storage.h"
#include "driver.h"
#include "helper.h"
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* ---- Return codes ---- */
#define STORAGE_OK            0
#define STORAGE_ERR_DRIVER    1
#define STORAGE_ERR_PARAM     2
#define STORAGE_ERR_FULL      3
#define STORAGE_ERR_LOG_FULL  4

#define SECTOR_SIZE 512
#define CRC_SIZE 4 // 32 bit
#define HEADER_SIZE 1
#define PAYLOAD_SIZE (SECTOR_SIZE - CRC_SIZE - HEADER_SIZE)
#define RAID_MIRRORS 3
#define RAID_OFFSET 30

/* Global driver pointer (assigned externally, e.g. from main.c) */
extern driver_t *active_driver;
extern uint32_t log_sector;

/*### DRIVER HELPERS ###*/
static int read_sector(uint32_t sector, uint8_t *buffer) {
    return active_driver->read_block(active_driver, sector, buffer);
}

static int write_sector(uint32_t sector, const uint8_t *buffer) {
    return active_driver->write_block(active_driver, sector, buffer);
}

/*### INTERNAL STATE FUNCTIONS ###*/
uint8_t get_last_sector(uint32_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t buffer[SECTOR_SIZE];
    int rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    *last_sector =
        ((uint32_t)buffer[0]) |
        ((uint32_t)buffer[1] << 8) |
        ((uint32_t)buffer[2] << 16);

    return STORAGE_OK;
}

uint8_t set_last_sector(const uint32_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t buffer[SECTOR_SIZE];
    int rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    buffer[0] = (uint8_t)(*last_sector & 0xFF);
    buffer[1] = (uint8_t)((*last_sector >> 8) & 0xFF);
    buffer[2] = (uint8_t)((*last_sector >> 16) & 0xFF);

    rc = write_sector(log_sector, buffer);
    return (rc == DRIVER_OK) ? STORAGE_OK : STORAGE_ERR_DRIVER;
}

/*### PUBLIC API ###*/
uint8_t setup_storage(void) {
    int rc = active_driver->init(active_driver);
    printf("[STORAGE] init: %d\r\n", rc);
    return (rc == DRIVER_OK) ? STORAGE_OK : STORAGE_ERR_DRIVER;
}

uint8_t test_save_msg(void) {
    uint8_t err;
    uint8_t msg = 5;
    for (uint32_t i = 0; i < 1024; i++) {
        err = save_msg(&msg);
        if (err != STORAGE_OK) return err;
    }
    return STORAGE_OK;
}

uint8_t save_msg(uint8_t *msg) {
    uint8_t buffer[SECTOR_SIZE];
    uint16_t last_msg;
    uint8_t is_first_full;

    int rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    is_first_full = buffer[5];

    last_msg =
        ((uint16_t)buffer[3]) |
        ((uint16_t)buffer[4] << 8);

    last_msg++;

    if (is_first_full) {
        if (last_msg == SECTOR_SIZE) {
            return STORAGE_ERR_LOG_FULL;
        } else {
            buffer[3] = (uint8_t)(last_msg & 0xFF);
            buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);

            rc = write_sector(log_sector, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

            rc = read_sector(log_sector + 1, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

            buffer[last_msg] = *msg;

            rc = write_sector(log_sector + 1, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;
        }
    } else {
        if (last_msg == SECTOR_SIZE) {
            last_msg = 0;
            is_first_full = 1;

            buffer[3] = (uint8_t)(last_msg & 0xFF);
            buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);
            buffer[5] = is_first_full;

            rc = write_sector(log_sector, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

            rc = read_sector(log_sector + 1, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

            buffer[last_msg] = *msg;

            rc = write_sector(log_sector + 1, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;
        } else {
            buffer[3] = (uint8_t)(last_msg & 0xFF);
            buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);
            buffer[last_msg] = *msg;

            rc = write_sector(log_sector, buffer);
            if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;
        }
    }

    return STORAGE_OK;
}

uint8_t raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    uint8_t rc;
    for (uint8_t i = 0; i < RAID_MIRRORS; i++) {
        rc = save_u8bit_values(buffer, len, header);
        if (rc != STORAGE_OK) return rc;
    }
    return STORAGE_OK;
}

uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    if (!buffer) return STORAGE_ERR_PARAM;
    if (len % PAYLOAD_SIZE != 0) return STORAGE_ERR_PARAM;

    uint32_t last_sector = 0;
    uint8_t rc = get_last_sector(&last_sector);
    if (rc != STORAGE_OK) return rc;

    uint16_t num_of_sectors = len / PAYLOAD_SIZE;
    uint32_t new_sector = last_sector;

    uint8_t sector_buffer[SECTOR_SIZE];

    for (uint16_t i = 0; i < num_of_sectors; i++) {
        new_sector++;

        for (uint16_t j = 0; j < SECTOR_SIZE; j++) sector_buffer[j] = 0;

        // header
        sector_buffer[0] = *header;

        // payload
        for (uint16_t k = 0; k < PAYLOAD_SIZE; k++) {
            sector_buffer[1 + k] = buffer[i * PAYLOAD_SIZE + k];
        }

        // CRC
        uint32_t crc = crc32(sector_buffer, HEADER_SIZE + PAYLOAD_SIZE);
        sector_buffer[508] = (uint8_t)(crc & 0xFF);
        sector_buffer[509] = (uint8_t)((crc >> 8) & 0xFF);
        sector_buffer[510] = (uint8_t)((crc >> 16) & 0xFF);
        sector_buffer[511] = (uint8_t)((crc >> 24) & 0xFF);

        int rcw = write_sector(new_sector, sector_buffer);
        if (rcw != DRIVER_OK) return STORAGE_ERR_DRIVER;
    }

    rc = set_last_sector(&new_sector);
    return (rc == STORAGE_OK) ? STORAGE_OK : rc;
}

/* First 3 bytes => last sector; 4th–5th => last log; 6th => first log full */
uint8_t init_log_sector(void) {
    uint8_t buffer[SECTOR_SIZE];
    for (uint16_t i = 0; i < SECTOR_SIZE; i++) buffer[i] = 0;

    int rc = write_sector(log_sector + 1, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    for (uint16_t i = 0; i < SECTOR_SIZE; i++) buffer[i] = 0;

    const uint32_t start_sector = 1;
    const uint16_t last_msg = 0;

    buffer[0] = (uint8_t)(start_sector & 0xFF);
    buffer[1] = (uint8_t)((start_sector >> 8) & 0xFF);
    buffer[2] = (uint8_t)((start_sector >> 16) & 0xFF);
    buffer[3] = (uint8_t)(last_msg & 0xFF);
    buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);

    rc = write_sector(log_sector, buffer);
    return (rc == DRIVER_OK) ? STORAGE_OK : STORAGE_ERR_DRIVER;
}

