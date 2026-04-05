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

    /* Data sector for mirror 0 is at logical sector 1 */
    uint32_t mirror0_lba = 1u;
    /* Corrupt payload byte 2 — this invalidates the CRC */
    int corrupt_rc = corrupt_sector_bytes(mirror0_lba, 2, 1);
    ASSERT_EQ(corrupt_rc, 0);

    /* raid_read should still succeed via mirror 1 */
    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 1u, payload);
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

    /* Zero the second half of sector 1 (mirror 0) → CRC mismatch */
    int corrupt_rc = corrupt_sector_bytes(1u, SECTOR_SIZE / 2u, SECTOR_SIZE / 2u);
    ASSERT_EQ(corrupt_rc, 0);

    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 1u, payload);
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

    /* Corrupt both mirrors at sector 1 */
    uint32_t mirror0_lba = 1u;
    uint32_t mirror1_lba = 1u + g_fault_ctx.mirror_offset;

    ASSERT_EQ(corrupt_sector_bytes(mirror0_lba, 2, 4), 0);
    ASSERT_EQ(corrupt_sector_bytes(mirror1_lba, 2, 4), 0);

    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 1u, payload);
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
    ASSERT_EQ(corrupt_sector_bytes(1u, 2, 4), 0);

    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_fault_ctx, 1u, payload);
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

void run_fault_tests(void) {
    printf("\n--- Fault injection tests ---\n");
    test_fault_single_mirror_bitflip();
    test_fault_torn_write();
    test_fault_all_mirrors_bad();
    test_fault_majority_voting();
    test_fault_metadata_partial_corruption();
}
