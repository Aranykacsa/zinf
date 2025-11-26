#include "driver.h"
#include "storage.h"
#include <stdio.h>
#include <stdint.h>

extern driver_t linux_driver;
driver_t *active_driver = &linux_driver;
uint32_t log_sector = 0;  // global required by storage.c
uint8_t status;

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

int main(void) {
    printf("=== MyFS Desktop Test ===\n");

    status = setup_storage();
    if (status != 0) {
        printf("Storage init failed.\n");
        return 1;
    }

    status = init_log_sector();
    if (status != 0) {
        printf("Failed to init log sector.\n");
        return 1;
    }

    uint8_t header = 0xAB;
    uint8_t payload[507];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = 12;

    printf("Writing test sector...\n");
    uint8_t rc = raid_u8bit_values(payload, sizeof(payload), &header);
    if (rc != 0) {
        printf("save_u8bit_values failed (%d)\n", rc);
        return 1;
    }

    printf("Write OK\n");

    printf("Writing test sector...\n");
    header = 0xBC;
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = 6;

    rc = raid_u8bit_values(payload, sizeof(payload), &header);
    if (rc != 0) {
        printf("save_u8bit_values failed (%d)\n", rc);
        return 1;
    }

    printf("Write OK\n");

    active_driver->deinit(active_driver);
    return 0;
}
