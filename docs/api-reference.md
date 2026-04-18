# API Reference

All public headers are under `src/core/` and `src/config/`. Include paths are configured in the Makefile so files can be included by short name (e.g. `#include "api.h"`).

All storage API functions take a `zinf_ctx_t *ctx` as their first argument. This struct holds all runtime state (driver pointer, sector geometry, mirror count, offsets). See [Configuration](configuration.md) for details.

---

## Return Codes

### Storage Return Codes (`config.h`)

| Constant | Value | Meaning |
|---|---|---|
| `STORAGE_OK` | `0` | Success |
| `STORAGE_ERR_PARAM` | `1` | Invalid parameter (e.g. NULL pointer) |
| `STORAGE_ERR_DRIVER` | `2` | Driver initialization or I/O failure |
| `STORAGE_ERR_LOG_FULL` | `3` | Message log capacity exhausted |
| `STORAGE_ERR_UNRECOVERABLE` | `4` | All mirrors failed CRC; data cannot be recovered |
| `STORAGE_WARN_DEGRADED` | `5` | **Non-fatal.** Write succeeded on ≥1 mirror but fewer than `mirror_count` (some mirrors were blacklisted). The write committed; caller may want to call `zinf_recover_sector()`. |

### Driver Return Codes (`driver.h`)

| Constant | Value | Meaning |
|---|---|---|
| `DRIVER_OK` | `0` | Success |
| `DRIVER_ERR_IO` | `1` | Read or write I/O failure |
| `DRIVER_ERR_INIT` | `2` | Driver initialization failure |
| `DRIVER_ERR_PARAM` | `3` | Invalid parameter |

---

## API Layer (`core/api/api.h`)

### `init_log_sector`

```c
uint8_t init_log_sector(zinf_ctx_t *ctx);
```

Initialize the log metadata sectors (`log_sector` and `log_sector+1`). Zeros all copy slots, version counters, write_pos, and flags. Call once after driver init, or after wiping the device.

**Returns:** `STORAGE_OK` or `STORAGE_ERR_DRIVER`

---

### `read_sector`

```c
int read_sector(zinf_ctx_t *ctx, uint64_t sector, uint8_t *buffer);
```

Read one sector from the device. No CRC check — use `raid_read` for verified reads.

| Parameter | Description |
|---|---|
| `ctx` | Runtime context (driver, sector_size, etc.) |
| `sector` | 64-bit Logical Block Address (LBA) |
| `buffer` | Caller-allocated buffer, must be at least `SECTOR_SIZE` bytes |

**Returns:** `DRIVER_OK` or `DRIVER_ERR_IO`

---

### `write_sector`

```c
int write_sector(zinf_ctx_t *ctx, uint64_t sector, const uint8_t *buffer);
```

Write one raw sector to the device. Does not apply RAID mirroring or CRC — use `log_raid_u8bit_values` for redundant writes.

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `sector` | 64-bit Logical Block Address (LBA) |
| `buffer` | Data to write, must be exactly `SECTOR_SIZE` bytes |

**Returns:** `DRIVER_OK` or `DRIVER_ERR_IO`

---

### `raid_read`

```c
uint8_t raid_read(zinf_ctx_t *ctx, uint64_t logical_sector, uint8_t *payload);
```

Read a logical sector from all mirrors, verify CRC on each, then apply majority voting.

| Parameter | Description |
|---|---|
| `ctx` | Runtime context (provides `mirror_count`, `mirror_offset`) |
| `logical_sector` | Logical sector index (0 = first data sector after metadata) |
| `payload` | Output buffer for the `PAYLOAD_SIZE`-byte payload (must not be NULL) |

**Behavior:**
- Reads up to `min(mirror_count, MAX_MIRRORS)` physical copies
- Each copy is CRC-verified; failed copies are skipped
- If mirror_count ≥ 3 (odd): majority voting selects the canonical value
- If mirror_count < 3: returns the first CRC-valid copy
- If no majority is found among valid copies: returns `STORAGE_ERR_UNRECOVERABLE`
- Returns `STORAGE_ERR_PARAM` if `payload` is NULL

**Returns:** `STORAGE_OK`, `STORAGE_ERR_PARAM`, `STORAGE_ERR_DRIVER`, or `STORAGE_ERR_UNRECOVERABLE`

---

### `save_msg`

```c
uint8_t save_msg(zinf_ctx_t *ctx, uint8_t *msg);
```

Append a single byte to the sequential message log. The log spans up to 983 bytes across `log_sector` (471 bytes after the 41-byte header) and `log_sector+1` (512 bytes).

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `msg` | Pointer to a single byte value to append |

**Returns:** `STORAGE_OK`, `STORAGE_ERR_LOG_FULL`, or `STORAGE_ERR_DRIVER`

