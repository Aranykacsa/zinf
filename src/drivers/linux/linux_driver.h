#pragma once
#include "driver.h"

/* Override the block device path before calling setup_storage().
   Default is "/dev/loop0". */
void linux_driver_set_path(const char *path);

extern driver_t linux_driver;
