#include "storage.h"

#include "driver.h"
#include "config.h"
#include "helper.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

extern driver_t *active_driver;
extern uint32_t log_sector;

static int write_sectors(uint32_t start_sector, const uint8_t *buffer, uint32_t count) {
    if (active_driver->write_blocks != NULL) {
        return active_driver->write_blocks(active_driver, start_sector, buffer, count);
    }

    for (uint32_t i = 0; i < count; i++) {
        int rc = active_driver->write_block(
            active_driver,
            start_sector + i,
            buffer + (i * config->sector_size)
        );
        if (rc != DRIVER_OK) return rc;
    }
    return DRIVER_OK;
}

uint8_t log_get_last_sector(uint32_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t buffer[SECTOR_SIZE];
    int rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    *last_sector =
        ((uint32_t)buffer[0])       |
        ((uint32_t)buffer[1] << 8)  |
        ((uint32_t)buffer[2] << 16);

    return STORAGE_OK;
}

uint8_t log_set_last_sector(const uint32_t *last_sector) {
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

uint8_t log_init_log_sector(void) {
    uint8_t buffer[SECTOR_SIZE];
    memset(buffer, 0, sizeof(buffer));

    int rc = write_sector(log_sector + 1, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    memset(buffer, 0, sizeof(buffer));

    const uint32_t start_sector = 1;
    const uint16_t last_msg     = 0;

    buffer[0] = (uint8_t)(start_sector & 0xFF);
    buffer[1] = (uint8_t)((start_sector >> 8) & 0xFF);
    buffer[2] = (uint8_t)((start_sector >> 16) & 0xFF);
    buffer[3] = (uint8_t)(last_msg & 0xFF);
    buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);
    buffer[5] = 0; /* is_first_full = 0 */

    rc = write_sector(log_sector, buffer);
    return (rc == DRIVER_OK) ? STORAGE_OK : STORAGE_ERR_DRIVER;
}

uint8_t log_raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    if (!buffer || !header) return STORAGE_ERR_PARAM;

    uint32_t last_log_index = 0;
    uint8_t rc = log_get_last_sector(&last_log_index);
    if (rc != STORAGE_OK) return rc;

    uint32_t num_chunks        = (uint32_t)(len / PAYLOAD_SIZE);
    uint32_t base_write_cursor = last_log_index + 1;

    size_t total_buffer_size = (size_t)num_chunks * config->sector_size;
    uint8_t *bulk_buffer = (uint8_t*)malloc(total_buffer_size);

    if (bulk_buffer != NULL) {
        for (uint32_t i = 0; i < num_chunks; i++) {
            uint8_t *sector_ptr = &bulk_buffer[i * config->sector_size];

            memset(sector_ptr, 0, config->sector_size);
            sector_ptr[0] = *header;

            memcpy(&sector_ptr[1],
                   &buffer[i * PAYLOAD_SIZE],
                   PAYLOAD_SIZE);

            uint32_t crc = crc32(sector_ptr, HEADER_SIZE + PAYLOAD_SIZE);

            sector_ptr[config->sector_size - 4] = (uint8_t)(crc & 0xFF);
            sector_ptr[config->sector_size - 3] = (uint8_t)((crc >> 8) & 0xFF);
            sector_ptr[config->sector_size - 2] = (uint8_t)((crc >> 16) & 0xFF);
            sector_ptr[config->sector_size - 1] = (uint8_t)((crc >> 24) & 0xFF);
        }

        for (uint8_t m = 0; m < config->mirror_count; m++) {
            uint32_t physical_start_addr =
                base_write_cursor + (uint32_t)m * config->mirror_offset;

            int drv_rc = write_sectors(physical_start_addr, bulk_buffer, num_chunks);
            if (drv_rc != DRIVER_OK) {
                free(bulk_buffer);
                return STORAGE_ERR_DRIVER;
            }
        }

        free(bulk_buffer);

        uint32_t new_last_sector = base_write_cursor + num_chunks - 1;
        return log_set_last_sector(&new_last_sector);
    }

    /* Low-RAM fallback: sector-by-sector */
    uint8_t sector_buffer[SECTOR_SIZE];
    uint32_t current_cursor = base_write_cursor;

    for (uint32_t i = 0; i < num_chunks; i++) {
        memset(sector_buffer, 0, sizeof(sector_buffer));
        sector_buffer[0] = *header;

        memcpy(&sector_buffer[1],
               &buffer[i * PAYLOAD_SIZE],
               PAYLOAD_SIZE);

        uint32_t crc = crc32(sector_buffer, HEADER_SIZE + PAYLOAD_SIZE);

        sector_buffer[config->sector_size - 4] = (uint8_t)(crc & 0xFF);
        sector_buffer[config->sector_size - 3] = (uint8_t)((crc >> 8) & 0xFF);
        sector_buffer[config->sector_size - 2] = (uint8_t)((crc >> 16) & 0xFF);
        sector_buffer[config->sector_size - 1] = (uint8_t)((crc >> 24) & 0xFF);

        for (uint8_t m = 0; m < config->mirror_count; m++) {
            uint32_t physical_addr =
                current_cursor + (uint32_t)m * config->mirror_offset;

            int rcw = write_sector(physical_addr, sector_buffer);
            if (rcw != DRIVER_OK) return STORAGE_ERR_DRIVER;
        }

        current_cursor++;
    }

    uint32_t new_last_sector = current_cursor - 1;
    return log_set_last_sector(&new_last_sector);
}

/* non-RAID compat */
uint8_t log_save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    if (!buffer || !header) return STORAGE_ERR_PARAM;
    if (len % PAYLOAD_SIZE != 0) return STORAGE_ERR_PARAM;

    uint32_t last_sector = 0;
    uint8_t rc = log_get_last_sector(&last_sector);
    if (rc != STORAGE_OK) return rc;

    uint16_t num_of_sectors = (uint16_t)(len / PAYLOAD_SIZE);
    uint32_t new_sector     = last_sector;

    uint8_t sector_buffer[SECTOR_SIZE];

    for (uint16_t i = 0; i < num_of_sectors; i++) {
        new_sector++;

        memset(sector_buffer, 0, sizeof(sector_buffer));
        sector_buffer[0] = *header;

        memcpy(&sector_buffer[1], &buffer[i * PAYLOAD_SIZE], PAYLOAD_SIZE);

        uint32_t crc = crc32(sector_buffer, HEADER_SIZE + PAYLOAD_SIZE);

        sector_buffer[config->sector_size - 4] = (uint8_t)(crc & 0xFF);
        sector_buffer[config->sector_size - 3] = (uint8_t)((crc >> 8) & 0xFF);
        sector_buffer[config->sector_size - 2] = (uint8_t)((crc >> 16) & 0xFF);
        sector_buffer[config->sector_size - 1] = (uint8_t)((crc >> 24) & 0xFF);

        int rcw = write_sector(new_sector, sector_buffer);
        if (rcw != DRIVER_OK) return STORAGE_ERR_DRIVER;
    }

    rc = log_set_last_sector(&new_sector);
    return (rc == STORAGE_OK) ? STORAGE_OK : rc;
}
