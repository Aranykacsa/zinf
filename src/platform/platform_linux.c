#include "config.h"
#include "driver.h"

extern driver_t linux_driver;

/* Default context for Linux targets */
static zinf_ctx_t g_zinf_ctx = {
    .driver           = NULL,
    .sector_size      = SECTOR_SIZE,
    .mirror_count     = (uint8_t)RAID_MIRRORS,
    .metadata_sectors = 2,
    .mirror_offset    = 0,
    .log_sector       = 0,
    .raid_offset      = 0,
};

zinf_ctx_t *zinf_ctx = &g_zinf_ctx;