**Capacity:** 983 bytes total (`MSG_LOG_TOTAL_CAP = MSG_LOG_CAP_S0 + MSG_LOG_CAP_S1`, where `MSG_LOG_CAP_S0 = 471`, `MSG_LOG_CAP_S1 = 512`)

---

### `get_last_sector`

```c
uint8_t get_last_sector(zinf_ctx_t *ctx, uint64_t *last_sector);
```

Read the last-written data sector LBA from log metadata. Picks the copy slot with the highest version using modular comparison (handles 0xFFFF → 0x0000 wraparound).

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `last_sector` | Output: last written sector LBA (must not be NULL) |

**Returns:** `STORAGE_OK`, `STORAGE_ERR_PARAM`, or `STORAGE_ERR_DRIVER`

---

### `raid_sensor_values`

```c
uint8_t raid_sensor_values(zinf_ctx_t *ctx, sensor_t *buffer, size_t len);
```

Serialize and RAID-write an array of sensor readings.

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `buffer` | Array of `sensor_t` structs |
| `len` | Number of elements in `buffer` |

All `len` sensors are serialized to wire format via `sensor_t_to_wire()`, concatenated into a single byte buffer, then written via one call to `log_raid_u8bit_values`. Each sensor occupies 8 bytes on the wire (two float32 LE values).

**Returns:** `STORAGE_OK` or `STORAGE_ERR_DRIVER`

---

## Storage Engine (`core/storage/storage.h`)

### `log_raid_u8bit_values`

```c
uint8_t log_raid_u8bit_values(zinf_ctx_t *ctx, uint8_t *buffer, size_t len, uint8_t *header);
```

Core RAID write function. Splits `buffer` into `PAYLOAD_SIZE`-byte chunks, stamps each with `header` and a CRC32, then writes each chunk to all mirror locations.

| Parameter | Description |
|---|---|
| `ctx` | Runtime context (provides `mirror_count`, `mirror_offset`) |
| `buffer` | Raw payload bytes |
| `len` | Length of `buffer` in bytes |
| `header` | 1-byte header value written at offset 0 of each sector |

**Write layout per sector:**
```
[0]        header byte
[1..507]   507 bytes of payload chunk
[508..511] CRC32 of bytes [0..507] (little-endian)
```

**RAID write:** For mirror `m` in `{0 … mirror_count-1}`:
```
physical = base_cursor + chunk_index + m * mirror_offset
```

Number of sectors written = `ceil(len / PAYLOAD_SIZE)`.

**Returns:** `STORAGE_OK` or `STORAGE_ERR_DRIVER`

---

### `log_get_last_sector`

```c
uint8_t log_get_last_sector(zinf_ctx_t *ctx, uint64_t *last_sector);
```

Internal: read the last-written sector LBA from copy slots in the metadata sector. Selects the copy slot with the highest version using modular 16-bit comparison.

**Returns:** `STORAGE_OK` or `STORAGE_ERR_DRIVER`

---

### `log_set_last_sector`

```c
uint8_t log_set_last_sector(zinf_ctx_t *ctx, const uint64_t *last_sector);
```

Internal: persist the last-written sector LBA to the next copy slot in round-robin order, incrementing the version counter.

**Returns:** `STORAGE_OK` or `STORAGE_ERR_DRIVER`

---

## Helper (`core/helper/helper.h`)

### `zinf_crc32`

```c
uint32_t zinf_crc32(const uint8_t *data, size_t len);
```

Compute a CRC32 checksum using the standard Ethernet/ZIP polynomial (`0xEDB88320`).

| Parameter | Description |
|---|---|
| `data` | Pointer to data buffer |
| `len` | Number of bytes to checksum |

**Returns:** 32-bit CRC value.

**Algorithm:** 256-entry lookup table (O(n)). Initialised lazily on first call. Initial CRC = `0xFFFFFFFF`, final CRC inverted (`~crc`).

> **Note:** Named `zinf_crc32` (not `crc32`) to avoid a symbol collision with the identically-named zlib function, which is pulled in transitively by libxlsxwriter → libminizip.

---

## Configuration (`config/config.h`)

See [Configuration](configuration.md) for the full reference on constants and `zinf_ctx_t`.

### `zinf_ctx_init_defaults`

```c
void zinf_ctx_init_defaults(zinf_ctx_t *ctx);
```

Initialize `*ctx` from compile-time constants. The caller must set `ctx->driver` before calling this.

```c
ctx->sector_size      = SECTOR_SIZE;
ctx->mirror_count     = min(RAID_MIRRORS, MAX_MIRRORS);
ctx->metadata_sectors = <yaml value>;
ctx->log_sector       = 0;
ctx->mirror_offset    = ctx->raid_offset;  // use previously set raid_offset
ctx->bad_sector_count = 0;                 // blacklist starts empty
```

---

