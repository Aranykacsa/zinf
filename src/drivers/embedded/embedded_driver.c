#include "driver.h"
#include <stdint.h>

static int emb_init(driver_t *self) { (void)self; return DRIVER_ERR_INIT; }
static void emb_deinit(driver_t *self) { (void)self; }

static int emb_read(driver_t *self, uint32_t lba, uint8_t *buf) {
    (void)self; (void)lba; (void)buf;
    return DRIVER_ERR_IO;
}

static int emb_write(driver_t *self, uint32_t lba, const uint8_t *buf) {
    (void)self; (void)lba; (void)buf;
    return DRIVER_ERR_IO;
}

driver_t embedded_driver = {
    .name = "embedded_stub",
    .sector_size = 512,
    .total_size_bytes = 0,
    .total_sectors = 0,
    .init = emb_init,
    .deinit = emb_deinit,
    .read_block = emb_read,
    .write_block = emb_write,
    .read_blocks = NULL,
    .write_blocks = NULL,
    .sync = NULL,
    .ctx = NULL
};
