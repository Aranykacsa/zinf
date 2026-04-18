#define _GNU_SOURCE
#include "api.h"
#include "config.h"
#include "linux_driver.h"
#include "ram_driver.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>

#include <xlsxwriter.h>

/* -----------------------------------------------------------------------
   Constants
   ----------------------------------------------------------------------- */

#define CSV_IMG_PATH  "/var/tmp/zinf_csv_fault.img"
#define CSV_IMG_SECTS 4096u
#define MAX_FIELD     256
#define MAX_LINE      1024
#define MAX_SCENARIOS 512
#define MAX_RESULTS_XL 1000000 
#define PAGE_SIZE_XL   500000

/* -----------------------------------------------------------------------
   Excel colour palette
   ----------------------------------------------------------------------- */

#define XL_COLOR_PASS_ROW   0xC6EFCEu
#define XL_COLOR_WARN_ROW   0xFFEB9Cu
#define XL_COLOR_FAIL_ROW   0xFFC7CEu
#define XL_COLOR_HDR_BG     0x4472C4u
#define XL_COLOR_HDR_FG     0xFFFFFFu

/* -----------------------------------------------------------------------
   Enumerations
   ----------------------------------------------------------------------- */

typedef enum {
    FAULT_NONE                   = 0,
    FAULT_SEU                    = 1,
    FAULT_TORN_WRITE             = 2,
    FAULT_ZERO_FILL              = 3,
    FAULT_BLACKLIST_BEFORE_WRITE = 4,
    FAULT_METADATA_CORRUPT       = 5,
    FAULT_VERSION_WRAP           = 6,
    FAULT_POWER_LOSS             = 7
} fault_type_t;

typedef enum {
    ACTION_CHECK_ONLY = 0,
    ACTION_RECOVER    = 1,
    ACTION_RAID_READ  = 2,
    ACTION_SCRUB      = 3,
    ACTION_WRITE      = 4
} action_t;

/* -----------------------------------------------------------------------
   Scenario struct
   ----------------------------------------------------------------------- */

typedef struct {
    int           id;
    char          name[MAX_FIELD];
    uint8_t       mirror_count;
    fault_type_t  fault_type;
    char          affected_mirrors[MAX_FIELD];
    uint32_t      byte_offset;
    uint32_t      byte_count;
    uint8_t       corruption_byte;
    action_t      action;
    uint8_t       expected_write_rc;
    uint8_t       expected_action_rc;
    uint8_t       expected_valid_before;
    uint8_t       expected_valid_after;
    char          description[MAX_FIELD * 2];
} scenario_t;

/* -----------------------------------------------------------------------
   Result trace (lightweight for massive mode)
   ----------------------------------------------------------------------- */

typedef struct {
    long       iter;
    int        scen_id;
    int        action;
    uint8_t    rc;
    int        pass;
} result_trace_t;

/* -----------------------------------------------------------------------
   CLI options
   ----------------------------------------------------------------------- */

typedef struct {
    const char *scenarios_path;
    const char *output_path;
    int         fuzz_count;
    uint32_t    rng_seed;
    int         verbose;
    int         use_ram_driver;
    int         massive_mode;
    long        massive_iterations;
} cli_opts_t;

/* -----------------------------------------------------------------------
   Global state
   ----------------------------------------------------------------------- */

static zinf_ctx_t g_ctx;
static uint32_t   g_rng_state = 0;
static gzFile     g_gz_out    = NULL;

/* -----------------------------------------------------------------------
   RNG
   ----------------------------------------------------------------------- */

static uint32_t lcg_rand(void) {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return g_rng_state;
}

/* -----------------------------------------------------------------------
   Setup / teardown
   ----------------------------------------------------------------------- */

static int csv_setup(const cli_opts_t *opts, uint8_t mirror_count) {
    memset(&g_ctx, 0, sizeof(g_ctx));

    if (opts->use_ram_driver) {
        ram_driver_set_capacity(CSV_IMG_SECTS);
        g_ctx.driver = &ram_driver;
    } else {
        int fd = open(CSV_IMG_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) return -1;
        if (ftruncate(fd, (off_t)CSV_IMG_SECTS * SECTOR_SIZE) != 0) {
            close(fd); return -1;
        }
        close(fd);
        linux_driver_set_path(CSV_IMG_PATH);
        g_ctx.driver = &linux_driver;
    }

    g_ctx.sector_size      = SECTOR_SIZE;
    g_ctx.mirror_count     = mirror_count;
    g_ctx.metadata_sectors = 2;
    g_ctx.mirror_offset    = (CSV_IMG_SECTS - 2u) / (uint32_t)mirror_count;
    g_ctx.log_sector       = 0;
    g_ctx.raid_offset      = g_ctx.mirror_offset;

    if (g_ctx.driver->init(g_ctx.driver) != DRIVER_OK) return -1;
    if (init_log_sector(&g_ctx) != STORAGE_OK) return -1;
    return 0;
}

