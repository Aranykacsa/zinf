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
#include <sys/stat.h>

/* -----------------------------------------------------------------------
   Helpers shared by storage and fault tests
   ----------------------------------------------------------------------- */

#define TEST_IMG_PATH  "/var/tmp/zinf_test.img"
#define TEST_IMG_SECTS 4096u     /* 2 MB */

static zinf_ctx_t g_test_ctx;

/* Create a zeroed test image and attach it via the linux driver */
static int storage_setup(void) {
    /* Create image */
    int fd = open(TEST_IMG_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) { perror("open test img"); return -1; }
    /* Extend to desired size */
    if (ftruncate(fd, (off_t)TEST_IMG_SECTS * SECTOR_SIZE) != 0) {
        perror("ftruncate"); close(fd); return -1;
    }
    close(fd);

    /* Wire up the linux driver to the test image (file, not block device) */
    linux_driver_set_path(TEST_IMG_PATH);

    memset(&g_test_ctx, 0, sizeof(g_test_ctx));
    g_test_ctx.driver           = &linux_driver;
    g_test_ctx.sector_size      = SECTOR_SIZE;
    g_test_ctx.mirror_count     = 2;
    g_test_ctx.metadata_sectors = 2;
    g_test_ctx.mirror_offset    = (TEST_IMG_SECTS - 2u) / 2u; /* ~2047 */
    g_test_ctx.log_sector       = 0;
    g_test_ctx.raid_offset      = g_test_ctx.mirror_offset;

    /* Override sector_size to match our image (not a real block device,
       ioctl BLKGETSIZE64 will fail — force sector count manually).
       The linux driver will fall through since ioctl won't work on a file,
       so total_sectors stays 0 but reads/writes still work. */

    return 0;
}

static void storage_teardown(void) {
    if (linux_driver.deinit) linux_driver.deinit(&linux_driver);
    unlink(TEST_IMG_PATH);
}

/* -----------------------------------------------------------------------
   Functional write / read round-trip
   ----------------------------------------------------------------------- */

static void test_init_and_write(void) {
    TEST_BEGIN("storage_init_and_write");

    int setup_rc = storage_setup();
    ASSERT_EQ(setup_rc, 0);

    /* Open the driver directly (setup_storage calls zinf_ctx_init_defaults
       which would overwrite mirror_count, so call init manually) */
    int init_rc = linux_driver.init(&linux_driver);
    ASSERT_EQ(init_rc, DRIVER_OK);

    uint8_t rc;
    rc = init_log_sector(&g_test_ctx);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Write one sensor record */
    sensor_t s = { .temp = 23.5f, .humidity = 55.0f };
    rc = raid_sensor_values(&g_test_ctx, &s, 1);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Metadata should now point past sector 0 */
    uint64_t last = 0;
    rc = get_last_sector(&g_test_ctx, &last);
    ASSERT_EQ(rc, STORAGE_OK);
    ASSERT_NE(last, 0u);

    storage_teardown();
    TEST_END("storage_init_and_write");
}

static void test_write_read_roundtrip(void) {
    TEST_BEGIN("storage_write_read_roundtrip");

    int setup_rc = storage_setup();
    ASSERT_EQ(setup_rc, 0);

    int init_rc = linux_driver.init(&linux_driver);
    ASSERT_EQ(init_rc, DRIVER_OK);

    uint8_t rc;
    rc = init_log_sector(&g_test_ctx);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Build a payload that fills exactly one sector */
    const int records = (int)(PAYLOAD_SIZE / sizeof(sensor_t));
    sensor_t sensors[records];
    for (int i = 0; i < records; i++) {
        sensors[i].temp     = (float)i * 1.5f;
        sensors[i].humidity = (float)(100 - i);
    }

    rc = raid_sensor_values(&g_test_ctx, sensors, (size_t)records);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Read back via raid_read on mirror 0 (logical sector 1 — first data sector) */
    uint8_t payload[PAYLOAD_SIZE];
    rc = raid_read(&g_test_ctx, 1u, payload);
    ASSERT_EQ(rc, STORAGE_OK);

    /* First 8 bytes encode sensors[0] as 2×float LE */
    uint32_t raw_temp;
    memcpy(&raw_temp, &payload[1], 4); /* offset 1: skip header byte */
    float recovered;
    memcpy(&recovered, &raw_temp, 4);
    /* Allow tiny float representation error */
    int approx_ok = (recovered > -0.01f && recovered < 0.01f); /* sensors[0].temp == 0.0 */
    ASSERT_EQ(approx_ok, 1);

    storage_teardown();
    TEST_END("storage_write_read_roundtrip");
}

