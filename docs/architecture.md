# Architecture

## System Overview

```
┌──────────────────────────────────────────────────┐
│                  Application                     │
│         (zinf_main.c / user code)                │
└───────────────────────┬──────────────────────────┘
                        │ zinf_ctx_t *ctx
┌───────────────────────▼──────────────────────────┐
│                  API Layer                        │
│   api.c / api.h                                  │
│   init_log_sector(), raid_sensor_values(),        │
│   save_msg(), read_sector(), write_sector(),      │
│   raid_read(), get_last_sector()                  │
└───────────────────────┬──────────────────────────┘
                        │
┌───────────────────────▼──────────────────────────┐
│               Storage Engine                     │
│   storage.c / storage.h                          │
│   RAID mirroring, CRC32 stamping,                │
│   log metadata management (format v4)            │
└───────────┬───────────────────────┬──────────────┘
            │                       │
            ▼                       ▼
┌─────────────────┐       ┌─────────────────────┐
│  Helper / CRC   │       │   Config             │
│  helper.c       │       │   config.c / .h      │
│  zinf_crc32()   │       │   constants, codegen │
└─────────────────┘       └─────────────────────┘
                        │
┌───────────────────────▼──────────────────────────┐
│               Driver Interface                   │
│   driver.h  (driver_t)                           │
│   ctx->driver routes I/O to selected backend     │
└───────────┬───────────────────────┬──────────────┘
            │                       │
            ▼                       ▼
┌──────────────────┐  ┌──────────────────────────┐  ┌──────────────────┐
│  Linux Driver    │  │  SD Card Driver          │  │  Mock Driver     │
│  linux_driver.c  │  │  sd_driver.c             │  │  ram_driver.c    │
│  O_DIRECT pread  │  │  SPI-mode SD/SDHC/SDXC   │  │  heap-backed     │
│  /dev/loop0      │  │  (embedded targets)      │  │  (tests only)    │
└──────────────────┘  └──────────────────────────┘  └──────────────────┘
```

## Layers

### 1. Application Layer
Entry point in `apps/zinf_main.c`. Handles CLI parsing, benchmark orchestration, and synthetic data generation. Allocates and initializes `zinf_ctx_t`, then calls into the API layer.

### 2. API Layer (`core/api/`)
Thin orchestration layer that:
- Exposes typed write helpers (`raid_sensor_values`, `save_msg`)
- Provides verified RAID read (`raid_read` with CRC check and majority voting)
- Provides raw sector I/O (`read_sector`, `write_sector`)
- Bridges between typed data structures and the byte-oriented storage engine

All functions accept `zinf_ctx_t *ctx` as their first argument.

### 3. Storage Engine (`core/storage/`)
Core logic for:
- **Log metadata management** — tracks the last-written sector LBA in a v4 metadata sector with 3 redundant copy slots and monotonic 16-bit version counters
- **Sector construction** — assembles header + payload + CRC32 into a full sector
- **RAID mirroring** — writes each sector to `mirror_count` physical locations separated by `mirror_offset` sectors

### 4. Helper Layer (`core/helper/`)
- **`helper.c`** — CRC32 implementation using a 256-entry lookup table (O(n), polynomial `0xEDB88320`)
- **`driver.h`** — Defines the `driver_t` interface all drivers must implement

### 5. Configuration (`config/`)
Generated from `zinf.yaml` by `tools/zinf_gen.py`. Provides compile-time constants, the `sensor_t` struct, serialization functions (`sensor_t_to_wire`), and `zinf_ctx_t`. No global mutable state — all runtime state is in `zinf_ctx_t`.

### 6. Driver Layer (`drivers/`)
Platform-specific block I/O. The `driver` field in `zinf_ctx_t` routes all reads/writes to the correct backend.

