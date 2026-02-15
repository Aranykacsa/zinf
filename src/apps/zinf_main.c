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
#include "data.h"
#include "log.h"

/* =========================================================
   Compatibility wrapper (bench/cli may call this name)
   ========================================================= */
uint8_t raid_u8bit_values(uint8_t *buffer, size_t len, uint8_t *header) {
    return log_raid_u8bit_values(buffer, len, header);
}

/* =========================================================
   CLI (inlined) — no core/cli/cli.c needed
   ========================================================= */

#ifndef CLI_MAX_LINE
#define CLI_MAX_LINE 256
#endif

#ifndef CLI_MAX_TOKENS
#define CLI_MAX_TOKENS 32
#endif

#ifndef SENSOR_WIRE_SIZE
#define SENSOR_WIRE_SIZE 8u
#endif

static char   g_line[CLI_MAX_LINE];
static size_t g_len = 0;

static void cli_prompt(void) { printf("> "); }

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s || !out) return 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return 0;
    *out = (uint32_t)v;
    return 1;
}

static int tokenize(char *line, char *argv[], int max_argv) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = p;

        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void cli_help(void) {
    printf("Commands:\r\n");
    printf("  help\r\n");
    printf("  cfg show\r\n");
    printf("  storage init\r\n");
    printf("  log init\r\n");
    printf("  log last\r\n");
    printf("  msg save <0-255>\r\n");
    printf("  msg test\r\n");
    printf("  raid u8 <header> <count> <v0> <v1> ...\r\n");
    printf("  raid sensor <count> <v0> <v1> ...\r\n");
}

static void cli_cfg_show(void) {
    printf("SECTOR_SIZE      : %u\r\n", (unsigned)config->sector_size);
    printf("PAYLOAD_SIZE     : %u\r\n", (unsigned)PAYLOAD_SIZE);
    printf("MIRRORS          : %u\r\n", (unsigned)config->mirror_count);
    printf("MIRROR_OFFSET    : %u\r\n", (unsigned)config->mirror_offset);
    printf("SENSOR_WIRE_SIZE : %u\r\n", (unsigned)SENSOR_WIRE_SIZE);
    printf("SENSOR/MAX       : %u\r\n", (unsigned)(PAYLOAD_SIZE / SENSOR_WIRE_SIZE));
}

static void cli_cmd_storage_init(void) {
    uint8_t rc = setup_storage();
    printf("setup_storage: %u\r\n", (unsigned)rc);
}

static void cli_cmd_log_init(void) {
    uint8_t rc = init_log_sector();
    printf("init_log_sector: %u\r\n", (unsigned)rc);
}

static void cli_cmd_log_last(void) {
    uint32_t last = 0;
    uint8_t rc = log_get_last_sector(&last);
    if (rc != STORAGE_OK) {
        printf("log_get_last_sector error: %u\r\n", (unsigned)rc);
        return;
    }
    printf("last_sector: %lu\r\n", (unsigned long)last);
}

static void cli_cmd_msg_save(const char *arg) {
    uint32_t v = 0;
    if (!parse_u32(arg, &v) || v > 255) {
        printf("usage: msg save <0-255>\r\n");
        return;
    }
    uint8_t b = (uint8_t)v;
    uint8_t rc = save_msg(&b);
    printf("save_msg: %u\r\n", (unsigned)rc);
}

static void cli_cmd_msg_test(void) {
    uint8_t rc = test_save_msg();
    printf("test_save_msg: %u\r\n", (unsigned)rc);
}

