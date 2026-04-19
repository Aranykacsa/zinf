#define _GNU_SOURCE
#include <xlsxwriter.h>
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
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* -----------------------------------------------------------------------
   Image geometry
   ----------------------------------------------------------------------- */
#define ADV_IMG_SECTS  65536u
#define ADV_MIRROR_OFF ((ADV_IMG_SECTS - 2u) / (uint32_t)RAID_MIRRORS)
#define LOOPBACK_IMG   "/var/tmp/zinf_advanced_loopback.img"

/* -----------------------------------------------------------------------
   LCG RNG
   ----------------------------------------------------------------------- */
static uint32_t g_rng = 0xDEADBEEFu;
static uint32_t lcg_r(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static uint32_t lcg(void)          { return lcg_r(&g_rng); }

/* -----------------------------------------------------------------------
   Monotonic wall-clock timer (milliseconds since program start).
   Using a fixed reference avoids precision loss when subtracting two
   large epoch-relative doubles.
   ----------------------------------------------------------------------- */
static struct timespec g_tstart;
static void timer_init(void) { clock_gettime(CLOCK_MONOTONIC, &g_tstart); }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec  - g_tstart.tv_sec)  * 1000.0
         + (double)(ts.tv_nsec - g_tstart.tv_nsec) / 1e6;
}

/* -----------------------------------------------------------------------
   Unpack LE float32 from 4 bytes
   ----------------------------------------------------------------------- */
static float unpack_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0]
               | ((uint32_t)p[1] <<  8)
               | ((uint32_t)p[2] << 16)
               | ((uint32_t)p[3] << 24);
    float f; memcpy(&f, &u, 4); return f;
}

/* -----------------------------------------------------------------------
   Excel formatting
   ----------------------------------------------------------------------- */
#define XL_NAVY   0x1F3864u
#define XL_GREEN  0xC6EFCEu
#define XL_RED    0xFFC7CEu
#define XL_AMBER  0xFFE0B2u
#define XL_GRAY   0xF2F2F2u

typedef struct {
    lxw_format *hdr;
    lxw_format *pass;
    lxw_format *fail;
    lxw_format *event;
    lxw_format *plain;
} xl_fmts_t;

static xl_fmts_t make_formats(lxw_workbook *wb) {
    xl_fmts_t f;
    f.hdr   = workbook_add_format(wb);
    format_set_bold(f.hdr);
    format_set_bg_color(f.hdr, XL_NAVY);
    format_set_font_color(f.hdr, LXW_COLOR_WHITE);

    f.pass  = workbook_add_format(wb);
    format_set_bg_color(f.pass, XL_GREEN);

    f.fail  = workbook_add_format(wb);
    format_set_bg_color(f.fail, XL_RED);

    f.event = workbook_add_format(wb);
    format_set_bg_color(f.event, XL_AMBER);
    format_set_bold(f.event);

    f.plain = workbook_add_format(wb);
    format_set_bg_color(f.plain, XL_GRAY);

    return f;
}

static lxw_format *xfmt(xl_fmts_t *f, int ok) { return ok ? f->pass : f->fail; }

static void xlh(lxw_worksheet *ws, int c, const char *s, xl_fmts_t *f) {
    worksheet_write_string(ws, 0, (lxw_col_t)c, s, f->hdr);
}

static void set_col_widths(lxw_worksheet *ws, const double *w, int n) {
    for (int c = 0; c < n; c++)
        worksheet_set_column(ws, (lxw_col_t)c, (lxw_col_t)c, w[c], NULL);
}

/* -----------------------------------------------------------------------
   Context setup
   ----------------------------------------------------------------------- */
static int setup_ram(zinf_ctx_t *ctx) {
    memset(ctx, 0, sizeof *ctx);
    ram_driver_set_capacity(ADV_IMG_SECTS);
    ctx->driver           = &ram_driver;
    ctx->sector_size      = SECTOR_SIZE;
    ctx->mirror_count     = RAID_MIRRORS;
    ctx->metadata_sectors = 2;
    ctx->mirror_offset    = ADV_MIRROR_OFF;
    ctx->log_sector       = 0;
    ctx->raid_offset      = ctx->mirror_offset;
    if (ctx->driver->init(ctx->driver) != DRIVER_OK) return -1;
    if (init_log_sector(ctx)           != STORAGE_OK) return -1;
    return 0;
}

static void ctx_teardown(zinf_ctx_t *ctx) {
    if (ctx->driver && ctx->driver->deinit)
        ctx->driver->deinit(ctx->driver);
}

/* -----------------------------------------------------------------------
   Benchmark accumulator
   ----------------------------------------------------------------------- */
typedef struct {
    int     n;          /* total iterations recorded */
    int     pass_n;
    long    total_ops;
    long    total_silent;
    double  lat_sum;
    double  lat_min;
    double  lat_max;
    double *lats;       /* [n] for percentile computation */
} bench_t;

static bench_t bench_init(int n) {
    bench_t b;
    memset(&b, 0, sizeof b);
    b.n       = n;
    b.lat_min = 1e18;
    b.lats = (double *)malloc((size_t)n * sizeof(double));
    return b;
}

static void bench_record(bench_t *b, int i, double lat_ms,
                         long ops, long silent, int pass) {
    b->lats[i] = lat_ms;
    b->lat_sum += lat_ms;
    if (lat_ms < b->lat_min) b->lat_min = lat_ms;
    if (lat_ms > b->lat_max) b->lat_max = lat_ms;
    b->total_ops    += ops;
    b->total_silent += silent;
    if (pass) b->pass_n++;
}

static int cmp_dbl(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Returns percentile p (0-100) after sorting a copy of lats[0..n-1]. */
static double bench_pct(bench_t *b, double p) {
    if (b->n <= 0) return 0.0;
    double *tmp = (double *)malloc((size_t)b->n * sizeof(double));
    memcpy(tmp, b->lats, (size_t)b->n * sizeof(double));
    qsort(tmp, (size_t)b->n, sizeof(double), cmp_dbl);
    int idx = (int)(p / 100.0 * b->n);
    if (idx >= b->n) idx = b->n - 1;
    double v = tmp[idx];
    free(tmp);
    return v;
}

static void bench_free(bench_t *b) { free(b->lats); b->lats = NULL; }

/* -----------------------------------------------------------------------
   Top-level result (one per test, populated after the benchmark loop)
   ----------------------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *description;
    int         passed;      /* 1 = all iterations passed */
    int         n_iter;
    int         pass_n;
    int         fail_n;
    long        total_ops;
    long        total_silent;
    double      lat_avg_ms;
    double      lat_min_ms;
    double      lat_max_ms;
    double      lat_p95_ms;
    double      lat_p99_ms;
    double      ops_per_sec;
    char        metric[256];
} result_t;

/* =======================================================================
   TEST 1 — Storage wipe
   Per-iteration fuzz: n_pre ∈ [50,300], n_post ∈ [10,80]
   Columns (one row per iteration):
     Iter | Seed | Pre-Writes | Post-Writes | Elapsed(ms) | Ops/sec |
     Wipe-Unrec | Post-OK | Post-Silent | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    long     ok, silent, unrecoverable;
    int      n_pre, n_post;
    uint32_t seed;
} sw_iter_t;

static sw_iter_t run_storage_wipe(uint32_t seed, int n_pre, int n_post) {
    sw_iter_t r = {.pass=1, .seed=seed};
    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint64_t post_lba[80]; float post_temp[80], post_hum[80];
    uint32_t rng = seed;

    /* Pre-wipe writes */
    r.n_pre = n_pre;
    for (int i = 0; i < n_pre; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        float t = (float)(lcg_r(&rng) % 10000u);
        float h = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=t, .humidity=h};
        raid_sensor_values(&ctx, &s, 1);
        r.ops++;
    }

    /* Wipe */
    ram_driver_drop_buffer();

    /* Reinit */
    if (init_log_sector(&ctx) != STORAGE_OK) { r.pass = 0; goto done; }

    /* Scrub on zeroed storage — just prove it doesn't crash */
    {
        zinf_scrub_report_t rep = {0};
        zinf_scrub(&ctx, 2u, (uint64_t)n_pre + 1u, &rep);
        r.unrecoverable = (long)rep.unrecoverable;
    }

    /* Volatile blacklist lost on power-cycle — clear it */
    zinf_clear_bad_sectors(&ctx);

    /* Post-wipe writes */
    r.n_post = n_post;
    for (int i = 0; i < n_post; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        post_lba[i]  = lb + 1u;
        post_temp[i] = (float)(1000u + lcg_r(&rng) % 5000u);
        post_hum[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s   = {.temp=post_temp[i], .humidity=post_hum[i]};
        uint8_t wrc  = raid_sensor_values(&ctx, &s, 1);
        if (wrc != STORAGE_OK && wrc != STORAGE_WARN_DEGRADED) r.pass = 0;
        r.ops++;
    }

    /* Post-wipe readback */
    {
        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < n_post; i++) {
            uint8_t rrc = raid_read(&ctx, post_lba[i], payload);
            r.ops++;
            if (rrc == STORAGE_ERR_UNRECOVERABLE) { r.pass = 0; continue; }
            if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
            float gt = unpack_f32(&payload[0]);
            float gh = unpack_f32(&payload[4]);
            if (fabsf(gt - post_temp[i]) < 0.001f &&
                fabsf(gh - post_hum[i])  < 0.001f)
                r.ok++;
            else {
                r.silent++;
                r.pass = 0;
            }
        }
    }

