---
id: embedded-porting
title: Embedded Porting Guide
sidebar_label: Embedded Porting
sidebar_position: 8
description: SPI wiring, CMakeLists.txt, and first-boot initialization for RP2350/Pico.
---

# Embedded Porting Guide

This guide covers compiling ZINF for a microcontroller, wiring the SD card driver to platform SPI, and first-boot initialization. The RP2350 (Raspberry Pi Pico 2) with a SPI-mode SD card is used as the concrete example throughout, but the approach applies to any Cortex-M target.

> **Production history:** ZINF's first real-world deployment used an **ATSAMD21G18AU** (ARM Cortex-M0+) on the Mimike-II Rev-I CanSat (Juhász Jenő Szakkollégium CosMIK, 2025). The RP2350 became the primary reference target in later development. The portable core compiled unchanged for both MCUs.

---

## Portable Core

The ZINF codebase is split into a portable core and Linux-specific tooling. Only the portable core is compiled into an MCU firmware image.

| Path | MCU build | Notes |
|---|---|---|
| `src/config/` | **Include** | Generated from `zinf.yaml`; no OS dependencies |
| `src/core/api/` | **Include** | Pure C11 |
| `src/core/storage/` | **Include** | Pure C11 |
| `src/core/helper/` | **Include** | CRC32 lookup table; uses only `memcpy`, `memset`, `memcmp` |
| `src/drivers/sd/` | **Include** | SPI-mode SD/SDHC/SDXC; platform callbacks supplied by you |
| `src/platform/platform_embedded.c` | **Include** | Defines `zinf_ctx` and wires `sd_driver` |
| `src/drivers/linux/` | **Exclude** | `O_DIRECT`, POSIX file I/O |
| `src/platform/platform_linux.c` | **Exclude** | Linux-specific |
| `src/apps/zinf_main.c` | **Exclude** | Linux CLI |
| `tests/` | **Exclude** | Uses Linux driver and `libxlsxwriter` |

**Minimum C standard:** C11.

**Required stdlib symbols:** `memcpy`, `memset`, `memcmp`, `<stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stddef.h>`. All are available in every Cortex-M toolchain's bundled libc (newlib, picolibc, etc.).

**`uint64_t` LBA fields:** Work correctly on 32-bit Cortex-M (two 32-bit registers). On 8-bit targets they are emulated in software; functionally correct but slow — consider keeping sector counts within 32 bits if flash is small.

---

## Cross-Compilation (bare-metal example)

```bash
arm-none-eabi-gcc -std=c11 -O2 -mcpu=cortex-m33 -mthumb \
    -Isrc/config \
    -Isrc/core \
    -Isrc/core/api \
    -Isrc/core/storage \
    -Isrc/core/helper \
    -Isrc/platform \
    -Isrc/drivers \
    -Isrc/drivers/sd \
    src/config/config.c \
    src/core/api/api.c \
    src/core/storage/storage.c \
    src/core/helper/helper.c \
    src/drivers/sd/sd_driver.c \
    src/platform/platform_embedded.c \
    your_app/main.c \
    your_app/platform_rp2350.c \
    -o firmware.elf
```

