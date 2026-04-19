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
static uint32_t lcg(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

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
   Version counter snapshot
   ----------------------------------------------------------------------- */
typedef struct { uint16_t s0, s1, s2; } vers_t;

static vers_t read_versions(zinf_ctx_t *ctx) {
    uint8_t meta[SECTOR_SIZE];
    read_sector(ctx, 0, meta);
    vers_t v;
    v.s0 = (uint16_t)(meta[16] | ((uint16_t)meta[17] << 8));
    v.s1 = (uint16_t)(meta[26] | ((uint16_t)meta[27] << 8));
    v.s2 = (uint16_t)(meta[36] | ((uint16_t)meta[37] << 8));
    return v;
}

/* Which slot had its version change (round-robin write indicator) */
static int slot_written(vers_t before, vers_t after) {
    if (after.s0 != before.s0) return 0;
    if (after.s1 != before.s1) return 1;
    if (after.s2 != before.s2) return 2;
    return -1;
}

/* -----------------------------------------------------------------------
   Excel formatting
   ----------------------------------------------------------------------- */
#define XL_NAVY   0x1F3864u  /* header background         */
#define XL_GREEN  0xC6EFCEu  /* pass row background        */
#define XL_RED    0xFFC7CEu  /* fail row background         */
#define XL_AMBER  0xFFE0B2u  /* event / inject row          */
#define XL_GRAY   0xF2F2F2u  /* neutral write-only row      */

typedef struct {
    lxw_format *hdr;    /* column header                 */
    lxw_format *pass;   /* assertion passed              */
    lxw_format *fail;   /* assertion failed              */
    lxw_format *event;  /* wipe / inject / section event */
    lxw_format *plain;  /* write-only data row           */
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

static void set_col_widths(lxw_worksheet *ws,
                           const double *widths, int ncols) {
    for (int c = 0; c < ncols; c++)
        worksheet_set_column(ws, (lxw_col_t)c, (lxw_col_t)c, widths[c], NULL);
}

/* -----------------------------------------------------------------------
   Context setup / teardown
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

static void teardown(zinf_ctx_t *ctx) {
    if (ctx->driver && ctx->driver->deinit)
        ctx->driver->deinit(ctx->driver);
}

/* -----------------------------------------------------------------------
   Test result (carries assertion counters for summary sheet)
   ----------------------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *description;
    int         passed;
    int         assertions;
    int         assertions_passed;
    char        metric[200];
} result_t;

static void pass_assert(result_t *r, int ok) {
    r->assertions++;
    if (ok) r->assertions_passed++;
    else    r->passed = 0;
}

/* =======================================================================
   TEST 1 — Storage wipe
   Columns:
     Phase | # | LBA | Exp Temp | Exp Hum | Write RC | Read RC |
     Got Temp | Got Hum | Temp OK | Hum OK | Scrub Chk | Scrub Rep |
     Scrub Unrec | Note | Pass
   ======================================================================= */
static result_t test_storage_wipe(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "StorageWipe",
        "ram_driver_drop_buffer() wipes all sectors; reinit + scrub must not crash; "
        "50 new writes after wipe must read back correctly.", 1, 0, 0, "" };

    /* Column headers */
    const char *hdrs[] = {
        "Phase","#","LBA","Exp Temp","Exp Hum","Write RC","Read RC",
        "Got Temp","Got Hum","Temp OK","Hum OK",
        "Scrub Chk","Scrub Rep","Scrub Unrec","Note","Pass"
    };
    for (int c = 0; c < 16; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = {
        14,5,8,9,9,10,9,9,9,8,7,10,10,12,22,6
    };
    set_col_widths(ws, widths, 16);
    worksheet_freeze_panes(ws, 1, 0);

    zinf_ctx_t ctx;
    lxw_row_t  row = 1;

    if (setup_ram(&ctx) != 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "setup failed"); return r;
    }

    /* --- Phase 1: write 200 records --- */
    uint64_t pre_lba[200]; float pre_temp[200], pre_hum[200]; uint8_t pre_wrc[200];
    for (int i = 0; i < 200; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        pre_lba[i]  = lb + 1;
        pre_temp[i] = (float)i;
        pre_hum[i]  = (float)(i % 100);
        sensor_t s  = { .temp=pre_temp[i], .humidity=pre_hum[i] };
        pre_wrc[i]  = raid_sensor_values(&ctx, &s, 1);

        lxw_format *fmt = f->plain;
        char wrc_s[8]; snprintf(wrc_s, sizeof wrc_s, "%u", pre_wrc[i]);
        worksheet_write_string(ws, row, 0, "PRE_WIPE_WRITE", fmt);
        worksheet_write_number(ws, row, 1, i+1,              fmt);
        worksheet_write_number(ws, row, 2, (double)pre_lba[i],fmt);
        worksheet_write_number(ws, row, 3, pre_temp[i],      fmt);
        worksheet_write_number(ws, row, 4, pre_hum[i],       fmt);
        worksheet_write_string(ws, row, 5, wrc_s,            fmt);
        worksheet_write_string(ws, row,15, "write before wipe",fmt);
        row++;
    }

    /* --- Wipe event --- */
    ram_driver_drop_buffer();
    {
        lxw_format *fmt = f->event;
        worksheet_write_string(ws, row, 0, "WIPE",             fmt);
        worksheet_write_string(ws, row,14, "ram_driver_drop_buffer() — all sectors zeroed",fmt);
        worksheet_write_string(ws, row,15, "event",            fmt);
        row++;
    }

    /* --- Reinit --- */
    {
        uint8_t rc = init_log_sector(&ctx);
        int ok = (rc == STORAGE_OK);
        pass_assert(&r, ok);
        lxw_format *fmt = xfmt(f, ok);
        char rc_s[8]; snprintf(rc_s, sizeof rc_s, "%u", rc);
        worksheet_write_string(ws, row, 0, "REINIT",          fmt);
        worksheet_write_string(ws, row, 5, rc_s,              fmt);
        worksheet_write_string(ws, row,14, "init_log_sector after wipe", fmt);
        worksheet_write_string(ws, row,15, ok?"PASS":"FAIL",  fmt);
        row++;
        if (!ok) goto done_wipe;
    }

    /* --- Scrub on wiped storage --- */
    {
        zinf_scrub_report_t rep = {0};
        uint8_t rc = zinf_scrub(&ctx, 2, 200, &rep);
        int ok = (rc == STORAGE_OK);
        pass_assert(&r, ok);
        lxw_format *fmt = xfmt(f, ok);
        char rc_s[8]; snprintf(rc_s, sizeof rc_s, "%u", rc);
        worksheet_write_string(ws, row, 0,  "SCRUB",                       fmt);
        worksheet_write_string(ws, row, 5,  rc_s,                          fmt);
        worksheet_write_number(ws, row, 11, rep.checked,                   fmt);
        worksheet_write_number(ws, row, 12, rep.repaired,                  fmt);
        worksheet_write_number(ws, row, 13, rep.unrecoverable,             fmt);
        worksheet_write_string(ws, row, 14, "zinf_scrub on zeroed storage", fmt);
        worksheet_write_string(ws, row, 15, ok?"PASS":"FAIL",              fmt);
        row++;
        if (!ok) goto done_wipe;

        /* Blacklist is volatile — a real MCU loses it on power-cycle.
           Clear it now so new writes are not blocked by the scrub's bad-sector marks. */
        zinf_clear_bad_sectors(&ctx);
        {
            lxw_format *efmt = f->event;
            worksheet_write_string(ws, row, 0, "CLEAR_BLACKLIST", efmt);
            worksheet_write_string(ws, row,14,
                "zinf_clear_bad_sectors — volatile blacklist reset after scrub", efmt);
            worksheet_write_string(ws, row,15, "event", efmt);
            row++;
        }
    }

    /* --- Post-wipe: write 50 new records, then verify each --- */
    {
        uint64_t post_lba[50]; float post_temp[50], post_hum[50];
        for (int i = 0; i < 50; i++) {
            uint64_t lb = 0; get_last_sector(&ctx, &lb);
            post_lba[i]  = lb + 1;
            post_temp[i] = (float)(1000 + i);
            post_hum[i]  = (float)(i % 100);
            sensor_t s   = { .temp=post_temp[i], .humidity=post_hum[i] };
            uint8_t wrc  = raid_sensor_values(&ctx, &s, 1);
            char wrc_s[8]; snprintf(wrc_s, sizeof wrc_s, "%u", wrc);
            lxw_format *fmt = f->plain;
            worksheet_write_string(ws, row, 0, "POST_WIPE_WRITE", fmt);
            worksheet_write_number(ws, row, 1, i+1,               fmt);
            worksheet_write_number(ws, row, 2, (double)post_lba[i],fmt);
            worksheet_write_number(ws, row, 3, post_temp[i],      fmt);
            worksheet_write_number(ws, row, 4, post_hum[i],       fmt);
            worksheet_write_string(ws, row, 5, wrc_s,             fmt);
            worksheet_write_string(ws, row,14, "write after reinit",fmt);
            row++;
        }

        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < 50; i++) {
            uint8_t rrc = raid_read(&ctx, post_lba[i], payload);
            float gt=0.0f, gh=0.0f; int tm=0, hm=0;
            if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
                gt = unpack_f32(&payload[0]);
                gh = unpack_f32(&payload[4]);
                tm = fabsf(gt - post_temp[i]) < 0.001f;
                hm = fabsf(gh - post_hum[i])  < 0.001f;
            }
            int ok = tm && hm;
            pass_assert(&r, ok);
            lxw_format *fmt = xfmt(f, ok);
            char rrc_s[8]; snprintf(rrc_s, sizeof rrc_s, "%u", rrc);
            worksheet_write_string(ws, row, 0, "POST_WIPE_VERIFY", fmt);
            worksheet_write_number(ws, row, 1, i+1,                fmt);
            worksheet_write_number(ws, row, 2, (double)post_lba[i],fmt);
            worksheet_write_number(ws, row, 3, post_temp[i],       fmt);
            worksheet_write_number(ws, row, 4, post_hum[i],        fmt);
            worksheet_write_string(ws, row, 6, rrc_s,              fmt);
            worksheet_write_number(ws, row, 7, gt,                 fmt);
            worksheet_write_number(ws, row, 8, gh,                 fmt);
            worksheet_write_string(ws, row, 9, tm?"yes":"no",      fmt);
            worksheet_write_string(ws, row,10, hm?"yes":"no",      fmt);
            worksheet_write_string(ws, row,14, "readback post-wipe",fmt);
            worksheet_write_string(ws, row,15, ok?"PASS":"FAIL",   fmt);
            row++;
        }
    }