done:
    ctx_teardown(&ctx);
    return r;
}

static result_t bench_storage_wipe(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "StorageWipe",
        "ram_driver_drop_buffer() wipes all sectors. reinit + scrub must not crash. "
        "Post-wipe writes with fuzzed count [50-300 pre, 10-80 post] must read back exactly.",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Pre-Writes","Post-Writes","Elapsed(ms)","Ops/sec",
        "Wipe-Unrec","Post-OK","Post-Silent","Pass"
    };
    for (int c = 0; c < 10; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 11, 12, 12, 11, 11, 9, 11, 6 };
    set_col_widths(ws, w, 10);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed = lcg();
        int n_pre  = 50  + (int)(lcg() % 251u);
        int n_post = 10  + (int)(lcg() % 71u);

        double t0 = now_ms();
        sw_iter_t r = run_storage_wipe(seed, n_pre, n_post);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? r.ops * 1000.0 / elapsed : 0.0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,        fmt);
        worksheet_write_string(ws, row, 1, seed_s,     fmt);
        worksheet_write_number(ws, row, 2, n_pre,      fmt);
        worksheet_write_number(ws, row, 3, n_post,     fmt);
        worksheet_write_number(ws, row, 4, elapsed,    fmt);
        worksheet_write_number(ws, row, 5, ops_ps,     fmt);
        worksheet_write_number(ws, row, 6, (double)r.unrecoverable, fmt);
        worksheet_write_number(ws, row, 7, (double)r.ok,            fmt);
        worksheet_write_number(ws, row, 8, (double)r.silent,        fmt);
        worksheet_write_string(ws, row, 9, r.pass?"PASS":"FAIL",    fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [1/11] StorageWipe       [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 2 — Degraded write
   Per-iteration fuzz: n_sectors ∈ [5,20], starting LBA randomised
   Columns:
     Iter | Seed | Sectors-Tested | Degraded-Seen | Read-Match | Silent |
     Elapsed(ms) | Ops/sec | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    int      sectors;
    int      degraded_seen;
    long     matched, silent;
    uint32_t seed;
} dw_iter_t;

static dw_iter_t run_degraded_write(uint32_t seed, int n_sectors) {
    dw_iter_t r = {.pass=1, .seed=seed, .sectors=n_sectors};
    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng = seed;
    /* Pad with some initial writes so blacklisted mirror-1 LBAs don't land on sector 1 */
    uint64_t start_lba = 2u + lcg_r(&rng) % 200u;
    {
        /* Pre-fill up to start_lba */
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        while (lb + 1u < start_lba) {
            sensor_t s = {.temp=0.0f, .humidity=0.0f};
            raid_sensor_values(&ctx, &s, 1);
            get_last_sector(&ctx, &lb);
            r.ops++;
        }
    }

    uint64_t lbas[20]; float temps[20], hums[20];
    uint8_t payload[PAYLOAD_SIZE];

    for (int i = 0; i < n_sectors; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 10000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);

        /* Blacklist mirror-1 of this logical sector */
        uint64_t m1_phys = lbas[i] + (uint64_t)ctx.mirror_offset;
        zinf_mark_bad_sector(&ctx, m1_phys);

        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        r.ops++;

        if (wrc == STORAGE_WARN_DEGRADED) r.degraded_seen++;
        else if (wrc != STORAGE_OK)       r.pass = 0;

        /* Read back */
        uint8_t rrc = raid_read(&ctx, lbas[i], payload);
        r.ops++;
        if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            float gt = unpack_f32(&payload[0]);
            float gh = unpack_f32(&payload[4]);
            if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
                r.matched++;
            else { r.silent++; r.pass = 0; }
        }

        /* Clear the blacklist so we don't saturate across sectors */
        zinf_clear_bad_sectors(&ctx);
    }

    ctx_teardown(&ctx);
    if (r.degraded_seen != n_sectors) r.pass = 0;
    if (r.silent > 0)                 r.pass = 0;
    return r;
}

static result_t bench_degraded_write(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "DegradedWrite",
        "Blacklist mirror-1 before each write. Write must return WARN_DEGRADED. "
        "Read via surviving mirror-0 must match exactly. Fuzz: n_sectors ∈ [5,20].",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Sectors-Tested","Degraded-Seen","Read-Match","Silent",
        "Elapsed(ms)","Ops/sec","Pass"
    };
    for (int c = 0; c < 9; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 15, 14, 12, 8, 12, 11, 6 };
    set_col_widths(ws, w, 9);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed     = lcg();
        int      n_sectors = 5 + (int)(lcg() % 16u);

        double t0 = now_ms();
        dw_iter_t r = run_degraded_write(seed, n_sectors);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? r.ops * 1000.0 / elapsed : 0.0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                    fmt);
        worksheet_write_string(ws, row, 1, seed_s,                 fmt);
        worksheet_write_number(ws, row, 2, n_sectors,              fmt);
        worksheet_write_number(ws, row, 3, r.degraded_seen,        fmt);
        worksheet_write_number(ws, row, 4, (double)r.matched,      fmt);
        worksheet_write_number(ws, row, 5, (double)r.silent,       fmt);
        worksheet_write_number(ws, row, 6, elapsed,                fmt);
        worksheet_write_number(ws, row, 7, ops_ps,                 fmt);
        worksheet_write_string(ws, row, 8, r.pass?"PASS":"FAIL",   fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [2/11] DegradedWrite     [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 3 — Blacklist overflow
   Per-iteration fuzz: n_total ∈ [MAX+1, MAX+MAX/2+1], starting_lba random
   Columns:
     Iter | Seed | Starting-LBA | Total-Attempts | Overflows-Caught |
     Final-Count | Max-Allowed | Elapsed(ms) | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    uint64_t starting_lba;
    int      n_total;
    int      overflow_caught;
    int      final_count;
    uint32_t seed;
} bo_iter_t;

static bo_iter_t run_blacklist_overflow(uint32_t seed, uint64_t start_lba, int n_total) {
    bo_iter_t r = {.pass=1, .seed=seed, .starting_lba=start_lba, .n_total=n_total};
    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    for (int i = 0; i < n_total; i++) {
        uint64_t lba     = start_lba + (uint64_t)i;
        uint8_t  rc      = zinf_mark_bad_sector(&ctx, lba);
        int should_full  = (i >= (int)MAX_BAD_SECTORS);

        if (should_full) {
            if (rc == STORAGE_ERR_PARAM) r.overflow_caught++;
            else                         r.pass = 0;
        } else {
            if (rc != STORAGE_OK)        r.pass = 0;
        }
        r.ops++;
    }

    r.final_count = (int)ctx.bad_sector_count;
    if (r.final_count != (int)MAX_BAD_SECTORS)   r.pass = 0;
    if (r.overflow_caught != n_total - (int)MAX_BAD_SECTORS) r.pass = 0;

    ctx_teardown(&ctx);
    return r;
}

static result_t bench_blacklist_overflow(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "BlacklistOverflow",
        "Mark more than MAX_BAD_SECTORS (16) physical sectors. First 16 must succeed. "
        "All subsequent calls must return STORAGE_ERR_PARAM. Count must never exceed 16. "
        "Fuzz: n_total ∈ [17,24], starting_lba random.",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Starting-LBA","Total-Attempts","Overflows-Caught",
        "Final-Count","Max-Allowed","Elapsed(ms)","Pass"
    };
    for (int c = 0; c < 9; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 13, 16, 17, 13, 13, 12, 6 };
    set_col_widths(ws, w, 9);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        uint64_t start_lba = lcg() % 10000u;
        int      n_total   = (int)MAX_BAD_SECTORS + 1 + (int)(lcg() % ((int)MAX_BAD_SECTORS/2 + 1));

        double t0 = now_ms();
        bo_iter_t r = run_blacklist_overflow(seed, start_lba, n_total);
        double elapsed = now_ms() - t0;

        bench_record(&b, i, elapsed, r.ops, 0, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                    fmt);
        worksheet_write_string(ws, row, 1, seed_s,                 fmt);
        worksheet_write_number(ws, row, 2, (double)start_lba,      fmt);
        worksheet_write_number(ws, row, 3, n_total,                fmt);
        worksheet_write_number(ws, row, 4, r.overflow_caught,      fmt);
        worksheet_write_number(ws, row, 5, r.final_count,          fmt);
        worksheet_write_number(ws, row, 6, MAX_BAD_SECTORS,        fmt);
        worksheet_write_number(ws, row, 7, elapsed,                fmt);
        worksheet_write_string(ws, row, 8, r.pass?"PASS":"FAIL",   fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [3/11] BlacklistOverflow [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = 0;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms, res.ops_per_sec);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 4 — Metadata corruption
   Per-iteration fuzz: n_records ∈ [50,300], n_corrupt ∈ [3,10]
   Columns:
     Iter | Seed | Records | Corruptions | Scrub-OK | Verify-OK | Verify-Lost |
     Silent | Elapsed(ms) | Ops/sec | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    int      n_records, n_corrupt;
    int      scrub_ok;
    long     ok, lost, silent;
    uint32_t seed;
} mc_iter_t;

static mc_iter_t run_metadata_corruption(uint32_t seed, int n_records, int n_corrupt) {
    mc_iter_t r = {.pass=1, .seed=seed, .n_records=n_records, .n_corrupt=n_corrupt};
    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng = seed;
    uint64_t lbas[300]; float temps[300], hums[300];
    uint64_t last = 0;

    /* Write n_records — capture last_lba BEFORE corrupting metadata */
    for (int i = 0; i < n_records; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 10000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        raid_sensor_values(&ctx, &s, 1);
        r.ops++;
    }
    get_last_sector(&ctx, &last); /* must be read before sector-0 corruption */

    /* Corrupt n_corrupt bytes in sector 0 metadata */
    for (int i = 0; i < n_corrupt; i++) {
        uint32_t off = 6u + lcg_r(&rng) % (SECTOR_SIZE - 6u); /* skip ZINF magic */
        uint8_t  val = (uint8_t)(lcg_r(&rng) & 0xFFu);
        ram_driver_corrupt(0u, off, val);
    }

    /* Scrub — uses last captured before corruption */
    zinf_clear_bad_sectors(&ctx);
    zinf_scrub_report_t rep = {0};
    uint8_t src = zinf_scrub(&ctx, 2u, last, &rep);
    r.scrub_ok = (src == STORAGE_OK);
    if (!r.scrub_ok) r.pass = 0;

    /* Verify all records — data sectors must be unaffected */
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < n_records; i++) {
        uint8_t rrc = raid_read(&ctx, lbas[i], payload);
        r.ops++;
        if (rrc == STORAGE_ERR_UNRECOVERABLE) { r.lost++; continue; }
        if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
        float gt = unpack_f32(&payload[0]);
        float gh = unpack_f32(&payload[4]);
        if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
            r.ok++;
        else { r.silent++; r.pass = 0; }
    }

    ctx_teardown(&ctx);
    return r;
}

