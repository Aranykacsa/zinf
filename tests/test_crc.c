#include "framework.h"
#include "helper.h"
#include "config.h"
#include <stdint.h>
#include <string.h>

/* Known-good CRC-32 values (Ethernet/ZIP poly 0xEDB88320).
   Reference values computed via Python: binascii.zinf_crc32(data) & 0xFFFFFFFF */

static void test_crc_empty(void) {
    TEST_BEGIN("crc32_empty");
    /* zinf_crc32(b"") == 0x00000000 */
    uint32_t got = zinf_crc32(NULL, 0);
    ASSERT_EQ(got, 0x00000000u);
    TEST_END("crc32_empty");
}

static void test_crc_single_byte(void) {
    TEST_BEGIN("crc32_single_byte_0x00");
    uint8_t data = 0x00;
    /* Python: binascii.zinf_crc32(b'\x00') & 0xFFFFFFFF == 0xD202EF8D */
    uint32_t got = zinf_crc32(&data, 1);
    ASSERT_EQ(got, 0xD202EF8Du);
    TEST_END("crc32_single_byte_0x00");
}

static void test_crc_known_string(void) {
    TEST_BEGIN("crc32_known_string");
    /* Python: binascii.zinf_crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926 */
    const uint8_t s[] = "123456789";
    uint32_t got = zinf_crc32(s, 9);
    ASSERT_EQ(got, 0xCBF43926u);
    TEST_END("crc32_known_string");
}

static void test_crc_full_sector_payload(void) {
    TEST_BEGIN("crc32_full_payload_size");
    /* Fill PAYLOAD_SIZE bytes with a known pattern, verify result is stable */
    uint8_t buf[HEADER_SIZE + PAYLOAD_SIZE];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i & 0xFFu);

    uint32_t first  = zinf_crc32(buf, HEADER_SIZE + PAYLOAD_SIZE);
    uint32_t second = zinf_crc32(buf, HEADER_SIZE + PAYLOAD_SIZE);
    /* Must be deterministic */
    ASSERT_EQ(first, second);
    /* Must change when data changes */
    buf[0] ^= 0xFFu;
    uint32_t third = zinf_crc32(buf, HEADER_SIZE + PAYLOAD_SIZE);
    ASSERT_NE(first, third);
    TEST_END("crc32_full_payload_size");
}

static void test_crc_incremental_sensitivity(void) {
    TEST_BEGIN("crc32_bit_sensitivity");
    uint8_t buf[16];
    memset(buf, 0xAA, sizeof(buf));
    uint32_t base = zinf_crc32(buf, sizeof(buf));
    /* Flipping any single bit must change the CRC */
    int all_different = 1;
    for (int byte = 0; byte < 16; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (uint8_t)(1u << bit);
            uint32_t altered = zinf_crc32(buf, sizeof(buf));
            if (altered == base) { all_different = 0; break; }
            buf[byte] ^= (uint8_t)(1u << bit); /* restore */
        }
        if (!all_different) break;
    }
    ASSERT_EQ(all_different, 1);
    TEST_END("crc32_bit_sensitivity");
}

void run_crc_tests(void) {
    printf("\n--- CRC tests ---\n");
    test_crc_empty();
    test_crc_single_byte();
    test_crc_known_string();
    test_crc_full_sector_payload();
    test_crc_incremental_sensitivity();
}
