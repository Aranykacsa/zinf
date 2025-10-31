#include "driver.h"
#include "storage.h"
#include <stdio.h>
#include <stdint.h>

extern driver_t linux_driver;
driver_t *active_driver = &linux_driver;
uint32_t log_sector = 0;  // global required by storage.c

int main(void) {
    printf("=== MyFS Desktop Test ===\n");
    if (setup_storage() != 10) {
        printf("Storage init failed.\n");
        return 1;
    }

    if (init_log_sector() != 10) {
        printf("Failed to init log sector.\n");
        return 1;
    }

    uint8_t header = 0xAB;
    uint8_t payload[507];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = 12;

    printf("Writing test sector...\n");
    uint8_t rc = save_u8bit_values(payload, sizeof(payload), &header);
    if (rc != 10) {
        printf("save_u8bit_values failed (%d)\n", rc);
        return 1;
    }

    printf("✅ Write OK, verifying data...\n");
    uint8_t buffer[512];
    active_driver->read_block(active_driver, 2, buffer);

    printf("First 16 bytes of sector 1: ");
    for (int i = 0; i < 16; i++)
        printf("%02X ", buffer[i]);
    printf("\n");

    active_driver->deinit(active_driver);
    return 0;
}

