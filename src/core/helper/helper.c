#include "helper.h"

/* CRC32 using a 256-entry lookup table (Ethernet/ZIP polynomial 0xEDB88320).
   One table entry per byte — O(n) vs the previous O(8n) bit-by-bit loop.
   Table is generated once at first call; ~1 KB of .data on embedded targets. */

static uint32_t crc_table[256];
static int      crc_table_ready = 0;

static void crc_table_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & ~((c & 1u) - 1u));
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

uint32_t crc32(const uint8_t *data, size_t len) {
    if (!crc_table_ready) crc_table_init();

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc_table[(crc ^ (uint32_t)data[i]) & 0xFFu];
    return ~crc;
}
