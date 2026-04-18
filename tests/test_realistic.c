#define _GNU_SOURCE
#include "api.h"
#include "config.h"
#include "ram_driver.h"
#include "linux_driver.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <time.h>

/* -----------------------------------------------------------------------
   Image sizing
   65536 sectors, 3 mirrors → mirror_offset = (65536-2)/3 = 21844
   Safe capacity before wrap: RESET_THRESHOLD
   ----------------------------------------------------------------------- */

#define REAL_IMG_SECTS   65536u
#define MAX_SHADOW       20000u
#define RESET_THRESHOLD  19000u

/* -----------------------------------------------------------------------
   Shadow record — remembers what was written and where
   ----------------------------------------------------------------------- */

typedef struct {
    uint64_t lba;
    float    temp;
    float    humidity;
    int      injected;   /* non-zero if a fault was injected on this sector */
} shadow_t;

/* -----------------------------------------------------------------------
   Accumulated statistics
   ----------------------------------------------------------------------- */

typedef struct {
    long writes_attempted;
    long writes_ok;
    long writes_degraded;
    long writes_failed;
    long errors_injected;
    long power_cycles;
    long format_cycles;
    long scrub_repaired;
    long scrub_unrecoverable;
    long verified_ok;
    long verified_lost;    /* STORAGE_ERR_UNRECOVERABLE — data is gone */
    long verified_silent;  /* STORAGE_OK but wrong bytes — this is a bug */
} stats_t;

/* -----------------------------------------------------------------------
   Globals
   ----------------------------------------------------------------------- */

static zinf_ctx_t g_ctx;
static shadow_t   g_shadow[MAX_SHADOW];
static uint32_t   g_shadow_count = 0;
static uint32_t   g_write_counter = 0;  /* monotonic, never resets — unique ID per record */
static uint32_t   g_rng = 0;
static stats_t    g_stats;

/* -----------------------------------------------------------------------
   LCG RNG
   ----------------------------------------------------------------------- */

static uint32_t lcg(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static float lcg_f(void) {
    return (float)(lcg() & 0xFFFFu) / 65535.0f;
}

/* -----------------------------------------------------------------------
   Unpack LE float32 from 4 bytes
   ----------------------------------------------------------------------- */

static float unpack_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0]
               | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16)
               | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* -----------------------------------------------------------------------
   Driver + context setup
   ----------------------------------------------------------------------- */

static int setup(void) {
    ram_driver_set_capacity(REAL_IMG_SECTS);
    g_ctx.driver           = &ram_driver;
    g_ctx.sector_size      = SECTOR_SIZE;
    g_ctx.mirror_count     = RAID_MIRRORS;
    g_ctx.metadata_sectors = 2;
    g_ctx.mirror_offset    = (REAL_IMG_SECTS - 2u) / (uint32_t)RAID_MIRRORS;
    g_ctx.log_sector       = 0;
    g_ctx.raid_offset      = g_ctx.mirror_offset;

    if (g_ctx.driver->init(g_ctx.driver) != DRIVER_OK) return -1;
    if (init_log_sector(&g_ctx) != STORAGE_OK)         return -1;
    return 0;
}

/* -----------------------------------------------------------------------
   Re-format in place (simulate SD card wipe without removing the driver)
   ----------------------------------------------------------------------- */

static void do_format(int verbose) {
    zinf_clear_bad_sectors(&g_ctx);
    init_log_sector(&g_ctx);
    g_shadow_count = 0;
    g_stats.format_cycles++;
    if (verbose) printf("[FORMAT] Re-formatted — write counter reset to 0 sectors\n");
}

/* -----------------------------------------------------------------------
   Write a random-sized batch of sensor records
   Each record: temp = (float)write_counter, humidity = counter % 100
   This gives each record a unique, verifiable value.
   ----------------------------------------------------------------------- */