static result_t bench_metadata_corruption(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "MetadataCorruption",
        "Write records, corrupt n_corrupt bytes in sector-0 metadata (skipping magic). "
        "Scrub then read all records back. Data sectors must be unaffected. "
        "Fuzz: n_records ∈ [50,300], n_corrupt ∈ [3,10].",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Records","Corruptions","Scrub-OK","Verify-OK","Verify-Lost",
        "Silent","Elapsed(ms)","Ops/sec","Pass"
    };
    for (int c = 0; c < 11; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 9, 12, 10, 11, 12, 8, 12, 11, 6 };
    set_col_widths(ws, w, 11);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        int      n_records = 50  + (int)(lcg() % 251u);
        int      n_corrupt = 3   + (int)(lcg() % 8u);

        double t0 = now_ms();
        mc_iter_t r = run_metadata_corruption(seed, n_records, n_corrupt);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? r.ops * 1000.0 / elapsed : 0.0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                   fmt);
        worksheet_write_string(ws, row, 1, seed_s,                fmt);
        worksheet_write_number(ws, row, 2, n_records,             fmt);
        worksheet_write_number(ws, row, 3, n_corrupt,             fmt);
        worksheet_write_string(ws, row, 4, r.scrub_ok?"yes":"NO", fmt);
        worksheet_write_number(ws, row, 5, (double)r.ok,          fmt);
        worksheet_write_number(ws, row, 6, (double)r.lost,        fmt);
        worksheet_write_number(ws, row, 7, (double)r.silent,      fmt);
        worksheet_write_number(ws, row, 8, elapsed,               fmt);
        worksheet_write_number(ws, row, 9, ops_ps,                fmt);
        worksheet_write_string(ws, row,10, r.pass?"PASS":"FAIL",  fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [4/11] MetadataCorruption[%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 5 — Version wraparound
   Per-iteration fuzz: patch_ver ∈ {0xFFFC, 0xFFFD, 0xFFFE}, varying
   number of pre-patch writes.
   Columns:
     Iter | Seed | Patch-Ver | Pre-Writes | Post-Writes | Match | Silent |
     Elapsed(ms) | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    uint16_t patch_ver;
    int      n_pre, n_post;
    int      matched;
    long     silent;
    uint32_t seed;
} vw_iter_t;

static vw_iter_t run_version_wrap(uint32_t seed, uint16_t patch_ver,
                                   int n_pre, int n_post) {
    vw_iter_t r = {.pass=1, .seed=seed, .patch_ver=patch_ver,
                   .n_pre=n_pre, .n_post=n_post};
    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng = seed;
    uint64_t lbas[10]; float temps[10], hums[10];
    int total = n_pre + n_post;
    if (total > 10) total = 10;
    n_pre  = total / 2;
    n_post = total - n_pre;

    /* Pre-patch writes */
    for (int i = 0; i < n_pre; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 5000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        raid_sensor_values(&ctx, &s, 1);
        r.ops++;
    }

    /* Patch all 3 copy-slot version fields to patch_ver AND synchronise
       last_sector across all slots.  Without the last_sector sync, all slots
       appear equally new after the patch, but the tie-breaking loop in
       log_get_last_sector always picks slot 2, which may hold a stale pointer
       from an older round-robin write — causing the next write to land on an
       already-occupied LBA (silent corruption). */
    uint64_t cur_last = 0;
    get_last_sector(&ctx, &cur_last);   /* capture before touching metadata */
    uint8_t meta[SECTOR_SIZE];
    read_sector(&ctx, 0, meta);
    uint8_t lo = (uint8_t)(patch_ver & 0xFFu);
    uint8_t hi = (uint8_t)(patch_ver >> 8);
    for (int s = 0; s < 3; s++) {
        int base = 8 + s * 10;  /* META_COPY_SLOT_BASE=8, META_COPY_STRIDE=10 */
        meta[base + 0] = (uint8_t)(cur_last        & 0xFFu);
        meta[base + 1] = (uint8_t)((cur_last >>  8) & 0xFFu);
        meta[base + 2] = (uint8_t)((cur_last >> 16) & 0xFFu);
        meta[base + 3] = (uint8_t)((cur_last >> 24) & 0xFFu);
        meta[base + 4] = (uint8_t)((cur_last >> 32) & 0xFFu);
        meta[base + 5] = (uint8_t)((cur_last >> 40) & 0xFFu);
        meta[base + 6] = (uint8_t)((cur_last >> 48) & 0xFFu);
        meta[base + 7] = (uint8_t)((cur_last >> 56) & 0xFFu);
        meta[base + 8] = lo;
        meta[base + 9] = hi;
    }
    write_sector(&ctx, 0, meta);

    /* Post-patch writes — versions step through 0xFFFF → 0x0000 → ... */
    for (int i = 0; i < n_post; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[n_pre + i]  = lb + 1u;
        temps[n_pre + i] = (float)(10000u + lcg_r(&rng) % 5000u);
        hums[n_pre + i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[n_pre + i], .humidity=hums[n_pre + i]};
        raid_sensor_values(&ctx, &s, 1);
        r.ops++;
    }

    /* Read back all records */
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < total; i++) {
        uint8_t rrc = raid_read(&ctx, lbas[i], payload);
        r.ops++;
        if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
        float gt = unpack_f32(&payload[0]);
        float gh = unpack_f32(&payload[4]);
        if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
            r.matched++;
        else { r.silent++; r.pass = 0; }
    }

    ctx_teardown(&ctx);
    return r;
}

static result_t bench_version_wrap(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "VersionWrap",
        "Patch all 3 copy-slot version fields to a value near 0xFFFF. "
        "Subsequent writes must step correctly through 0xFFFF→0x0000 wraparound. "
        "All records must read back intact. Fuzz: patch_ver ∈ {0xFFFC,0xFFFD,0xFFFE}.",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Patch-Ver","Pre-Writes","Post-Writes","Match","Silent",
        "Elapsed(ms)","Pass"
    };
    for (int c = 0; c < 9; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 12, 11, 12, 8, 8, 12, 6 };
    set_col_widths(ws, w, 9);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        uint16_t patch_ver = (uint16_t)(0xFFFCu + lcg() % 3u);
        int      n_pre     = 1 + (int)(lcg() % 5u);
        int      n_post    = 1 + (int)(lcg() % 5u);

        double t0 = now_ms();
        vw_iter_t r = run_version_wrap(seed, patch_ver, n_pre, n_post);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? r.ops * 1000.0 / elapsed : 0.0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12], ver_s[10];
        snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        snprintf(ver_s,  sizeof ver_s,  "0x%04X",  patch_ver);
        worksheet_write_number(ws, row, 0, i+1,                 fmt);
        worksheet_write_string(ws, row, 1, seed_s,              fmt);
        worksheet_write_string(ws, row, 2, ver_s,               fmt);
        worksheet_write_number(ws, row, 3, r.n_pre,             fmt);
        worksheet_write_number(ws, row, 4, r.n_post,            fmt);
        worksheet_write_number(ws, row, 5, r.matched,           fmt);
        worksheet_write_number(ws, row, 6, (double)r.silent,    fmt);
        worksheet_write_number(ws, row, 7, elapsed,             fmt);
        worksheet_write_string(ws, row, 8, r.pass?"PASS":"FAIL",fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [5/11] VersionWrap       [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }

        (void)ops_ps;
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 6 — Full-range scrub
   Per-iteration fuzz: n_records ∈ [100,500], n_faults ∈ [10,40]
   Columns:
     Iter | Seed | Records | Faults | Repaired | Unrecoverable | Verify-OK |
     Verify-Lost | Silent | Elapsed(ms) | Ops/sec | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    int      n_records, n_faults;
    uint32_t repaired, unrecoverable;
    long     ok, lost, silent;
    uint32_t seed;
} frs_iter_t;

