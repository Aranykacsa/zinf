#include "config.h"
#include <string.h>

/* zinf_ctx is defined in the platform file.
   This translation unit only provides the initialiser helper. */

void zinf_ctx_init_defaults(zinf_ctx_t *ctx) {
    ctx->sector_size      = SECTOR_SIZE;
    ctx->mirror_count     = (uint8_t)RAID_MIRRORS;
    if (ctx->mirror_count > MAX_MIRRORS) ctx->mirror_count = (uint8_t)MAX_MIRRORS;
    ctx->metadata_sectors = 2u;
    ctx->log_sector       = 0u;
    if (ctx->raid_offset == 0u) ctx->raid_offset = 30u;
    ctx->mirror_offset    = ctx->raid_offset;
    ctx->bad_sector_count = 0u;
}

/* Pack sensor_t into wire format (little-endian). */
size_t sensor_t_to_wire(uint8_t *out, const sensor_t *s) {
    { uint32_t _u; memcpy(&_u, &s->temp, 4); out[0]=(uint8_t)((_u>>0)&0xFFu); out[1]=(uint8_t)((_u>>8)&0xFFu); out[2]=(uint8_t)((_u>>16)&0xFFu); out[3]=(uint8_t)((_u>>24)&0xFFu); }
    { uint32_t _u; memcpy(&_u, &s->humidity, 4); out[4]=(uint8_t)((_u>>0)&0xFFu); out[5]=(uint8_t)((_u>>8)&0xFFu); out[6]=(uint8_t)((_u>>16)&0xFFu); out[7]=(uint8_t)((_u>>24)&0xFFu); }
    return 8u;
}