done_wipe:
    teardown(&ctx);
    snprintf(r.metric, sizeof r.metric,
             "pre_writes=200 post_writes=50 assertions=%d passed=%d",
             r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   TEST 2 — Degraded write
   Columns:
     Write# | Logical LBA | Mirror-0 Phys | Mirror-1 Phys (blklst) |
     Exp Write RC | Actual Write RC | RC Correct |
     Exp Temp | Exp Hum | Read RC |
     Got Temp | Got Hum | Temp Match | Hum Match |
     Mirror-0 Health | Mirror-1 Health | Pass
   ======================================================================= */
static result_t test_degraded_write(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "DegradedWrite",
        "Blacklist mirror-1 of each target sector before writing. "
        "Write must return WARN_DEGRADED. Read via surviving mirror-0 must match exactly.",
        1, 0, 0, "" };

    const char *hdrs[] = {
        "Write#","Logical LBA","Mirror-0 Phys","Mirror-1 Phys (blklst)",
        "Exp Write RC","Actual Write RC","RC Correct",
        "Exp Temp","Exp Hum","Read RC",
        "Got Temp","Got Hum","Temp Match","Hum Match",
        "M0 Health","M1 Health","Pass"
    };
    for (int c = 0; c < 17; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = {
        7,11,13,20,13,15,10,9,9,8,9,9,10,10,12,12,6
    };
    set_col_widths(ws, widths, 17);
    worksheet_freeze_panes(ws, 1, 0);

    zinf_ctx_t ctx;
    lxw_row_t  row = 1;
    if (setup_ram(&ctx) != 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "setup failed"); return r;
    }

    int degraded_seen = 0, silent = 0;

    for (int i = 0; i < 10; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        uint64_t next_lba = lb + 1;
        uint64_t m0_phys  = next_lba;
        uint64_t m1_phys  = next_lba + ctx.mirror_offset;
        zinf_mark_bad_sector(&ctx, m1_phys);

        float et = (float)(200 + i), eh = (float)(i % 100);
        sensor_t s = { .temp=et, .humidity=eh };
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        if (wrc == STORAGE_WARN_DEGRADED) degraded_seen++;

        /* Check mirror health */
        zinf_sector_health_t health = {0};
        zinf_check_sector(&ctx, next_lba, &health);
        const char *m0h = (health.status[0] == ZINF_MIRROR_OK)        ? "OK" :
                          (health.status[0] == ZINF_MIRROR_CRC_FAIL)   ? "CRC_FAIL" :
                          (health.status[0] == ZINF_MIRROR_BLACKLIST)  ? "BLACKLIST" : "IO_ERR";
        const char *m1h = (health.status[1] == ZINF_MIRROR_OK)        ? "OK" :
                          (health.status[1] == ZINF_MIRROR_CRC_FAIL)   ? "CRC_FAIL" :
                          (health.status[1] == ZINF_MIRROR_BLACKLIST)  ? "BLACKLIST" : "IO_ERR";

        /* Read back */
        uint8_t payload[PAYLOAD_SIZE];
        uint8_t rrc = raid_read(&ctx, next_lba, payload);
        float gt=0.0f, gh=0.0f; int tm=0, hm=0;
        if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            gt = unpack_f32(&payload[0]); gh = unpack_f32(&payload[4]);
            tm = fabsf(gt-et)<0.001f; hm = fabsf(gh-eh)<0.001f;
            if (!tm || !hm) silent++;
        }

        int rc_ok   = (wrc == STORAGE_WARN_DEGRADED);
        int data_ok = tm && hm;
        int pass    = rc_ok && data_ok;
        pass_assert(&r, rc_ok);
        pass_assert(&r, data_ok);

        char wrc_s[8], rrc_s[8], m0s[20], m1s[20];
        snprintf(wrc_s, sizeof wrc_s, "%u",   wrc);
        snprintf(rrc_s, sizeof rrc_s, "%u",   rrc);
        snprintf(m0s,   sizeof m0s,   "%" PRIu64, m0_phys);
        snprintf(m1s,   sizeof m1s,   "%" PRIu64, m1_phys);

        lxw_format *fmt = xfmt(f, pass);
        worksheet_write_number(ws, row, 0,  i+1,              fmt);
        worksheet_write_number(ws, row, 1,  (double)next_lba, fmt);
        worksheet_write_string(ws, row, 2,  m0s,              fmt);
        worksheet_write_string(ws, row, 3,  m1s,              fmt);
        worksheet_write_string(ws, row, 4,  "WARN_DEGRADED(5)",fmt);
        worksheet_write_string(ws, row, 5,  wrc_s,            fmt);
        worksheet_write_string(ws, row, 6,  rc_ok?"yes":"NO", fmt);
        worksheet_write_number(ws, row, 7,  et,               fmt);
        worksheet_write_number(ws, row, 8,  eh,               fmt);
        worksheet_write_string(ws, row, 9,  rrc_s,            fmt);
        worksheet_write_number(ws, row,10,  gt,               fmt);
        worksheet_write_number(ws, row,11,  gh,               fmt);
        worksheet_write_string(ws, row,12,  tm?"yes":"NO",    fmt);
        worksheet_write_string(ws, row,13,  hm?"yes":"NO",    fmt);
        worksheet_write_string(ws, row,14,  m0h,              fmt);
        worksheet_write_string(ws, row,15,  m1h,              fmt);
        worksheet_write_string(ws, row,16,  pass?"PASS":"FAIL",fmt);
        row++;

        zinf_clear_bad_sectors(&ctx);
    }

    teardown(&ctx);
    if (silent) r.passed = 0;
    snprintf(r.metric, sizeof r.metric,
             "degraded_seen=%d/10 silent=%d assertions=%d passed=%d",
             degraded_seen, silent, r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   TEST 3 — Blacklist overflow
   Columns:
     Attempt# | Physical LBA | Count Before | Mark Result | Count After |
     Expected Result | Count Correct | Max Allowed | Overflow Flag | Note | Pass
   ======================================================================= */
static result_t test_blacklist_overflow(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "BlacklistOverflow",
        "Call zinf_mark_bad_sector 20 times (MAX_BAD_SECTORS=16). "
        "First 16 must return STORAGE_OK and increment count. "
        "Attempts 17-20 must return STORAGE_ERR_PARAM and leave count at 16.",
        1, 0, 0, "" };

    const char *hdrs[] = {
        "Attempt#","Physical LBA","Count Before","Mark Result",
        "Count After","Expected Result","Count Correct",
        "Max Allowed","Overflow Flag","Note","Pass"
    };
    for (int c = 0; c < 11; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = { 9,13,13,13,11,16,14,12,13,30,6 };
    set_col_widths(ws, widths, 11);
    worksheet_freeze_panes(ws, 1, 0);

    zinf_ctx_t ctx;
    lxw_row_t  row = 1;
    if (setup_ram(&ctx) != 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "setup failed"); return r;
    }

    int overflow_caught = 0;

    for (int i = 0; i < 20; i++) {
        uint64_t lba      = (uint64_t)(100 + i);
        uint8_t  cnt_before = ctx.bad_sector_count;
        uint8_t  rc       = zinf_mark_bad_sector(&ctx, lba);
        uint8_t  cnt_after  = ctx.bad_sector_count;

        int expected_full  = (i >= (int)MAX_BAD_SECTORS);
        const char *exp_s  = expected_full ? "ERR_PARAM(1)" : "STORAGE_OK(0)";
        const char *rc_s   = (rc == STORAGE_OK)        ? "OK(0)" :
                             (rc == STORAGE_ERR_PARAM)  ? "ERR_PARAM(1)" : "unknown";

        int rc_ok    = expected_full ? (rc == STORAGE_ERR_PARAM)
                                     : (rc == STORAGE_OK);
        int cnt_ok   = expected_full ? (cnt_after == MAX_BAD_SECTORS)
                                     : (cnt_after == (uint8_t)(i + 1));
        int overflow = (rc == STORAGE_ERR_PARAM);
        if (overflow) overflow_caught++;

        int pass = rc_ok && cnt_ok;
        pass_assert(&r, rc_ok);
        pass_assert(&r, cnt_ok);

        lxw_format *fmt = xfmt(f, pass);
        char note[48];
        if (expected_full)
            snprintf(note, sizeof note, "slot %d/%u — list full, must reject", i+1, MAX_BAD_SECTORS);
        else
            snprintf(note, sizeof note, "slot %d/%u — normal add", i+1, MAX_BAD_SECTORS);

        worksheet_write_number(ws, row, 0, i+1,                      fmt);
        worksheet_write_number(ws, row, 1, (double)lba,              fmt);
        worksheet_write_number(ws, row, 2, cnt_before,               fmt);
        worksheet_write_string(ws, row, 3, rc_s,                     fmt);
        worksheet_write_number(ws, row, 4, cnt_after,                fmt);
        worksheet_write_string(ws, row, 5, exp_s,                    fmt);
        worksheet_write_string(ws, row, 6, rc_ok?"yes":"NO",         fmt);
        worksheet_write_number(ws, row, 7, MAX_BAD_SECTORS,          fmt);
        worksheet_write_string(ws, row, 8, overflow?"YES":"-",       fmt);
        worksheet_write_string(ws, row, 9, note,                     fmt);
        worksheet_write_string(ws, row,10, pass?"PASS":"FAIL",       fmt);
        row++;
    }

    teardown(&ctx);
    snprintf(r.metric, sizeof r.metric,
             "overflow_caught=%d/4 max_count=%u assertions=%d passed=%d",
             overflow_caught, ctx.bad_sector_count,
             r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   TEST 4 — Metadata corruption
   Columns:
     Phase | # | LBA | Field / Exp Temp | Orig Val / Exp Hum |
     New Val / Write RC | Read RC | Got Temp | Got Hum |
     Temp Match | Hum Match | Note | Pass
   ======================================================================= */
static result_t test_metadata_corruption(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "MetadataCorruption",
        "Write 100 data records. Corrupt 5 bytes in sector-0 metadata "
        "(version counter slots, write cursor). Scrub and verify all 100 records. "
        "Data sectors must be unaffected by metadata sector corruption.",
        1, 0, 0, "" };

    const char *hdrs[] = {
        "Phase","#","LBA","Field / Exp Temp","Orig Val / Exp Hum",
        "New Val / Write RC","Read RC","Got Temp","Got Hum",
        "Temp Match","Hum Match","Note","Pass"
    };
    for (int c = 0; c < 13; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = { 14,5,8,22,18,18,8,9,9,10,10,32,6 };
    set_col_widths(ws, widths, 13);
    worksheet_freeze_panes(ws, 1, 0);

    zinf_ctx_t ctx;
    lxw_row_t  row = 1;
    if (setup_ram(&ctx) != 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "setup failed"); return r;
    }

    /* WRITE phase: 100 records */
    uint64_t slba[100]; float stemp[100], shum[100];
    for (int i = 0; i < 100; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        slba[i]  = lb + 1;
        stemp[i] = (float)(300 + i);
        shum[i]  = (float)(i % 100);
        sensor_t s = { .temp=stemp[i], .humidity=shum[i] };
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        int ok = (wrc == STORAGE_OK || wrc == STORAGE_WARN_DEGRADED);

        lxw_format *fmt = ok ? f->plain : f->fail;
        char wrc_s[8]; snprintf(wrc_s, sizeof wrc_s, "%u", wrc);
        worksheet_write_string(ws, row, 0, "WRITE",    fmt);
        worksheet_write_number(ws, row, 1, i+1,        fmt);
        worksheet_write_number(ws, row, 2, (double)slba[i], fmt);
        worksheet_write_number(ws, row, 3, stemp[i],   fmt);
        worksheet_write_number(ws, row, 4, shum[i],    fmt);
        worksheet_write_string(ws, row, 5, wrc_s,      fmt);
        worksheet_write_string(ws, row,12, ok?"-":"write failed", fmt);
        row++;
    }

    /* CORRUPT phase: 5 bytes in sector 0 */
    static const uint32_t c_offsets[5] = { 16, 17, 26, 27, 38 };
    static const char *c_fields[5] = {
        "Slot-0 version lo (byte 16)",
        "Slot-0 version hi (byte 17)",
        "Slot-1 version lo (byte 26)",
        "Slot-1 version hi (byte 27)",
        "Write cursor lo  (byte 38)"
    };
    uint8_t meta0[SECTOR_SIZE];
    for (int i = 0; i < 5; i++) {
        read_sector(&ctx, 0, meta0);
        uint8_t orig = meta0[c_offsets[i]];
        uint8_t newv = (uint8_t)(lcg() | 0x80u); /* ensure change */
        if (newv == orig) newv ^= 0xFFu;
        ram_driver_corrupt(0, c_offsets[i], newv);

        char orig_s[12], new_s[12];
        snprintf(orig_s, sizeof orig_s, "0x%02X (%3u)", orig, orig);
        snprintf(new_s,  sizeof new_s,  "0x%02X (%3u)", newv, newv);

        lxw_format *fmt = f->event;
        worksheet_write_string(ws, row, 0, "CORRUPT",        fmt);
        worksheet_write_number(ws, row, 1, i+1,              fmt);
        worksheet_write_string(ws, row, 3, c_fields[i],      fmt);
        worksheet_write_string(ws, row, 4, orig_s,           fmt);
        worksheet_write_string(ws, row, 5, new_s,            fmt);
        worksheet_write_string(ws, row,12, "injected",        fmt);
        row++;
    }

    /* SCRUB phase */
    zinf_clear_bad_sectors(&ctx);
    zinf_scrub_report_t rep = {0};
    uint64_t last = 0; get_last_sector(&ctx, &last);
    uint8_t src = zinf_scrub(&ctx, 2, last, &rep);
    {
        int ok = (src == STORAGE_OK);
        pass_assert(&r, ok);
        lxw_format *fmt = xfmt(f, ok);
        char rc_s[8]; snprintf(rc_s, sizeof rc_s, "%u", src);
        char note[64];
        snprintf(note, sizeof note,
                 "chk=%u healthy=%u rep=%u unrec=%u",
                 rep.checked, rep.healthy, rep.repaired, rep.unrecoverable);
        worksheet_write_string(ws, row, 0, "SCRUB",          fmt);
        worksheet_write_string(ws, row, 5, rc_s,             fmt);
        worksheet_write_string(ws, row,11, note,             fmt);
        worksheet_write_string(ws, row,12, ok?"PASS":"FAIL", fmt);
        row++;
    }

    /* VERIFY phase: read back all 100 */
    long ok_cnt = 0, lost_cnt = 0, silent_cnt = 0;
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < 100; i++) {
        uint8_t rrc = raid_read(&ctx, slba[i], payload);
        float gt=0.0f, gh=0.0f; int tm=0, hm=0;
        const char *note = "";
        if (rrc == STORAGE_ERR_UNRECOVERABLE) {
            lost_cnt++; note = "unrecoverable";
        } else if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            gt = unpack_f32(&payload[0]); gh = unpack_f32(&payload[4]);
            tm = fabsf(gt-stemp[i])<0.001f; hm = fabsf(gh-shum[i])<0.001f;
            if (tm && hm) { ok_cnt++; note = "match"; }
            else          { silent_cnt++; note = "SILENT CORRUPTION"; }
        }

        int ok = (rrc != STORAGE_ERR_UNRECOVERABLE) && tm && hm;
        pass_assert(&r, ok);
        lxw_format *fmt = xfmt(f, ok);
        char rrc_s[8]; snprintf(rrc_s, sizeof rrc_s, "%u", rrc);
        worksheet_write_string(ws, row, 0, "VERIFY",             fmt);
        worksheet_write_number(ws, row, 1, i+1,                  fmt);
        worksheet_write_number(ws, row, 2, (double)slba[i],      fmt);
        worksheet_write_number(ws, row, 3, stemp[i],             fmt);
        worksheet_write_number(ws, row, 4, shum[i],              fmt);
        worksheet_write_string(ws, row, 6, rrc_s,                fmt);
        worksheet_write_number(ws, row, 7, gt,                   fmt);
        worksheet_write_number(ws, row, 8, gh,                   fmt);
        worksheet_write_string(ws, row, 9, tm?"yes":"NO",        fmt);
        worksheet_write_string(ws, row,10, hm?"yes":"NO",        fmt);
        worksheet_write_string(ws, row,11, note,                 fmt);
        worksheet_write_string(ws, row,12, ok?"PASS":"FAIL",     fmt);
        row++;
    }

    teardown(&ctx);
    snprintf(r.metric, sizeof r.metric,
             "ok=%ld lost=%ld silent=%ld assertions=%d passed=%d",
             ok_cnt, lost_cnt, silent_cnt, r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   TEST 5 — Version counter wraparound
   Columns:
     Event | # | LBA | Exp Temp | Exp Hum |
     S0 Before | S1 Before | S2 Before |
     Write RC | Slot Written |
     S0 After | S1 After | S2 After |
     Read RC | Got Temp | Got Hum | Match | Pass
   ======================================================================= */
static result_t test_version_wraparound(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "VersionWrap",
        "Seed 3 writes, then patch all 3 copy-slot version counters to 0xFFFE. "
        "Write 4 more records stepping through 0xFFFE→0xFFFF→0x0000→0x0001. "
        "Verify read-back at every version value including the wraparound boundary.",
        1, 0, 0, "" };

    const char *hdrs[] = {
        "Event","#","LBA","Exp Temp","Exp Hum",
        "S0 Before","S1 Before","S2 Before",
        "Write RC","Slot Written",
        "S0 After","S1 After","S2 After",
        "Read RC","Got Temp","Got Hum","Match","Pass"
    };
    for (int c = 0; c < 18; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = {
        16,5,8,9,9, 10,10,10, 9,13, 9,9,9, 8,9,9,7,6
    };
    set_col_widths(ws, widths, 18);
    worksheet_freeze_panes(ws, 1, 0);

    zinf_ctx_t ctx;
    lxw_row_t  row = 1;
    if (setup_ram(&ctx) != 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "setup failed"); return r;
    }

    int write_n = 0, silent = 0;
    uint64_t wlba[8]; float wtemp[8], whum[8];

    /* Helper: write one record, capture before/after versions, read back */
    uint8_t payload[PAYLOAD_SIZE];

    /* Seed writes (3 records, normal) */
    for (int i = 0; i < 3; i++) {
        vers_t vb = read_versions(&ctx);
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        wlba[write_n]  = lb + 1;
        wtemp[write_n] = (float)(500 + i);
        whum[write_n]  = (float)(i % 100);
        sensor_t s = { .temp=wtemp[write_n], .humidity=whum[write_n] };
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        vers_t va = read_versions(&ctx);
        int slot = slot_written(vb, va);

        uint8_t rrc = raid_read(&ctx, wlba[write_n], payload);
        float gt=0.0f, gh=0.0f; int m=0;
        if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            gt = unpack_f32(&payload[0]); gh = unpack_f32(&payload[4]);
            m  = fabsf(gt-wtemp[write_n])<0.001f && fabsf(gh-whum[write_n])<0.001f;
            if (!m) silent++;
        }
        int ok = (wrc == STORAGE_OK) && m;
        pass_assert(&r, ok);

        char slot_s[4]; snprintf(slot_s, sizeof slot_s, slot>=0?"%d":"-", slot);
        char s0b[8],s1b[8],s2b[8],s0a[8],s1a[8],s2a[8];
        snprintf(s0b,sizeof s0b,"0x%04X",vb.s0); snprintf(s1b,sizeof s1b,"0x%04X",vb.s1); snprintf(s2b,sizeof s2b,"0x%04X",vb.s2);
        snprintf(s0a,sizeof s0a,"0x%04X",va.s0); snprintf(s1a,sizeof s1a,"0x%04X",va.s1); snprintf(s2a,sizeof s2a,"0x%04X",va.s2);
        char wrc_s[4], rrc_s[4]; snprintf(wrc_s,sizeof wrc_s,"%u",wrc); snprintf(rrc_s,sizeof rrc_s,"%u",rrc);

        lxw_format *fmt = xfmt(f, ok);
        worksheet_write_string(ws,row, 0,"SEED_WRITE",   fmt);
        worksheet_write_number(ws,row, 1,write_n+1,      fmt);
        worksheet_write_number(ws,row, 2,(double)wlba[write_n],fmt);
        worksheet_write_number(ws,row, 3,wtemp[write_n], fmt);
        worksheet_write_number(ws,row, 4,whum[write_n],  fmt);
        worksheet_write_string(ws,row, 5,s0b,            fmt);
        worksheet_write_string(ws,row, 6,s1b,            fmt);
        worksheet_write_string(ws,row, 7,s2b,            fmt);
        worksheet_write_string(ws,row, 8,wrc_s,          fmt);
        worksheet_write_string(ws,row, 9,slot_s,         fmt);
        worksheet_write_string(ws,row,10,s0a,            fmt);
        worksheet_write_string(ws,row,11,s1a,            fmt);
        worksheet_write_string(ws,row,12,s2a,            fmt);
        worksheet_write_string(ws,row,13,rrc_s,          fmt);
        worksheet_write_number(ws,row,14,gt,             fmt);
        worksheet_write_number(ws,row,15,gh,             fmt);
        worksheet_write_string(ws,row,16,m?"yes":"NO",   fmt);
        worksheet_write_string(ws,row,17,ok?"PASS":"FAIL",fmt);
        row++; write_n++;
    }

    /* Patch all 3 version counters to 0xFFFE */
    {
        vers_t vb = read_versions(&ctx);
        uint8_t meta[SECTOR_SIZE]; read_sector(&ctx, 0, meta);
        meta[16]=0xFE; meta[17]=0xFF;
        meta[26]=0xFE; meta[27]=0xFF;
        meta[36]=0xFE; meta[37]=0xFF;
        write_sector(&ctx, 0, meta);
        vers_t va = read_versions(&ctx);

        char s0b[8],s1b[8],s2b[8],s0a[8],s1a[8],s2a[8];
        snprintf(s0b,sizeof s0b,"0x%04X",vb.s0); snprintf(s1b,sizeof s1b,"0x%04X",vb.s1); snprintf(s2b,sizeof s2b,"0x%04X",vb.s2);
        snprintf(s0a,sizeof s0a,"0x%04X",va.s0); snprintf(s1a,sizeof s1a,"0x%04X",va.s1); snprintf(s2a,sizeof s2a,"0x%04X",va.s2);

        lxw_format *fmt = f->event;
        worksheet_write_string(ws,row, 0,"PATCH",                     fmt);
        worksheet_write_string(ws,row, 5,s0b,                         fmt);
        worksheet_write_string(ws,row, 6,s1b,                         fmt);
        worksheet_write_string(ws,row, 7,s2b,                         fmt);
        worksheet_write_string(ws,row, 9,"manual patch",              fmt);
        worksheet_write_string(ws,row,10,s0a,                         fmt);
        worksheet_write_string(ws,row,11,s1a,                         fmt);
        worksheet_write_string(ws,row,12,s2a,                         fmt);
        worksheet_write_string(ws,row,17,"set all to 0xFFFE",         fmt);
        row++;
    }

    /* Post-patch writes: 4 records stepping through wraparound */
    for (int i = 0; i < 4; i++) {
        vers_t vb = read_versions(&ctx);
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        wlba[write_n]  = lb + 1;
        wtemp[write_n] = (float)(600 + i);
        whum[write_n]  = (float)(i % 100);
        sensor_t s = { .temp=wtemp[write_n], .humidity=whum[write_n] };
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        vers_t va = read_versions(&ctx);
        int slot = slot_written(vb, va);

        uint8_t rrc = raid_read(&ctx, wlba[write_n], payload);
        float gt=0.0f, gh=0.0f; int m=0;
        if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            gt = unpack_f32(&payload[0]); gh = unpack_f32(&payload[4]);
            m  = fabsf(gt-wtemp[write_n])<0.001f && fabsf(gh-whum[write_n])<0.001f;
            if (!m) silent++;
        }
        int ok = (wrc == STORAGE_OK || wrc == STORAGE_WARN_DEGRADED) && m;
        pass_assert(&r, ok);

        char slot_s[4]; snprintf(slot_s, sizeof slot_s, slot>=0?"%d":"-", slot);
        char s0b[8],s1b[8],s2b[8],s0a[8],s1a[8],s2a[8];
        snprintf(s0b,sizeof s0b,"0x%04X",vb.s0); snprintf(s1b,sizeof s1b,"0x%04X",vb.s1); snprintf(s2b,sizeof s2b,"0x%04X",vb.s2);
        snprintf(s0a,sizeof s0a,"0x%04X",va.s0); snprintf(s1a,sizeof s1a,"0x%04X",va.s1); snprintf(s2a,sizeof s2a,"0x%04X",va.s2);
        char wrc_s[4], rrc_s[4]; snprintf(wrc_s,sizeof wrc_s,"%u",wrc); snprintf(rrc_s,sizeof rrc_s,"%u",rrc);

        lxw_format *fmt = xfmt(f, ok);
        worksheet_write_string(ws,row, 0,"POST_WRAP_WRITE",fmt);
        worksheet_write_number(ws,row, 1,write_n+1,        fmt);
        worksheet_write_number(ws,row, 2,(double)wlba[write_n],fmt);
        worksheet_write_number(ws,row, 3,wtemp[write_n],   fmt);
        worksheet_write_number(ws,row, 4,whum[write_n],    fmt);
        worksheet_write_string(ws,row, 5,s0b,              fmt);
        worksheet_write_string(ws,row, 6,s1b,              fmt);
        worksheet_write_string(ws,row, 7,s2b,              fmt);
        worksheet_write_string(ws,row, 8,wrc_s,            fmt);
        worksheet_write_string(ws,row, 9,slot_s,           fmt);
        worksheet_write_string(ws,row,10,s0a,              fmt);
        worksheet_write_string(ws,row,11,s1a,              fmt);
        worksheet_write_string(ws,row,12,s2a,              fmt);
        worksheet_write_string(ws,row,13,rrc_s,            fmt);
        worksheet_write_number(ws,row,14,gt,               fmt);
        worksheet_write_number(ws,row,15,gh,               fmt);
        worksheet_write_string(ws,row,16,m?"yes":"NO",     fmt);
        worksheet_write_string(ws,row,17,ok?"PASS":"FAIL", fmt);
        row++; write_n++;
    }

    teardown(&ctx);
    if (silent) r.passed = 0;
    snprintf(r.metric, sizeof r.metric,
             "total_writes=%d silent=%d assertions=%d passed=%d",
             write_n, silent, r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   TEST 6 — Full-range scrub
   Columns:
     Phase | # | Zone | LBA | Exp Temp | Exp Hum | Write RC |
     Corrupt Mirror | Corrupt Offset | Corrupt Byte |
     Scrub Chk | Scrub Hlthy | Scrub Rep | Scrub Unrec |
     Read RC | Got Temp | Got Hum | Match | Pass
   ======================================================================= */
static result_t test_full_range_scrub(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "FullRangeScrub",
        "Write 500 records. Inject 30 single-mirror faults: 10 in early range "
        "(LBA 2-11), 10 in mid range, 10 in late range. Scrub full range. "
        "Single-mirror faults must be repaired. Verify all 500 records: silent must be 0.",
        1, 0, 0, "" };

    const char *hdrs[] = {
        "Phase","#","Zone","LBA","Exp Temp","Exp Hum","Write RC",
        "Corrupt Mirror","Corrupt Offset","Corrupt Byte",
        "Scrub Chk","Scrub Hlthy","Scrub Rep","Scrub Unrec",
        "Read RC","Got Temp","Got Hum","Match","Pass"
    };
    for (int c = 0; c < 19; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = {
        14,5,8,8,9,9,9,
        15,14,13,
        10,11,10,12,
        8,9,9,7,6
    };
    set_col_widths(ws, widths, 19);
    worksheet_freeze_panes(ws, 1, 0);

    zinf_ctx_t ctx;
    lxw_row_t  row = 1;
    if (setup_ram(&ctx) != 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "setup failed"); return r;
    }

    /* Write 500 records */
    uint64_t slba[500]; float stemp[500], shum[500];
    for (int i = 0; i < 500; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        slba[i]  = lb + 1;
        stemp[i] = (float)(700 + i);
        shum[i]  = (float)(i % 100);
        sensor_t s = { .temp=stemp[i], .humidity=shum[i] };
        uint8_t wrc = raid_sensor_values(&ctx, &s, 1);
        char wrc_s[8]; snprintf(wrc_s, sizeof wrc_s, "%u", wrc);
        lxw_format *fmt = f->plain;
        worksheet_write_string(ws, row, 0, "WRITE",       fmt);
        worksheet_write_number(ws, row, 1, i+1,           fmt);
        worksheet_write_string(ws, row, 2, "-",           fmt);
        worksheet_write_number(ws, row, 3, (double)slba[i],fmt);
        worksheet_write_number(ws, row, 4, stemp[i],      fmt);
        worksheet_write_number(ws, row, 5, shum[i],       fmt);
        worksheet_write_string(ws, row, 6, wrc_s,         fmt);
        row++;
    }
    uint64_t last_lba = 0; get_last_sector(&ctx, &last_lba);

    /* Inject 30 single-mirror faults across three zones */
    static const char *zone_names[3] = { "early", "mid", "late" };
    int faults = 0;
    for (int zone = 0; zone < 3; zone++) {
        uint64_t base;
        if      (zone == 0) base = 2;
        else if (zone == 1) base = 2 + (last_lba - 2) / 2;
        else                base = last_lba >= 9 ? last_lba - 9 : last_lba;

        for (int j = 0; j < 10; j++) {
            uint64_t lba = base + (uint64_t)j;
            if (lba > last_lba) break;
            uint32_t off = 0;          /* byte 0 of payload */
            uint8_t  byt = 0xFFu;
            ram_driver_corrupt(lba, off, byt);
            faults++;

            char lba_s[20], off_s[8], byt_s[8];
            snprintf(lba_s, sizeof lba_s, "%" PRIu64, lba);
            snprintf(off_s, sizeof off_s, "%u",  off);
            snprintf(byt_s, sizeof byt_s, "0x%02X", byt);

            lxw_format *fmt = f->event;
            worksheet_write_string(ws, row, 0, "INJECT",       fmt);
            worksheet_write_number(ws, row, 1, faults,         fmt);
            worksheet_write_string(ws, row, 2, zone_names[zone],fmt);
            worksheet_write_string(ws, row, 3, lba_s,          fmt);
            worksheet_write_string(ws, row, 7, "mirror-0",     fmt);
            worksheet_write_string(ws, row, 8, off_s,          fmt);
            worksheet_write_string(ws, row, 9, byt_s,          fmt);
            row++;
        }
    }

    /* Full-range scrub */
    zinf_clear_bad_sectors(&ctx);
    zinf_scrub_report_t rep = {0};
    uint8_t src = zinf_scrub(&ctx, 2, last_lba, &rep);
    {
        int ok = (src == STORAGE_OK);
        pass_assert(&r, ok);
        /* repaired should equal faults (all single-mirror) */
        int rep_ok = (rep.repaired == (uint32_t)faults);
        pass_assert(&r, rep_ok);
        char rc_s[8]; snprintf(rc_s, sizeof rc_s, "%u", src);
        lxw_format *fmt = xfmt(f, ok && rep_ok);
        worksheet_write_string(ws, row, 0,  "SCRUB",           fmt);
        worksheet_write_string(ws, row, 6,  rc_s,              fmt);
        worksheet_write_number(ws, row,10,  rep.checked,       fmt);
        worksheet_write_number(ws, row,11,  rep.healthy,       fmt);
        worksheet_write_number(ws, row,12,  rep.repaired,      fmt);
        worksheet_write_number(ws, row,13,  rep.unrecoverable, fmt);
        worksheet_write_string(ws, row,18,  (ok&&rep_ok)?"PASS":"FAIL", fmt);
        row++;
    }

    /* Verify all 500 records */
    long ok_cnt=0, lost_cnt=0, silent_cnt=0;
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < 500; i++) {
        uint8_t rrc = raid_read(&ctx, slba[i], payload);
        float gt=0.0f, gh=0.0f; int m=0;
        if (rrc == STORAGE_ERR_UNRECOVERABLE) { lost_cnt++; }
        else if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            gt = unpack_f32(&payload[0]); gh = unpack_f32(&payload[4]);
            m  = fabsf(gt-stemp[i])<0.001f && fabsf(gh-shum[i])<0.001f;
            if (m) ok_cnt++; else silent_cnt++;
        }
        int ok = m;
        pass_assert(&r, ok);
        lxw_format *fmt = xfmt(f, ok);
        char rrc_s[8]; snprintf(rrc_s, sizeof rrc_s, "%u", rrc);
        worksheet_write_string(ws, row, 0, "VERIFY",         fmt);
        worksheet_write_number(ws, row, 1, i+1,              fmt);
        worksheet_write_number(ws, row, 3, (double)slba[i],  fmt);
        worksheet_write_number(ws, row, 4, stemp[i],         fmt);
        worksheet_write_number(ws, row, 5, shum[i],          fmt);
        worksheet_write_string(ws, row,14, rrc_s,            fmt);
        worksheet_write_number(ws, row,15, gt,               fmt);
        worksheet_write_number(ws, row,16, gh,               fmt);
        worksheet_write_string(ws, row,17, m?"yes":"NO",     fmt);
        worksheet_write_string(ws, row,18, ok?"PASS":"FAIL", fmt);
        row++;
    }

    teardown(&ctx);
    snprintf(r.metric, sizeof r.metric,
             "faults=%d rep=%u unrec=%u ok=%ld lost=%ld silent=%ld "
             "assertions=%d passed=%d",
             faults, rep.repaired, rep.unrecoverable,
             ok_cnt, lost_cnt, silent_cnt,
             r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   TEST 7 — Linux loopback integrity
   Columns:
     Phase | # | LBA | Exp Temp | Exp Hum | Write RC |
     Read RC | Got Temp | Got Hum | Temp Diff | Hum Diff | Match | Pass
   ======================================================================= */
static result_t test_loopback_integrity(lxw_worksheet *ws, xl_fmts_t *f) {
    result_t r = { "Loopback",
        "Write 100 sensor records via the linux block-device driver (real pread/pwrite). "
        "Read back each record and verify bytes match exactly. "
        "Temp diff and humidity diff must both be 0.000. No fault injection.",
        1, 0, 0, "" };

    const char *hdrs[] = {
        "Phase","#","LBA","Exp Temp","Exp Hum","Write RC",
        "Read RC","Got Temp","Got Hum","Temp Diff","Hum Diff","Match","Pass"
    };
    for (int c = 0; c < 13; c++) xlh(ws, c, hdrs[c], f);

    const double widths[] = { 8,5,8,9,9,9,8,9,9,10,10,7,6 };
    set_col_widths(ws, widths, 13);
    worksheet_freeze_panes(ws, 1, 0);

    /* Create image file */
    int fd = open(LOOPBACK_IMG, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "cannot create %s", LOOPBACK_IMG);
        return r;
    }
    if (ftruncate(fd, (off_t)ADV_IMG_SECTS * SECTOR_SIZE) != 0) {
        close(fd); r.passed = 0;
        snprintf(r.metric, sizeof r.metric, "ftruncate failed"); return r;
    }
    close(fd);

    linux_driver_set_path(LOOPBACK_IMG);
    zinf_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.driver           = &linux_driver;
    ctx.sector_size      = SECTOR_SIZE;
    ctx.mirror_count     = RAID_MIRRORS;
    ctx.metadata_sectors = 2;
    ctx.mirror_offset    = ADV_MIRROR_OFF;
    ctx.log_sector       = 0;
    ctx.raid_offset      = ctx.mirror_offset;

    if (ctx.driver->init(ctx.driver) != DRIVER_OK) {
        r.passed = 0; snprintf(r.metric, sizeof r.metric, "linux_driver init failed");
        unlink(LOOPBACK_IMG); return r;
    }
    if (init_log_sector(&ctx) != STORAGE_OK) {
        teardown(&ctx); r.passed = 0;
        snprintf(r.metric, sizeof r.metric, "init_log_sector failed");
        unlink(LOOPBACK_IMG); return r;
    }

    /* Write 100 records, capturing LBA and write RC */
    uint64_t slba[100]; float stemp[100], shum[100]; uint8_t swrc[100];
    lxw_row_t row = 1;
    for (int i = 0; i < 100; i++) {
        uint64_t lb = 0; get_last_sector(&ctx, &lb);
        slba[i]  = lb + 1;
        stemp[i] = (float)(900 + i);
        shum[i]  = (float)(i % 100);
        sensor_t s = { .temp=stemp[i], .humidity=shum[i] };
        swrc[i]  = raid_sensor_values(&ctx, &s, 1);

        char wrc_s[8]; snprintf(wrc_s, sizeof wrc_s, "%u", swrc[i]);
        lxw_format *fmt = f->plain;
        worksheet_write_string(ws, row, 0, "WRITE",          fmt);
        worksheet_write_number(ws, row, 1, i+1,              fmt);
        worksheet_write_number(ws, row, 2, (double)slba[i],  fmt);
        worksheet_write_number(ws, row, 3, stemp[i],         fmt);
        worksheet_write_number(ws, row, 4, shum[i],          fmt);
        worksheet_write_string(ws, row, 5, wrc_s,            fmt);
        row++;
    }

    /* Verify 100 records */
    int matched = 0;
    uint8_t payload[PAYLOAD_SIZE];
    for (int i = 0; i < 100; i++) {
        uint8_t rrc = raid_read(&ctx, slba[i], payload);
        float gt=0.0f, gh=0.0f, tdiff=0.0f, hdiff=0.0f; int m=0;
        if (rrc == STORAGE_OK || rrc == STORAGE_WARN_DEGRADED) {
            gt    = unpack_f32(&payload[0]);
            gh    = unpack_f32(&payload[4]);
            tdiff = gt - stemp[i];
            hdiff = gh - shum[i];
            m     = fabsf(tdiff)<0.001f && fabsf(hdiff)<0.001f;
        }
        if (m) matched++;
        pass_assert(&r, m);

        lxw_format *fmt = xfmt(f, m);
        char rrc_s[8]; snprintf(rrc_s, sizeof rrc_s, "%u", rrc);
        worksheet_write_string(ws, row, 0, "VERIFY",         fmt);
        worksheet_write_number(ws, row, 1, i+1,              fmt);
        worksheet_write_number(ws, row, 2, (double)slba[i],  fmt);
        worksheet_write_number(ws, row, 3, stemp[i],         fmt);
        worksheet_write_number(ws, row, 4, shum[i],          fmt);
        worksheet_write_string(ws, row, 6, rrc_s,            fmt);
        worksheet_write_number(ws, row, 7, gt,               fmt);
        worksheet_write_number(ws, row, 8, gh,               fmt);
        worksheet_write_number(ws, row, 9, tdiff,            fmt);
        worksheet_write_number(ws, row,10, hdiff,            fmt);
        worksheet_write_string(ws, row,11, m?"yes":"NO",     fmt);
        worksheet_write_string(ws, row,12, m?"PASS":"FAIL",  fmt);
        row++;
    }

    teardown(&ctx);
    unlink(LOOPBACK_IMG);
    snprintf(r.metric, sizeof r.metric,
             "%d/100 matched assertions=%d passed=%d",
             matched, r.assertions, r.assertions_passed);
    return r;
}