With the Pico SDK, add the ZINF sources to a CMake library instead (see [RP2350 / Pico SDK](#rp2350--pico-sdk)).

---

## RP2350 / Pico SDK

### 1. CMakeLists.txt

```cmake
# In your project's CMakeLists.txt

add_library(zinf STATIC
    ${ZINF_DIR}/src/config/config.c
    ${ZINF_DIR}/src/core/api/api.c
    ${ZINF_DIR}/src/core/storage/storage.c
    ${ZINF_DIR}/src/core/helper/helper.c
    ${ZINF_DIR}/src/drivers/sd/sd_driver.c
    ${ZINF_DIR}/src/platform/platform_embedded.c
)

target_include_directories(zinf PUBLIC
    ${ZINF_DIR}/src/config
    ${ZINF_DIR}/src/core
    ${ZINF_DIR}/src/core/api
    ${ZINF_DIR}/src/core/storage
    ${ZINF_DIR}/src/core/helper
    ${ZINF_DIR}/src/platform
    ${ZINF_DIR}/src/drivers
    ${ZINF_DIR}/src/drivers/sd
)

target_link_libraries(your_target zinf hardware_spi hardware_gpio)
```

### 2. `platform_rp2350.c`

Create this file in your application directory. It provides the four SPI callbacks and wires up `zinf_ctx`.

```c
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "config.h"
#include "sd_driver.h"
#include "api.h"

/* RP2350 SPI pin assignments — adjust to your wiring */
#define SD_SPI_PORT   spi0
#define SD_PIN_MISO   16
#define SD_PIN_CS     17
#define SD_PIN_SCK    18
#define SD_PIN_MOSI   19

/* -----------------------------------------------------------------------
   sd_spi_ops_t callbacks
   ----------------------------------------------------------------------- */

static void rp2350_cs_set(spi_handle_t h, int active) {
    (void)h;
    gpio_put(SD_PIN_CS, active ? 0 : 1);   /* active-low chip-select */
}

static uint8_t rp2350_xfer(spi_handle_t h, uint8_t tx) {
    (void)h;
    uint8_t rx = 0;
    spi_write_read_blocking(SD_SPI_PORT, &tx, &rx, 1);
    return rx;
}

static void rp2350_tx_buf(spi_handle_t h, const uint8_t *buf, size_t len) {
    (void)h;
    spi_write_blocking(SD_SPI_PORT, buf, len);
}

static void rp2350_rx_buf(spi_handle_t h, uint8_t *buf, size_t len) {
    (void)h;
    /* Send 0xFF dummy bytes while receiving */
    spi_read_blocking(SD_SPI_PORT, 0xFF, buf, len);
}

static const sd_spi_ops_t rp2350_sd_ops = {
    .cs_set  = rp2350_cs_set,
    .xfer    = rp2350_xfer,
    .tx_buf  = rp2350_tx_buf,
    .rx_buf  = rp2350_rx_buf,
};

/* -----------------------------------------------------------------------
   zinf_ctx — defined here for this platform
   ----------------------------------------------------------------------- */

extern driver_t sd_driver;

static zinf_ctx_t g_zinf_ctx = {
    .driver           = NULL,
    .sector_size      = SECTOR_SIZE,
    .mirror_count     = (uint8_t)RAID_MIRRORS,
    .metadata_sectors = 2,
    .mirror_offset    = 0,
    .log_sector       = 0,
    .raid_offset      = 0,
};

zinf_ctx_t *zinf_ctx = &g_zinf_ctx;

/* -----------------------------------------------------------------------
   Public init — call from main() before any ZINF API
   ----------------------------------------------------------------------- */

void zinf_platform_init(void) {
    /* Configure SPI peripheral */
    spi_init(SD_SPI_PORT, 400 * 1000);          /* 400 kHz for init */
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);

    /* Wire the SD driver */
    sd_driver_init_spi(NULL, &rp2350_sd_ops);   /* handle unused — NULL is fine */
    zinf_ctx->driver = &sd_driver;
}
```

> **SPI clock speed:** The SD card initialization sequence requires ≤ 400 kHz. After `sd_init()` returns `DRIVER_OK`, raise the clock to the maximum your wiring supports (typically 25 MHz for SD, up to 50 MHz for SDHC).
>
> ```c
> spi_set_baudrate(SD_SPI_PORT, 25 * 1000 * 1000);
> ```

---

## First-Boot Initialization

```c
#include "api.h"
#include "config.h"

int main(void) {
    /* 1. Initialize the SPI peripheral and wire the SD driver */
    zinf_platform_init();

    /* 2. Open the device and query its size */
    if (zinf_ctx->driver->init(zinf_ctx->driver) != DRIVER_OK) {
        /* SD card not detected or initialization failed — halt or retry */
        for (;;) tight_loop_contents();
    }

    /* 3. Compute mirror_offset from actual card capacity */
    uint64_t usable = zinf_ctx->driver->total_sectors
                      - (uint64_t)zinf_ctx->metadata_sectors;
    zinf_ctx->mirror_offset = usable / zinf_ctx->mirror_count;
    zinf_ctx->raid_offset   = zinf_ctx->mirror_offset;

    /* 4. Initialize storage — only needed on first boot or after format.
          If the card was pre-formatted from the host (zinf_cli -d /dev/sdX),
          skip steps 4 and 5. */
    if (setup_storage(zinf_ctx) != STORAGE_OK) {
        /* write failed — check wiring and card type */
        for (;;) tight_loop_contents();
    }

    /* 5. Initialize the message log */
    if (init_log_sector(zinf_ctx) != STORAGE_OK) {
        for (;;) tight_loop_contents();
    }

    /* 6. (Optional) Rebuild the bad-sector blacklist */
    uint64_t last = 0;
    get_last_sector(zinf_ctx, &last);
    if (last > 0) {
        zinf_scrub_report_t report;
        zinf_scrub(zinf_ctx, 1u, last, &report);
        /* report.unrecoverable > 0 → some data is permanently lost */
    }

    /* Storage is ready — start writing sensor data */
    sensor_t s = { .temp = 23.5f, .humidity = 61.0f };
    raid_sensor_values(zinf_ctx, &s, 1);

    for (;;) tight_loop_contents();
}
```

**Pre-formatted cards:** If you ran `sudo zinf format /dev/sdb` on your Linux machine before inserting the card, skip steps 4 and 5 — the metadata is already in place. The MCU can start writing immediately after steps 1–3.

---

## EEPROM

No EEPROM driver is currently included. To add one:

1. Create `src/drivers/eeprom/eeprom_driver.c` (and `.h`).
2. Copy `src/drivers/embedded/embedded_driver.c` as a starting point — it is a compilable stub with all functions returning errors.
3. Implement `init`, `read_block`, and `write_block` for your EEPROM's page protocol (I2C or SPI). Each "block" is 512 bytes (`SECTOR_SIZE`); your implementation must handle page-boundary splits internally if the EEPROM's native page size is smaller.
4. Create `src/platform/platform_eeprom.c` following the same pattern as `platform_embedded.c`.
5. Add the new sources to your CMakeLists.txt instead of `sd_driver.c` and `platform_embedded.c`.

The rest of the ZINF codebase (API, storage engine, CRC) requires no changes — the `driver_t` interface is the only coupling.

---

## Testing on MCU

All tests in `tests/` are **host-only** — they use the Linux driver (`O_DIRECT`, POSIX file I/O) and, for the CSV suite, `libxlsxwriter`. Neither can run on an MCU.

### Recommended strategy

**Step 1 — Validate on host first.**  
Run the full unit suite on your development machine before touching the MCU:

```bash
cd tests && make -B run    # 28 tests, no hardware needed
make csv                   # 172 fault scenarios, validates the full RAID/recovery stack
```

The test suite runs against the same `api.c`, `storage.c`, and `helper.c` that will be compiled into your firmware. If they pass on the host, the logic is correct.

**Step 2 — Minimal UART runner on target.**  
The portable core compiles unchanged for the MCU. A minimal target test runner needs only a `uart_putchar()` and replaces `printf` with character-by-character output:

```c
/* Minimal assert helper — no stdlib printf needed */
static void test_report(const char *name, int pass) {
    const char *status = pass ? "[OK] " : "[FAIL] ";
    while (*status) uart_putchar(*status++);
    while (*name)   uart_putchar(*name++);
    uart_putchar('\r'); uart_putchar('\n');
}

/* Example test — write one sector and read it back */
static void test_write_read_roundtrip(void) {
    sensor_t s = { .temp = 23.5f, .humidity = 61.0f };
    uint8_t rc = raid_sensor_values(zinf_ctx, &s, 1);
    test_report("write_read_roundtrip", rc == STORAGE_OK);
}
```

The assertions are simple comparisons against `STORAGE_OK` / `STORAGE_ERR_*` — the same constants used in the host tests.

**Step 3 — Run scrub after each power cycle.**  
Call `zinf_scrub()` at startup to detect and repair any corruption caused by unexpected power loss:

```c
zinf_scrub_report_t report;
zinf_scrub(zinf_ctx, 1u, last_sector, &report);
```

See [API Reference — `zinf_scrub`](api-reference.md#zinf_scrub) for details.
