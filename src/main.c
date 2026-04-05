#define _POSIX_C_SOURCE 199309L 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

/* --- ZINF INCLUDES --- */
#include "driver.h"
#include "storage.h"
#include "config.h"
/* --- ZINF CONSTANTS --- */


extern driver_t linux_driver;
driver_t *active_driver = &linux_driver;

uint32_t log_sector = 0; 

#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>

/***************************************************************
 * Correct RAID_OFFSET calculation for ZINF
 ***************************************************************/
static uint32_t compute_raid_offset(const char *devpath) {
    int fd = open(devpath, O_RDONLY);
    if (fd < 0) {
        perror("open loopdev");
        return 30; // fallback
    }

    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) {
        perror("BLKGETSIZE64");
        close(fd);
        return 30;
    }
    close(fd);

    uint64_t total_sectors = bytes / SECTOR_SIZE;

    if (total_sectors < 32) {
        // very small loop device → safe but small offset
        return 4;
    }

    // usable log area starts at sector 2
    uint64_t usable = total_sectors - 2;

    uint32_t offset = (uint32_t)(usable / RAID_MIRRORS);

    if (offset < 8)
        offset = 8;  // minimum offset

    return offset;
}


// Helper: wipe loop device (silent)
// ---------------------------------------------------------------
void wipe_loop_device() {
    system("dd if=/dev/zero of=/dev/loop0 bs=1M count=5 status=none");
}

// ---------------------------------------------------------------
// Reset entire ZINF system
// ---------------------------------------------------------------
void reset_zinf() {
    if (active_driver->deinit)
        active_driver->deinit(active_driver);

    wipe_loop_device();
    log_sector = 0;

    RAID_OFFSET = compute_raid_offset("/dev/loop0");
    if (setup_storage() != 0) {
        fprintf(stderr, "Storage setup failed\n");
        exit(1);
    }
    if (init_log_sector() != 0) {
        fprintf(stderr, "Log init failed\n");
        exit(1);
    }
}

// ---------------------------------------------------------------
uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ---------------------------------------------------------------
//                    MAIN BENCHMARK (OPTION A)
// ---------------------------------------------------------------
int main(void) {
    printf("PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten\n");

    // Chunk counts = number of *sectors* written per benchmark step
    int CHUNK_COUNTS[] = {
        1, 2, 4, 6, 8, 10, 12, 14, 16,
        32, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152
    };
    int NUM_TESTS = sizeof(CHUNK_COUNTS) / sizeof(CHUNK_COUNTS[0]);

    // Write ~500kB per test
    const int TARGET_TOTAL_BYTES = 500 * 1024;

    // Allocate ONE SECTOR ONLY (MCU realistic)
    uint8_t sector_payload[SECTOR_SIZE];
    memset(sector_payload, 0xAB, PAYLOAD_SIZE);

    uint8_t header = 0x01;  // example ZINF header

    for (int t = 0; t < NUM_TESTS; t++) {
        int chunks = CHUNK_COUNTS[t];
        int write_size = chunks * PAYLOAD_SIZE;

        reset_zinf();

        uint64_t max_latency = 0;
        uint64_t total_latency = 0;
        int ops = 0;
        int total_bytes = 0;

        uint64_t t_start = get_time_ns();

        while (total_bytes < TARGET_TOTAL_BYTES) {

            uint64_t t0 = get_time_ns();

            // -------------------------------------------------------
            // OPTION A: write *SECTOR BY SECTOR*, no big buffers
            // -------------------------------------------------------
            for (int i = 0; i < chunks; i++) {
                uint8_t rc = raid_u8bit_values(sector_payload, PAYLOAD_SIZE, &header);

                if (rc != 0) {
                    fprintf(stderr, "ZINF write error rc=%d\n", rc);
                    exit(1);
                }
            }

            uint64_t t1 = get_time_ns();
            uint64_t dt = t1 - t0;

            if (dt > max_latency) max_latency = dt;
            total_latency += dt;
            ops++;

            total_bytes += write_size;
        }

        uint64_t t_end = get_time_ns();
        double duration = (t_end - t_start) / 1e9;

        double throughput_kb = (total_bytes / 1024.0) / duration;
        double avg_us = (total_latency / (double)ops) / 1000.0;
        double max_us = max_latency / 1000.0;

        printf("%d,%.2f,%.2f,%.2f,%d\n",
               write_size, throughput_kb, max_us, avg_us, chunks);

        fprintf(stderr,
                "Chunks: %4d (%6d B) | Speed: %8.2f KB/s | MaxLat: %8.2f us | AvgLat: %8.2f us\n",
                chunks, write_size, throughput_kb, max_us, avg_us);
    }

    return 0;
}