/* =======================================================================
   main
   ======================================================================= */
int main(int argc, char *argv[]) {
    const char *out_prefix = "advanced_results";
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc) out_prefix = argv[++i];

    char xl_path[256];
    snprintf(xl_path, sizeof xl_path, "%s.xlsx", out_prefix);

    lxw_workbook *wb   = workbook_new(xl_path);
    xl_fmts_t     fmts = make_formats(wb);

    lxw_worksheet *ws_wipe  = workbook_add_worksheet(wb, "StorageWipe");
    lxw_worksheet *ws_deg   = workbook_add_worksheet(wb, "DegradedWrite");
    lxw_worksheet *ws_blk   = workbook_add_worksheet(wb, "BlacklistOverflow");
    lxw_worksheet *ws_meta  = workbook_add_worksheet(wb, "MetadataCorruption");
    lxw_worksheet *ws_ver   = workbook_add_worksheet(wb, "VersionWrap");
    lxw_worksheet *ws_scrub = workbook_add_worksheet(wb, "FullRangeScrub");
    lxw_worksheet *ws_loop  = workbook_add_worksheet(wb, "Loopback");
    lxw_worksheet *ws_sum   = workbook_add_worksheet(wb, "Summary");

    result_t results[7];
    printf("ZINF advanced tests\n");

