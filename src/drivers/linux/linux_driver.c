#define _GNU_SOURCE
#include <unistd.h>

#include "driver.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

typedef struct {
    int fd;
    const char *path;
} linux_ctx_t;

static int linux_init(driver_t *self) {
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;
    ctx->fd = open(ctx->path, O_RDWR | O_SYNC);
    if (ctx->fd < 0) {
        perror("[linux_driver] open");
        return DRIVER_ERR_INIT;
    }

    uint64_t bytes = 0;
    if (ioctl(ctx->fd, BLKGETSIZE64, &bytes) == -1) {
        perror("[linux_driver] ioctl(BLKGETSIZE64)");
        self->total_size_bytes = 0;
        self->total_sectors = 0;
    } else {
        self->total_size_bytes = bytes;
        self->total_sectors = bytes / self->sector_size;
        printf("[linux_driver] Detected size: %.2f MB (%lu sectors)\n",
               bytes / (1024.0 * 1024.0),
               (unsigned long)self->total_sectors);
    }

    printf("[linux_driver] Opened %s\n", ctx->path);
    return DRIVER_OK;
}

static int linux_read(driver_t *self, uint32_t lba, uint8_t *buf) {
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;
    if (!buf) return DRIVER_ERR_PARAM;
    off_t offset = (off_t)lba * self->sector_size;
    ssize_t rc = pread(ctx->fd, buf, self->sector_size, offset);
    return (rc == (ssize_t)self->sector_size) ? DRIVER_OK : DRIVER_ERR_IO;
}

static int linux_write(driver_t *self, uint32_t lba, const uint8_t *buf) {
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;
    if (!buf) return DRIVER_ERR_PARAM;
    off_t offset = (off_t)lba * self->sector_size;
    ssize_t rc = pwrite(ctx->fd, buf, self->sector_size, offset);
    return (rc == (ssize_t)self->sector_size) ? DRIVER_OK : DRIVER_ERR_IO;
}

static int linux_sync(driver_t *self) {
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;
    return (fsync(ctx->fd) == 0) ? DRIVER_OK : DRIVER_ERR_IO;
}

static void linux_deinit(driver_t *self) {
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;
    if (ctx->fd >= 0) close(ctx->fd);
    ctx->fd = -1;
    printf("[linux_driver] Closed device\n");
}

static linux_ctx_t ctx = {
    .fd = -1,
    .path = "/dev/loop0"   // change if your loopback differs
};

driver_t linux_driver = {
    .name = "linux",
    .sector_size = 512,
    .ctx = &ctx,
    .init = linux_init,
    .read_block = linux_read,
    .write_block = linux_write,
    .sync = linux_sync,
    .deinit = linux_deinit
};