## Fault Detection and Recovery (`core/api/api.h`)

All recovery functions are **explicitly called** — nothing fires automatically.

### Mirror status codes

Used in `zinf_sector_health_t.status[]`:

| Constant | Value | Meaning |
|---|---|---|
| `ZINF_MIRROR_OK` | `0` | CRC verified |
| `ZINF_MIRROR_IO_ERR` | `1` | `read_sector` failed |
| `ZINF_MIRROR_CRC_FAIL` | `2` | CRC mismatch |
| `ZINF_MIRROR_BLACKLIST` | `3` | LBA is in the bad-sector list — skipped |

### `zinf_sector_health_t`

```c
typedef struct {
    uint8_t status[MAX_MIRRORS]; /* per-mirror status (ZINF_MIRROR_*) */
    uint8_t valid_count;         /* mirrors with status == ZINF_MIRROR_OK */
} zinf_sector_health_t;
```

### `zinf_scrub_report_t`

```c
typedef struct {
    uint32_t checked;       /* logical sectors examined */
    uint32_t healthy;       /* all mirrors OK, no repair needed */
    uint32_t repaired;      /* >=1 mirror was repaired successfully */
    uint32_t unrecoverable; /* no valid mirror found */
} zinf_scrub_report_t;
```

---

### Bad-sector blacklist helpers (`config.h` — static inline)

These are generated into `config.h` and are available wherever `config.h` is included. They operate only on `zinf_ctx_t` fields and never do I/O.

```c
bool    zinf_is_bad_sector(const zinf_ctx_t *ctx, uint64_t lba);
uint8_t zinf_mark_bad_sector(zinf_ctx_t *ctx, uint64_t lba);
void    zinf_clear_bad_sectors(zinf_ctx_t *ctx);
```

| Function | Returns | Notes |
|---|---|---|
| `zinf_is_bad_sector` | `true`/`false` | Linear scan of `bad_sectors[]` |
| `zinf_mark_bad_sector` | `STORAGE_OK` or `STORAGE_ERR_PARAM` | `STORAGE_OK` if already listed (idempotent); `STORAGE_ERR_PARAM` if list is full |
| `zinf_clear_bad_sectors` | `void` | Resets `bad_sector_count` to 0 |

The list capacity is `MAX_BAD_SECTORS` (from `zinf.yaml`; default 16, 128 bytes on embedded targets).

---

### `zinf_check_sector`

```c
uint8_t zinf_check_sector(zinf_ctx_t *ctx, uint64_t logical_sector,
                           zinf_sector_health_t *out);
```

Read all mirrors for `logical_sector` and verify CRC on each. Fills `*out` with per-mirror status and `valid_count`. **Pure read — no writes, no blacklist mutations, no side effects.**

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `logical_sector` | Logical sector index (same space as `raid_read`) |
| `out` | Output health snapshot (must not be NULL) |

**Returns:** `STORAGE_ERR_PARAM` if `ctx` or `out` is NULL; `STORAGE_OK` otherwise (per-mirror errors are encoded in `out->status[]`).

---

### `zinf_recover_sector`

```c
uint8_t zinf_recover_sector(zinf_ctx_t *ctx, uint64_t logical_sector);
```

Repair bad mirrors by copying from the best valid mirror. For each mirror that fails CRC or I/O:

1. Write the good copy to that physical LBA.
2. Re-read and re-verify CRC.
3. If re-verify fails → `zinf_mark_bad_sector(ctx, physical_lba)`.

If no valid mirror exists at all, all non-blacklisted physical LBAs are blacklisted and `STORAGE_ERR_UNRECOVERABLE` is returned.

**Returns:** `STORAGE_OK` (≥1 mirror valid, repair may be partial), `STORAGE_ERR_UNRECOVERABLE`, or `STORAGE_ERR_PARAM`.

---

### `zinf_scrub`

```c
uint8_t zinf_scrub(zinf_ctx_t *ctx, uint64_t start, uint64_t end,
                   zinf_scrub_report_t *report);
```

Iterate logical sectors `[start..end]` inclusive. For each sector: run `zinf_check_sector`; if not fully healthy, call `zinf_recover_sector`. Accumulate results in `*report` (may be NULL to discard). **Never stops early** — unrecoverable sectors are counted and processing continues.

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `start` | First logical sector to check (inclusive) |
| `end` | Last logical sector to check (inclusive) |
| `report` | Output statistics (NULL = discard) |

**Returns:** `STORAGE_ERR_PARAM` if `ctx` is NULL; `STORAGE_OK` otherwise.

**Typical startup pattern:**

```c
// After driver init and zinf_ctx_init_defaults():
zinf_scrub_report_t report;
zinf_scrub(ctx, 1u, last_written_sector, &report);
// report.unrecoverable > 0 → some data is permanently lost
// Blacklist is now populated for future writes
```
