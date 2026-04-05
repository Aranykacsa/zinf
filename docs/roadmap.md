# ZINF — Future Implementation & Refactor Roadmap

This document is derived from the thesis *"Mérésadatgyűjtésre optimalizált fájlrendszer erőforráskritikus beágyazott rendszerekhez"* (University of Pécs, 2026) and the current codebase. It captures every planned feature, known architectural gap, and refactoring need identified in both sources.

---

## Current Status Summary

The MVP (as defined in the thesis, Chapter 6) is complete:

| Feature | Status |
|---|---|
| RAID-1 mirroring (2 mirrors) | Done |
| CRC-32 per-sector integrity | Done |
| Append-only / CoW data blocks | Done |
| Metadata block (alpha + omega sectors) | Done |
| Driver abstraction layer (`driver_t`) | Done |
| Linux loopback driver | Done |
| Config system (`config.c/h`) | Done |
| CLI + benchmark tooling | Done |
| First real deployment (Mimike-II Rev-I CanSat) | Done |

---

## Part 1 — Refactors (Fix What Exists)

These address correctness gaps and technical debt identified in the thesis and current code.

---

### R1 — Encapsulate global state into a context struct

**Problem:** `RAID_OFFSET`, `log_sector`, `active_driver`, `config` are process-wide globals. This makes multi-instance use impossible and unit testing difficult.

**Plan:**
```c
typedef struct zinf_ctx_t {
    driver_t    *driver;
    config_t     config;
    uint32_t     log_sector;
    uint32_t     raid_offset;
} zinf_ctx_t;
```

Pass `zinf_ctx_t *` to all API and storage functions instead of reading globals. Platform files provide a default instance. This is a pure refactor — no behavior change.

**Files:** `config.h`, `config.c`, `core/api/api.h`, `core/api/api.c`, `core/storage/storage.h`, `core/storage/storage.c`, `platform/*.c`

---

### R2 — Fix sector addressing width (24-bit → 32-bit)

**Problem:** `log_get_last_sector` / `log_set_last_sector` encode the sector index as a 24-bit little-endian value in the metadata sector (bytes 0–2). The thesis notes 32-bit addressing is sufficient for current use (max 2 TB) and is already planned for upgrade.

**Plan:** Change metadata layout to store the last-sector pointer as a full 32-bit LE value at offset 0–3. Update both read and write paths. This is a breaking on-disk format change — document it as format version 2.

**Files:** `core/storage/storage.c`, `docs/data-formats.md`

---

### R3 — Harden the last-sector pointer (redundant copies + versioning)

**Problem (thesis §9.11):** The last-written sector index is stored only once, at the start of the metadata sector. A power-loss during that write leaves the pointer corrupted with no way to recover it without scanning all sectors.

**Current partial fix:** The pointer is stored in multiple locations without versioning — there is no way to tell which copy is newest after a crash.

**Plan:**
- Store the pointer in N locations in the metadata sector (e.g., at offsets 0, 8, 16).
- Add a 2-byte monotonic version counter alongside each copy.
- On read: find the copy with the highest version that passes a CRC check.
- On write: increment the version and write to the *next* slot (round-robin), never overwriting the previous valid copy until the new one is confirmed.

**Files:** `core/storage/storage.c`, `docs/data-formats.md`

---

### R4 — Add CRC verification to the read path

**Problem:** `read_sector()` is a raw pass-through — it does not verify the stored CRC-32. The CRC check only happens in the external reader tool, not in the library itself.

**Plan:** Add `raid_read(uint32_t logical_sector, uint8_t *buf)` that:
1. Reads mirror 0 at `logical_sector`.
2. Verifies CRC-32 (bytes 508–511 against bytes 0–507).
3. On CRC failure, falls back to mirror 1 (and mirror 2 if M=3).
4. Returns the first valid mirror's payload, stripping the header and CRC.
5. Returns `STORAGE_ERR_UNRECOVERABLE` if all mirrors fail.

Keep `read_sector()` as the raw low-level primitive.

**Files:** `core/api/api.h`, `core/api/api.c`, `core/storage/storage.h`, `core/storage/storage.c`

---

### R5 — Remove hardcoded device path from Linux driver

**Problem:** `/dev/loop0` is hardcoded in `linux_driver.c` inside the `linux_ctx_t` initializer. Any different device requires a recompile.

**Plan:** Add a `linux_driver_init(const char *path)` factory function that fills `ctx.path` before returning `&linux_driver`. Document that the caller must call this before `setup_storage()`.

**Files:** `drivers/linux/linux_driver.c`, `drivers/linux/linux_driver.h` (new)

---

### R6 — Normalize message log capacity calculation

