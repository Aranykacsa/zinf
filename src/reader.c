#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"

/* COMPILATION:
 *   gcc -O2 -o ./reader ./reader.c ./core/config.c -I./core
 *
 * USAGE:
 *   sudo ./reader /dev/sdb
 */

#define SUPER_SECTOR_1 0
#define SUPER_SECTOR_2 1
#define PATH_PAYLOAD "./.out/payload.csv"
#define PATH_METADATA "./.out/meta.csv"

/* ---- Terminal colors ---- */
#define CLR_RESET  "\033[0m"
#define CLR_RED    "\033[31m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_CYAN   "\033[36m"
#define CLR_MAG    "\033[35m"

/* ---- CRC32 (same as firmware) ---- */
uint32_t crc32_u8bit(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/* ---- Read exactly N bytes ---- */
int read_bytes(FILE *f, void *buf, size_t n) {
    size_t r = fread(buf, 1, n, f);
    return (r == n) ? 0 : -1;
}

/* ---- Detect drive geometry ---- */
uint32_t detect_total_sectors(FILE *f) {
    fseek(f, 0, SEEK_END);
    long total_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total_bytes <= 0) return 0;
    return (uint32_t)(total_bytes / SECTOR_SIZE);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device_or_file>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    uint32_t total_sectors = detect_total_sectors(f);
    if (!total_sectors) {
        fprintf(stderr, "Failed to detect total sectors\n");
        fclose(f);
        return 1;
    }
    RAID_OFFSET = total_sectors / RAID_MIRRORS;

    printf(CLR_CYAN "\n=== Reader Configuration ===\n" CLR_RESET);
    printf("File: %s\n", path);
    printf("Sector size  : %u bytes\n", SECTOR_SIZE);
    printf("Total sectors: %u\n", total_sectors);
    printf("RAID mirrors : %u\n", RAID_MIRRORS);
    printf("RAID offset  : %u\n\n", RAID_OFFSET);

    uint8_t sector[SECTOR_SIZE];

    /* --- Read sector 0 --- */
    if (read_bytes(f, sector, SECTOR_SIZE) != 0) {
        fprintf(stderr, "Failed to read sector 0\n");
        fclose(f);
        return 1;
    }

    uint32_t last_sector = sector[0] | (sector[1] << 8) | (sector[2] << 16);
    uint16_t last_msg = sector[3] | (sector[4] << 8);
    uint8_t is_first_full = sector[5];

    printf(CLR_MAG "=== Supersector Metadata ===\n" CLR_RESET);
    printf("Last sector   : %u\n", last_sector);
    printf("Last msg idx  : %u\n", last_msg);
    printf("First log full: %u\n\n", is_first_full);

    /* --- Open CSV files --- */
    FILE *csv_payload = fopen(PATH_PAYLOAD, "w");
    FILE *csv_meta = fopen(PATH_METADATA, "w");
    if (!csv_payload || !csv_meta) {
        perror("fopen CSV");
        fclose(f);
        return 1;
    }

    fprintf(csv_payload, "status,header,payload(hex...),crc_stored,crc_calc\n");
    fprintf(csv_meta, "type,last_sector,last_msg,is_first_full,raw(hex...)\n");

    /* --- Sector 0 raw metadata --- */
    fprintf(csv_meta, "sector0,%u,%u,%u,\"", last_sector, last_msg, is_first_full);
    for (int i = 0; i < SECTOR_SIZE; i++)
        fprintf(csv_meta, "%02x ", sector[i]);
    fprintf(csv_meta, "\"\n");

    printf(CLR_MAG "=== Reading RAID Sectors ===\n" CLR_RESET);

    uint32_t ok_total = 0, bad_total = 0;

    for (uint32_t logical = 2; logical <= last_sector; logical++) {
        uint32_t stored_crc[RAID_MIRRORS];
        uint32_t calc_crc[RAID_MIRRORS];
        int crc_ok[RAID_MIRRORS];
        uint8_t headers[RAID_MIRRORS];
        uint8_t payloads[RAID_MIRRORS][PAYLOAD_SIZE];

        printf(CLR_YELLOW "\nLogical sector %u\n" CLR_RESET, logical);
        printf("------------------------------------------------------------\n");

        for (int m = 0; m < RAID_MIRRORS; m++) {
            uint32_t physical = logical + m * RAID_OFFSET;

            if (fseek(f, (long)physical * SECTOR_SIZE, SEEK_SET) != 0 ||
                read_bytes(f, sector, SECTOR_SIZE) != 0) {
                fprintf(stderr, CLR_RED "Read failed for sector %u (mirror %d)\n" CLR_RESET,
                        physical, m);
                crc_ok[m] = 0;
                continue;
            }

            headers[m] = sector[0];
            memcpy(payloads[m], &sector[1], PAYLOAD_SIZE);

            stored_crc[m] = sector[508] |
                            (sector[509] << 8) |
                            (sector[510] << 16) |
                            (sector[511] << 24);

            calc_crc[m] = crc32_u8bit(sector, HEADER_SIZE + PAYLOAD_SIZE);
            crc_ok[m] = (stored_crc[m] == calc_crc[m]);

            printf(" Mirror %d @ sector %-8u  Header: 0x%02X  Stored CRC: 0x%08X  Calc CRC: 0x%08X  [%s]\n",
                   m, physical, headers[m], stored_crc[m], calc_crc[m],
                   crc_ok[m] ? (CLR_GREEN "OK" CLR_RESET) : (CLR_RED "BAD" CLR_RESET));
        }

        /* --- Decide which mirror to trust --- */
        int chosen = -1;
        for (int m = 0; m < RAID_MIRRORS; m++)
            if (crc_ok[m]) { chosen = m; break; }

        const char *status = (chosen >= 0) ? "CRC_OK" : "CRC_FAIL";
        int use = (chosen >= 0) ? chosen : 0;

        if (chosen >= 0) ok_total++;
        else bad_total++;

        printf(" -> Result: %s (using mirror %d)\n",
               (chosen >= 0) ? (CLR_GREEN "VALID" CLR_RESET) : (CLR_RED "CORRUPTED" CLR_RESET),
               use);

        /* --- Save to CSV --- */
        fprintf(csv_payload, "%s,%u,\"", status, headers[use]);
        for (int i = 0; i < PAYLOAD_SIZE; i++)
            fprintf(csv_payload, "%02x ", payloads[use][i]);
        fprintf(csv_payload, "\",%u,%u\n", stored_crc[use], calc_crc[use]);
    }

    printf(CLR_CYAN "\n=== RAID Integrity Summary ===\n" CLR_RESET);
    printf("Valid sectors  : %u\n", ok_total);
    printf("Corrupted sect : %u\n", bad_total);
    printf("Mirrors used   : %u\n", RAID_MIRRORS);
    printf("RAID offset    : %u\n", RAID_OFFSET);
    printf("Output files   : %s, %s\n\n", PATH_PAYLOAD, PATH_METADATA);

    fclose(csv_meta);
    fclose(csv_payload);
    fclose(f);
    return 0;
}