static frs_iter_t run_full_range_scrub(uint32_t seed, int n_records, int n_faults) {
    frs_iter_t r = {.pass=1, .seed=seed, .n_records=n_records, .n_faults=n_faults};
    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng = seed;
    uint64_t lbas[500]; float temps[500], hums[500];

    /* Write n_records */
    for (int i = 0; i < n_records; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 10000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        raid_sensor_values(&ctx, &s, 1);
        r.ops++;
    }

    uint64_t last_lba = 0; get_last_sector(&ctx, &last_lba);

    /* Inject n_faults single-mirror corruptions across early/mid/late zones */
    int faults_per_zone = n_faults / 3;
    int zones[3][2] = {
        { 0,                faults_per_zone     },
        { faults_per_zone,  faults_per_zone * 2 },
        { faults_per_zone * 2, n_faults         }
    };
    uint64_t range = last_lba > 2u ? last_lba - 2u : 1u;

    for (int z = 0; z < 3; z++) {
        for (int j = zones[z][0]; j < zones[z][1]; j++) {
            /* Pick a random LBA within the zone's 1/3 of the written range */
            uint64_t zone_base = 2u + (uint64_t)z * (range / 3u);
            uint64_t zone_len  = range / 3u;
            if (zone_len == 0u) zone_len = 1u;
            uint64_t victim = zone_base + lcg_r(&rng) % zone_len;
            if (victim > last_lba) victim = last_lba;
            uint32_t off = lcg_r(&rng) % SECTOR_SIZE;
            uint8_t  byt = (uint8_t)(lcg_r(&rng) & 0xFFu);
            /* Corrupt only mirror-0 (single-mirror hit → repairable by scrub) */
            ram_driver_corrupt(victim, off, byt);
        }
    }

    /* Scrub */
    zinf_clear_bad_sectors(&ctx);
    zinf_scrub_report_t rep = {0};
    zinf_scrub(&ctx, 2u, last_lba, &rep);
    r.repaired       = rep.repaired;
    r.unrecoverable  = rep.unrecoverable;

    /* Verify all records */
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < n_records; i++) {
        uint8_t rrc = raid_read(&ctx, lbas[i], payload);
        r.ops++;
        if (rrc == STORAGE_ERR_UNRECOVERABLE) { r.lost++; continue; }
        if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
        float gt = unpack_f32(&payload[0]);
        float gh = unpack_f32(&payload[4]);
        if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
            r.ok++;
        else { r.silent++; r.pass = 0; }
    }
    if (r.silent > 0) r.pass = 0;

    ctx_teardown(&ctx);
    return r;
}

static result_t bench_full_range_scrub(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "FullRangeScrub",
        "Write records, inject single-mirror faults across early/mid/late zones, "
        "run full-range scrub, verify all records. silent must always be 0. "
        "Fuzz: n_records ∈ [100,500], n_faults ∈ [10,40].",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Records","Faults","Repaired","Unrecoverable","Verify-OK",
        "Verify-Lost","Silent","Elapsed(ms)","Ops/sec","Pass"
    };
    for (int c = 0; c < 12; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 9, 8, 10, 15, 11, 13, 8, 12, 11, 6 };
    set_col_widths(ws, w, 12);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        int      n_records = 100 + (int)(lcg() % 401u);
        int      n_faults  = 10  + (int)(lcg() % 31u);

        double t0 = now_ms();
        frs_iter_t r = run_full_range_scrub(seed, n_records, n_faults);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? r.ops * 1000.0 / elapsed : 0.0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                    fmt);
        worksheet_write_string(ws, row, 1, seed_s,                 fmt);
        worksheet_write_number(ws, row, 2, n_records,              fmt);
        worksheet_write_number(ws, row, 3, n_faults,               fmt);
        worksheet_write_number(ws, row, 4, (double)r.repaired,     fmt);
        worksheet_write_number(ws, row, 5, (double)r.unrecoverable,fmt);
        worksheet_write_number(ws, row, 6, (double)r.ok,           fmt);
        worksheet_write_number(ws, row, 7, (double)r.lost,         fmt);
        worksheet_write_number(ws, row, 8, (double)r.silent,       fmt);
        worksheet_write_number(ws, row, 9, elapsed,                fmt);
        worksheet_write_number(ws, row,10, ops_ps,                 fmt);
        worksheet_write_string(ws, row,11, r.pass?"PASS":"FAIL",   fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [6/11] FullRangeScrub    [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 7 — Loopback integrity (linux driver)
   Per-iteration fuzz: n_records ∈ [50,200]
   The image file is created once and reused across all iterations.
   Columns:
     Iter | Seed | Records | Matched | Elapsed(ms) | Throughput(KB/s) | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    int      n_records;
    int      matched;
    uint32_t seed;
} lb_iter_t;

static lb_iter_t run_loopback(zinf_ctx_t *ctx, uint32_t seed, int n_records) {
    lb_iter_t r = {.pass=1, .seed=seed, .n_records=n_records};

    uint32_t rng = seed;
    uint64_t lbas[200]; float temps[200], hums[200];

    zinf_clear_bad_sectors(ctx);
    if (init_log_sector(ctx) != STORAGE_OK) { r.pass = 0; return r; }

    for (int i = 0; i < n_records; i++) {
        uint64_t lb = 0; get_last_sector(ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 10000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        uint8_t wrc = raid_sensor_values(ctx, &s, 1);
        if (wrc != STORAGE_OK && wrc != STORAGE_WARN_DEGRADED) r.pass = 0;
        r.ops++;
    }

    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < n_records; i++) {
        uint8_t rrc = raid_read(ctx, lbas[i], payload);
        r.ops++;
        if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) { r.pass = 0; continue; }
        float gt = unpack_f32(&payload[0]);
        float gh = unpack_f32(&payload[4]);
        if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
            r.matched++;
        else r.pass = 0;
    }

    return r;
}

