#include "config.h"

/* The host tools set this at runtime */
uint32_t RAID_OFFSET = 0;

/* Default config instance */
static config_t g_cfg;

config_t *config = &g_cfg;

void config_init_defaults(void) {
    g_cfg.sector_size   = SECTOR_SIZE;
    g_cfg.mirror_count  = (uint8_t)RAID_MIRRORS;

    /* mirror_offset comes from RAID_OFFSET (host computed).
       If not set yet, keep a safe fallback. */
    if (RAID_OFFSET == 0) RAID_OFFSET = 30;
    g_cfg.mirror_offset = RAID_OFFSET;
}