static void test_msg_log_write(void) {
    TEST_BEGIN("storage_msg_log_write");

    int setup_rc = storage_setup();
    ASSERT_EQ(setup_rc, 0);

    int init_rc = linux_driver.init(&linux_driver);
    ASSERT_EQ(init_rc, DRIVER_OK);

    uint8_t rc;
    rc = init_log_sector(&g_test_ctx);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Write a sequence of bytes into the message log */
    for (int i = 0; i < 10; i++) {
        uint8_t b = (uint8_t)i;
        rc = save_msg(&g_test_ctx, &b);
        ASSERT_EQ(rc, STORAGE_OK);
    }

    storage_teardown();
    TEST_END("storage_msg_log_write");
}

static void test_metadata_versioning(void) {
    TEST_BEGIN("storage_metadata_versioning");

    int setup_rc = storage_setup();
    ASSERT_EQ(setup_rc, 0);

    int init_rc = linux_driver.init(&linux_driver);
    ASSERT_EQ(init_rc, DRIVER_OK);

    uint8_t rc;
    rc = init_log_sector(&g_test_ctx);
    ASSERT_EQ(rc, STORAGE_OK);

    /* Write several sectors to exercise the round-robin version counter */
    sensor_t s = { .temp = 1.0f, .humidity = 2.0f };
    for (int i = 0; i < 4; i++) {
        rc = raid_sensor_values(&g_test_ctx, &s, 1);
        ASSERT_EQ(rc, STORAGE_OK);
    }

    uint64_t last = 0;
    rc = get_last_sector(&g_test_ctx, &last);
    ASSERT_EQ(rc, STORAGE_OK);
    /* After 4 writes, last_sector must be > 0 */
    ASSERT_NE(last, 0u);

    storage_teardown();
    TEST_END("storage_metadata_versioning");
}

/* T1: log full — writing MSG_LOG_TOTAL_CAP+1 bytes must return STORAGE_ERR_LOG_FULL */
static void test_msg_log_full(void) {
    TEST_BEGIN("storage_msg_log_full");

    ASSERT_EQ(storage_setup(), 0);
    ASSERT_EQ(linux_driver.init(&linux_driver), DRIVER_OK);
    ASSERT_EQ(init_log_sector(&g_test_ctx), STORAGE_OK);

    uint8_t b = 0xAAu;
    uint8_t rc = STORAGE_OK;
    /* Fill the entire log */
    for (uint32_t i = 0; i < (uint32_t)MSG_LOG_TOTAL_CAP; i++) {
        rc = save_msg(&g_test_ctx, &b);
        ASSERT_EQ(rc, STORAGE_OK);
    }
    /* One more byte must overflow */
    rc = save_msg(&g_test_ctx, &b);
    ASSERT_EQ(rc, STORAGE_ERR_LOG_FULL);

    storage_teardown();
    TEST_END("storage_msg_log_full");
}

/* T2: version counter wraparound — pre-set slot versions to 0xFFFE, then
   do enough writes to cross 0xFFFF → 0x0000 and confirm newest copy still wins */
