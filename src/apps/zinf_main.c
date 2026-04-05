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

#include "api.h"
#include "config.h"
#include "linux_driver.h"

/* Forward declaration — zinf_ctx is defined in platform_linux.c */
extern zinf_ctx_t *zinf_ctx;
extern driver_t    linux_driver;

/* =========================================================
   CLI
   ========================================================= */

#define CLI_MAX_LINE   256
#define CLI_MAX_TOKENS  32
#define SENSOR_WIRE_SIZE 8u

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
    printf("Commands:\r\n"
           "  help\r\n"
           "  cfg show\r\n"
           "  storage init\r\n"
           "  log init\r\n"
           "  msg save <0-255>\r\n"
           "  msg test\r\n"
           "  raid sensor <count> <v0> <v1> ...\r\n");
}

static void cli_cfg_show(void) {
    printf("SECTOR_SIZE      : %u\r\n", (unsigned)zinf_ctx->sector_size);
    printf("PAYLOAD_SIZE     : %u\r\n", (unsigned)PAYLOAD_SIZE);
    printf("RAID_MIRRORS     : %u\r\n", (unsigned)zinf_ctx->mirror_count);
    printf("RAID_OFFSET      : %u\r\n", (unsigned)zinf_ctx->raid_offset);
    printf("SENSOR_WIRE_SIZE : %u\r\n", (unsigned)SENSOR_WIRE_SIZE);
    printf("SENSOR/MAX       : %u\r\n", (unsigned)(PAYLOAD_SIZE / SENSOR_WIRE_SIZE));
}

static void cli_cmd_storage_init(void) {
    uint8_t rc = setup_storage(zinf_ctx);
    printf("setup_storage: %u\r\n", (unsigned)rc);
}

static void cli_cmd_log_init(void) {
    uint8_t rc = init_log_sector(zinf_ctx);
    printf("init_log_sector: %u\r\n", (unsigned)rc);
}

static void cli_cmd_msg_save(const char *arg) {
    uint32_t v = 0;
    if (!parse_u32(arg, &v) || v > 255) {
        printf("usage: msg save <0-255>\r\n");
        return;
    }
    uint8_t b = (uint8_t)v;
    uint8_t rc = save_msg(zinf_ctx, &b);
    printf("save_msg: %u\r\n", (unsigned)rc);
}

static void cli_cmd_msg_test(void) {
    uint8_t rc = test_save_msg(zinf_ctx);
    printf("test_save_msg: %u\r\n", (unsigned)rc);
}

static void cli_cmd_raid_sensor(int argc, char *argv[]) {
    if (argc < 4) { printf("usage: raid sensor <count> <v0> <v1> ...\r\n"); return; }

    uint32_t count_u32 = 0;
    if (!parse_u32(argv[2], &count_u32)) { printf("invalid count\r\n"); return; }
    if (count_u32 == 0) { printf("count must be > 0\r\n"); return; }

    uint32_t provided = (uint32_t)(argc - 3);
    if (provided < count_u32) {
        printf("need %lu values, got %lu\r\n",
               (unsigned long)count_u32, (unsigned long)provided);
        return;
    }

    const uint32_t max_records = (uint32_t)(PAYLOAD_SIZE / SENSOR_WIRE_SIZE);
    if (count_u32 > max_records) {
        printf("count too big (max %lu per payload)\r\n", (unsigned long)max_records);
        return;
    }

    sensor_t buf[count_u32];
    for (uint32_t i = 0; i < count_u32; i++) {
        uint32_t v = 0;
        if (!parse_u32(argv[3 + i], &v) || v > 255) {
            printf("invalid value at index %lu\r\n", (unsigned long)i);
            return;
        }
        buf[i].temp     = (float)v;
        buf[i].humidity = (float)v;
    }

    uint8_t rc = raid_sensor_values(zinf_ctx, buf, (size_t)count_u32);
    printf("raid_sensor_values: %u\r\n", (unsigned)rc);
}

static void cli_process_line(const char *line_in) {
    if (!line_in) return;
    char line[CLI_MAX_LINE];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *argv[CLI_MAX_TOKENS];
    int argc = tokenize(line, argv, CLI_MAX_TOKENS);
    if (argc == 0) { cli_prompt(); return; }

    if (strcmp(argv[0], "help") == 0) { cli_help(); }
    else if (strcmp(argv[0], "cfg") == 0 && argc >= 2 && strcmp(argv[1], "show") == 0)
        cli_cfg_show();
    else if (strcmp(argv[0], "storage") == 0 && argc >= 2 && strcmp(argv[1], "init") == 0)
        cli_cmd_storage_init();
    else if (strcmp(argv[0], "log") == 0 && argc >= 2 && strcmp(argv[1], "init") == 0)
        cli_cmd_log_init();
    else if (strcmp(argv[0], "msg") == 0) {
        if      (argc >= 2 && strcmp(argv[1], "test") == 0) cli_cmd_msg_test();
        else if (argc >= 3 && strcmp(argv[1], "save") == 0) cli_cmd_msg_save(argv[2]);
        else printf("usage: msg save <0-255> | msg test\r\n");
    }
    else if (strcmp(argv[0], "raid") == 0 && argc >= 2 && strcmp(argv[1], "sensor") == 0)
        cli_cmd_raid_sensor(argc, argv);
    else
        printf("unknown command: %s\r\n", argv[0]);

    cli_prompt();
}

