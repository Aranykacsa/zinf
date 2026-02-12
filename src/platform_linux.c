#include "driver.h"
#include <stdint.h>

/* from drivers/linux */
extern driver_t linux_driver;

/* globals used by core */
driver_t *active_driver = &linux_driver;
uint32_t log_sector = 0;
