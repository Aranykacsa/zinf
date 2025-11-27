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

/* --- ZINF CONSTANTS (Must match storage.c) --- */
#define SECTOR_SIZE 512
#define PAYLOAD_SIZE 507 // 512 - 4 (CRC) - 1 (Header)

/* --- GLOBALS --- */
extern driver_t linux_driver;
driver_t *active_driver = &linux_driver;
uint32_t log_sector = 0; 

// Helper: Wipe /dev/loop0
void wipe_loop_device() {
    // Suppress output with > /dev/null
    system("dd if=/dev/zero of=/dev/loop0 bs=1M count=5 status=none");
}

void reset_zinf() {
    if (active_driver->deinit) active_driver->deinit(active_driver);
    wipe_loop_device();
    log_sector = 0;
    
    if (setup_storage() != 0) {
        fprintf(stderr, "Storage setup failed\n");
        exit(1);
    }
    if (init_log_sector() != 0) {
        fprintf(stderr, "Log init failed\n");
        exit(1);
    }
}

uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(void) {
    // CSV Header
    printf("PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten\n");

    // We MUST test multiples of 507 (PAYLOAD_SIZE)
    // 1 chunk, 2 chunks, 4 chunks, 8 chunks, 16 chunks
    int CHUNK_COUNTS[] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 32}; 
    int NUM_TESTS = sizeof(CHUNK_COUNTS) / sizeof(CHUNK_COUNTS[0]);
    
    // Total data to write per test run (e.g. ~500KB)
    const int TARGET_WRITE_TOTAL = 500 * 1024; 

    uint8_t header = 0x01; 
    
    // Allocate max buffer needed (32 * 507 is approx 16KB)
    uint8_t *buffer = malloc(32 * 512); 

    for (int i = 0; i < NUM_TESTS; i++) {
        int num_chunks = CHUNK_COUNTS[i];
        int write_size = num_chunks * PAYLOAD_SIZE; // e.g., 507, 1014...
        
        reset_zinf(); 
        memset(buffer, 0xAB, write_size);

        int total_bytes_written = 0;
        int operations_count = 0;
        
        uint64_t start_bench = get_time_ns();
        uint64_t max_latency = 0;
        uint64_t total_latency = 0;

        while (total_bytes_written < TARGET_WRITE_TOTAL) {
            uint64_t t0 = get_time_ns();
            
            // --- CALL ZINF ---
            // This will trigger RAID_MIRRORS (3) * num_chunks physical writes
            uint8_t rc = raid_u8bit_values(buffer, write_size, &header);
            
            uint64_t t1 = get_time_ns();
            uint64_t latency = t1 - t0;

            if (rc != 0) {
                fprintf(stderr, "Write error rc=%d at size %d\n", rc, write_size);
                break;
            }

            if (latency > max_latency) max_latency = latency;
            total_latency += latency;
            
            total_bytes_written += write_size;
            operations_count++;
        }
        
        uint64_t end_bench = get_time_ns();
        double duration = (end_bench - start_bench) / 1e9;
        
        double speed_kbps = (total_bytes_written / 1024.0) / duration;
        double avg_lat = (total_latency / (double)operations_count) / 1000.0;
        double max_lat = max_latency / 1000.0;

        printf("%d,%.2f,%.2f,%.2f,%d\n", 
               write_size, speed_kbps, max_lat, avg_lat, num_chunks);
               
        fprintf(stderr, "Chunks: %2d (%5d B) | Speed: %6.2f KB/s | MaxLat: %6.2f us\n", 
                num_chunks, write_size, speed_kbps, max_lat);
    }

    free(buffer);

/*    status = init_log_sector();
    if (status != 0) {
        printf("Failed to init log sector.\n");
        return 1;
    }

    uint8_t header = 0xAB;
    uint8_t payload[507];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = 12;

    printf("Writing test sector...\n");
    uint8_t rc = raid_u8bit_values(payload, sizeof(payload), &header);
    if (rc != 0) {
        printf("save_u8bit_values failed (%d)\n", rc);
        return 1;
    }

    printf("Write OK\n");

    printf("Writing test sector...\n");
    header = 0xBC;
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = 6;

    rc = raid_u8bit_values(payload, sizeof(payload), &header);
    if (rc != 0) {
        printf("save_u8bit_values failed (%d)\n", rc);
        return 1;
    }

    printf("Write OK\n");

    active_driver->deinit(active_driver);
    return 0;*/
}
