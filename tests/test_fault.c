#define _GNU_SOURCE
#include "framework.h"
#include "api.h"
#include "config.h"
#include "linux_driver.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* -----------------------------------------------------------------------
   Fault injection helpers
   Directly pwrite() into the image file to corrupt specific sectors.
   ----------------------------------------------------------------------- */

#define TEST_IMG_PATH   "/var/tmp/zinf_fault.img"
#define TEST_IMG_SECTS  4096u

static zinf_ctx_t g_fault_ctx;

static int fault_setup(uint8_t mirror_count) {
    int fd = open(TEST_IMG_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) { perror("open fault img"); return -1; }
    if (ftruncate(fd, (off_t)TEST_IMG_SECTS * SECTOR_SIZE) != 0) {
        perror("ftruncate"); close(fd); return -1;
    }
    close(fd);

    linux_driver_set_path(TEST_IMG_PATH);

    memset(&g_fault_ctx, 0, sizeof(g_fault_ctx));
    g_fault_ctx.driver           = &linux_driver;
    g_fault_ctx.sector_size      = SECTOR_SIZE;
    g_fault_ctx.mirror_count     = mirror_count;
    g_fault_ctx.metadata_sectors = 2;
    g_fault_ctx.mirror_offset    = (TEST_IMG_SECTS - 2u) / (uint32_t)mirror_count;
    g_fault_ctx.log_sector       = 0;
    g_fault_ctx.raid_offset      = g_fault_ctx.mirror_offset;

    int rc = linux_driver.init(&linux_driver);
    if (rc != DRIVER_OK) return -1;

    if (init_log_sector(&g_fault_ctx) != STORAGE_OK) return -1;
    return 0;
}

static void fault_teardown(void) {
    if (linux_driver.deinit) linux_driver.deinit(&linux_driver);
    unlink(TEST_IMG_PATH);
}

/* Corrupt `count` bytes starting at byte offset `off` inside a sector LBA */
static int corrupt_sector_bytes(uint32_t lba, uint32_t byte_off, uint32_t count) {
    int fd = open(TEST_IMG_PATH, O_RDWR);
    if (fd < 0) return -1;

    off_t file_off = (off_t)lba * SECTOR_SIZE + (off_t)byte_off;
    uint8_t zeroes[SECTOR_SIZE];
    memset(zeroes, 0xDE, count < SECTOR_SIZE ? count : SECTOR_SIZE);

    ssize_t w = pwrite(fd, zeroes, count, file_off);
    close(fd);
    return (w == (ssize_t)count) ? 0 : -1;
}

/* -----------------------------------------------------------------------
   Fault scenarios
   ----------------------------------------------------------------------- */

/* 1. Single-byte bitflip in mirror 0 → raid_read falls back to mirror 1 */
static void test_fault_single_mirror_bitflip(void) {
    TEST_BEGIN("fault_single_mirror_bitflip");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 42.0f, .humidity = 80.0f };
    uint8_t rc = raid_sensor_values(&g_fault_ctx, &s, 1);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Data sector for mirror 0 is at logical sector 2 */
    uint32_t mirror0_lba = 2u;
    /* Corrupt payload byte 2 — this invalidates the CRC */
    int corrupt_rc = corrupt_sector_bytes(mirror0_lba, 2, 1);
    ASSERT_EQ(corrupt_rc, 0);

    /* raid_read should still succeed via mirror 1 */
    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 2u, payload);
    ASSERT_EQ(rc, STORAGE_OK);

    fault_teardown();
    TEST_END("fault_single_mirror_bitflip");
}

