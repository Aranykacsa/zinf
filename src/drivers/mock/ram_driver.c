#include "driver.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    uint64_t size;
} ram_ctx_t;

static int ram_init(driver_t *self) {
    ram_ctx_t *ctx = (ram_ctx_t *)self->ctx;
    if (ctx->data) free(ctx->data);
    
    ctx->data = (uint8_t *)calloc(self->total_sectors, self->sector_size);
    if (!ctx->data) return DRIVER_ERR_INIT;
    
    self->total_size_bytes = (uint64_t)self->total_sectors * self->sector_size;
    return DRIVER_OK;
}

static void ram_deinit(driver_t *self) {
    ram_ctx_t *ctx = (ram_ctx_t *)self->ctx;
    if (ctx->data) {
        free(ctx->data);
        ctx->data = NULL;
    }
}

static int ram_read(driver_t *self, uint64_t lba, uint8_t *buf) {
    ram_ctx_t *ctx = (ram_ctx_t *)self->ctx;
    if (lba >= self->total_sectors) return DRIVER_ERR_IO;
    memcpy(buf, ctx->data + (lba * self->sector_size), self->sector_size);
    return DRIVER_OK;
}

static int ram_write(driver_t *self, uint64_t lba, const uint8_t *buf) {
    ram_ctx_t *ctx = (ram_ctx_t *)self->ctx;
    if (lba >= self->total_sectors) return DRIVER_ERR_IO;
    memcpy(ctx->data + (lba * self->sector_size), buf, self->sector_size);
    return DRIVER_OK;
}

static ram_ctx_t g_ram_ctx = {0};

driver_t ram_driver = {
    .name = "ram_mock",
    .sector_size = 512,
    .total_sectors = 65536, // Increase to 32MB
    .ctx = &g_ram_ctx,
    .init = ram_init,
    .deinit = ram_deinit,
    .read_block = ram_read,
    .write_block = ram_write,
    .read_blocks = NULL,
    .write_blocks = NULL,
    .sync = NULL
};

/* Special testing API for fault injection */
void ram_driver_set_capacity(uint64_t sectors) {
    ram_driver.total_sectors = sectors;
}

void ram_driver_corrupt(uint64_t lba, uint32_t offset, uint8_t val) {
    ram_ctx_t *ctx = (ram_ctx_t *)ram_driver.ctx;
    if (ctx->data && lba < ram_driver.total_sectors && offset < ram_driver.sector_size) {
        ctx->data[lba * ram_driver.sector_size + offset] = val;
    }
}

void ram_driver_drop_buffer(void) {
    ram_ctx_t *ctx = (ram_ctx_t *)ram_driver.ctx;
    if (ctx->data) {
        memset(ctx->data, 0, ram_driver.total_sectors * ram_driver.sector_size);
    }
}