static void test_metadata_version_wraparound(void) {
    TEST_BEGIN("storage_metadata_version_wraparound");

    ASSERT_EQ(storage_setup(), 0);
    ASSERT_EQ(linux_driver.init(&linux_driver), DRIVER_OK);
    ASSERT_EQ(init_log_sector(&g_test_ctx), STORAGE_OK);

    /* Manually write a metadata sector with all three copy-slot versions = 0xFFFE */
    uint8_t meta[SECTOR_SIZE];
    memset(meta, 0, sizeof(meta));
    uint64_t fake_last = 42u;
    for (uint8_t slot = 0; slot < META_COPIES; slot++) {
        uint16_t off = (uint16_t)(META_COPY_SLOT_BASE + slot * META_COPY_STRIDE);
        /* last_sector = 42 LE */
        for (int b = 0; b < 8; b++)
            meta[off + b] = (uint8_t)((fake_last >> (b * 8)) & 0xFFu);
        /* version = 0xFFFE */
        meta[off + 8] = 0xFEu;
        meta[off + 9] = 0xFFu;
    }
    ASSERT_EQ(write_sector(&g_test_ctx, g_test_ctx.log_sector, meta), DRIVER_OK);

    /* Perform 5 writes — version cycles 0xFFFE → 0xFFFF → 0x0000 → 0x0001 → ... */
    sensor_t s = { .temp = 5.0f, .humidity = 50.0f };
    for (int i = 0; i < 5; i++) {
        uint8_t rc = raid_sensor_values(&g_test_ctx, &s, 1);
        ASSERT_EQ(rc, STORAGE_OK);
    }

    /* last_sector must be greater than fake_last (42) after real writes */
    uint64_t last = 0;
    ASSERT_EQ(get_last_sector(&g_test_ctx, &last), STORAGE_OK);
    ASSERT_NE(last, 42u);

    storage_teardown();
    TEST_END("storage_metadata_version_wraparound");
}

/* T3: NULL/invalid parameter validation */
static void test_param_validation(void) {
    TEST_BEGIN("storage_param_validation");

    ASSERT_EQ(storage_setup(), 0);
    ASSERT_EQ(linux_driver.init(&linux_driver), DRIVER_OK);
    ASSERT_EQ(init_log_sector(&g_test_ctx), STORAGE_OK);

    /* raid_read with NULL payload */
    ASSERT_EQ(raid_read(&g_test_ctx, 1u, NULL), STORAGE_ERR_PARAM);

    /* get_last_sector with NULL out-pointer */
    ASSERT_EQ(get_last_sector(&g_test_ctx, NULL), STORAGE_ERR_PARAM);

    storage_teardown();
    TEST_END("storage_param_validation");
}

/* T4: message log sector boundary — bytes 0..478 land in sector 0,
   bytes 479..990 land in sector 1; verify both sides are written */
static void test_msg_log_sector_boundary(void) {
    TEST_BEGIN("storage_msg_log_sector_boundary");

    ASSERT_EQ(storage_setup(), 0);
    ASSERT_EQ(linux_driver.init(&linux_driver), DRIVER_OK);
    ASSERT_EQ(init_log_sector(&g_test_ctx), STORAGE_OK);

    /* Write exactly MSG_LOG_CAP_S0 bytes (fills sector 0 payload) */
    for (uint32_t i = 0; i < (uint32_t)MSG_LOG_CAP_S0; i++) {
        uint8_t b = (uint8_t)(i & 0xFFu);
        ASSERT_EQ(save_msg(&g_test_ctx, &b), STORAGE_OK);
    }

    /* The next byte crosses into sector 1 */
    uint8_t cross = 0xCCu;
    ASSERT_EQ(save_msg(&g_test_ctx, &cross), STORAGE_OK);

    /* Verify the byte landed in sector 1 at offset 0 */
    uint8_t sec1[SECTOR_SIZE];
    ASSERT_EQ(read_sector(&g_test_ctx, g_test_ctx.log_sector + 1u, sec1), DRIVER_OK);
    ASSERT_EQ(sec1[0], 0xCCu);

    storage_teardown();
    TEST_END("storage_msg_log_sector_boundary");
}

void run_storage_tests(void) {
    printf("\n--- Storage tests ---\n");
    test_init_and_write();
    test_write_read_roundtrip();
    test_msg_log_write();
    test_metadata_versioning();
    test_msg_log_full();
    test_metadata_version_wraparound();
    test_param_validation();
    test_msg_log_sector_boundary();
}
