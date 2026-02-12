#include "data.h"

#include "storage.h"
#include "driver.h"
#include "config.h"
#include "log.h"

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
        if (last_msg == config->sector_size) {
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
        if (last_msg == config->sector_size) {
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

/* === compatibility wrappers (so main.c stays unchanged) === */
uint8_t init_log_sector(void) {
    return log_init_log_sector();
}

uint8_t raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    return log_raid_u8bit_values(buffer, len, header);
}

uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    return log_save_u8bit_values(buffer, len, header);
}
