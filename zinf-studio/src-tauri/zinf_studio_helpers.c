/* zinf_studio_helpers.c
 * Thin C helpers called from Rust to avoid exposing zinf_ctx_t field layout
 * through the FFI boundary. */
#include "config.h"
#include "driver.h"
#include "linux_driver.h"

extern zinf_ctx_t *zinf_ctx;

/* Wire linux_driver into the global zinf_ctx before calling setup_storage().
 * Mirrors what zinf_main.c does at line 494:
 *   zinf_ctx->driver = &linux_driver;
 */
void zinf_studio_use_linux_driver(void) {
    zinf_ctx->driver = &linux_driver;
}

/* Recompute mirror_offset from the driver's total_sectors after setup_storage().
 * The CLI does this via compute_raid_offset() using ioctl(BLKGETSIZE64); Studio
 * opens the backing .img file directly so the ioctl fails and zinf_ctx_init_defaults
 * falls back to offset=30.  This helper applies the same formula as the CLI so the
 * mirror_offset matches what zinf format used when the image was created.
 *
 * Formula (mirrors zinf_main.c:compute_raid_offset):
 *   usable = total_sectors - metadata_sectors
 *   offset = max(8, usable / mirror_count)
 */
void zinf_studio_fix_mirror_offset(void) {
    uint64_t total = zinf_ctx->driver->total_sectors;
    if (total < 4u) return;
    uint8_t  mc     = (zinf_ctx->mirror_count > 0u) ? zinf_ctx->mirror_count : 2u;
    uint8_t  meta   = (zinf_ctx->metadata_sectors > 0u) ? zinf_ctx->metadata_sectors : 2u;
    uint64_t usable = (total > meta) ? (total - meta) : 0u;
    uint64_t offset = (mc > 0u) ? (usable / mc) : 8u;
    if (offset < 8u) offset = 8u;
    zinf_ctx->raid_offset   = offset;
    zinf_ctx->mirror_offset = offset;
}