static void cli_rx_char(char c) {
    if (c == '\r') return;
    if (c == '\n') {
        g_line[g_len] = '\0';
        printf("\r\n");
        cli_process_line(g_line);
        g_len = 0;
        return;
    }
    if (c == '\b' || c == 127) {
        if (g_len > 0) { g_len--; printf("\b \b"); }
        return;
    }
    if (g_len < (CLI_MAX_LINE - 1)) {
        g_line[g_len++] = c;
        putchar(c);
    }
}

/* =========================================================
   Utilities
   ========================================================= */

static void print_usage(const char *argv0) {
    printf("Usage:\n"
           "  %s bench                Run benchmark (needs sudo)\n"
           "  %s cli                  Interactive CLI\n"
           "  %s read <img>           Image geometry info\n"
           "  %s help                 Show this help\n"
           "  %s <cmd...>             One-shot CLI command\n",
           argv0, argv0, argv0, argv0, argv0);
}

static void join_argv(char *out, size_t out_sz, int argc, char **argv, int start) {
    size_t pos = 0;
    out[0] = '\0';
    for (int i = start; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (pos + n + 2 >= out_sz) { fprintf(stderr, "Command too long\n"); exit(2); }
        memcpy(&out[pos], argv[i], n);
        pos += n;
        if (i != argc - 1) out[pos++] = ' ';
    }
    out[pos] = '\0';
}

/* =========================================================
   Reader
   ========================================================= */

static uint64_t detect_total_sectors(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    fclose(f);
    return (sz <= 0) ? 0 : (uint64_t)sz / (uint64_t)SECTOR_SIZE;
}

static int run_reader(const char *img) {
    uint64_t total = detect_total_sectors(img);
    if (total == 0) { printf("Could not read '%s'.\n", img); return 1; }
    uint64_t usable = (total > 2) ? (total - 2) : 0;
    uint64_t offset = (zinf_ctx->mirror_count > 0)
                      ? (usable / zinf_ctx->mirror_count) : 0;
    printf("Image          : %s\n",      img);
    printf("Sector size    : %u\n",      (unsigned)SECTOR_SIZE);
    printf("Payload size   : %u\n",      (unsigned)PAYLOAD_SIZE);
    printf("Mirrors        : %u\n",      (unsigned)zinf_ctx->mirror_count);
    printf("Total sectors  : %llu\n",   (unsigned long long)total);
    printf("Usable sectors : %llu\n",   (unsigned long long)usable);
    printf("RAID offset    : %llu\n",   (unsigned long long)offset);
    return 0;
}

/* =========================================================
   Benchmark
   ========================================================= */

static uint64_t compute_raid_offset(const char *devpath) {
    int fd = open(devpath, O_RDONLY);
    if (fd < 0) { perror("open"); return 30u; }

    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) { perror("BLKGETSIZE64"); close(fd); return 30u; }
    close(fd);

    uint64_t total_sectors = bytes / SECTOR_SIZE;
    if (total_sectors < 32) return 4u;

    uint64_t usable = total_sectors - 2u;
    uint64_t offset = (zinf_ctx->mirror_count > 0)
                      ? (usable / zinf_ctx->mirror_count) : 30u;
    if (offset < 8u) offset = 8u;

    /* Validate: mirror_count should be odd for majority voting */
    if (zinf_ctx->mirror_count > 1 && (zinf_ctx->mirror_count % 2u) == 0) {
        fprintf(stderr, "[WARN] mirror_count=%u is even — majority voting disabled. "
                        "Consider using an odd number.\n",
                (unsigned)zinf_ctx->mirror_count);
    }

    return offset;
}

static void wipe_loop_device(const char *dev) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "dd if=/dev/zero of=%s bs=1M count=5 status=none", dev);
    system(cmd);
}

static void reset_zinf(const char *dev) {
    wipe_loop_device(dev);

    zinf_ctx->raid_offset  = compute_raid_offset(dev);
    zinf_ctx->mirror_offset = zinf_ctx->raid_offset;

    if (setup_storage(zinf_ctx) != STORAGE_OK) {
        fprintf(stderr, "Storage setup failed\n"); exit(1);
    }
    if (init_log_sector(zinf_ctx) != STORAGE_OK) {
        fprintf(stderr, "Log init failed\n"); exit(1);
    }
}

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}
static inline float noise_f(uint32_t *s, float amp) {
    uint32_t r = xs32(s) & 0xFFFFu;
    return (((float)r / 32767.5f) - 1.0f) * amp;
}
static inline float tri01(uint32_t phase, uint32_t period) {
    if (!period) return 0.0f;
    uint32_t p = phase % period, half = period / 2u;
    if (!half) return 0.0f;
    return (p < half) ? (float)p / (float)half
                       : (float)(period - p) / (float)half;
}

