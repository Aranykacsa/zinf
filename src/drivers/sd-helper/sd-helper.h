#ifndef SD_DRIVER_H
#define SD_DRIVER_H

#include "driver.h"
#include "sd-helper.h"
#include "spi.h"
#include "variables.h" // Include variables.h to get spi_s1 definition

/**
 * @brief Generic driver interface for the SD card.
 */
extern driver_t g_sd_driver;

#endif /* SD_DRIVER_H */