#define RUN(idx, label, fn, ws) do { \
    printf("  [%d/7] %-22s", (idx)+1, (label)); fflush(stdout); \
    results[idx] = fn((ws), &fmts); \
    printf("%s\n", results[idx].passed ? "PASS" : "FAIL"); \
} while(0)

    RUN(0, "StorageWipe",        test_storage_wipe,         ws_wipe);
    RUN(1, "DegradedWrite",      test_degraded_write,       ws_deg);
    RUN(2, "BlacklistOverflow",  test_blacklist_overflow,   ws_blk);
    RUN(3, "MetadataCorruption", test_metadata_corruption,  ws_meta);
    RUN(4, "VersionWrap",        test_version_wraparound,   ws_ver);
    RUN(5, "FullRangeScrub",     test_full_range_scrub,     ws_scrub);
    RUN(6, "Loopback",           test_loopback_integrity,   ws_loop);
#undef RUN

    /* Summary sheet */
    const char *sum_hdrs[] = {
        "#","Test","Description","Result",
        "Total Assertions","Passed","Failed","Metric"
    };
    for (int c = 0; c < 8; c++)
        worksheet_write_string(ws_sum, 0, (lxw_col_t)c, sum_hdrs[c], fmts.hdr);

    const double sum_widths[] = { 4, 20, 60, 8, 17, 8, 8, 60 };
    set_col_widths(ws_sum, sum_widths, 8);
    worksheet_freeze_panes(ws_sum, 1, 0);

    int any_fail = 0;
    for (int i = 0; i < 7; i++) {
        int failed = results[i].assertions - results[i].assertions_passed;
        lxw_format *fmt = xfmt(&fmts, results[i].passed);
        worksheet_write_number(ws_sum, (lxw_row_t)(i+1), 0, i+1,                        fmt);
        worksheet_write_string(ws_sum, (lxw_row_t)(i+1), 1, results[i].name,            fmt);
        worksheet_write_string(ws_sum, (lxw_row_t)(i+1), 2, results[i].description,     fmt);
        worksheet_write_string(ws_sum, (lxw_row_t)(i+1), 3, results[i].passed?"PASS":"FAIL", fmt);
        worksheet_write_number(ws_sum, (lxw_row_t)(i+1), 4, results[i].assertions,      fmt);
        worksheet_write_number(ws_sum, (lxw_row_t)(i+1), 5, results[i].assertions_passed,fmt);
        worksheet_write_number(ws_sum, (lxw_row_t)(i+1), 6, failed,                     fmt);
        worksheet_write_string(ws_sum, (lxw_row_t)(i+1), 7, results[i].metric,          fmt);
        if (!results[i].passed) any_fail = 1;
    }

    workbook_close(wb);

    printf("\nResults → %s\n", xl_path);
    printf("Overall: %s\n", any_fail ? "FAIL" : "PASS");
    return any_fail ? 1 : 0;
}