static void sensor_generate(sensor_t *s, uint32_t tick) {
    float tw = tri01(tick, 2000u);
    float hw = tri01(tick, 2600u);
    uint32_t rng = 0xA5A5u ^ (tick * 2654435761u);
    float temp = 18.0f + 12.0f * tw + noise_f(&rng, 0.15f);
    float hum  = 65.0f - 30.0f * hw + noise_f(&rng, 0.40f);
    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;
    s->temp = temp; s->humidity = hum;
}

static int run_benchmark(void) {
    const char *dev = "/dev/loop0";
    printf("PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten\n");

    int CHUNK_COUNTS[] = { 1,2,4,6,8,10,12,14,16,32,1024,2048,4096 };
    int NUM_TESTS = (int)(sizeof(CHUNK_COUNTS) / sizeof(CHUNK_COUNTS[0]));
    const int TARGET_TOTAL_BYTES = 500 * 1024;
    const int max_records = (int)(PAYLOAD_SIZE / SENSOR_WIRE_SIZE);

    if (max_records <= 0) { fprintf(stderr, "PAYLOAD_SIZE too small\n"); return 1; }

    sensor_t sensors[max_records];
    uint32_t tick = 0;

    for (int t = 0; t < NUM_TESTS; t++) {
        int chunks     = CHUNK_COUNTS[t];
        int write_size = chunks * (int)PAYLOAD_SIZE;

        reset_zinf(dev);

        uint64_t max_lat = 0, total_lat = 0;
        int ops = 0, total_bytes = 0;
        uint64_t t_start = get_time_ns();

        while (total_bytes < TARGET_TOTAL_BYTES) {
            uint64_t t0 = get_time_ns();
            for (int i = 0; i < chunks; i++) {
                for (int k = 0; k < max_records; k++) sensor_generate(&sensors[k], tick++);
                uint8_t rc = raid_sensor_values(zinf_ctx, sensors, (size_t)max_records);
                if (rc != STORAGE_OK) { fprintf(stderr, "write error rc=%u\n", rc); return 1; }
            }
            uint64_t dt = get_time_ns() - t0;
            if (dt > max_lat) max_lat = dt;
            total_lat += dt; ops++;
            total_bytes += write_size;
        }

        double dur = (get_time_ns() - t_start) / 1e9;
        printf("%d,%.2f,%.2f,%.2f,%d\n",
               write_size,
               (total_bytes / 1024.0) / dur,
               max_lat / 1000.0,
               (total_lat / (double)ops) / 1000.0,
               chunks);
        fprintf(stderr, "Chunks:%4d (%6dB) Speed:%8.2fKB/s MaxLat:%8.2fus AvgLat:%8.2fus\n",
                chunks, write_size,
                (total_bytes / 1024.0) / dur,
                max_lat / 1000.0,
                (total_lat / (double)ops) / 1000.0);
    }
    return 0;
}

/* =========================================================
   CLI mode
   ========================================================= */

static void cli_init_storage(void) {
    zinf_ctx->driver      = &linux_driver;
    zinf_ctx->raid_offset = compute_raid_offset("/dev/loop0");
    if (setup_storage(zinf_ctx) != STORAGE_OK) {
        fprintf(stderr, "setup_storage failed (need sudo?)\n"); exit(1);
    }
}

static int run_cli_interactive(void) {
    cli_init_storage();
    printf("ZINF CLI. Type 'help'.\n");
    cli_prompt(); fflush(stdout);
    int c;
    while ((c = getchar()) != EOF) { cli_rx_char((char)c); fflush(stdout); }
    return 0;
}

static int run_cli_one_shot(int argc, char **argv, int start) {
    cli_init_storage();
    char line[512];
    join_argv(line, sizeof(line), argc, argv, start);
    cli_process_line(line);
    return 0;
}

/* =========================================================
   Main
   ========================================================= */

int main(int argc, char **argv) {
    /* Wire up the Linux driver into the default context */
    zinf_ctx->driver = &linux_driver;

    if (argc < 2) { print_usage(argv[0]); return 0; }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0)
        { print_usage(argv[0]); return 0; }
    if (strcmp(argv[1], "bench") == 0) return run_benchmark();
    if (strcmp(argv[1], "cli")   == 0) return run_cli_interactive();
    if (strcmp(argv[1], "read")  == 0) {
        if (argc < 3) { fprintf(stderr, "usage: %s read <img>\n", argv[0]); return 2; }
        return run_reader(argv[2]);
    }
    return run_cli_one_shot(argc, argv, 1);
}
