#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>

#include "config.h"
#include "storage.h"
#include "cli.h"

/* -----------------------------
   Utilities
----------------------------- */

static void print_usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s bench                 Run benchmark (needs sudo for /dev/loop0)\n", argv0);
    printf("  %s cli                   Interactive CLI (needs sudo for /dev/loop0)\n", argv0);
    printf("  %s read <img>             Reader (info) for an image file\n", argv0);
    printf("  %s help                   Show this help\n", argv0);
    printf("\nConvenience:\n");
    printf("  %s <cli command...>       Run one CLI command (non-interactive)\n", argv0);
    printf("\nExamples:\n");
    printf("  sudo %s bench\n", argv0);
    printf("  sudo %s cli\n", argv0);
    printf("  sudo %s log init\n", argv0);
    printf("  %s read testdisk.img\n", argv0);
}

/* join argv[start..] into one space-separated string */
static void join_argv(char *out, size_t out_sz, int argc, char **argv, int start) {
    size_t pos = 0;
    out[0] = '\0';

    for (int i = start; i < argc; i++) {
        const char *w = argv[i];
        size_t n = strlen(w);
        if (pos + n + 2 >= out_sz) {
            fprintf(stderr, "Command too long\n");
            exit(2);
        }
        memcpy(&out[pos], w, n);
        pos += n;
        if (i != argc - 1) out[pos++] = ' ';
    }
    out[pos] = '\0';
}

/* -----------------------------
   Reader (currently geometry/info)
----------------------------- */

static uint64_t detect_total_sectors(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    fclose(f);
    if (sz <= 0) return 0;

    return (uint64_t)sz / (uint64_t)SECTOR_SIZE;
}

static int run_reader(const char *img) {
    uint64_t total_sectors = detect_total_sectors(img);
    if (total_sectors == 0) {
        printf("Could not read image '%s' (or empty).\n", img);
        return 1;
    }

    printf("Image          : %s\n", img);
    printf("Sector size    : %u\n", (unsigned)SECTOR_SIZE);
    printf("Payload size   : %u\n", (unsigned)PAYLOAD_SIZE);
    printf("Mirrors        : %u\n", (unsigned)RAID_MIRRORS);

    uint64_t usable = (total_sectors > 2) ? (total_sectors - 2) : 0;
    uint32_t raid_offset = (RAID_MIRRORS > 0) ? (uint32_t)(usable / RAID_MIRRORS) : 0;

    printf("Total sectors  : %llu\n", (unsigned long long)total_sectors);
    printf("Usable sectors : %llu\n", (unsigned long long)usable);
    printf("Raid offset    : %u\n", raid_offset);

    return 0;
}

/* -----------------------------
   Benchmark (your current logic)
----------------------------- */

static uint32_t compute_raid_offset(const char *devpath) {
    int fd = open(devpath, O_RDONLY);
    if (fd < 0) {
        perror("open loopdev");
        return 30;
    }

    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) {
        perror("BLKGETSIZE64");
        close(fd);
        return 30;
    }
    close(fd);

    uint64_t total_sectors = bytes / SECTOR_SIZE;

    if (total_sectors < 32) return 4;

    uint64_t usable = total_sectors - 2;
    uint32_t offset = (uint32_t)(usable / RAID_MIRRORS);

    if (offset < 8) offset = 8;
    return offset;
}

static void wipe_loop_device(void) {
    system("dd if=/dev/zero of=/dev/loop0 bs=1M count=5 status=none");
}

static void reset_zinf(void) {
    wipe_loop_device();

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

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int run_benchmark(void) {
    printf("PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten\n");

    int CHUNK_COUNTS[] = { 1,2,4,6,8,10,12,14,16, 32, 1024, 2048, 4096 };
    int NUM_TESTS = (int)(sizeof(CHUNK_COUNTS) / sizeof(CHUNK_COUNTS[0]));
    const int TARGET_TOTAL_BYTES = 500 * 1024;

    uint8_t sector_payload[SECTOR_SIZE];
    memset(sector_payload, 0xAB, sizeof(sector_payload));

    uint8_t header = 0x01;

    for (int t = 0; t < NUM_TESTS; t++) {
        int chunks = CHUNK_COUNTS[t];
        int write_size = chunks * (int)PAYLOAD_SIZE;

        reset_zinf();

        uint64_t max_latency = 0;
        uint64_t total_latency = 0;
        int ops = 0;
        int total_bytes = 0;

        uint64_t t_start = get_time_ns();

        while (total_bytes < TARGET_TOTAL_BYTES) {
            uint64_t t0 = get_time_ns();

            for (int i = 0; i < chunks; i++) {
                uint8_t rc = raid_u8bit_values(sector_payload, PAYLOAD_SIZE, &header);
                if (rc != 0) {
                    fprintf(stderr, "ZINF write error rc=%u\n", (unsigned)rc);
                    return 1;
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

/* -----------------------------
   CLI mode (interactive + one-shot)
----------------------------- */

static void cli_init_storage(void) {
    /* mimic bench: compute RAID_OFFSET based on /dev/loop0 size */
    RAID_OFFSET = compute_raid_offset("/dev/loop0");

    if (setup_storage() != STORAGE_OK) {
        fprintf(stderr, "setup_storage failed (need sudo for /dev/loop0?)\n");
        exit(1);
    }
}

static int run_cli_interactive(void) {
    cli_init_storage();

    printf("ZINF CLI. Type 'help'.\n> ");
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {
        cli_rx_char((char)c);
        fflush(stdout);
    }
    return 0;
}

static int run_cli_one_shot(int argc, char **argv, int start_index) {
    cli_init_storage();

    char line[512];
    join_argv(line, sizeof(line), argc, argv, start_index);
    cli_process_line(line);
    return 0;
}

/* -----------------------------
   Main dispatch
----------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "bench") == 0) {
        return run_benchmark();
    }

    if (strcmp(argv[1], "cli") == 0) {
        return run_cli_interactive();
    }

    if (strcmp(argv[1], "read") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s read <img>\n", argv[0]);
            return 2;
        }
        return run_reader(argv[2]);
    }

    /* Otherwise treat argv[1..] as a one-shot CLI command */
    return run_cli_one_shot(argc, argv, 1);
}