/* Raw bytes raid */
static void cli_cmd_raid_u8(int argc, char *argv[]) {
    if (argc < 5) {
        printf("usage: raid u8 <header> <count> <v0> <v1> ...\r\n");
        return;
    }

    uint32_t header_u32 = 0, count_u32 = 0;
    if (!parse_u32(argv[2], &header_u32) || header_u32 > 255 ||
        !parse_u32(argv[3], &count_u32)) {
        printf("invalid header/count\r\n");
        return;
    }

    uint32_t count = count_u32;
    if (count == 0) { printf("count must be > 0\r\n"); return; }

    uint32_t provided = (uint32_t)(argc - 4);
    if (provided < count) {
        printf("need %lu values, got %lu\r\n",
               (unsigned long)count, (unsigned long)provided);
        return;
    }

    if (count > PAYLOAD_SIZE) {
        printf("count too big for payload (max %u)\r\n", (unsigned)PAYLOAD_SIZE);
        return;
    }

    uint8_t buf[PAYLOAD_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = 0;
        if (!parse_u32(argv[4 + i], &v) || v > 255) {
            printf("invalid value at index %lu\r\n", (unsigned long)i);
            return;
        }
        buf[i] = (uint8_t)v;
    }

    uint8_t header = (uint8_t)header_u32;
    uint8_t rc = raid_u8bit_values(buf, (size_t)count, &header);
    printf("raid_u8bit_values: %u\r\n", (unsigned)rc);
}

/* Sensor raid: each v becomes one sensor sample (temp=v, humidity=v) */
static void cli_cmd_raid_sensor(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: raid sensor <count> <v0> <v1> ...\r\n");
        return;
    }

    uint32_t count_u32 = 0;
    if (!parse_u32(argv[2], &count_u32)) {
        printf("invalid count\r\n");
        return;
    }

    uint32_t count = count_u32;
    if (count == 0) { printf("count must be > 0\r\n"); return; }

    uint32_t provided = (uint32_t)(argc - 3);
    if (provided < count) {
        printf("need %lu values, got %lu\r\n",
               (unsigned long)count, (unsigned long)provided);
        return;
    }

    const uint32_t max_records = (uint32_t)(PAYLOAD_SIZE / SENSOR_WIRE_SIZE);
    if (count > max_records) {
        printf("count too big (max %lu sensor records per payload)\r\n",
               (unsigned long)max_records);
        return;
    }

    sensor_t buf[count];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = 0;
        if (!parse_u32(argv[3 + i], &v) || v > 255) {
            printf("invalid value at index %lu\r\n", (unsigned long)i);
            return;
        }
        buf[i].temp = (float)v;
        buf[i].humidity = (float)v;
    }

    uint8_t rc = raid_sensor_values(buf, (size_t)count);
    printf("raid_sensor_values: %u\r\n", (unsigned)rc);
}

static void cli_process_line_local(const char *line_in) {
    if (!line_in) return;

    char line[CLI_MAX_LINE];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *argv[CLI_MAX_TOKENS];
    int argc = tokenize(line, argv, CLI_MAX_TOKENS);

    if (argc == 0) { cli_prompt(); return; }

    if (strcmp(argv[0], "help") == 0) { cli_help(); cli_prompt(); return; }

    if (strcmp(argv[0], "cfg") == 0 && argc >= 2 && strcmp(argv[1], "show") == 0) {
        cli_cfg_show(); cli_prompt(); return;
    }

    if (strcmp(argv[0], "storage") == 0) {
        if (argc >= 2 && strcmp(argv[1], "init") == 0) cli_cmd_storage_init();
        else printf("usage: storage init\r\n");
        cli_prompt(); return;
    }

    if (strcmp(argv[0], "log") == 0) {
        if (argc >= 2 && strcmp(argv[1], "init") == 0) cli_cmd_log_init();
        else if (argc >= 2 && strcmp(argv[1], "last") == 0) cli_cmd_log_last();
        else printf("usage: log init | log last\r\n");
        cli_prompt(); return;
    }

    if (strcmp(argv[0], "msg") == 0) {
        if (argc >= 2 && strcmp(argv[1], "test") == 0) cli_cmd_msg_test();
        else if (argc >= 3 && strcmp(argv[1], "save") == 0) cli_cmd_msg_save(argv[2]);
        else printf("usage: msg save <0-255> | msg test\r\n");
        cli_prompt(); return;
    }

    if (strcmp(argv[0], "raid") == 0) {
        if (argc >= 2 && strcmp(argv[1], "u8") == 0) cli_cmd_raid_u8(argc, argv);
        else if (argc >= 2 && strcmp(argv[1], "sensor") == 0) cli_cmd_raid_sensor(argc, argv);
        else printf("usage: raid u8 ... | raid sensor ...\r\n");
        cli_prompt(); return;
    }

    printf("unknown command: %s\r\n", argv[0]);
    cli_prompt();
}