static result_t bench_loopback(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "Loopback",
        "End-to-end byte integrity via linux block-device driver on a real image file. "
        "Write records with known values, read back, compare exactly. "
        "Fuzz: n_records ∈ [50,200]. Image reused across iterations.",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Records","Matched","Elapsed(ms)","Throughput(KB/s)","Pass"
    };
    for (int c = 0; c < 7; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 9, 9, 12, 17, 6 };
    set_col_widths(ws, w, 7);
    worksheet_freeze_panes(ws, 1, 0);

    /* Create the image file once */
    int fd = open(LOOPBACK_IMG, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)ADV_IMG_SECTS * SECTOR_SIZE) != 0) {
        if (fd >= 0) close(fd);
        res.passed = 0;
        snprintf(res.metric, sizeof res.metric, "cannot create %s", LOOPBACK_IMG);
        return res;
    }
    close(fd);

    zinf_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.driver           = &linux_driver;
    ctx.sector_size      = SECTOR_SIZE;
    ctx.mirror_count     = RAID_MIRRORS;
    ctx.metadata_sectors = 2;
    ctx.mirror_offset    = ADV_MIRROR_OFF;
    ctx.log_sector       = 0;
    ctx.raid_offset      = ctx.mirror_offset;

    linux_driver_set_path(LOOPBACK_IMG);
    if (ctx.driver->init(ctx.driver) != DRIVER_OK) {
        unlink(LOOPBACK_IMG);
        res.passed = 0;
        snprintf(res.metric, sizeof res.metric, "linux_driver init failed");
        return res;
    }
    if (init_log_sector(&ctx) != STORAGE_OK) {
        ctx_teardown(&ctx); unlink(LOOPBACK_IMG);
        res.passed = 0;
        snprintf(res.metric, sizeof res.metric, "init_log_sector failed");
        return res;
    }

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        int      n_records = 50 + (int)(lcg() % 151u);

        double t0 = now_ms();
        lb_iter_t r = run_loopback(&ctx, seed, n_records);
        double elapsed = now_ms() - t0;
        /* Throughput: (reads+writes) × 512 bytes / elapsed_ms → KB/s */
        double kbps = elapsed > 0.0
            ? (double)r.ops * SECTOR_SIZE / elapsed  /* bytes/ms = KB/s */
            : 0.0;

        bench_record(&b, i, elapsed, r.ops, 0, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                fmt);
        worksheet_write_string(ws, row, 1, seed_s,             fmt);
        worksheet_write_number(ws, row, 2, n_records,          fmt);
        worksheet_write_number(ws, row, 3, r.matched,          fmt);
        worksheet_write_number(ws, row, 4, elapsed,            fmt);
        worksheet_write_number(ws, row, 5, kbps,               fmt);
        worksheet_write_string(ws, row, 6, r.pass?"PASS":"FAIL",fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [7/11] Loopback          [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    ctx_teardown(&ctx);
    unlink(LOOPBACK_IMG);

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = 0;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    double avg_kbps  = res.ops_per_sec * SECTOR_SIZE / 1024.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms throughput=%.0f KB/s",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms, avg_kbps);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 8 — Repair Cycle (10 rounds of corrupt → scrub → verify)
   Per-iteration fuzz: n_records ∈ [100,400], fault_rate ∈ [1%,5%]
   10 rounds per iteration: inject faults on random mirrors → power-cycle
   scrub → verify all records. Cumulative damage tracked across rounds.
   Excel sheet: one row per round (10 rows × 1000 iters = 10,000 rows)
   Columns:
     Iter | Seed | Round | N-Records | Fault-Rate(%) | Faults |
     Repaired | New-Unrec | Cum-Unrec | Verify-OK | Verify-Lost | Silent | Pass
   ======================================================================= */

typedef struct {
    int      pass;           /* 1 if all 10 rounds had silent=0 */
    long     total_ops;
    int      n_records;
    float    fault_rate;
    int      faults[10];
    int      repaired[10];
    int      new_unrec[10];
    int      cum_unrec[10];
    int      ok[10];
    int      lost[10];
    int      silent[10];
    uint32_t seed;
} rc_iter_t;

static rc_iter_t run_repair_cycle(uint32_t seed, int n_records, float fault_rate) {
    rc_iter_t r;
    memset(&r, 0, sizeof r);
    r.seed       = seed;
    r.n_records  = n_records;
    r.fault_rate = fault_rate;
    r.pass       = 1;

    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng = seed;

    /* Shadow buffer — max 400 entries */
    uint64_t shadow_lba[400];
    float    shadow_temp[400], shadow_hum[400];
    int      shadow_lost[400];
    memset(shadow_lost, 0, sizeof shadow_lost);

    /* Initial write batch */
    for (int i = 0; i < n_records; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        shadow_lba[i]  = lb + 1u;
        shadow_temp[i] = (float)(lcg_r(&rng) % 10000u);
        shadow_hum[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=shadow_temp[i], .humidity=shadow_hum[i]};
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        if (wrc != STORAGE_OK && wrc != STORAGE_WARN_DEGRADED) r.pass = 0;
        r.total_ops++;
    }

    uint64_t last_lba = 0; get_last_sector(&ctx, &last_lba);
    int cum_unrec = 0;

    for (int round = 0; round < 10; round++) {
        /* ---- Fault injection ---- */
        int n_faults = (int)((float)n_records * fault_rate);
        if (n_faults < 1) n_faults = 1;
        r.faults[round] = n_faults;

        for (int fi = 0; fi < n_faults; fi++) {
            int      rec_idx = (int)(lcg_r(&rng) % (uint32_t)n_records);
            uint64_t log_lba = shadow_lba[rec_idx];
            int      mirror  = (int)(lcg_r(&rng) & 1u);
            uint64_t phys    = log_lba + (uint64_t)mirror * (uint64_t)ADV_MIRROR_OFF;
            uint32_t off     = lcg_r(&rng) % (uint32_t)SECTOR_SIZE;
            uint8_t  val     = (uint8_t)(lcg_r(&rng) & 0xFFu);
            ram_driver_corrupt(phys, off, val);
        }

        /* ---- Power-cycle scrub ---- */
        zinf_clear_bad_sectors(&ctx);
        zinf_scrub_report_t rep = {0};
        zinf_scrub(&ctx, 2u, last_lba, &rep);
        r.repaired[round] = (int)rep.repaired;

        /* ---- Verify all records ---- */
        int newly_lost = 0, ok = 0, lost_total = 0, silent = 0;
        uint8_t payload[PAYLOAD_SIZE];

        for (int i = 0; i < n_records; i++) {
            if (shadow_lost[i]) { lost_total++; continue; }
            uint8_t rc = raid_read(&ctx, shadow_lba[i], payload);
            r.total_ops++;
            if (rc == STORAGE_ERR_UNRECOVERABLE) {
                shadow_lost[i] = 1;
                newly_lost++;
                lost_total++;
            } else if (rc == STORAGE_OK || rc == STORAGE_WARN_DEGRADED) {
                float gt = unpack_f32(&payload[0]);
                float gh = unpack_f32(&payload[4]);
                if (fabsf(gt - shadow_temp[i]) < 0.001f &&
                    fabsf(gh - shadow_hum[i])  < 0.001f)
                    ok++;
                else { silent++; r.pass = 0; }
            }
        }

        cum_unrec         += newly_lost;
        r.new_unrec[round] = newly_lost;
        r.cum_unrec[round] = cum_unrec;
        r.ok[round]        = ok;
        r.lost[round]      = lost_total;
        r.silent[round]    = silent;
    }

    ctx_teardown(&ctx);
    return r;
}

static result_t bench_repair_cycle(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "RepairCycle",
        "Write records once, then 10 rounds of: inject realistic faults (1-5%) on random "
        "mirrors → power-cycle scrub → verify all records. Cumulative damage tracked. "
        "silent must always be 0. Fuzz: n_records ∈ [100,400], fault_rate ∈ [1%,5%].",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Round","N-Records","Fault-Rate(%)","Faults",
        "Repaired","New-Unrec","Cum-Unrec","Verify-OK","Verify-Lost","Silent","Pass"
    };
    for (int c = 0; c < 13; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 7, 11, 14, 8, 10, 11, 11, 11, 13, 8, 6 };
    set_col_widths(ws, w, 13);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed       = lcg();
        int      n_records  = 100 + (int)(lcg() % 301u);
        float    fault_rate = 0.01f + (float)(lcg() % 5u) * 0.01f;  /* 1–5% */

        double t0 = now_ms();
        rc_iter_t r = run_repair_cycle(seed, n_records, fault_rate);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? (double)r.total_ops * 1000.0 / elapsed : 0.0;

        long iter_silent = 0;
        for (int rnd = 0; rnd < 10; rnd++) iter_silent += r.silent[rnd];

        bench_record(&b, i, elapsed, r.total_ops, iter_silent, r.pass);

        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);

        /* One row per round */
        for (int rnd = 0; rnd < 10; rnd++) {
            lxw_row_t   row     = (lxw_row_t)(i * 10 + rnd + 1);
            int         rnd_ok  = (r.silent[rnd] == 0);
            lxw_format *fmt     = xfmt(f, rnd_ok);

            worksheet_write_number(ws, row,  0, i+1,                           fmt);
            worksheet_write_string(ws, row,  1, seed_s,                        fmt);
            worksheet_write_number(ws, row,  2, rnd+1,                         fmt);
            worksheet_write_number(ws, row,  3, n_records,                     fmt);
            worksheet_write_number(ws, row,  4, (double)(fault_rate * 100.0f), fmt);
            worksheet_write_number(ws, row,  5, r.faults[rnd],                 fmt);
            worksheet_write_number(ws, row,  6, r.repaired[rnd],               fmt);
            worksheet_write_number(ws, row,  7, r.new_unrec[rnd],              fmt);
            worksheet_write_number(ws, row,  8, r.cum_unrec[rnd],              fmt);
            worksheet_write_number(ws, row,  9, r.ok[rnd],                     fmt);
            worksheet_write_number(ws, row, 10, r.lost[rnd],                   fmt);
            worksheet_write_number(ws, row, 11, r.silent[rnd],                 fmt);
            worksheet_write_string(ws, row, 12, rnd_ok ? "PASS" : "FAIL",      fmt);
        }

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [8/11] RepairCycle       [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }

        (void)ops_ps;  /* reported via metric string */
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 9 — Message Log Interleave
   Interleave raid_sensor_values and save_msg. n_msgs > 471 triggers overflow
   to log_sector+1. Records must not be corrupted after overflow.
   Fuzz: n_records ∈ [10,50], n_msgs ∈ [200,600]
   Columns:
     Iter | Seed | N-Records | N-Msgs | Overflow-Triggered |
     Records-OK | Records-Silent | Msgs-OK | Elapsed(ms) | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    int      n_records, n_msgs;
    int      overflow_triggered;
    long     records_ok, records_silent;
    long     msgs_ok;
    uint32_t seed;
} mli_iter_t;

