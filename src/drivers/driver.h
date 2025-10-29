#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Generic block device interface for the filesystem.
 *
 * Every backend (e.g. SD card, USB flash drive, RAM disk, or file mock)
 * must implement this structure and its function pointers.
 */
typedef struct driver {
    const char *name;        ///< Human-readable identifier (e.g. "sd", "linux", "mock")
    uint32_t sector_size;    ///< Usually 512 bytes; can differ for advanced devices
    void *ctx;               ///< Optional context pointer (e.g. FILE* or SPI handle)

    int  (*init)(struct driver *self);
    int  (*read_block)(struct driver *self, uint32_t lba, uint8_t *buffer);
    int  (*write_block)(struct driver *self, uint32_t lba, const uint8_t *buffer);
    int  (*sync)(struct driver *self);
    void (*deinit)(struct driver *self);
} driver_t;

/* === Standardized return codes (positive for warnings, negative for errors) === */
#define DRIVER_OK           0
#define DRIVER_ERR_IO      -1
#define DRIVER_ERR_PARAM   -2
#define DRIVER_ERR_INIT    -3
#define DRIVER_ERR_UNSUPP  -4

#endif /* DRIVER_H */