/* 2. Torn write — zero out second half of a sector in mirror 0 */
static void test_fault_torn_write(void) {
    TEST_BEGIN("fault_torn_write_mirror0");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 15.0f, .humidity = 50.0f };
    uint8_t rc = raid_sensor_values(&g_fault_ctx, &s, 1);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Zero the second half of sector 2 (mirror 0) → CRC mismatch */
    int corrupt_rc = corrupt_sector_bytes(2u, SECTOR_SIZE / 2u, SECTOR_SIZE / 2u);
    ASSERT_EQ(corrupt_rc, 0);

    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 2u, payload);
    ASSERT_EQ(rc, STORAGE_OK);  /* mirror 1 should be intact */

    fault_teardown();
    TEST_END("fault_torn_write_mirror0");
}

/* 3. All mirrors bad → raid_read must return STORAGE_ERR_UNRECOVERABLE */
static void test_fault_all_mirrors_bad(void) {
    TEST_BEGIN("fault_all_mirrors_bad");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 9.0f, .humidity = 30.0f };
    uint8_t rc = raid_sensor_values(&g_fault_ctx, &s, 1);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Corrupt both mirrors at sector 2 */
    uint32_t mirror0_lba = 2u;
    uint32_t mirror1_lba = 2u + g_fault_ctx.mirror_offset;

    ASSERT_EQ(corrupt_sector_bytes(mirror0_lba, 2, 4), 0);
    ASSERT_EQ(corrupt_sector_bytes(mirror1_lba, 2, 4), 0);

    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 2u, payload);
    ASSERT_EQ(rc, STORAGE_ERR_UNRECOVERABLE);

    fault_teardown();
    TEST_END("fault_all_mirrors_bad");
}

/* 4. Majority voting with 3 mirrors: corrupt mirror 0, others agree → OK */
static void test_fault_majority_voting(void) {
    TEST_BEGIN("fault_majority_voting_3mirrors");

    int setup_rc = fault_setup(3);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 7.0f, .humidity = 65.0f };
    uint8_t rc = raid_sensor_values(&g_fault_ctx, &s, 1);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Corrupt only mirror 0 */
    ASSERT_EQ(corrupt_sector_bytes(2u, 2, 4), 0);

    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 2u, payload);
    ASSERT_EQ(rc, STORAGE_OK);  /* mirrors 1 and 2 have majority */

    fault_teardown();
    TEST_END("fault_majority_voting_3mirrors");
}

/* 5. Metadata corruption: corrupt two of three copy slots → third wins */
static void test_fault_metadata_partial_corruption(void) {
    TEST_BEGIN("fault_metadata_partial_corruption");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    /* Write enough sectors to exercise all 3 metadata copy slots */
    sensor_t s = { .temp = 1.0f, .humidity = 1.0f };
    for (int i = 0; i < 5; i++) {
        uint8_t rc = raid_sensor_values(&g_fault_ctx, &s, 1);
        ASSERT_EQ(rc, STORAGE_OK);
    }

    /* Corrupt copy slots 0 and 1 (first 12 bytes of sector 0) */
    ASSERT_EQ(corrupt_sector_bytes(0u, 0u, 12u), 0);

    /* log_get_last_sector should still return the value from slot 2 */
    uint64_t last = 0;
    uint8_t rc = get_last_sector(&g_fault_ctx, &last);
    ASSERT_EQ(rc, STORAGE_OK);
    ASSERT_NE(last, 0u);

    fault_teardown();
    TEST_END("fault_metadata_partial_corruption");
}

/* -----------------------------------------------------------------------
   Blacklist unit tests — pure ctx manipulation, no I/O
   ----------------------------------------------------------------------- */

/* 6. Mark an LBA bad; is_bad returns true for it, false for others */
static void test_blacklist_mark_and_check(void) {
    TEST_BEGIN("blacklist_mark_and_check");

    zinf_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ASSERT_EQ(zinf_is_bad_sector(&ctx, 42u), false);
    ASSERT_EQ(zinf_mark_bad_sector(&ctx, 42u), STORAGE_OK);
    ASSERT_EQ(zinf_is_bad_sector(&ctx, 42u), true);
    ASSERT_EQ(zinf_is_bad_sector(&ctx, 43u), false);
    /* Marking twice returns OK and does not duplicate */
    ASSERT_EQ(zinf_mark_bad_sector(&ctx, 42u), STORAGE_OK);
    ASSERT_EQ(ctx.bad_sector_count, 1u);

    TEST_END("blacklist_mark_and_check");
}