static void cli_rx_char_local(char c) {
    if (c == '\r') return;

    if (c == '\n') {
        g_line[g_len] = '\0';
        printf("\r\n");
        cli_process_line_local(g_line);
        g_len = 0;
        return;
    }

    if (c == '\b' || c == 127) {
        if (g_len > 0) {
            g_len--;
            printf("\b \b");
        }
        return;
    }

    if (g_len < (CLI_MAX_LINE - 1)) {
        g_line[g_len++] = c;
        putchar(c);
    }
}

/* =========================================================
   Utilities for main modes
   ========================================================= */

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
   Reader
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
   Benchmark helpers
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

/* Synthetic sensor generator for benchmark */
static inline uint32_t xs32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline float noise_f(uint32_t *state, float amp) {
    uint32_t r = xs32(state) & 0xFFFFu;
    float n = ((float)r / 32767.5f) - 1.0f;
    return n * amp;
}

static inline float tri01(uint32_t phase, uint32_t period) {
    if (period == 0) return 0.0f;
    uint32_t p = phase % period;
    uint32_t half = period / 2u;
    if (half == 0) return 0.0f;
    if (p < half) return (float)p / (float)half;
    return (float)(period - p) / (float)half;
}

static void sensor_generate(sensor_t *s, uint32_t tick) {
    const uint32_t TEMP_PERIOD = 2000u;
    const uint32_t HUM_PERIOD  = 2600u;

    float t_wave = tri01(tick, TEMP_PERIOD);
    float h_wave = tri01(tick, HUM_PERIOD);

    float temp = 18.0f + 12.0f * t_wave;
    float hum  = 65.0f - 30.0f * h_wave;

    uint32_t rng = 0xA5A5u ^ (tick * 2654435761u);
    temp += noise_f(&rng, 0.15f);
    hum  += noise_f(&rng, 0.40f);

    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;

    s->temp = temp;
    s->humidity = hum;
}

static int run_benchmark(void) {
    printf("PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten\n");

    int CHUNK_COUNTS[] = { 1,2,4,6,8,10,12,14,16, 32, 1024, 2048, 4096 };
    int NUM_TESTS = (int)(sizeof(CHUNK_COUNTS) / sizeof(CHUNK_COUNTS[0]));
    const int TARGET_TOTAL_BYTES = 500 * 1024;

    const int max_records = (int)(PAYLOAD_SIZE / SENSOR_WIRE_SIZE);
    if (max_records <= 0) {
        fprintf(stderr, "PAYLOAD_SIZE too small for SENSOR_WIRE_SIZE\n");
        return 1;
    }

    sensor_t sensors[max_records];
    uint32_t tick = 0;

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
                for (int k = 0; k < max_records; k++) {
                    sensor_generate(&sensors[k], tick++);
                }

                uint8_t rc = raid_sensor_values(sensors, (size_t)max_records);
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
    RAID_OFFSET = compute_raid_offset("/dev/loop0");

    if (setup_storage() != STORAGE_OK) {
        fprintf(stderr, "setup_storage failed (need sudo for /dev/loop0?)\n");
        exit(1);
    }
}

static int run_cli_interactive(void) {
    cli_init_storage();

    printf("ZINF CLI. Type 'help'.\n");
    cli_prompt();
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {
        cli_rx_char_local((char)c);
        fflush(stdout);
    }
    return 0;
}

static int run_cli_one_shot(int argc, char **argv, int start_index) {
    cli_init_storage();

    char line[512];
    join_argv(line, sizeof(line), argc, argv, start_index);
    cli_process_line_local(line);
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

    return run_cli_one_shot(argc, argv, 1);
}