static void do_write_batch(int verbose, int max_batch, float error_rate) {
    int batch = (int)(lcg() % (uint32_t)(max_batch - 9)) + 10;

    for (int i = 0; i < batch; i++) {

        /* Re-format if approaching capacity */
        if (g_shadow_count >= RESET_THRESHOLD) {
            do_format(verbose);
        }

        sensor_t s = {
            .temp     = (float)g_write_counter,
            .humidity = (float)(g_write_counter % 100)
        };

        uint64_t lba_before = 0;
        get_last_sector(&g_ctx, &lba_before);

        g_stats.writes_attempted++;
        uint8_t rc = raid_sensor_values(&g_ctx, &s, 1);

        if (rc == STORAGE_OK || rc == STORAGE_WARN_DEGRADED) {
            if (rc == STORAGE_OK) g_stats.writes_ok++;
            else                  g_stats.writes_degraded++;

            g_shadow[g_shadow_count++] = (shadow_t){
                .lba      = lba_before + 1u,
                .temp     = s.temp,
                .humidity = s.humidity,
                .injected = 0
            };
            g_write_counter++;
        } else {
            g_stats.writes_failed++;
        }

        /* -------------------------------------------------------
           Fault injection: with probability error_rate, corrupt
           1 or 2 mirrors of a random previously-written sector.
           This simulates a cosmic-ray bitflip or partial erase.
           ------------------------------------------------------- */
        if (g_shadow_count > 0 && lcg_f() < error_rate) {
            uint32_t idx     = lcg() % g_shadow_count;
            uint64_t victim  = g_shadow[idx].lba;
            int      nmirr   = (int)(lcg() % 2u) + 1;  /* 1 or 2 mirrors */

            for (int m = 0; m < nmirr && m < (int)g_ctx.mirror_count; m++) {
                uint64_t phys = victim + (uint64_t)m * g_ctx.mirror_offset;
                uint32_t off  = lcg() % SECTOR_SIZE;
                ram_driver_corrupt(phys, off, (uint8_t)lcg());
            }
            g_shadow[idx].injected++;
            g_stats.errors_injected++;
        }
    }
}

/* -----------------------------------------------------------------------
   Power cycle:
   1. Clear the in-RAM blacklist (simulates MCU losing volatile state)
   2. Run zinf_scrub to rebuild the blacklist and repair degraded mirrors
   3. Read back every shadow entry and compare to expected value
   ----------------------------------------------------------------------- */

static void do_power_cycle(int verbose) {
    /* Step 1: lose volatile state */
    zinf_clear_bad_sectors(&g_ctx);

    /* Step 2: scrub — rebuilds blacklist and repairs 1-mirror corruptions */
    uint64_t last = 0;
    get_last_sector(&g_ctx, &last);

    zinf_scrub_report_t rep = {0};
    if (last >= 2u)
        zinf_scrub(&g_ctx, 2u, last, &rep);

    g_stats.power_cycles++;
    g_stats.scrub_repaired      += rep.repaired;
    g_stats.scrub_unrecoverable += rep.unrecoverable;

    /* Step 3: verify every record in the shadow buffer */
    long cyc_ok     = 0;
    long cyc_lost   = 0;
    long cyc_silent = 0;
    uint8_t payload[PAYLOAD_SIZE];

    for (uint32_t i = 0; i < g_shadow_count; i++) {
        uint8_t rc = raid_read(&g_ctx, g_shadow[i].lba, payload);

        if (rc == STORAGE_ERR_UNRECOVERABLE) {
            g_stats.verified_lost++;
            cyc_lost++;
            continue;
        }

        if (rc != STORAGE_OK && rc != STORAGE_WARN_DEGRADED)
            continue;

        /* Wire format: [0..3] = temp f32 LE, [4..7] = humidity f32 LE */
        float got_temp = unpack_f32(&payload[0]);
        float got_hum  = unpack_f32(&payload[4]);

        if (fabsf(got_temp - g_shadow[i].temp)     < 0.001f &&
            fabsf(got_hum  - g_shadow[i].humidity) < 0.001f) {
            g_stats.verified_ok++;
            cyc_ok++;
        } else {
            /* STORAGE_OK was returned but the bytes are wrong.
               This must never happen — it means ZINF reported success
               while silently handing back corrupted data. */
            g_stats.verified_silent++;
            cyc_silent++;
            fprintf(stderr,
                "[BUG] Silent corruption at LBA=%" PRIu64
                ": expected temp=%.1f got %.1f\n",
                g_shadow[i].lba, g_shadow[i].temp, got_temp);
        }
    }

    if (verbose) {
        printf("[CYCLE #%4ld] scrub: chk=%u rep=%u unrec=%u | "
               "verify: ok=%ld lost=%ld silent=%ld | "
               "shadow=%u\n",
               g_stats.power_cycles,
               rep.checked, rep.repaired, rep.unrecoverable,
               cyc_ok, cyc_lost, cyc_silent,
               g_shadow_count);
    }
}

/* -----------------------------------------------------------------------
   Final summary
   ----------------------------------------------------------------------- */