**Problem:** `save_msg()` has magic constants for the two-sector message log layout (506 bytes in sector 0 after the 6-byte header, 516 bytes in sector 1). These are not derived from the configurable constants and will silently break if `SECTOR_SIZE` or the header layout changes.

**Plan:** Replace magic numbers with derived constants:

```c
#define MSG_LOG_HEADER_SIZE  6
#define MSG_LOG_CAP_S0       (SECTOR_SIZE - MSG_LOG_HEADER_SIZE)   // 506
#define MSG_LOG_CAP_S1       SECTOR_SIZE                            // 512 (no header in s1)
#define MSG_LOG_TOTAL_CAP    (MSG_LOG_CAP_S0 + MSG_LOG_CAP_S1)     // 1018
```

**Files:** `config.h`, `core/api/api.c`

---

## Part 2 — New Features (Thesis §6 and §8)

Features described in the thesis as planned but not yet implemented.

---

### F1 — Configurable mirror count (M mirrors, majority voting)

**Thesis reference:** §8.1.1, §9.10

**Problem:** `RAID_MIRRORS` is hardcoded to 2. With 2 mirrors, you cannot do majority voting — you can only detect corruption (one bad mirror), not determine which one is correct without external knowledge.

**Plan:**
- Make mirror count a runtime parameter (part of `zinf_ctx_t`).
- Enforce M ≥ 1 and M odd (for majority voting to be meaningful). The thesis explicitly states M must be a positive odd integer > 1.
- In `log_raid_u8bit_values`: write to all M mirrors.
- In `raid_read` (from R4): collect up to M reads, verify each CRC, return the majority-agreed payload. If strict majority (> M/2) agree, return that value. Otherwise return `STORAGE_ERR_UNRECOVERABLE`.
- `RAID_OFFSET` formula: `usable / M` (generalizes current `usable / 2`).

**Files:** `config.h`, `config.c`, `core/storage/storage.c`, `core/api/api.c`

---

### F2 — YAML-based configuration

**Thesis reference:** §8.1.2, §9.9

The thesis describes replacing the hand-written `config.h`/`config.c` pair with a YAML file that is parsed by a code-generation step, producing the C header and source automatically.

**Plan (two phases):**

**Phase 2a — YAML schema definition:**

Define `zinf.yaml` format:
```yaml
zinf:
  sector_size: 512
  mirror_count: 3        # must be odd
  metadata_sectors: 2    # number of metadata sectors per mirror
  data_types:
    - name: sensor_t
      fields:
        - { name: temp,     type: float }
        - { name: humidity, type: float }
        - { name: ax,       type: int16_t }
        - { name: ay,       type: int16_t }
        - { name: az,       type: int16_t }
```

**Phase 2b — Code generator:**

A small Python or C tool (`tools/zinf_gen.py`) reads `zinf.yaml` and emits:
- `src/config/config.h` — constants and `sensor_t` typedef
- `src/config/config.c` — `config_init_defaults()` body

The Makefile runs the generator before compilation if the YAML is newer than the generated files.

**Files:** `tools/zinf_gen.py` (new), `zinf.yaml` (new), `src/Makefile`

---

### F3 — Desktop GUI (Tauri + Svelte)

**Thesis reference:** §8.3

A platform-independent desktop application for configuring ZINF and reading back stored data.

**Plan:**

```
zinf-gui/              (new top-level directory)
├── src-tauri/         (Rust backend)
│   ├── src/
│   │   ├── main.rs
│   │   ├── commands.rs    — Tauri commands wrapping zinf C library via FFI
│   │   └── reader.rs      — RAID read + CSV export
│   └── Cargo.toml
├── src/               (Svelte frontend)
│   ├── App.svelte
│   ├── views/
│   │   ├── ConfigGui.svelte    — graphical parameter editor
│   │   └── YamlEditor.svelte  — YAML editor with syntax highlighting
│   └── lib/
└── package.json
```

**Two views (dual-view, thesis §8.3):**
1. **Graphical GUI** — visual sliders and dropdowns for sector size, mirror count, data type fields
2. **YAML editor** — raw YAML with syntax highlighting, synced live with the GUI view

**Tauri backend exposes:**
- `read_image(path, raid_offset, mirrors)` → JSON array of records
- `write_config(yaml_text)` → generates and validates config
- `export_csv(path)` → calls the reader logic and saves CSV

**Files:** New `zinf-gui/` directory. No changes to `src/`.

---

### F4 — Configurable metadata sector count

**Thesis reference:** §8.1.2

Currently the metadata block is always exactly 2 sectors (alpha + omega = 1024 bytes). The thesis notes that up to 127 additional sectors could be allocated for message storage (63 KB total) without changing the pointer format.

