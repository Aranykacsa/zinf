#pragma once
#include <stdint.h>

typedef struct driver_t driver_t;

/* return codes */
#define DRIVER_OK        0
#define DRIVER_ERR_IO    1
#define DRIVER_ERR_INIT  2
#define DRIVER_ERR_PARAM 3

struct driver_t {
    /* metadata */
    const char *name;

    /* geometry */
    uint32_t sector_size;
    uint64_t total_size_bytes;
    uint64_t total_sectors;

    /* lifecycle */
    int  (*init)(driver_t *self);
    void (*deinit)(driver_t *self);

    /* I/O */
    int (*read_block)(driver_t *self, uint64_t lba, uint8_t *buf);
    int (*write_block)(driver_t *self, uint64_t lba, const uint8_t *buf);

    /* optional multi-block (may be NULL) */
    int (*read_blocks)(driver_t *self, uint64_t lba, uint8_t *buf, uint32_t count);
    int (*write_blocks)(driver_t *self, uint64_t lba, const uint8_t *buf, uint32_t count);

    /* optional flush (may be NULL) */
    int (*sync)(driver_t *self);

    /* driver private context */
    void *ctx;
};
