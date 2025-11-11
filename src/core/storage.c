#include "storage.h"
#include "config.h"
#include "driver.h"
#include "helper.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- Return codes ---- */
#define STORAGE_OK 0
#define STORAGE_ERR_DRIVER 1
#define STORAGE_ERR_PARAM 2
#define STORAGE_ERR_FULL 3
#define STORAGE_ERR_LOG_FULL 4
#define STORAGE_ERR_META 5

/* Global driver pointer (assigned externally, e.g. from main.c) */
extern driver_t *active_driver;
extern uint32_t log_sector;

/*### DRIVER HELPERS ###*/
static int read_sector(uint32_t sector, uint8_t *buffer) {
  if (!active_driver || !buffer)
    return STORAGE_ERR_PARAM;
  return active_driver->read_block(active_driver, sector, buffer);
}

static int write_sector(uint32_t sector, const uint8_t *buffer) {
  if (!active_driver || !buffer)
    return STORAGE_ERR_PARAM;
  return active_driver->write_block(active_driver, sector, buffer);
}

/*### INTERNAL STATE FUNCTIONS ###*/
/* === INTERNAL STATE FUNCTIONS WITH CRC === */
uint8_t get_last_sector(uint32_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t  buffer[3][SECTOR_SIZE];
    uint32_t value[3];
    uint8_t  valid[3] = {0};

    for (uint8_t i = 0; i < RAID_MIRRORS; i++) {
        uint32_t meta_sector = log_sector + (i * RAID_OFFSET);
        int rc = read_sector(meta_sector, buffer[i]);
        if (rc != DRIVER_OK) continue;

        uint32_t stored_crc =
              ((uint32_t)buffer[i][SECTOR_SIZE-4])
            | ((uint32_t)buffer[i][SECTOR_SIZE-3] << 8)
            | ((uint32_t)buffer[i][SECTOR_SIZE-2] << 16)
            | ((uint32_t)buffer[i][SECTOR_SIZE-1] << 24);

        uint32_t calc_crc = crc32(buffer[i], SECTOR_SIZE - 4);
        if (stored_crc == calc_crc) {
            value[i] = ((uint32_t)buffer[i][0])
                     | ((uint32_t)buffer[i][1] << 8)
                     | ((uint32_t)buffer[i][2] << 16);
            valid[i] = 1;
        } else {
            printf("[META] mirror %u CRC mismatch\n", i);
        }
    }

    // choose majority or any valid copy
    uint8_t valid_count = valid[0] + valid[1] + valid[2];
    if (valid_count == 0) return STORAGE_ERR_META;

    uint32_t candidate = 0;
    if (valid[0] && valid[1] && value[0] == value[1]) candidate = value[0];
    else if (valid[1] && valid[2] && value[1] == value[2]) candidate = value[1];
    else if (valid[0] && valid[2] && value[0] == value[2]) candidate = value[0];
    else if (valid[0]) candidate = value[0];
    else if (valid[1]) candidate = value[1];
    else candidate = value[2];

    *last_sector = candidate;
    return STORAGE_OK;
}


uint8_t set_last_sector(const uint32_t *last_sector) {
    if (!last_sector) return STORAGE_ERR_PARAM;

    uint8_t buffer[SECTOR_SIZE];
    int rc = read_sector(log_sector, buffer);
    if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;

    // update value
    buffer[0] = (uint8_t)(*last_sector & 0xFF);
    buffer[1] = (uint8_t)((*last_sector >> 8) & 0xFF);
    buffer[2] = (uint8_t)((*last_sector >> 16) & 0xFF);

    // recompute CRC
    uint32_t crc = crc32(buffer, SECTOR_SIZE - 4);
    buffer[SECTOR_SIZE-4] = (uint8_t)(crc & 0xFF);
    buffer[SECTOR_SIZE-3] = (uint8_t)((crc >> 8) & 0xFF);
    buffer[SECTOR_SIZE-2] = (uint8_t)((crc >> 16) & 0xFF);
    buffer[SECTOR_SIZE-1] = (uint8_t)((crc >> 24) & 0xFF);

    // write all mirrors
    for (uint8_t i = 0; i < RAID_MIRRORS; i++) {
        uint32_t meta_sector = log_sector + (i * RAID_OFFSET);
        rc = write_sector(meta_sector, buffer);
        if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;
    }

    if (active_driver->sync) active_driver->sync(active_driver);
    return STORAGE_OK;
}


uint8_t init_log_sector(void) {
    RAID_OFFSET = (uint32_t)floor(active_driver->total_sectors / RAID_MIRRORS);
    if (RAID_OFFSET == 0) return STORAGE_ERR_PARAM;
    printf("RAID_OFFSET: %u\n", RAID_OFFSET);

    // prepare zeroed metadata
    uint8_t buffer[SECTOR_SIZE];
    for (uint16_t i = 0; i < SECTOR_SIZE; i++) buffer[i] = 0;

    const uint32_t start_sector = 1;
    const uint16_t last_msg = 0;
    buffer[0] = (uint8_t)(start_sector & 0xFF);
    buffer[1] = (uint8_t)((start_sector >> 8) & 0xFF);
    buffer[2] = (uint8_t)((start_sector >> 16) & 0xFF);
    buffer[3] = (uint8_t)(last_msg & 0xFF);
    buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);
    buffer[5] = 0; // not full

    // compute CRC
    uint32_t crc = crc32(buffer, SECTOR_SIZE - 4);
    buffer[SECTOR_SIZE-4] = (uint8_t)(crc & 0xFF);
    buffer[SECTOR_SIZE-3] = (uint8_t)((crc >> 8) & 0xFF);
    buffer[SECTOR_SIZE-2] = (uint8_t)((crc >> 16) & 0xFF);
    buffer[SECTOR_SIZE-1] = (uint8_t)((crc >> 24) & 0xFF);

    // write to all mirrors
    for (uint8_t i = 0; i < RAID_MIRRORS; i++) {
        uint32_t meta_sector = log_sector + (i * RAID_OFFSET);
        int rc = write_sector(meta_sector, buffer);
        if (rc != DRIVER_OK) return STORAGE_ERR_DRIVER;
    }

    if (active_driver->sync) active_driver->sync(active_driver);
    return STORAGE_OK;
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
    if (err != STORAGE_OK)
      return err;
  }
  return STORAGE_OK;
}

