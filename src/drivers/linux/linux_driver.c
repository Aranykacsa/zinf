#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "string.h"

#ifdef __linux__
#include <linux/fs.h>
#endif

#include "driver.h"

#define SECTOR_SIZE_DEFAULT 512

typedef struct {
    int         fd;
    const char *path;
    uint8_t    *bounce;    // 512-byte aligned bounce buffer
} linux_ctx_t;

// ------------------- Helper -------------------

static void *alloc_aligned(size_t size, size_t align) {
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
}

// ------------------- Single read ------------------------

static int linux_read(driver_t *self, uint64_t lba, uint8_t *buf)
{
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;

    off_t off = (off_t)lba * (off_t)self->sector_size;
    ssize_t rc = pread(ctx->fd, ctx->bounce, self->sector_size, off);

    if (rc != self->sector_size) {
        perror("[linux_driver] pread");
        return DRIVER_ERR_IO;
    }

    memcpy(buf, ctx->bounce, self->sector_size);
    return DRIVER_OK;
}

// ------------------- Single write ------------------------

static int linux_write(driver_t *self, uint64_t lba, const uint8_t *buf)
{
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;

    memcpy(ctx->bounce, buf, self->sector_size);

    off_t off = (off_t)lba * (off_t)self->sector_size;
    ssize_t rc = pwrite(ctx->fd, ctx->bounce, self->sector_size, off);

    if (rc != self->sector_size) {
        perror("[linux_driver] pwrite");
        return DRIVER_ERR_IO;
    }

    return DRIVER_OK;
}

// ------------------- Init / Deinit ----------------------

static int linux_init(driver_t *self)
{
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;

    self->sector_size = SECTOR_SIZE_DEFAULT;

    ctx->bounce = alloc_aligned(self->sector_size, self->sector_size);
    if (!ctx->bounce) {
        fprintf(stderr, "[linux_driver] cannot allocate aligned bounce buffer\n");
        return DRIVER_ERR_INIT;
    }

    ctx->fd = open(ctx->path, O_RDWR | O_DIRECT);
    if (ctx->fd < 0) {
        perror("[linux_driver] open");
        return DRIVER_ERR_INIT;
    }

    uint64_t bytes = 0;
    if (ioctl(ctx->fd, BLKGETSIZE64, &bytes) == 0) {
        self->total_size_bytes = bytes;
        self->total_sectors    = bytes / self->sector_size;
    } else {
        self->total_sectors = 0;
    }

    return DRIVER_OK;
}

static void linux_deinit(driver_t *self)
{
    linux_ctx_t *ctx = (linux_ctx_t *)self->ctx;

    if (ctx->fd >= 0) close(ctx->fd);
    if (ctx->bounce) free(ctx->bounce);

    fprintf(stderr, "[linux_driver] Closed %s\n", ctx->path);
}

// ------------------- Global driver ----------------------

static linux_ctx_t ctx = {
    .fd = -1,
    .path = "/dev/loop0",
    .bounce = NULL
};

void linux_driver_set_path(const char *path) {
    ctx.path = path;
}

driver_t linux_driver = {
    .name          = "linux_raw",
    .sector_size   = SECTOR_SIZE_DEFAULT,
    .ctx           = &ctx,
    .init          = linux_init,
    .read_block    = linux_read,
    .write_block   = linux_write,
    .read_blocks   = NULL,
    .write_blocks  = NULL,
    .sync          = NULL,
    .deinit        = linux_deinit
};