| Driver | Path | Use |
|---|---|---|
| `linux_driver` | `drivers/linux/` | Host development, CI, benchmark (`O_DIRECT` pread/pwrite) |
| `sd_driver` | `drivers/sd/` | Embedded MCU targets (SPI-mode SD/SDHC/SDXC) |
| `ram_driver` | `drivers/mock/` | Unit tests and fuzz engine (malloc'd heap, fault-injectable) |
| stub | `drivers/embedded/` | Starting template — all functions return errors |

### 7. Platform Selection (`platform/`)
Each platform file defines the global `zinf_ctx` pointer and wires up the driver. Only one platform file is compiled per build target.

| File | Driver wired | Notes |
|---|---|---|
| `platform_linux.c` | `linux_driver` | Used by the CLI and benchmark |
| `platform_embedded.c` | `sd_driver` | Used by MCU firmware; requires `sd_driver_init_spi()` before `setup_storage()` |

```c
// platform_linux.c
#include "config.h"
#include "linux_driver.h"

static zinf_ctx_t g_zinf_ctx;
zinf_ctx_t *zinf_ctx = &g_zinf_ctx;
// Caller calls linux_driver_set_path() + zinf_ctx_init_defaults() before setup_storage()
```

---

## RAID Mirroring Design

### Goal
Every write produces `mirror_count` independent on-disk copies. If one region degrades, the others can be used for recovery. With 3+ mirrors, majority voting detects bit-rot even when all copies are readable but disagree.

### Mirror Placement

Mirrors are placed at a computed offset, not interleaved:

```
Physical sector layout:

Sector 0            → Log metadata (primary)
Sector 1            → Log metadata (secondary)
Sector 2            → Data chunk 0, Mirror 0
Sector 3            → Data chunk 1, Mirror 0
...
Sector N            → Data chunk N-2, Mirror 0
Sector N+OFFSET     → Data chunk 0, Mirror 1
Sector N+1+OFFSET   → Data chunk 1, Mirror 1
...
```

### Mirror Offset Computation

`mirror_offset` / `raid_offset` is computed at runtime from the actual device size and stored in `zinf_ctx_t`:

```
total_sectors = device_bytes / sector_size
usable        = total_sectors - metadata_sectors
mirror_offset = usable / mirror_count
minimum       = 8  (enforced lower bound)
```

For a 5 MB device (10240 sectors, mirror_count=2, metadata_sectors=2):
```
usable        = 10238
mirror_offset = 5119
```

### Physical Address Formula

For logical chunk index `i` and mirror number `m` (0-based):

```
physical_sector = (last_log_index + 1 + i) + m * mirror_offset
```

### Majority Voting (3+ mirrors)

`raid_read` collects CRC-valid copies from all mirrors and counts how many match byte-for-byte. A copy wins if it appears more than `mirror_count / 2` times. If no majority is found, `STORAGE_ERR_UNRECOVERABLE` is returned — callers must handle degraded reads explicitly.

---

## Initialization Sequence

```
1. Platform allocates zinf_ctx_t and sets ctx->driver
2. zinf_ctx_init_defaults(&ctx) — fills sector_size, mirror_count, metadata_sectors
3. Compute mirror_offset from device size; set ctx->raid_offset = ctx->mirror_offset
4. ctx->driver->init(ctx->driver) — opens device, queries size
5. init_log_sector(&ctx) — zeros and writes log header to sectors 0 and 1
```

After this sequence the system is ready to accept write calls.

---

## Write Data Flow (Sensor)

```
raid_sensor_values(ctx, sensor_t *buf, size_t len)   [api.c]
  │
  ├─ serialize all sensors via sensor_t_to_wire() → flat byte buffer
  │
  └─ log_raid_u8bit_values(ctx, wire_buf, wire_len, header)   [storage.c]
       │
       ├─ log_get_last_sector(ctx) → base_cursor
       │
       ├─ for each PAYLOAD_SIZE-byte chunk:
       │    sector[0]      = header byte
       │    sector[1:508]  = payload
       │    sector[508:512] = zinf_crc32(sector[0:508])
       │
       ├─ for mirror m in {0 … mirror_count-1}:
       │    addr = base_cursor + chunk_index + m * mirror_offset
       │    write_sector(ctx, addr, sector_buf)
       │         └─ ctx->driver->write_block(addr, buf)   [linux_driver.c]
       │                └─ pwrite(fd, bounce, offset)
       │
       └─ log_set_last_sector(ctx, base_cursor + num_chunks - 1)
```

## Read Data Flow (RAID)

```
raid_read(ctx, logical_sector, payload_out)          [api.c]
  │
  ├─ for mirror m in {0 … min(mirror_count, MAX_MIRRORS)-1}:
  │    physical = logical_sector + m * mirror_offset
  │    read_sector(ctx, physical, sector_buf)
  │    crc32_check(sector_buf)
  │    → add to candidates[] if CRC ok
  │
  ├─ if mirror_count < 3: return first valid candidate
  │
  ├─ majority vote: pick candidate appearing > mirror_count/2 times
  │
  └─ if no majority: return STORAGE_ERR_UNRECOVERABLE
```

---

## Runtime Context (`zinf_ctx_t`)

All state that was previously global is now encapsulated in `zinf_ctx_t`:

| Field | Type | Description |
|---|---|---|
| `driver` | `driver_t *` | Pointer to the active block driver |
| `sector_size` | `uint32_t` | Bytes per sector (default `SECTOR_SIZE`) |
| `mirror_count` | `uint8_t` | Number of RAID mirrors (max `MAX_MIRRORS = 5`) |
| `metadata_sectors` | `uint8_t` | Sectors reserved for log metadata |
| `mirror_offset` | `uint64_t` | Sector spacing between mirror copies |
| `log_sector` | `uint64_t` | LBA of the primary metadata sector (default 0) |
| `raid_offset` | `uint64_t` | Runtime-computed mirror spacing (used by init) |
| `bad_sectors[]` | `uint64_t[MAX_BAD_SECTORS]` | In-RAM bad-sector blacklist (not persisted; rebuilt by `zinf_scrub()`) |
| `bad_sector_count` | `uint8_t` | Number of entries currently in `bad_sectors[]` |