static mli_iter_t run_msg_log_interleave(uint32_t seed, int n_records, int n_msgs) {
    mli_iter_t r;
    memset(&r, 0, sizeof r);
    r.pass = 1; r.seed = seed;
    r.n_records = n_records; r.n_msgs = n_msgs;

    zinf_ctx_t ctx;
    if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng   = seed;
    uint64_t lbas[50]; float temps[50], hums[50];
    uint8_t  msg_vals[600];
    int      msg_count = 0;

    /* Interleave: drain msgs proportionally before each sensor write */
    for (int i = 0; i < n_records; i++) {
        int target = (i + 1) * n_msgs / n_records;
        while (msg_count < target && msg_count < n_msgs) {
            msg_vals[msg_count] = (uint8_t)(lcg_r(&rng) & 0xFFu);
            save_msg(&ctx, &msg_vals[msg_count]);
            msg_count++;
        }
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 10000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        if (wrc != STORAGE_OK && wrc != STORAGE_WARN_DEGRADED) r.pass = 0;
        r.ops++;
    }
    /* Drain any remaining msgs */
    while (msg_count < n_msgs) {
        msg_vals[msg_count] = (uint8_t)(lcg_r(&rng) & 0xFFu);
        save_msg(&ctx, &msg_vals[msg_count]);
        msg_count++;
    }
    r.overflow_triggered = (msg_count > (int)MSG_LOG_CAP_S0);

    /* Verify sensor records */
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < n_records; i++) {
        uint8_t rrc = raid_read(&ctx, lbas[i], payload);
        r.ops++;
        if (rrc == STORAGE_ERR_UNRECOVERABLE) { r.pass = 0; continue; }
        if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
        float gt = unpack_f32(&payload[0]);
        float gh = unpack_f32(&payload[4]);
        if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
            r.records_ok++;
        else { r.records_silent++; r.pass = 0; }
    }

    /* Verify message log byte content via raw sector reads */
    {
        int expected = msg_count < (int)MSG_LOG_TOTAL_CAP
                       ? msg_count : (int)MSG_LOG_TOTAL_CAP;
        uint8_t meta0[SECTOR_SIZE];
        read_sector(&ctx, ctx.log_sector, meta0);

        int s0_bytes = expected < (int)MSG_LOG_CAP_S0
                       ? expected : (int)MSG_LOG_CAP_S0;
        for (int m = 0; m < s0_bytes; m++) {
            if (meta0[META_HDR_SIZE + m] == msg_vals[m])
                r.msgs_ok++;
            else
                r.pass = 0;
        }
        if (expected > (int)MSG_LOG_CAP_S0) {
            uint8_t meta1[SECTOR_SIZE];
            read_sector(&ctx, ctx.log_sector + 1u, meta1);
            int s1_bytes = expected - (int)MSG_LOG_CAP_S0;
            for (int m = 0; m < s1_bytes; m++) {
                if (meta1[m] == msg_vals[(int)MSG_LOG_CAP_S0 + m])
                    r.msgs_ok++;
                else
                    r.pass = 0;
            }
        }
    }

    if (r.records_silent > 0) r.pass = 0;
    ctx_teardown(&ctx);
    return r;
}

