#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver.h"
#include "storage.h"

#define SECTOR_SIZE 512
#define PAYLOAD_SIZE 507

extern driver_t linux_driver;
driver_t *active_driver = &linux_driver;

uint32_t log_sector = 0; 
driver_t *drv = &linux_driver;

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ULL + ts.tv_nsec;
}

int main() {
    setup_storage();
    init_log_sector();

    uint8_t header = 1;
    uint8_t buf[SECTOR_SIZE];
    memset(buf, 0x55, PAYLOAD_SIZE);

    uint64_t maxlat = 0;
    uint64_t totlat = 0;
    uint64_t minlat = UINT64_MAX;

    const uint64_t target_writes = 3ull * 1024ull * 1024ull; // 3 million writes
    uint64_t count = 0;

    uint64_t start = now_ns();

    while (count < target_writes) {
        uint64_t t0 = now_ns();
        raid_u8bit_values(buf, PAYLOAD_SIZE, &header);
        uint64_t t1 = now_ns();

        uint64_t dt = t1 - t0;
        if (dt > maxlat) maxlat = dt;
        if (dt < minlat) minlat = dt;
        totlat += dt;

        count++;
        if ((count % 50000) == 0) {
            double avg = (totlat / (double)count) / 1000.0;
            printf("writes=%lu avg=%.2fus max=%.2fus min=%.2fus\n",
                count, avg, maxlat/1000.0, minlat/1000.0);
        }
    }

    uint64_t end = now_ns();

    double seconds = (end - start) / 1e9;
    double throughput = (count * PAYLOAD_SIZE) / (1024.0*1024.0*seconds);

    printf("\n=== ENDURANCE SUMMARY ===\n");
    printf("Total writes: %lu\n", count);
    printf("Avg Latency: %.2f us\n", (totlat/count)/1000.0);
    printf("Max Latency: %.2f ms\n", maxlat/1e6);
    printf("Min Latency: %.2f us\n", minlat/1000.0);
    printf("Throughput: %.2f MB/s\n\n", throughput);

    return 0;
}