static void print_summary(float error_rate) {
    long total_verified = g_stats.verified_ok
                        + g_stats.verified_lost
                        + g_stats.verified_silent;
    double integrity = total_verified > 0
        ? 100.0 * (double)g_stats.verified_ok / (double)total_verified
        : 100.0;

    /* Expected loss: sectors where ALL mirrors were hit by injected faults */
    long injected_total = g_stats.errors_injected;

    printf("\n=== REALISTIC FUZZ SUMMARY ===\n");
    printf("Error injection rate:     %.2f%%\n",   error_rate * 100.0f);
    printf("Power cycles:             %ld\n",       g_stats.power_cycles);
    printf("Format cycles:            %ld\n",       g_stats.format_cycles);
    printf("\n");
    printf("Writes attempted:         %ld\n",       g_stats.writes_attempted);
    printf("  Succeeded (OK):         %ld\n",       g_stats.writes_ok);
    printf("  Succeeded (degraded):   %ld\n",       g_stats.writes_degraded);
    printf("  Failed:                 %ld\n",       g_stats.writes_failed);
    printf("\n");
    printf("Faults injected:          %ld\n",       injected_total);
    printf("Repaired by scrub:        %ld\n",       g_stats.scrub_repaired);
    printf("Unrecoverable:            %ld\n",       g_stats.scrub_unrecoverable);
    printf("\n");
    printf("Verified OK:              %ld\n",       g_stats.verified_ok);
    printf("Verified LOST:            %ld  (expected after unrecoverable faults)\n",
                                                    g_stats.verified_lost);
    printf("Silent corruption:        %ld  <-- must be 0\n",
                                                    g_stats.verified_silent);
    printf("\n");
    printf("Data integrity:           %.6f%%\n",    integrity);

    if (g_stats.verified_silent > 0) {
        printf("\n*** BUG: %ld instance(s) of silent corruption detected ***\n",
               g_stats.verified_silent);
    } else {
        printf("No silent corruption. ZINF never returned OK with wrong data.\n");
    }
}

/* -----------------------------------------------------------------------
   CLI
   ----------------------------------------------------------------------- */

static void usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -n <cycles>  number of power cycles (default: 1000)\n");
    printf("  -e <rate>    fault injection rate 0.0-1.0 (default: 0.01 = 1%%)\n");
    printf("  -b <size>    max write batch size per cycle (default: 100)\n");
    printf("  -r <seed>    RNG seed (default: time())\n");
    printf("  -v           verbose: print each power cycle result\n");
    printf("  -h           show this help\n");
    printf("\nExamples:\n");
    printf("  %s -n 1000 -v              # 1000 power cycles, verbose\n", prog);
    printf("  %s -n 5000 -e 0.05         # 5000 cycles, 5%% fault rate\n", prog);
    printf("  %s -n 500  -e 0.20 -v      # brutal: 20%% fault rate\n", prog);
}

int main(int argc, char *argv[]) {
    int   cycles    = 1000;
    float err_rate  = 0.01f;
    int   max_batch = 100;
    int   verbose   = 0;
    g_rng = (uint32_t)time(NULL);

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-n") == 0 && i+1 < argc) cycles    = atoi(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i+1 < argc) err_rate  = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) max_batch = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i+1 < argc) g_rng     = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-v") == 0) verbose  = 1;
        else if (strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
    }

    if (max_batch < 10) max_batch = 10;

    if (setup() != 0) {
        fprintf(stderr, "Setup failed\n");
        return 1;
    }

    printf("Realistic fuzz: %d power cycles, fault_rate=%.2f%%, max_batch=%d, seed=%u\n",
           cycles, err_rate * 100.0f, max_batch, g_rng);

    /* Randomise how many write batches happen between each power cycle (3–7) */
    int batches_per_cycle = (int)(lcg() % 5u) + 3;
    int batch_count = 0;

    for (int c = 0; c < cycles; c++) {
        do_write_batch(verbose, max_batch, err_rate);
        batch_count++;

        if (batch_count >= batches_per_cycle) {
            do_power_cycle(verbose);
            batch_count = 0;
            batches_per_cycle = (int)(lcg() % 5u) + 3;
        }

        if (!verbose && c % 50 == 0) {
            printf("\r[%4d/%d] writes=%ld injected=%ld repaired=%ld lost=%ld",
                   c, cycles,
                   g_stats.writes_ok + g_stats.writes_degraded,
                   g_stats.errors_injected,
                   g_stats.scrub_repaired,
                   g_stats.scrub_unrecoverable);
            fflush(stdout);
        }
    }

    /* Final power cycle to flush and verify anything not yet checked */
    do_power_cycle(verbose);
    printf("\n");

    print_summary(err_rate);

    if (g_ctx.driver->deinit)
        g_ctx.driver->deinit(g_ctx.driver);

    return g_stats.verified_silent > 0 ? 1 : 0;
}