static result_t bench_msg_log_interleave(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "MsgLogInterleave",
        "Interleave raid_sensor_values and save_msg. n_msgs>471 triggers overflow to "
        "log_sector+1. Records undamaged; message bytes match exactly. "
        "Fuzz: n_records ∈ [10,50], n_msgs ∈ [200,600].",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","N-Records","N-Msgs","Overflow-Triggered",
        "Records-OK","Records-Silent","Msgs-OK","Elapsed(ms)","Pass"
    };
    for (int c = 0; c < 10; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 10, 8, 19, 12, 15, 10, 12, 6 };
    set_col_widths(ws, w, 10);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        int      n_records = 10 + (int)(lcg() % 41u);
        int      n_msgs    = 200 + (int)(lcg() % 401u);

        double t0 = now_ms();
        mli_iter_t r = run_msg_log_interleave(seed, n_records, n_msgs);
        double elapsed = now_ms() - t0;

        bench_record(&b, i, elapsed, r.ops, r.records_silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                              fmt);
        worksheet_write_string(ws, row, 1, seed_s,                           fmt);
        worksheet_write_number(ws, row, 2, n_records,                        fmt);
        worksheet_write_number(ws, row, 3, n_msgs,                           fmt);
        worksheet_write_string(ws, row, 4, r.overflow_triggered ? "yes":"no",fmt);
        worksheet_write_number(ws, row, 5, (double)r.records_ok,             fmt);
        worksheet_write_number(ws, row, 6, (double)r.records_silent,         fmt);
        worksheet_write_number(ws, row, 7, (double)r.msgs_ok,                fmt);
        worksheet_write_number(ws, row, 8, elapsed,                          fmt);
        worksheet_write_string(ws, row, 9, r.pass ? "PASS" : "FAIL",         fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [9/11] MsgLogInterleave [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 10 — Disk Full
   Write until STORAGE_ERR_FULL. Verify exact boundary; verify pre-full
   records intact; verify 5 post-error writes all return STORAGE_ERR_FULL.
   Uses a 1024-sector image so the test runs in ~500 writes per iteration.
   Fuzz: n_prefill ∈ [10,50]
   Columns:
     Iter | Seed | Pre-Fill | Fills-Until-Full | Post-Err-Rejected |
     Total-Writes | Capacity | Records-OK | Silent | Pass
   ======================================================================= */

#define DISKFULL_SECTS 1024u
#define DISKFULL_MO    ((DISKFULL_SECTS - 2u) / (uint32_t)RAID_MIRRORS)  /* = 511 */

static int setup_diskfull(zinf_ctx_t *ctx) {
    memset(ctx, 0, sizeof *ctx);
    ram_driver_set_capacity(DISKFULL_SECTS);
    ctx->driver           = &ram_driver;
    ctx->sector_size      = SECTOR_SIZE;
    ctx->mirror_count     = RAID_MIRRORS;
    ctx->metadata_sectors = 2;
    ctx->mirror_offset    = DISKFULL_MO;
    ctx->log_sector       = 0;
    ctx->raid_offset      = ctx->mirror_offset;
    if (ctx->driver->init(ctx->driver) != DRIVER_OK) return -1;
    if (init_log_sector(ctx)           != STORAGE_OK) return -1;
    return 0;
}

typedef struct {
    int      pass;
    long     ops;
    int      n_prefill;
    int      fills_until_full;
    int      post_err_rejected;
    long     records_ok, silent;
    uint32_t seed;
} df_iter_t;

static df_iter_t run_disk_full(uint32_t seed, int n_prefill) {
    df_iter_t r;
    memset(&r, 0, sizeof r);
    r.pass = 1; r.seed = seed; r.n_prefill = n_prefill;

    zinf_ctx_t ctx;
    if (setup_diskfull(&ctx) != 0) { r.pass = 0; return r; }

    uint32_t rng = seed;
    uint64_t lbas[50]; float temps[50], hums[50];

    /* Prefill n_prefill records */
    for (int i = 0; i < n_prefill; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        lbas[i]  = lb + 1u;
        temps[i] = (float)(lcg_r(&rng) % 10000u);
        hums[i]  = (float)(lcg_r(&rng) % 100u);
        sensor_t s = {.temp=temps[i], .humidity=hums[i]};
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        if (wrc != STORAGE_OK && wrc != STORAGE_WARN_DEGRADED) { r.pass = 0; goto done; }
        r.ops++;
    }

    /* Fill until STORAGE_ERR_FULL */
    {
        sensor_t s = {.temp=0.0f, .humidity=0.0f};
        for (;;) {
            uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
            r.ops++;
            if (wrc == STORAGE_ERR_FULL) break;
            if (wrc != STORAGE_OK && wrc != STORAGE_WARN_DEGRADED) { r.pass = 0; goto done; }
            r.fills_until_full++;
        }
    }

    /* 5 more writes after FULL — all must return STORAGE_ERR_FULL */
    {
        sensor_t s = {.temp=1.0f, .humidity=1.0f};
        for (int i = 0; i < 5; i++) {
            uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
            r.ops++;
            if (wrc == STORAGE_ERR_FULL) r.post_err_rejected++;
            else                          r.pass = 0;
        }
    }

    /* Verify last_sector is within the valid mirror-0 range */
    {
        uint64_t last = 0;
        get_last_sector(&ctx, &last);
        uint64_t m0_ceiling = (uint64_t)ctx.metadata_sectors + ctx.mirror_offset;
        if (last >= m0_ceiling) r.pass = 0;
    }

    /* Verify total writes == capacity (mirror_offset) */
    {
        int total   = n_prefill + r.fills_until_full;
        int capacity = (int)ctx.mirror_offset;  /* = DISKFULL_MO = 511 */
        if (total != capacity) r.pass = 0;
    }

    /* Verify prefill records are intact */
    {
        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < n_prefill; i++) {
            uint8_t rrc = raid_read(&ctx, lbas[i], payload);
            r.ops++;
            if (rrc == STORAGE_ERR_UNRECOVERABLE) { r.pass = 0; continue; }
            if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
            float gt = unpack_f32(&payload[0]);
            float gh = unpack_f32(&payload[4]);
            if (fabsf(gt - temps[i]) < 0.001f && fabsf(gh - hums[i]) < 0.001f)
                r.records_ok++;
            else { r.silent++; r.pass = 0; }
        }
    }

done:
    ctx_teardown(&ctx);
    /* Restore capacity for subsequent tests that use setup_ram */
    ram_driver_set_capacity(ADV_IMG_SECTS);
    return r;
}

static result_t bench_disk_full(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "DiskFull",
        "Write 1024-sector image to capacity. STORAGE_ERR_FULL must fire exactly at "
        "mirror_offset writes. 5 post-error writes must all return STORAGE_ERR_FULL. "
        "Pre-fill records remain intact. Fuzz: n_prefill ∈ [10,50].",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","Pre-Fill","Fills-Until-Full","Post-Err-Rejected",
        "Total-Writes","Capacity","Records-OK","Silent","Pass"
    };
    for (int c = 0; c < 10; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 9, 17, 18, 13, 10, 12, 8, 6 };
    set_col_widths(ws, w, 10);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed     = lcg();
        int      n_prefill = 10 + (int)(lcg() % 41u);

        double t0 = now_ms();
        df_iter_t r = run_disk_full(seed, n_prefill);
        double elapsed = now_ms() - t0;
        double ops_ps  = elapsed > 0.0 ? r.ops * 1000.0 / elapsed : 0.0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                            fmt);
        worksheet_write_string(ws, row, 1, seed_s,                         fmt);
        worksheet_write_number(ws, row, 2, n_prefill,                      fmt);
        worksheet_write_number(ws, row, 3, r.fills_until_full,             fmt);
        worksheet_write_number(ws, row, 4, r.post_err_rejected,            fmt);
        worksheet_write_number(ws, row, 5, n_prefill + r.fills_until_full, fmt);
        worksheet_write_number(ws, row, 6, (double)DISKFULL_MO,            fmt);
        worksheet_write_number(ws, row, 7, (double)r.records_ok,           fmt);
        worksheet_write_number(ws, row, 8, (double)r.silent,               fmt);
        worksheet_write_string(ws, row, 9, r.pass ? "PASS" : "FAIL",       fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [10/11] DiskFull        [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }

        (void)ops_ps;
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   TEST 11 — Double Fault
   Two sub-scenarios per iteration using the same initial write batch:
   A: corrupt mirror-0 → recover → new fault on mirror-1 → scrub → readable
   B: corrupt mirror-0 → recover → corrupt mirror-0 again → corrupt mirror-1
      → scrub → STORAGE_ERR_UNRECOVERABLE, all other records still intact
   PASS = scen_a_pass && scen_b_pass && silent == 0.
   Fuzz: n_records ∈ [20,100], sector_x = random record index
   Columns:
     Iter | Seed | N-Records | Sector-X | ScenA-Pass | ScenB-Pass | Silent | Pass
   ======================================================================= */

typedef struct {
    int      pass;
    long     ops;
    int      n_records;
    int      sector_x;
    int      scen_a_pass;
    int      scen_b_pass;
    long     silent;
    uint32_t seed;
} dft_iter_t;

/* Write n_records into ctx, filling shadow_lba/temp/hum arrays. */
static void dft_write_records(zinf_ctx_t *ctx, uint32_t *rng, int n_records,
                               uint64_t *shadow_lba, float *shadow_temp,
                               float *shadow_hum, long *ops) {
    for (int i = 0; i < n_records; i++) {
        uint64_t lb = 0; get_last_sector(ctx, &lb);
        shadow_lba[i]  = lb + 1u;
        shadow_temp[i] = (float)(lcg_r(rng) % 10000u);
        shadow_hum[i]  = (float)(lcg_r(rng) % 100u);
        sensor_t s = {.temp=shadow_temp[i], .humidity=shadow_hum[i]};
        raid_sensor_values(ctx, &s, 1);
        (*ops)++;
    }
}

static dft_iter_t run_double_fault(uint32_t seed, int n_records, int sector_x) {
    dft_iter_t r;
    memset(&r, 0, sizeof r);
    r.pass = 1; r.seed = seed;
    r.n_records = n_records; r.sector_x = sector_x;

    uint64_t shadow_lba[100]; float shadow_temp[100], shadow_hum[100];

    /* ---- Sub-scenario A ---- */
    {
        zinf_ctx_t ctx;
        if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

        uint32_t rng = seed;
        dft_write_records(&ctx, &rng, n_records,
                          shadow_lba, shadow_temp, shadow_hum, &r.ops);

        uint64_t lba_x  = shadow_lba[sector_x];
        uint64_t m0_phys = lba_x;
        uint64_t m1_phys = lba_x + (uint64_t)ADV_MIRROR_OFF;

        /* Step 1: corrupt mirror 0 */
        ram_driver_corrupt(m0_phys, 2u, 0xDEu);

        /* Step 2: recover sector — repairs mirror 0 from mirror 1 */
        uint8_t rec = zinf_recover_sector(&ctx, lba_x);
        if (rec != STORAGE_OK) { r.scen_a_pass = 0; r.pass = 0; goto done_a; }

        /* Step 3: new fault on mirror 1 (simulates power-loss after repair) */
        ram_driver_corrupt(m1_phys, 2u, 0xBEu);

        /* Step 4: scrub single sector — should repair mirror 1 from mirror 0 */
        zinf_clear_bad_sectors(&ctx);
        zinf_scrub_report_t rep = {0};
        zinf_scrub(&ctx, lba_x, lba_x, &rep);

        /* Step 5: read back — must succeed with correct data */
        {
            uint8_t payload[PAYLOAD_SIZE];
            uint8_t rrc = raid_read(&ctx, lba_x, payload);
            r.ops++;
            if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
                float gt = unpack_f32(&payload[0]);
                float gh = unpack_f32(&payload[4]);
                if (fabsf(gt - shadow_temp[sector_x]) < 0.001f &&
                    fabsf(gh - shadow_hum[sector_x])  < 0.001f)
                    r.scen_a_pass = 1;
                else { r.silent++; r.pass = 0; }
            } else {
                r.scen_a_pass = 0; r.pass = 0;
            }
        }

done_a:
        ctx_teardown(&ctx);
    }

    /* ---- Sub-scenario B ---- */
    {
        zinf_ctx_t ctx;
        if (setup_ram(&ctx) != 0) { r.pass = 0; return r; }

        uint32_t rng = seed;
        dft_write_records(&ctx, &rng, n_records,
                          shadow_lba, shadow_temp, shadow_hum, &r.ops);

        uint64_t lba_x   = shadow_lba[sector_x];
        uint64_t m0_phys  = lba_x;
        uint64_t m1_phys  = lba_x + (uint64_t)ADV_MIRROR_OFF;

        /* Step 1: corrupt mirror 0 (pattern A) */
        ram_driver_corrupt(m0_phys, 2u, 0xDEu);

        /* Step 2: recover — mirrors mirror 1 into mirror 0 */
        zinf_recover_sector(&ctx, lba_x);

        /* Step 3: corrupt mirror 0 again (pattern B — different value) */
        ram_driver_corrupt(m0_phys, 3u, 0xADu);

        /* Step 4: corrupt mirror 1 (pattern C) */
        ram_driver_corrupt(m1_phys, 4u, 0xBEu);

        /* Step 5: scrub — both mirrors bad → unrecoverable, LBAs blacklisted */
        zinf_clear_bad_sectors(&ctx);
        zinf_scrub_report_t rep = {0};
        zinf_scrub(&ctx, lba_x, lba_x, &rep);
        /* rep.unrecoverable should be 1 */

        /* Step 6: read sector_x → must be STORAGE_ERR_UNRECOVERABLE */
        {
            uint8_t payload[PAYLOAD_SIZE];
            uint8_t rrc = raid_read(&ctx, lba_x, payload);
            r.ops++;
            if (rrc == STORAGE_ERR_UNRECOVERABLE) {
                r.scen_b_pass = 1;
            } else if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
                /* Got data back — check if it's wrong (silent corruption) */
                float gt = unpack_f32(&payload[0]);
                float gh = unpack_f32(&payload[4]);
                if (fabsf(gt - shadow_temp[sector_x]) > 0.001f ||
                    fabsf(gh - shadow_hum[sector_x])  > 0.001f)
                    r.silent++;
                /* Whether correct or wrong, sub-B expected UNRECOVERABLE */
                r.scen_b_pass = 0; r.pass = 0;
            } else {
                r.scen_b_pass = 0; r.pass = 0;
            }
        }

        /* Step 7: read all other records — must all be intact */
        {
            uint8_t payload[PAYLOAD_SIZE];
            for (int i = 0; i < n_records; i++) {
                if (i == sector_x) continue;
                uint8_t rrc = raid_read(&ctx, shadow_lba[i], payload);
                r.ops++;
                if (rrc == STORAGE_ERR_UNRECOVERABLE) { r.pass = 0; continue; }
                if (rrc != STORAGE_OK && rrc != STORAGE_WARN_DEGRADED) continue;
                float gt = unpack_f32(&payload[0]);
                float gh = unpack_f32(&payload[4]);
                if (fabsf(gt - shadow_temp[i]) > 0.001f ||
                    fabsf(gh - shadow_hum[i])  > 0.001f) {
                    r.silent++; r.pass = 0;
                }
            }
        }

        ctx_teardown(&ctx);
    }

    if (!r.scen_a_pass || !r.scen_b_pass || r.silent > 0) r.pass = 0;
    return r;
}