**Plan:**
- Add `metadata_sectors` to `zinf_ctx_t` (default 2).
- Generalize `log_init_log_sector()` to zero-initialize N sectors instead of 2.
- Generalize `save_msg()` capacity: `total_capacity = (N sectors × SECTOR_SIZE) - MSG_LOG_HEADER_SIZE`.
- Expose `metadata_sectors` in the YAML config (F2).

**Files:** `config.h`, `core/storage/storage.c`, `core/api/api.c`

---

### F5 — 64-bit sector addressing

**Thesis reference:** §8.1.2

The thesis notes that 32-bit addressing (max ~2 TB at 512 B/sector) is sufficient for now but recommends upgrading to 64-bit (8 bytes) for future scalability to 8,388,608 PB.

**Plan:**
- Change `uint32_t` sector addresses to `uint64_t` everywhere in the API and storage engine.
- Update the metadata sector layout to store the last-sector pointer as 8 bytes (breaking format change — document as format version 3, combine with R2).
- Update `driver_t` fields: `total_sectors` and `total_size_bytes` are already `uint64_t` — verify consistency.
- The Linux driver's `pread`/`pwrite` offset is `off_t` which is 64-bit on modern Linux — no change needed there.

**Files:** `config.h`, `core/helper/driver.h`, `core/storage/storage.c`, `core/api/api.c`

---

### F6 — SD card driver (re-implementation)

**Thesis reference:** §9.1, §9.8

The original implementation targeted an SD card via SPI on an ATSAMD21G18AU MCU. The driver abstraction was created specifically to extract this. The SD driver was removed when the standalone project was created. It needs to be re-implemented as a proper `driver_t` plugin.

**Plan:**
```
drivers/sd/
├── sd_driver.h    — public header, exposes sd_driver_init(spi_handle_t)
└── sd_driver.c    — implements driver_t for SD/SDHC/SDXC via SPI
```

**Addressing modes:**
- SD (≤ 2 GB): byte address = `lba × 512`
- SDHC/SDXC (> 2 GB): block address = `lba`

Detection at `init` time via CMD58 (OCR register).

**Files:** `drivers/sd/` (new)

---

### F7 — Automated fault-injection test suite

**Thesis reference:** §10.3

The thesis describes a multi-level fault injection test methodology (bitflip, torn write, mirror inconsistency) run manually. This should become an automated test suite integrated into the build.

**Plan:**

```
tests/
├── framework.h         — minimal assert macros, no external deps
├── test_crc.c          — unit tests for crc32()
├── test_storage.c      — functional tests: write/read round-trip
├── test_fault.c        — fault injection: bitflip, torn write, mirror mismatch
└── Makefile
```

**Fault injection approach:**
- Use the Linux loopback driver against a fresh image file.
- After writing, `pwrite()` directly to the image file to corrupt specific sectors.
- Call `raid_read()` and verify correct recovery or correct error code.

**Test matrix (from thesis §10.3.1):**
- 0 corrupted sectors → all reads succeed
- Low corruption (300/4096 sectors) → full recovery via mirror
- Medium corruption (1300/4096) → majority voting needed (requires F1 with M=3)
- High corruption (2800/4096) → partial data loss, `STORAGE_ERR_UNRECOVERABLE` on affected sectors

**Files:** `tests/` (expanded)

---

## Part 3 — Nice-to-Have (Post-MVP)

These are explicitly listed in the thesis as out of scope (Not-MVP, Chapter 7) but architecturally consistent with the design.

| Item | Notes |
|---|---|
| Wear leveling | The append-only pattern already reduces random writes. True wear leveling requires block erase tracking — suitable for raw NAND driver only. |
| Encryption | Not a requirement. Would add a thin layer above the sector packing step. |
| Timestamp support | Alpha sector already has a `fejléc` (header) field reserved for time. A `uint32_t` Unix timestamp fits in 4 bytes of the existing 507-byte payload region. |
| POSIX-style read API | Not required; the thesis explicitly excludes POSIX interfaces. |
| Multi-stream logging | Currently a single append log. Multiple independent streams would require per-stream metadata sectors. |
| Compression | Explicitly excluded (would break determinism). |

---

## Suggested Implementation Order

| Priority | Item | Reason |
|---|---|---|
| 1 | R2 + R3 | On-disk format correctness — power-loss safety gap |
| 2 | R4 | RAID is incomplete without a reading path that verifies CRC |
| 3 | R1 | Enables testing; required for F1 and F3 |
| 4 | R5, R6 | Small cleanups, low risk |
| 5 | F1 (M=3) | Unlocks majority voting; required for the full fault tolerance model |
| 6 | F7 | Validates everything above; should gate further features |
| 7 | F2 (YAML config) | Enables F3; required for the GUI |
| 8 | F4, F5 | Scalability improvements |
| 9 | F3 (GUI) | Largest effort; depends on F2 |
| 10 | F6 (SD driver) | Hardware-specific; depends on R1 for clean driver swap |
