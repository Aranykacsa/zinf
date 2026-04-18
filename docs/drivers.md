# Drivers

## The `driver_t` Interface (`core/helper/driver.h`)

All storage backends implement the `driver_t` struct:

```c
typedef struct driver_t {
    /* metadata — filled by driver after init */
    const char *name;
    uint32_t    sector_size;
    uint64_t    total_size_bytes;
    uint64_t    total_sectors;

    /* driver-private context pointer */
    void *ctx;

    /* lifecycle */
    int  (*init)(driver_t *self);
    void (*deinit)(driver_t *self);

    /* single-sector I/O (required) */
    int (*read_block)(driver_t *self, uint64_t lba, uint8_t *buf);
    int (*write_block)(driver_t *self, uint64_t lba, const uint8_t *buf);

    /* multi-sector I/O (optional, NULL if not supported) */
    int (*read_blocks)(driver_t *self, uint64_t lba, uint8_t *buf, uint32_t count);
    int (*write_blocks)(driver_t *self, uint64_t lba, const uint8_t *buf, uint32_t count);

    /* optional flush */
    int (*sync)(driver_t *self);
} driver_t;
```

All LBA parameters are `uint64_t` to support devices larger than 2 TB.

### Field Reference

| Field | Required | Description |
|---|---|---|
| `name` | Yes | Human-readable driver name |
| `sector_size` | Yes | Bytes per sector (512 for ZINF) |
| `total_size_bytes` | After init | Device capacity in bytes |
| `total_sectors` | After init | Device capacity in sectors |
| `ctx` | Driver use | Private context (file descriptors, buffers, etc.) |
| `init` | Yes | Open device, populate metadata fields |
| `deinit` | Yes | Close device, free resources |
| `read_block` | Yes | Read one sector at LBA |
| `write_block` | Yes | Write one sector at LBA |
| `read_blocks` | Optional | Read N sectors starting at LBA |
| `write_blocks` | Optional | Write N sectors starting at LBA |
| `sync` | Optional | Flush write buffers (NULL = no-op) |

---

## Linux Driver (`drivers/linux/linux_driver.c`)

Targets Linux block devices and regular files via raw POSIX I/O. Default device: `/dev/loop0`.

### Context

```c
typedef struct {
    int         fd;       // open file descriptor
    const char *path;     // device path
    uint8_t    *bounce;   // sector-aligned I/O buffer
} linux_ctx_t;
```

### Global Instance

```c
driver_t linux_driver = {
    .name        = "linux_raw",
    .sector_size = 512,
    .ctx         = &ctx,
    .init        = linux_init,
    .deinit      = linux_deinit,
    .read_block  = linux_read,
    .write_block = linux_write,
    .read_blocks  = NULL,
    .write_blocks = NULL,
    .sync         = NULL,
};
```

### `linux_driver_set_path`

```c
void linux_driver_set_path(const char *path);
```

Change the device path before calling `init`. Useful in tests to redirect the driver to a temporary image file rather than `/dev/loop0`.

### `linux_init`

1. Allocates a 512-byte aligned bounce buffer with `posix_memalign(512)`.  
   Required for `O_DIRECT` — the kernel rejects unaligned buffers.
2. Opens the device with `O_RDWR | O_DIRECT`.  
   `O_DIRECT` bypasses the page cache for deterministic, cache-free I/O.
3. Queries total device size with the `BLKGETSIZE64` ioctl (block devices only;  
   for regular files this ioctl fails, `total_sectors` is set to 0).
4. Logs to `stderr`: `[linux_driver] RAW open <path> (fd=N, sectors=M)`.

**Returns:** `DRIVER_OK` or `DRIVER_ERR_INIT`

### `linux_read`

```
offset = lba * sector_size
pread(fd, bounce, sector_size, offset)
memcpy(buf, bounce, sector_size)
```

The bounce buffer mediates between the caller's potentially unaligned buffer and the kernel's `O_DIRECT` alignment requirement.

**Returns:** `DRIVER_OK` or `DRIVER_ERR_IO`

### `linux_write`

```
memcpy(bounce, buf, sector_size)
offset = lba * sector_size
pwrite(fd, bounce, sector_size, offset)
```

**Returns:** `DRIVER_OK` or `DRIVER_ERR_IO`

### `linux_deinit`

Closes the file descriptor and frees the bounce buffer. Logs to `stderr`.

### Notes

- To redirect I/O to a file, call `linux_driver_set_path("/var/tmp/myfile.img")` before `init`.
- `O_DIRECT` is not supported on tmpfs (e.g. `/tmp` on most distros). Use a real filesystem (ext4, btrfs, xfs) for test images.
- Requires `sudo` (or `CAP_SYS_RAWIO`) for direct block device access.
- All driver messages go to `stderr` to avoid polluting benchmark CSV output on `stdout`.

---

## SD Card Driver (`drivers/sd/sd_driver.c`)

SPI-mode SD driver for embedded targets. Supports SD (v1), SDHC, and SDXC cards. Intended for microcontrollers; not compiled into the Linux binary.

### SPI Operations Interface

