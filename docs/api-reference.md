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
| `STORAGE_ERR_UNRECOVERABLE` | `4` | All mirrors disagree; data cannot be recovered |

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

Append a single byte to the sequential message log. The log spans up to 991 bytes across `log_sector` (479 bytes after the 33-byte header) and `log_sector+1` (512 bytes).

| Parameter | Description |
|---|---|
| `ctx` | Runtime context |
| `msg` | Pointer to a single byte value to append |

**Returns:** `STORAGE_OK`, `STORAGE_ERR_LOG_FULL`, or `STORAGE_ERR_DRIVER`

**Capacity:** 991 bytes total (`MSG_LOG_TOTAL_CAP = MSG_LOG_CAP_S0 + MSG_LOG_CAP_S1`)

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

### `crc32`

```c
uint32_t crc32(const uint8_t *data, size_t len);
```

Compute a CRC32 checksum using the standard Ethernet/ZIP polynomial (`0xEDB88320`).

| Parameter | Description |
|---|---|
| `data` | Pointer to data buffer |
| `len` | Number of bytes to checksum |

**Returns:** 32-bit CRC value.

**Algorithm:** 256-entry lookup table (O(n)). Initialised lazily on first call. Initial CRC = `0xFFFFFFFF`, final CRC inverted (`~crc`).

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
```
