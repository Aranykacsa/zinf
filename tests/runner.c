#include "framework.h"

/* Shared counters — all test files reference these via extern in framework.h */
int g_tests_run    = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;

void run_crc_tests(void);
void run_storage_tests(void);
void run_fault_tests(void);

int main(void) {
    printf("=== ZINF Test Suite ===\n");
    run_crc_tests();
    run_storage_tests();
    run_fault_tests();
    test_summary();
    return 0;
}