/* 7. Fill the blacklist to MAX_BAD_SECTORS; the next mark returns STORAGE_ERR_PARAM */
static void test_blacklist_overflow(void) {
    TEST_BEGIN("blacklist_overflow");

    zinf_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    for (uint8_t i = 0; i < MAX_BAD_SECTORS; i++)
        ASSERT_EQ(zinf_mark_bad_sector(&ctx, (uint64_t)i + 100u), STORAGE_OK);

    ASSERT_EQ(ctx.bad_sector_count, MAX_BAD_SECTORS);
    /* One more — must fail */
    ASSERT_EQ(zinf_mark_bad_sector(&ctx, 999u), STORAGE_ERR_PARAM);

    TEST_END("blacklist_overflow");
}

/* 8. Mark sectors, clear, verify none are bad */
static void test_blacklist_clear(void) {
    TEST_BEGIN("blacklist_clear");

    zinf_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ASSERT_EQ(zinf_mark_bad_sector(&ctx, 10u), STORAGE_OK);
    ASSERT_EQ(zinf_mark_bad_sector(&ctx, 20u), STORAGE_OK);
    ASSERT_EQ(zinf_is_bad_sector(&ctx, 10u), true);

    zinf_clear_bad_sectors(&ctx);
    ASSERT_EQ(ctx.bad_sector_count, 0u);
    ASSERT_EQ(zinf_is_bad_sector(&ctx, 10u), false);
    ASSERT_EQ(zinf_is_bad_sector(&ctx, 20u), false);

    TEST_END("blacklist_clear");
}

/* -----------------------------------------------------------------------
   Health-check tests
   ----------------------------------------------------------------------- */

/* 9. Write one sector; check_sector reports all mirrors OK */
static void test_check_sector_all_ok(void) {
    TEST_BEGIN("check_sector_all_ok");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 10.0f, .humidity = 50.0f };
    ASSERT_EQ(raid_sensor_values(&g_fault_ctx, &s, 1), STORAGE_OK);

    zinf_sector_health_t h;
    ASSERT_EQ(zinf_check_sector(&g_fault_ctx, 2u, &h), STORAGE_OK);
    ASSERT_EQ(h.valid_count, 2u);
    ASSERT_EQ(h.status[0], ZINF_MIRROR_OK);
    ASSERT_EQ(h.status[1], ZINF_MIRROR_OK);

    fault_teardown();
    TEST_END("check_sector_all_ok");
}

/* 10. Write, corrupt mirror 0; check_sector reports CRC_FAIL on mirror 0 only */
static void test_check_sector_one_corrupt(void) {
    TEST_BEGIN("check_sector_one_corrupt");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 20.0f, .humidity = 60.0f };
    ASSERT_EQ(raid_sensor_values(&g_fault_ctx, &s, 1), STORAGE_OK);

    ASSERT_EQ(corrupt_sector_bytes(2u, 2u, 4u), 0);

    zinf_sector_health_t h;
    ASSERT_EQ(zinf_check_sector(&g_fault_ctx, 2u, &h), STORAGE_OK);
    ASSERT_EQ(h.valid_count, 1u);
    ASSERT_EQ(h.status[0], ZINF_MIRROR_CRC_FAIL);
    ASSERT_EQ(h.status[1], ZINF_MIRROR_OK);

    fault_teardown();
    TEST_END("check_sector_one_corrupt");
}

