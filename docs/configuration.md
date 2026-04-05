# Configuration

## Compile-Time Constants (`config/config.h`)

Generated from `zinf.yaml` by `tools/zinf_gen.py`. Do not edit `config.h` or `config.c` directly — regenerate them:

```bash
python3 tools/zinf_gen.py zinf.yaml src/config/
```

### Sector Geometry

| Constant | Value | Description |
|---|---|---|
| `SECTOR_SIZE` | `512` | Block device sector size in bytes |
| `HEADER_SIZE` | `1` | Bytes reserved for the per-sector header |
| `PAYLOAD_SIZE` | `507` | Usable payload bytes per sector (`SECTOR_SIZE - HEADER_SIZE - 4`) |
| `RAID_MIRRORS` | `2` | Default mirror count (compile-time, overridden at runtime via ctx) |
| `MAX_MIRRORS` | `5` | Hard upper bound on mirror_count; `raid_read` candidates array is sized for this |

`PAYLOAD_SIZE` is derived as:

```
PAYLOAD_SIZE = SECTOR_SIZE - HEADER_SIZE - sizeof(uint32_t)
             = 512         - 1           - 4
             = 507
```

### Metadata Layout (format v3)

| Constant | Value | Description |
|---|---|---|
| `META_COPY_STRIDE` | `10` | Bytes per metadata copy slot (8 LBA + 2 version) |
| `META_COPIES` | `3` | Number of redundant copy slots |
| `META_WRITE_POS_OFF` | `30` | Offset of `write_pos` field (= `META_COPIES * META_COPY_STRIDE`) |
| `META_FLAGS_OFF` | `32` | Offset of `flags` byte |
| `META_HDR_SIZE` | `33` | Total metadata header size; message log starts here |

### Message Log Capacity

| Constant | Value | Description |
|---|---|---|
| `MSG_LOG_CAP_S0` | `479` | Bytes in the first metadata sector (`SECTOR_SIZE - META_HDR_SIZE`) |
| `MSG_LOG_CAP_S1` | `512` | Bytes in the second metadata sector |
| `MSG_LOG_TOTAL_CAP` | `991` | Total message log capacity across both sectors |

---

## `zinf_ctx_t` Struct

```c
typedef struct zinf_ctx_t {
    struct driver_t *driver;
    uint32_t         sector_size;       /* bytes per sector (default SECTOR_SIZE) */
    uint8_t          mirror_count;      /* number of RAID mirrors                 */
    uint8_t          metadata_sectors;  /* sectors reserved for metadata          */
    uint64_t         mirror_offset;     /* sectors between mirror copies          */
    uint64_t         log_sector;        /* LBA of metadata sector (default 0)     */
    uint64_t         raid_offset;       /* runtime-computed mirror spacing        */
} zinf_ctx_t;

extern zinf_ctx_t *zinf_ctx;  /* global pointer, set by platform file */
```

There is no longer a global `config` struct or a `RAID_OFFSET` global. All runtime state lives in `zinf_ctx_t`.

### `zinf_ctx_init_defaults`

```c
void zinf_ctx_init_defaults(zinf_ctx_t *ctx);
```

Initialize `*ctx` from compile-time constants. The caller must set `ctx->driver` before calling this.

```c
ctx->sector_size      = SECTOR_SIZE;
ctx->mirror_count     = min((uint8_t)RAID_MIRRORS, (uint8_t)MAX_MIRRORS);
ctx->metadata_sectors = <yaml metadata_sectors>;
ctx->log_sector       = 0u;
ctx->mirror_offset    = ctx->raid_offset;  // use raid_offset set before this call
```

---

## `zinf.yaml` — Master Configuration

`zinf.yaml` at the repository root is the single source of truth for compile-time settings:

```yaml
zinf:
  sector_size:      512   # bytes per sector (power of 2, 64–65536)
  mirror_count:     2     # default number of RAID copies
  header_size:      1     # reserved bytes at start of each sector
  metadata_sectors: 2     # sectors reserved for log metadata

  data_types:
    - name: sensor_t
      fields:
        - { name: temp,     type: float }
        - { name: humidity, type: float }
```

`zinf_gen.py` validates the YAML (unknown types, wire size vs PAYLOAD_SIZE) and emits:
- `src/config/config.h` — all `#define` constants, `sensor_t` typedef, `zinf_ctx_t` typedef, and function declarations
- `src/config/config.c` — `zinf_ctx_init_defaults()` and `sensor_t_to_wire()` serialization function

---

## `sensor_t` Struct

```c
typedef struct sensor_t {
    float temp;      /* temperature in degrees Celsius */
    float humidity;  /* relative humidity in percent (0–100) */
} sensor_t;
```

Used as input to `raid_sensor_values()`. Serialized to 8 bytes on the wire by `sensor_t_to_wire()` (see [Data Formats](data-formats.md)).

---

## Log Sector

```c
uint64_t log_sector;  /* field in zinf_ctx_t, default 0 */
```

The LBA of the primary metadata sector. Changing this allows the metadata to live somewhere other than sector 0 — useful if sector 0 is reserved for a partition table or bootloader on the target hardware.

---

## Error Codes

### Storage (`config.h`)

```c
#define STORAGE_OK                0u
#define STORAGE_ERR_PARAM         1u
#define STORAGE_ERR_DRIVER        2u
#define STORAGE_ERR_LOG_FULL      3u
#define STORAGE_ERR_UNRECOVERABLE 4u
```

### Driver (`driver.h`)

```c
#define DRIVER_OK        0
#define DRIVER_ERR_IO    1
#define DRIVER_ERR_INIT  2
#define DRIVER_ERR_PARAM 3
```

---

## Summary of All State

| Symbol | Type | Where | Set When |
|---|---|---|---|
| `SECTOR_SIZE` | `#define` | `config.h` | Compile time (from YAML) |
| `PAYLOAD_SIZE` | `#define` | `config.h` | Compile time (derived) |
| `RAID_MIRRORS` | `#define` | `config.h` | Compile time (from YAML) |
| `MAX_MIRRORS` | `#define` | `config.h` | Compile time (fixed = 5) |
| `zinf_ctx_t` | struct | `config.h` | Runtime, per-instance |
| `zinf_ctx` | `zinf_ctx_t *` | `platform_*.c` | Link time (platform file) |