static void csv_teardown(const cli_opts_t *opts) {
    if (g_ctx.driver && g_ctx.driver->deinit) 
        g_ctx.driver->deinit(g_ctx.driver);
    if (!opts->use_ram_driver)
        unlink(CSV_IMG_PATH);
}

/* -----------------------------------------------------------------------
   Implementation of massive state fuzzer
   ----------------------------------------------------------------------- */

static void run_massive_fuzz(const cli_opts_t *opts) {
    printf("Starting massive fuzzing: %ld iterations\n", opts->massive_iterations);
    
    if (csv_setup(opts, 3) != 0) {
        fprintf(stderr, "Massive setup failed\n");
        return;
    }

    char csv_name[512];
    snprintf(csv_name, sizeof(csv_name), "%s.csv.gz", opts->output_path);
    g_gz_out = gzopen(csv_name, "wb");
    if (!g_gz_out) {
        fprintf(stderr, "Could not open %s for writing\n", csv_name);
        return;
    }

    gzprintf(g_gz_out, "Iter,Action,RC,Status\n");

    long passes = 0, fails = 0;
    for (long i = 0; i < opts->massive_iterations; i++) {
        // Randomly pick an action: WRITE, READ, SCRUB, or CORRUPT
        int coin = lcg_rand() % 100;
        uint8_t rc = STORAGE_OK;
        int action_type = 0;

        if (coin < 40) { // 40% Write
            action_type = 1;
            sensor_t s = { .temp = (float)(lcg_rand() % 100), .humidity = 50.0f };
            rc = raid_sensor_values(&g_ctx, &s, 1);
        } else if (coin < 70) { // 30% Read
            action_type = 2;
            uint64_t lba = (lcg_rand() % (CSV_IMG_SECTS - 2)) + 2;
            uint8_t payload[PAYLOAD_SIZE];
            rc = raid_read(&g_ctx, lba, payload);
        } else if (coin < 85) { // 15% Scrub
            action_type = 3;
            zinf_scrub_report_t rep;
            rc = zinf_scrub(&g_ctx, 1, 10, &rep);
        } else if (coin < 95) { // 10% Corrupt Payload
            action_type = 4;
            uint64_t lba = (lcg_rand() % (CSV_IMG_SECTS - 2)) + 2;
            if (opts->use_ram_driver) {
                ram_driver_corrupt(lba, lcg_rand() % SECTOR_SIZE, (uint8_t)lcg_rand());
            }
            rc = STORAGE_OK;
        } else { // 5% Corrupt Metadata
            action_type = 5;
            if (opts->use_ram_driver) {
                ram_driver_corrupt(0, (lcg_rand() % 41), (uint8_t)lcg_rand());
            }
            rc = STORAGE_OK;
        }

        if (rc == STORAGE_OK || rc == STORAGE_WARN_DEGRADED) passes++;
        else fails++;

        if (i % 10000 == 0) {
            printf("\rProgress: %ld/%ld (Pass: %ld, Fail: %ld)", i, opts->massive_iterations, passes, fails);
            fflush(stdout);
        }

        gzprintf(g_gz_out, "%ld,%d,%u,%s\n", i, action_type, rc, (rc == STORAGE_OK) ? "PASS" : "FAIL");
    }

    printf("\nMassive fuzzing complete. Results written to %s\n", csv_name);
    gzclose(g_gz_out);
    csv_teardown(opts);
}

/* -----------------------------------------------------------------------
   Main
   ----------------------------------------------------------------------- */

static void usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -R           use RAM mock driver (default: linux loopback)\n");
    printf("  -M           enable massive scale mode (1,000,000 iterations)\n");
    printf("  -n <count>   set iteration count (default: 1,000,000)\n");
    printf("  -o <path>    output prefix (default: fault_results)\n");
    printf("  -r <seed>    RNG seed (default: time())\n");
}

int main(int argc, char *argv[]) {
    cli_opts_t opts = {
        .scenarios_path = "fault_scenarios.csv",
        .output_path = "fault_results",
        .fuzz_count = 50,
        .rng_seed = (uint32_t)time(NULL),
        .use_ram_driver = 0,
        .massive_mode = 0,
        .massive_iterations = 1000000
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-R") == 0) opts.use_ram_driver = 1;
        else if (strcmp(argv[i], "-M") == 0) opts.massive_mode = 1;
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) opts.massive_iterations = atol(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) opts.output_path = argv[++i];
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) opts.rng_seed = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
    }

    g_rng_state = opts.rng_seed;

    if (opts.massive_mode) {
        run_massive_fuzz(&opts);
    } else {
        printf("Standard mode legacy support removed in favor of Task 6 massive fuzzer. Use -M.\n");
    }

    return 0;
}