/* 11. Blacklist mirror 0 LBA; check_sector reports BLACKLIST for that mirror */
static void test_check_sector_blacklisted_mirror(void) {
    TEST_BEGIN("check_sector_blacklisted_mirror");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 30.0f, .humidity = 70.0f };
    ASSERT_EQ(raid_sensor_values(&g_fault_ctx, &s, 1), STORAGE_OK);

    /* Mirror 0 physical LBA = 2 + 0 * mirror_offset = 2 */
    ASSERT_EQ(zinf_mark_bad_sector(&g_fault_ctx, 2u), STORAGE_OK);

    zinf_sector_health_t h;
    ASSERT_EQ(zinf_check_sector(&g_fault_ctx, 2u, &h), STORAGE_OK);
    ASSERT_EQ(h.status[0], ZINF_MIRROR_BLACKLIST);
    ASSERT_EQ(h.status[1], ZINF_MIRROR_OK);
    ASSERT_EQ(h.valid_count, 1u);

    fault_teardown();
    TEST_END("check_sector_blacklisted_mirror");
}

/* -----------------------------------------------------------------------
   Recovery tests
   ----------------------------------------------------------------------- */

/* 12. Write, corrupt mirror 0, recover; mirror 0 passes CRC after repair */
static void test_recover_repairs_bad_mirror(void) {
    TEST_BEGIN("recover_repairs_bad_mirror");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 5.0f, .humidity = 40.0f };
    ASSERT_EQ(raid_sensor_values(&g_fault_ctx, &s, 1), STORAGE_OK);

    ASSERT_EQ(corrupt_sector_bytes(2u, 2u, 4u), 0);

    /* Confirm mirror 0 is bad */
    zinf_sector_health_t before;
    ASSERT_EQ(zinf_check_sector(&g_fault_ctx, 2u, &before), STORAGE_OK);
    ASSERT_EQ(before.status[0], ZINF_MIRROR_CRC_FAIL);

    ASSERT_EQ(zinf_recover_sector(&g_fault_ctx, 2u), STORAGE_OK);

    /* Both mirrors should now pass CRC */
    zinf_sector_health_t after;
    ASSERT_EQ(zinf_check_sector(&g_fault_ctx, 2u, &after), STORAGE_OK);
    ASSERT_EQ(after.valid_count, 2u);
    ASSERT_EQ(after.status[0], ZINF_MIRROR_OK);
    ASSERT_EQ(after.status[1], ZINF_MIRROR_OK);

    fault_teardown();
    TEST_END("recover_repairs_bad_mirror");
}

/* 13. Corrupt all mirrors; recover returns STORAGE_ERR_UNRECOVERABLE
        and both physical LBAs end up in the blacklist */
static void test_recover_unrecoverable(void) {
    TEST_BEGIN("recover_unrecoverable");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 3.0f, .humidity = 20.0f };
    ASSERT_EQ(raid_sensor_values(&g_fault_ctx, &s, 1), STORAGE_OK);

    uint64_t mirror0_lba = 2u;
    uint64_t mirror1_lba = 2u + g_fault_ctx.mirror_offset;
    ASSERT_EQ(corrupt_sector_bytes((uint32_t)mirror0_lba, 2u, 4u), 0);
    ASSERT_EQ(corrupt_sector_bytes((uint32_t)mirror1_lba, 2u, 4u), 0);

    ASSERT_EQ(zinf_recover_sector(&g_fault_ctx, 2u), STORAGE_ERR_UNRECOVERABLE);

    /* Both LBAs must now be blacklisted */
    ASSERT_EQ(zinf_is_bad_sector(&g_fault_ctx, mirror0_lba), true);
    ASSERT_EQ(zinf_is_bad_sector(&g_fault_ctx, mirror1_lba), true);

    fault_teardown();
    TEST_END("recover_unrecoverable");
}

/* -----------------------------------------------------------------------
   Write-path blacklist test
   ----------------------------------------------------------------------- */

/* 14. Blacklist mirror 0 LBA; write returns STORAGE_WARN_DEGRADED;
        mirror 0 sector is untouched, mirror 1 has valid data */
