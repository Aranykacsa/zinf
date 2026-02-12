#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* Minimal stub reader that just prints config + computed totals.
   Replace with your real log parsing logic. */

static uint64_t detect_total_sectors(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    fclose(f);
    if (sz <= 0) return 0;

    return (uint64_t)sz / (uint64_t)SECTOR_SIZE;
}

int main(int argc, char **argv) {
    const char *img = (argc >= 2) ? argv[1] : "zinf.img";

    uint64_t total_sectors = detect_total_sectors(img);
    if (total_sectors == 0) {
        printf("Could not read image '%s' (or empty). This is just a stub reader.\n", img);
        return 1;
    }

    printf("Image          : %s\n", img);
    printf("Sector size    : %u\n", (unsigned)SECTOR_SIZE);
    printf("Payload size   : %u\n", (unsigned)PAYLOAD_SIZE);
    printf("Mirrors        : %u\n", (unsigned)RAID_MIRRORS);

    /* Example host-style RAID offset */
    uint64_t usable = (total_sectors > 2) ? (total_sectors - 2) : 0;
    uint32_t raid_offset = (RAID_MIRRORS > 0) ? (uint32_t)(usable / RAID_MIRRORS) : 0;

    printf("Total sectors  : %llu\n", (unsigned long long)total_sectors);
    printf("Usable sectors : %llu\n", (unsigned long long)usable);
    printf("Raid offset    : %u\n", raid_offset);

    return 0;
}