The driver is hardware-agnostic. Callers provide a `sd_spi_ops_t` with platform-specific callbacks:

```c
typedef struct {
    void (*cs_set)(int assert);              /* chip-select: 1=assert, 0=deassert */
    uint8_t (*xfer)(uint8_t byte);           /* SPI transfer: send byte, return received byte */
    void (*tx_buf)(const uint8_t *b, size_t n);  /* optional bulk TX (NULL = use xfer) */
    void (*rx_buf)(uint8_t *b, size_t n);        /* optional bulk RX (NULL = use xfer) */
} sd_spi_ops_t;
```

### Key Commands

| Command | Description |
|---|---|
| CMD0 | GO_IDLE_STATE — reset to SPI mode |
| CMD8 | SEND_IF_COND — voltage check (SD v2 detection) |
| CMD17 | READ_SINGLE_BLOCK |
| CMD18 | READ_MULTIPLE_BLOCK |
| CMD24 | WRITE_BLOCK |
| CMD25 | WRITE_MULTIPLE_BLOCK |
| CMD58 | READ_OCR — CCS bit determines SDHC byte vs block addressing |
| ACMD41 | SD_SEND_OP_COND — initialization with HCS=1 for SDHC |

### Address Translation

SDHC/SDXC cards use block addresses. SD (v1) cards use byte addresses. The driver detects card type via CMD58 CCS bit after ACMD41 and applies the correct translation automatically.

### Global Instance

```c
extern driver_t sd_driver;
```

### `sd_driver_init_spi`

```c
void sd_driver_init_spi(spi_handle_t handle, const sd_spi_ops_t *ops);
```

Call once before `ctx->driver->init()` to bind the platform SPI handle and callbacks. After this, call `ctx->driver->init(ctx->driver)` to run the SD initialization sequence (CMD0 → CMD8 → ACMD41 → CMD58).

| Parameter | Notes |
|---|---|
| `handle` | Opaque pointer passed back into every callback — typically an MCU SPI peripheral handle. May be `NULL` if your callbacks do not need it. |
| `ops` | `cs_set` and `xfer` are required. `tx_buf` and `rx_buf` may be `NULL`; the driver falls back to repeated `xfer()` calls. |

`init()` populates `sd_driver.sector_size` (512) and leaves `total_sectors` at 0 — the SD protocol requires a separate CSD read for exact capacity, which is not yet implemented. Set `mirror_offset` from your known card size or from the value reported by `BLKGETSIZE64` on the host.

**RP2350 / Pico SDK example:**

```c
static void my_cs_set(spi_handle_t h, int active) {
    (void)h;
    gpio_put(SD_PIN_CS, active ? 0 : 1);
}

static uint8_t my_xfer(spi_handle_t h, uint8_t tx) {
    (void)h;
    uint8_t rx = 0;
    spi_write_read_blocking(spi0, &tx, &rx, 1);
    return rx;
}

static void my_tx_buf(spi_handle_t h, const uint8_t *buf, size_t len) {
    (void)h;
    spi_write_blocking(spi0, buf, len);
}

static void my_rx_buf(spi_handle_t h, uint8_t *buf, size_t len) {
    (void)h;
    spi_read_blocking(spi0, 0xFF, buf, len);
}

static const sd_spi_ops_t my_ops = {
    .cs_set  = my_cs_set,
    .xfer    = my_xfer,
    .tx_buf  = my_tx_buf,
    .rx_buf  = my_rx_buf,
};

/* In your init sequence: */
sd_driver_init_spi(NULL, &my_ops);
zinf_ctx->driver = &sd_driver;
zinf_ctx->driver->init(zinf_ctx->driver);
```

See [Embedded Porting Guide](embedded-porting.md) for the complete RP2350 wiring and first-boot initialization pattern.

---

## Implementing a New Driver

To port ZINF to a new platform:

1. Create `drivers/<target>/<target>_driver.c`.
   `drivers/embedded/embedded_driver.c` is a compilable stub — all functions return errors but the file structure, `driver_t` initializer, and function signatures are correct. Copy it as a starting point and replace the function bodies.
2. Define a private context struct for your hardware state.
3. Implement `init`: open/initialize the storage peripheral, fill `total_sectors` and `total_size_bytes`.
4. Implement `read_block` and `write_block`: read/write exactly `sector_size` bytes at the given 64-bit LBA.
5. Optionally implement `read_blocks`/`write_blocks` for DMA or burst transfers.
6. Create `platform/platform_<target>.c` that sets `zinf_ctx->driver = &<target>_driver`.
7. Update the Makefile (or CMakeLists.txt) to compile your new files instead of the Linux ones.

The rest of the ZINF codebase (API, storage engine, CRC) requires no changes.

---

## Return Codes Summary

| Code | Value | Returned by |
|---|---|---|
| `DRIVER_OK` | 0 | All functions on success |
| `DRIVER_ERR_IO` | 1 | `read_block`, `write_block` on I/O failure |
| `DRIVER_ERR_INIT` | 2 | `init` on initialization failure |
| `DRIVER_ERR_PARAM` | 3 | Any function if a parameter is invalid |