static result_t bench_double_fault(lxw_worksheet *ws, xl_fmts_t *f, int n_iters) {
    result_t res = {
        "DoubleFault",
        "Sub-A: recover → new fault on mirror-1 → scrub → sector still readable. "
        "Sub-B: recover → double-corrupt both mirrors → scrub → STORAGE_ERR_UNRECOVERABLE, "
        "all other records intact. silent must always be 0. "
        "Fuzz: n_records ∈ [20,100], sector_x random.",
        1, n_iters, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ""
    };

    const char *hdrs[] = {
        "Iter","Seed","N-Records","Sector-X",
        "ScenA-Pass","ScenB-Pass","Silent","Pass"
    };
    for (int c = 0; c < 8; c++) xlh(ws, c, hdrs[c], f);
    const double w[] = { 6, 12, 11, 10, 12, 12, 8, 6 };
    set_col_widths(ws, w, 8);
    worksheet_freeze_panes(ws, 1, 0);

    bench_t b = bench_init(n_iters);

    for (int i = 0; i < n_iters; i++) {
        uint32_t seed      = lcg();
        int      n_records = 20 + (int)(lcg() % 81u);
        int      sector_x  = (int)(lcg() % (uint32_t)n_records);

        double t0 = now_ms();
        dft_iter_t r = run_double_fault(seed, n_records, sector_x);
        double elapsed = now_ms() - t0;

        bench_record(&b, i, elapsed, r.ops, r.silent, r.pass);

        lxw_row_t row = (lxw_row_t)(i + 1);
        lxw_format *fmt = xfmt(f, r.pass);
        char seed_s[12]; snprintf(seed_s, sizeof seed_s, "0x%08X", seed);
        worksheet_write_number(ws, row, 0, i+1,                              fmt);
        worksheet_write_string(ws, row, 1, seed_s,                           fmt);
        worksheet_write_number(ws, row, 2, n_records,                        fmt);
        worksheet_write_number(ws, row, 3, sector_x,                         fmt);
        worksheet_write_string(ws, row, 4, r.scen_a_pass ? "PASS" : "FAIL",  fmt);
        worksheet_write_string(ws, row, 5, r.scen_b_pass ? "PASS" : "FAIL",  fmt);
        worksheet_write_number(ws, row, 6, (double)r.silent,                 fmt);
        worksheet_write_string(ws, row, 7, r.pass ? "PASS" : "FAIL",         fmt);

        if (!r.pass) res.passed = 0;

        if ((i+1) % 100 == 0 || i == n_iters-1) {
            printf("\r  [11/11] DoubleFault     [%4d/%d] pass=%d fail=%d",
                   i+1, n_iters, b.pass_n, i+1-b.pass_n);
            fflush(stdout);
        }
    }

    res.pass_n       = b.pass_n;
    res.fail_n       = n_iters - b.pass_n;
    res.total_ops    = b.total_ops;
    res.total_silent = b.total_silent;
    res.lat_avg_ms   = n_iters > 0 ? b.lat_sum / n_iters : 0.0;
    res.lat_min_ms   = b.lat_min < 1e17 ? b.lat_min : 0.0;
    res.lat_max_ms   = b.lat_max;
    res.lat_p95_ms   = bench_pct(&b, 95.0);
    res.lat_p99_ms   = bench_pct(&b, 99.0);
    res.ops_per_sec  = b.lat_sum > 0.0 ? b.total_ops * 1000.0 / b.lat_sum : 0.0;
    snprintf(res.metric, sizeof res.metric,
             "pass=%.1f%% lat_avg=%.2fms lat_p95=%.2fms ops/sec=%.0f silent=%ld",
             100.0*b.pass_n/n_iters, res.lat_avg_ms, res.lat_p95_ms,
             res.ops_per_sec, b.total_silent);
    bench_free(&b);
    printf("\n");
    return res;
}

/* =======================================================================
   Summary sheet
   ======================================================================= */
static void write_summary(lxw_worksheet *ws, xl_fmts_t *f,
                          result_t *results, int n) {
    const char *hdrs[] = {
        "#","Test","Iterations","Pass","Fail","Pass%",
        "Avg Lat(ms)","Min Lat(ms)","Max Lat(ms)","p95 Lat(ms)","p99 Lat(ms)",
        "Ops/sec","Total Ops","Total Silent","Description"
    };
    for (int c = 0; c < 15; c++)
        worksheet_write_string(ws, 0, (lxw_col_t)c, hdrs[c], f->hdr);

    const double w[] = {
        4, 20, 11, 7, 7, 8,
        13, 13, 13, 13, 13,
        12, 12, 14, 60
    };
    set_col_widths(ws, w, 15);
    worksheet_freeze_panes(ws, 1, 0);

    for (int i = 0; i < n; i++) {
        result_t *r = &results[i];
        lxw_format *fmt = xfmt(f, r->passed);
        lxw_row_t row = (lxw_row_t)(i + 1);
        double pass_pct = r->n_iter > 0
            ? 100.0 * r->pass_n / r->n_iter : 0.0;

        worksheet_write_number(ws, row, 0, i+1,             fmt);
        worksheet_write_string(ws, row, 1, r->name,         fmt);
        worksheet_write_number(ws, row, 2, r->n_iter,       fmt);
        worksheet_write_number(ws, row, 3, r->pass_n,       fmt);
        worksheet_write_number(ws, row, 4, r->fail_n,       fmt);
        worksheet_write_number(ws, row, 5, pass_pct,        fmt);
        worksheet_write_number(ws, row, 6, r->lat_avg_ms,   fmt);
        worksheet_write_number(ws, row, 7, r->lat_min_ms,   fmt);
        worksheet_write_number(ws, row, 8, r->lat_max_ms,   fmt);
        worksheet_write_number(ws, row, 9, r->lat_p95_ms,   fmt);
        worksheet_write_number(ws, row,10, r->lat_p99_ms,   fmt);
        worksheet_write_number(ws, row,11, r->ops_per_sec,  fmt);
        worksheet_write_number(ws, row,12, (double)r->total_ops,    fmt);
        worksheet_write_number(ws, row,13, (double)r->total_silent, fmt);
        worksheet_write_string(ws, row,14, r->description,  fmt);
    }
}

/* =======================================================================
   main
   ======================================================================= */
int main(int argc, char *argv[]) {
    const char *out_prefix = "advanced_results";
    int         n_iters    = 1000;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-o") == 0 && i+1 < argc) out_prefix = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) n_iters    = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-o prefix] [-n iters]\n", argv[0]);
            printf("  -o <prefix>  output file prefix (default: advanced_results)\n");
            printf("  -n <iters>   iterations per test (default: 1000)\n");
            return 0;
        }
    }
    if (n_iters < 1) n_iters = 1;
    timer_init();   /* fix reference point for sub-ms timing precision */

    char xl_path[256];
    snprintf(xl_path, sizeof xl_path, "%s.xlsx", out_prefix);

    lxw_workbook  *wb     = workbook_new(xl_path);
    xl_fmts_t      fmts   = make_formats(wb);

    lxw_worksheet *ws_sum    = workbook_add_worksheet(wb, "Summary");
    lxw_worksheet *ws_wipe   = workbook_add_worksheet(wb, "StorageWipe");
    lxw_worksheet *ws_deg    = workbook_add_worksheet(wb, "DegradedWrite");
    lxw_worksheet *ws_blk    = workbook_add_worksheet(wb, "BlacklistOverflow");
    lxw_worksheet *ws_meta   = workbook_add_worksheet(wb, "MetadataCorruption");
    lxw_worksheet *ws_ver    = workbook_add_worksheet(wb, "VersionWrap");
    lxw_worksheet *ws_scrub  = workbook_add_worksheet(wb, "FullRangeScrub");
    lxw_worksheet *ws_loop   = workbook_add_worksheet(wb, "Loopback");
    lxw_worksheet *ws_repair = workbook_add_worksheet(wb, "RepairCycle");

    printf("ZINF advanced benchmark — %d iterations per test\n", n_iters);

    result_t results[8];
    results[0] = bench_storage_wipe       (ws_wipe,   &fmts, n_iters);
    results[1] = bench_degraded_write     (ws_deg,    &fmts, n_iters);
    results[2] = bench_blacklist_overflow (ws_blk,    &fmts, n_iters);
    results[3] = bench_metadata_corruption(ws_meta,   &fmts, n_iters);
    results[4] = bench_version_wrap       (ws_ver,    &fmts, n_iters);
    results[5] = bench_full_range_scrub   (ws_scrub,  &fmts, n_iters);
    results[6] = bench_loopback           (ws_loop,   &fmts, n_iters);
    results[7] = bench_repair_cycle       (ws_repair, &fmts, n_iters);

    write_summary(ws_sum, &fmts, results, 8);
    workbook_close(wb);

    printf("\nResults → %s\n\n", xl_path);
    printf("%-22s %6s %6s %6s  %s\n",
           "Test", "Iter", "Pass", "Fail", "Metric");
    printf("%-22s %6s %6s %6s  %s\n",
           "----", "----", "----", "----", "------");

    int any_fail = 0;
    for (int i = 0; i < 8; i++) {
        result_t *r = &results[i];
        printf("%-22s %6d %6d %6d  %s\n",
               r->name, r->n_iter, r->pass_n, r->fail_n, r->metric);
        if (!r->passed) any_fail = 1;
    }
    printf("\nOverall: %s\n", any_fail ? "FAIL" : "PASS");
    return any_fail ? 1 : 0;
}