uint8_t save_msg(uint8_t *msg) {
  uint8_t buffer[SECTOR_SIZE];
  uint16_t last_msg;
  uint8_t is_first_full;

  int rc = read_sector(log_sector, buffer);
  if (rc != DRIVER_OK)
    return STORAGE_ERR_DRIVER;

  is_first_full = buffer[5];

  last_msg = ((uint16_t)buffer[3]) | ((uint16_t)buffer[4] << 8);

  last_msg++;

  if (is_first_full) {
    if (last_msg == SECTOR_SIZE) {
      return STORAGE_ERR_LOG_FULL;
    } else {
      buffer[3] = (uint8_t)(last_msg & 0xFF);
      buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);

      rc = write_sector(log_sector, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

      rc = read_sector(log_sector + 1, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

      buffer[last_msg] = *msg;

      rc = write_sector(log_sector + 1, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;
    }
  } else {
    if (last_msg == SECTOR_SIZE) {
      last_msg = 0;
      is_first_full = 1;

      buffer[3] = (uint8_t)(last_msg & 0xFF);
      buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);
      buffer[5] = is_first_full;

      rc = write_sector(log_sector, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

      rc = read_sector(log_sector + 1, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;

      buffer[last_msg] = *msg;

      rc = write_sector(log_sector + 1, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;
    } else {
      buffer[3] = (uint8_t)(last_msg & 0xFF);
      buffer[4] = (uint8_t)((last_msg >> 8) & 0xFF);
      buffer[last_msg] = *msg;

      rc = write_sector(log_sector, buffer);
      if (rc != DRIVER_OK)
        return STORAGE_ERR_DRIVER;
    }
  }

  return STORAGE_OK;
}

uint8_t raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
  if (!active_driver)
    return STORAGE_ERR_DRIVER;

  uint8_t rc;
  uint32_t last_sector = 0;
  rc = get_last_sector(&last_sector);
  if (rc != STORAGE_OK)
    return rc;

  if (len % PAYLOAD_SIZE != 0)
    return STORAGE_ERR_PARAM;
  uint32_t nsectors = (uint32_t)(len / PAYLOAD_SIZE);

  // ✅ next logical sector to write (last written is inclusive)
  uint32_t base = last_sector + 1;

  // Write the SAME logical span to all mirrors
  for (uint8_t i = 0; i < RAID_MIRRORS; i++) {
    uint32_t start_sector = base + (i * RAID_OFFSET);
    rc = save_u8bit_values(buffer, len, header, &start_sector);
    if (rc != STORAGE_OK)
      return rc;
  }

  // ✅ update last written logical sector (inclusive)
  uint32_t new_last = base + nsectors - 1;
  return set_last_sector(&new_last);
}

uint8_t save_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header,
                          uint32_t *start_raid_sector) {
  if (!buffer || !header || !active_driver)
    return STORAGE_ERR_PARAM;
  if (len % PAYLOAD_SIZE != 0)
    return STORAGE_ERR_PARAM;

  uint16_t num_of_sectors = len / PAYLOAD_SIZE;

  // local cursor (VALUE), first write goes exactly to *start_raid_sector
  uint32_t target = *start_raid_sector;

  // derive mirror slice bounds from RAID_OFFSET (keeps mirrors isolated)
  uint32_t mirror_index = target / RAID_OFFSET;
  uint32_t slice_start = mirror_index * RAID_OFFSET;
  uint32_t slice_end = slice_start + RAID_OFFSET; // exclusive

  uint8_t sector_buffer[SECTOR_SIZE];

  for (uint16_t i = 0; i < num_of_sectors; i++) {
    if (target >= active_driver->total_sectors)
      return STORAGE_ERR_FULL;
    if (target >= slice_end)
      return STORAGE_ERR_FULL;

    // clear
    for (uint16_t j = 0; j < SECTOR_SIZE; j++)
      sector_buffer[j] = 0;

    // header
    sector_buffer[0] = *header;

    // payload
    for (uint16_t k = 0; k < PAYLOAD_SIZE; k++)
      sector_buffer[1 + k] = buffer[i * PAYLOAD_SIZE + k];

    // CRC (end of sector)
    uint32_t crc = crc32(sector_buffer, HEADER_SIZE + PAYLOAD_SIZE);
    const uint16_t off = SECTOR_SIZE - 4;
    sector_buffer[off + 0] = (uint8_t)(crc & 0xFF);
    sector_buffer[off + 1] = (uint8_t)((crc >> 8) & 0xFF);
    sector_buffer[off + 2] = (uint8_t)((crc >> 16) & 0xFF);
    sector_buffer[off + 3] = (uint8_t)((crc >> 24) & 0xFF);

    int rcw = write_sector(target, sector_buffer);
    if (rcw != DRIVER_OK)
      return STORAGE_ERR_DRIVER;

    target++; // advance for next sector within this mirror slice
  }

  // hand back next-free sector in this mirror slice
  *start_raid_sector = target;
  return STORAGE_OK;
}
