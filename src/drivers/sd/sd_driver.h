#pragma once
#include <stdint.h>
#include <stddef.h>
#include "driver.h"

/* -----------------------------------------------------------------------
   SD card driver (SPI mode)

   Supports SD, SDHC, and SDXC cards.
   Addressing mode (byte vs. block) is auto-detected at init via CMD58 OCR.

   Usage:
     1. Call sd_driver_init_spi() with your platform SPI handle before
        calling setup_storage().
     2. Provide implementations of the three platform SPI callbacks below
        for your specific MCU.
   ----------------------------------------------------------------------- */

/* Opaque SPI handle — supplied by the caller. */
typedef void *spi_handle_t;

/* Platform SPI callbacks — must be implemented by the application.
   All return 0 on success, non-zero on error. */
typedef struct sd_spi_ops_t {
    /* Assert / deassert the SD chip-select line (active low). */
    void (*cs_set)(spi_handle_t h, int active);

    /* Transmit one byte, return the simultaneously received byte. */
    uint8_t (*xfer)(spi_handle_t h, uint8_t tx);

    /* Transmit `len` bytes from `buf`; received bytes are discarded.
       May be NULL — the driver falls back to repeated xfer() calls. */
    void (*tx_buf)(spi_handle_t h, const uint8_t *buf, size_t len);

    /* Receive `len` bytes into `buf`; 0xFF is sent as the dummy TX byte.
       May be NULL — the driver falls back to repeated xfer() calls. */
    void (*rx_buf)(spi_handle_t h, uint8_t *buf, size_t len);
} sd_spi_ops_t;

/* Call once before setup_storage() to configure the SPI handle and ops. */
void sd_driver_init_spi(spi_handle_t handle, const sd_spi_ops_t *ops);

extern driver_t sd_driver;
