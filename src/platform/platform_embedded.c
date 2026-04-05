#include "config.h"
#include "driver.h"

/* Use the SD card driver on embedded targets.
   Call sd_driver_init_spi() before setup_storage(). */
extern driver_t sd_driver;

/* Default context for embedded targets */
static zinf_ctx_t g_zinf_ctx = {
    .driver           = NULL,   /* set before calling setup_storage() */
    .sector_size      = SECTOR_SIZE,
    .mirror_count     = (uint8_t)RAID_MIRRORS,
    .metadata_sectors = 2,
    .mirror_offset    = 0,
    .log_sector       = 0,
    .raid_offset      = 0,
};

zinf_ctx_t *zinf_ctx = &g_zinf_ctx;
