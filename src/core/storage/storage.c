#include "storage.h"
#include "driver.h"
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

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

/*### HELPERS ###*/
uint32_t crc32_u8bit(const uint8_t *data, size_t len) {
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

/*### DRIVER HELPERS ###*/
static int read_sector(uint32_t sector, uint8_t *buffer) {
    return active_driver->read_block(active_driver, sector, buffer);
}

static int write_sector(uint32_t sector, const uint8_t *buffer) {
    return active_driver->write_block(active_driver, sector, buffer);
}

/**
 * @brief Multi-sector READ helper
 * Automatikusan választ a natív multi-block read és a szoftveres ciklus között.
 */
/*static int read_sectors(uint32_t start_sector, uint8_t *buffer, uint32_t count) {
    // Ha a driver támogatja a kötegelt olvasást, használjuk azt
    if (active_driver->read_blocks != NULL) {
        return active_driver->read_blocks(active_driver, start_sector, buffer, count);
    } 
    
    // Fallback: Egyesével olvassuk be a szektorokat
    for (uint32_t i = 0; i < count; i++) {
        int rc = active_driver->read_block(active_driver, start_sector + i, buffer + (i * SECTOR_SIZE));
        if (rc != DRIVER_OK) return rc;
    }
    return DRIVER_OK;
}*/

/**
 * @brief Multi-sector WRITE helper
 * Automatikusan választ a natív multi-block write és a szoftveres ciklus között.
 */
static int write_sectors(uint32_t start_sector, const uint8_t *buffer, uint32_t count) {
    // Ha a driver támogatja a kötegelt írást, használjuk azt (Sokkal gyorsabb!)
    if (active_driver->write_blocks != NULL) {
        return active_driver->write_blocks(active_driver, start_sector, buffer, count);
    } 
    
    // Fallback: Egyesével írjuk ki a szektorokat
    for (uint32_t i = 0; i < count; i++) {
        int rc = active_driver->write_block(active_driver, start_sector + i, buffer + (i * SECTOR_SIZE));
        if (rc != DRIVER_OK) return rc;
    }
    return DRIVER_OK;
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
    if (!buffer || !header) return STORAGE_ERR_PARAM;
    // Szigorú 507 bájtos igazítás ellenőrzése (PAYLOAD_SIZE)
    if (len % PAYLOAD_SIZE != 0) return STORAGE_ERR_PARAM;

    // 1. Utolsó írt pozíció lekérése
    uint32_t last_log_index = 0;
    uint8_t rc = get_last_sector(&last_log_index);
    if (rc != STORAGE_OK) return rc;

    uint32_t num_chunks = len / PAYLOAD_SIZE;
    uint32_t base_write_cursor = last_log_index + 1;

    /* --- RAM OPTIMALIZÁCIÓS ÁG --- */
    /* Megpróbáljuk egyben allokálni a puffert. Ha sikerül, akkor az új
     * write_sectors helper segítségével írjuk ki. Ez a leggyorsabb módszer. */
    size_t total_buffer_size = num_chunks * SECTOR_SIZE;
    uint8_t *bulk_buffer = (uint8_t*)malloc(total_buffer_size);

    if (bulk_buffer != NULL) {
        // A. Előkészítés (Header + Data + CRC) a RAM-ban
        for (uint32_t i = 0; i < num_chunks; i++) {
            uint8_t *sector_ptr = &bulk_buffer[i * SECTOR_SIZE];
            
            memset(sector_ptr, 0, SECTOR_SIZE);
            sector_ptr[0] = *header;
            memcpy(&sector_ptr[1], &buffer[i * PAYLOAD_SIZE], PAYLOAD_SIZE);
            
            uint32_t crc = crc32_u8bit(sector_ptr, HEADER_SIZE + PAYLOAD_SIZE);
            sector_ptr[508] = (uint8_t)(crc & 0xFF);
            sector_ptr[509] = (uint8_t)((crc >> 8) & 0xFF);
            sector_ptr[510] = (uint8_t)((crc >> 16) & 0xFF);
            sector_ptr[511] = (uint8_t)((crc >> 24) & 0xFF);
        }

        // B. Írás tükrönként - Itt használjuk az új helpert!
        for (uint8_t m = 0; m < RAID_MIRRORS; m++) {
            uint32_t physical_start_addr = base_write_cursor + (m * RAID_OFFSET);

            // write_sectors dönt: ha van driver->write_blocks, akkor egyben küldi,
            // ha nincs, akkor ciklusban egyesével.
            int drv_rc = write_sectors(physical_start_addr, bulk_buffer, num_chunks);

            if (drv_rc != DRIVER_OK) {
                free(bulk_buffer);
                return STORAGE_ERR_DRIVER;
            }
        }

        free(bulk_buffer);
        
        uint32_t new_last_sector = base_write_cursor + num_chunks - 1;
        return set_last_sector(&new_last_sector);
    }

    /* --- ALACSONY RAM FALLBACK ÁG --- */
    /* Ha a malloc sikertelen volt (pl. kicsi a RAM), akkor kénytelenek vagyunk
     * szektoronként előkészíteni és írni az adatot. */
    uint8_t sector_buffer[SECTOR_SIZE];
    uint32_t current_cursor = base_write_cursor;

    for (uint16_t i = 0; i < num_chunks; i++) {
        // Szektor előkészítése
        memset(sector_buffer, 0, SECTOR_SIZE);
        sector_buffer[0] = *header;
        memcpy(&sector_buffer[1], &buffer[i * PAYLOAD_SIZE], PAYLOAD_SIZE);
        
        uint32_t crc = crc32_u8bit(sector_buffer, HEADER_SIZE + PAYLOAD_SIZE);
        sector_buffer[508] = (uint8_t)(crc & 0xFF);
        sector_buffer[509] = (uint8_t)((crc >> 8) & 0xFF);
        sector_buffer[510] = (uint8_t)((crc >> 16) & 0xFF);
        sector_buffer[511] = (uint8_t)((crc >> 24) & 0xFF);

        // Írás minden tükörre
        for (uint8_t m = 0; m < RAID_MIRRORS; m++) {
            uint32_t physical_addr = current_cursor + (m * RAID_OFFSET);
            // Itt sima write_sector-t használunk, mert egyesével haladunk
            int rcw = write_sector(physical_addr, sector_buffer);
            if (rcw != DRIVER_OK) return STORAGE_ERR_DRIVER;
        }
        
        current_cursor++;
    }

    uint32_t new_last_sector = current_cursor - 1;
    return set_last_sector(&new_last_sector);
}

uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    if (!buffer) return STORAGE_ERR_PARAM;
    if (len % PAYLOAD_SIZE != 0) return STORAGE_ERR_PARAM;

    // Ez a függvény most már csak egy wrapper a single sector íráshoz,
    // de a raid_u8bit_values a preferált a nagy adatokhoz.
    // Itt is használhatnánk a get_last_sector logikát, de az eredeti kód
    // ezt a szekvenciális logikát követte.
    
    // MEGJEGYZÉS: Ha ezt a függvényt is gyorsítani szeretnéd, 
    // érdemes lenne átirányítani a raid_u8bit_values-ra, vagy 
    // implementálni benne is a malloc + write_sectors logikát.
    // A jelenlegi formájában ez lassú (single sector write).

    uint32_t last_sector = 0;
    uint8_t rc = get_last_sector(&last_sector);
    if (rc != STORAGE_OK) return rc;

    uint16_t num_of_sectors = len / PAYLOAD_SIZE;
    uint32_t new_sector = last_sector;

    uint8_t sector_buffer[SECTOR_SIZE];

    for (uint16_t i = 0; i < num_of_sectors; i++) {
        new_sector++;

        for (uint16_t j = 0; j < SECTOR_SIZE; j++) sector_buffer[j] = 0;

        sector_buffer[0] = *header;
        for (uint16_t k = 0; k < PAYLOAD_SIZE; k++) {
            sector_buffer[1 + k] = buffer[i * PAYLOAD_SIZE + k];
        }

        uint32_t crc = crc32_u8bit(sector_buffer, HEADER_SIZE + PAYLOAD_SIZE);
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