static void test_write_skips_blacklisted_mirror(void) {
    TEST_BEGIN("write_skips_blacklisted_mirror");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    /* Mirror 0 for logical sector 2 is physical LBA 2 */
    uint64_t mirror0_lba = 2u;
    ASSERT_EQ(zinf_mark_bad_sector(&g_fault_ctx, mirror0_lba), STORAGE_OK);

    sensor_t s = { .temp = 99.0f, .humidity = 11.0f };
    uint8_t  rc = raid_sensor_values(&g_fault_ctx, &s, 1);
    ASSERT_EQ(rc, STORAGE_WARN_DEGRADED);

    /* Mirror 0 sector should be all zeros (never written) */
    uint8_t buf0[SECTOR_SIZE];
    ASSERT_EQ(read_sector(&g_fault_ctx, mirror0_lba, buf0), DRIVER_OK);
    uint8_t all_zero = 1u;
    for (uint32_t i = 0; i < SECTOR_SIZE; i++)
        if (buf0[i] != 0u) { all_zero = 0u; break; }
    ASSERT_EQ(all_zero, 1u);

    /* Mirror 1 should have valid CRC */
    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 2u, payload);
    ASSERT_EQ(rc, STORAGE_OK);

    fault_teardown();
    TEST_END("write_skips_blacklisted_mirror");
}

/* -----------------------------------------------------------------------
   Scrub test
   ----------------------------------------------------------------------- */

/* 15. Write 4 logical sectors; corrupt mirror 0 in sectors 2 and 4;
        scrub [2,5]; verify report and that repaired sectors pass CRC */
static void test_scrub_range(void) {
    TEST_BEGIN("scrub_range");

    int setup_rc = fault_setup(2);
    ASSERT_EQ(setup_rc, 0);

    sensor_t s = { .temp = 25.0f, .humidity = 65.0f };
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(raid_sensor_values(&g_fault_ctx, &s, 1), STORAGE_OK);

    /* Corrupt mirror 0 of logical sectors 2 and 4
       (physical mirror 0 LBA = logical sector index; data starts at LBA 2) */
    ASSERT_EQ(corrupt_sector_bytes(2u, 2u, 4u), 0);  /* sector 2, mirror 0 */
    ASSERT_EQ(corrupt_sector_bytes(4u, 2u, 4u), 0);  /* sector 4, mirror 0 */

    zinf_scrub_report_t report;
    uint8_t rc = zinf_scrub(&g_fault_ctx, 2u, 5u, &report);
    ASSERT_EQ(rc, STORAGE_OK);
    ASSERT_EQ(report.checked,       4u);
    ASSERT_EQ(report.repaired,      2u);
    ASSERT_EQ(report.healthy,       2u);
    ASSERT_EQ(report.unrecoverable, 0u);

    /* All four sectors should now have all mirrors healthy */
    for (uint64_t logical = 2u; logical <= 5u; logical++) {
        zinf_sector_health_t h;
        ASSERT_EQ(zinf_check_sector(&g_fault_ctx, logical, &h), STORAGE_OK);
        ASSERT_EQ(h.valid_count, 2u);
    }

    fault_teardown();
    TEST_END("scrub_range");
}

void run_fault_tests(void) {
    printf("\n--- Fault injection tests ---\n");
    test_fault_single_mirror_bitflip();
    test_fault_torn_write();
    test_fault_all_mirrors_bad();
    test_fault_majority_voting();
    test_fault_metadata_partial_corruption();
    /* Blacklist unit tests */
    test_blacklist_mark_and_check();
    test_blacklist_overflow();
    test_blacklist_clear();
    /* Health-check tests */
    test_check_sector_all_ok();
    test_check_sector_one_corrupt();
    test_check_sector_blacklisted_mirror();
    /* Recovery tests */
    test_recover_repairs_bad_mirror();
    test_recover_unrecoverable();
    /* Write-path blacklist test */
    test_write_skips_blacklisted_mirror();
    /* Scrub test */
    test_scrub_range();
}
