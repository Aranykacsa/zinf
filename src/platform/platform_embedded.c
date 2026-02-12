#include "driver.h"
#include <stdint.h>

/* Replace this with your real embedded storage driver */
extern driver_t embedded_driver;

/* globals used by core */
driver_t *active_driver = &embedded_driver;
uint32_t log_sector = 0;
