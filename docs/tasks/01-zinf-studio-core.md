# Task 1: ZINF Studio Core & Backend (Tauri + Rust FFI)

**Context:**
ZINF Studio is a cross-platform desktop application that replaces the CLI for researchers. Build a Tauri application (Rust backend, SvelteKit frontend) that wraps the existing ZINF C library via FFI to perform raw block device reads and RAID verification.

---

## UI Layout (Polished MVP)

Three-panel layout using TailwindCSS:

```
┌────────────────────────────────────────────────────────────┐
│  ZINF Studio                                    [⚙ Config] │
├──────────────┬─────────────────────────────────────────────┤
│ Devices      │  Sector Data / CSV Preview                  │
│              │                                             │
│ /dev/sdb     │  sector | field_0 | field_1 | field_2 ...  │
│   zinf v4    │  0      | 23.50   | 65.00   | ...          │
│ /dev/sdc     │  1      | 23.51   | 64.99   | ...          │
│              │  ...                                        │
│ [Scan]       │                                             │
│              │  [Extract to CSV]  [Verify Integrity]       │
└──────────────┴─────────────────────────────────────────────┘
```

**Left sidebar — Device List:**
- `scan_zinf_devices()` populates a list of detected ZINF block devices.
- Each entry shows: device path + format version (read from sector 0 magic bytes).
- `[Scan]` button re-runs the scan.
- Selecting a device loads a preview of the first N sectors into the main panel.

**Main panel — Data Preview:**
- Table with one row per logical sector, one column per sensor field defined in `zinf.yaml`.
- Column headers are the `name` values from `zinf.yaml` `data_types[0].fields`.
- Rows stream in via Tauri events as sectors are read; show a loading indicator while in progress.

**Settings drawer (⚙ Config button):**
- Opens a right-side drawer.
- Two tabs: **Visual** (dropdowns/sliders for `mirror_count`, `sector_size`, `metadata_sectors`) and **YAML** (raw editor with syntax highlighting, synced live with the Visual tab).
- "Save & Regenerate" button calls `generate_config(yaml_text)` which writes `zinf.yaml` and re-runs `zinf_gen.py`.

---

## Build Pipeline Order

The build must run in this sequence:

1. `python3 tools/zinf_gen.py zinf.yaml src/config/` — regenerates `config.h` / `config.c`
2. `cc` crate in `build.rs` compiles the C sources (depends on generated `config.h`)
3. `bindgen` in `build.rs` generates `$OUT_DIR/bindings.rs` from `api.h` + `config.h`

In `build.rs`, use `cargo:rerun-if-changed=../../zinf.yaml` so that any YAML change triggers a full rebuild.

---

## Step-by-Step Instructions for LLM

### 1. Initialize the Tauri Application

- Create `zinf-studio/` at the repository root.
- Run `cargo create-tauri-app` targeting SvelteKit, TypeScript, and TailwindCSS.
- In `tauri.conf.json`, add these permissions:
  ```json
  "fs": { "all": true, "scope": ["/dev/**", "/sys/block/**"] },
  "shell": { "open": false }
  ```
- Set `"withGlobalTauri": true` to expose the Tauri API in the frontend.

### 2. C Library Integration (Rust FFI)

- In `zinf-studio/src-tauri/Cargo.toml`:
  ```toml
  [build-dependencies]
  cc = "1"
  bindgen = "0.69"

  [dependencies]
  libc = "0.2"
  ```
- Create `zinf-studio/src-tauri/build.rs`:
  - Compile `../../src/core/api/api.c`, `../../src/core/storage/storage.c`, `../../src/core/helper/helper.c`, `../../src/config/config.c`.
  - Include paths: `../../src/core/api/`, `../../src/core/storage/`, `../../src/core/helper/`, `../../src/config/`, `../../src/drivers/linux/`.
  - Run `bindgen` on `../../src/core/api/api.h` with `clang_args` pointing to the same include dirs.
  - Emit `$OUT_DIR/bindings.rs`.
- In `src-tauri/src/lib.rs`: `include!(concat!(env!("OUT_DIR"), "/bindings.rs"));`

### 3. Block Device Access Module (`src-tauri/src/hardware.rs`)

- Implement `fn open_device(path: &str) -> Result<std::fs::File, String>` using `OpenOptions::new().read(true).write(true).custom_flags(libc::O_DIRECT).open(path)`.
- Implement `fn read_sector_raw(file: &mut File, lba: u64, sector_size: usize) -> Result<Vec<u8>, String>` using `pread` via `libc::pread64`.
- Implement Tauri command `scan_zinf_devices() -> Vec<DeviceInfo>`:
  - Walk `/sys/block/` to enumerate block device names.
  - For each device, read sector 0 via `read_sector_raw`.
  - Check bytes `[0..4]` == `[0x5A, 0x49, 0x4E, 0x46]` (magic `ZINF`).
  - If match: push `DeviceInfo { path: "/dev/<name>", version: u16_le(bytes[4..6]) }`.
  - Returns `Vec<DeviceInfo>` (serialized to JSON for the frontend).

### 4. Data Extraction & Verification Command

- Implement Tauri command `extract_data(device_path: String, output_csv: String) -> Result<(), String>`:
  - Open device, call `setup_storage` FFI to initialize `zinf_ctx_t`.
  - Read `zinf.yaml` from disk (located next to the `zinf-studio` binary, fallback to `../../zinf.yaml`).
  - Parse `data_types[0].fields` to determine column names and types (`float` → `f32`, `int16_t` → `i16`, etc.).
  - Iterate logical sectors 1..`last_sector`. For each:
    - Call FFI `raid_read(&mut ctx, sector, payload.as_mut_ptr())`.
    - Deserialize payload bytes according to field types (LE byte order, matching `sensor_to_wire` in `api.c`).
    - Write one CSV row.
  - Emit Tauri event `"extract-progress"` with `{ current: u64, total: u64 }` every 100 sectors.
- Implement Tauri command `verify_integrity(device_path: String) -> ScrubReport`:
  - Calls FFI `zinf_scrub(&mut ctx, 1, last_sector, &mut report)`.
  - Returns `ScrubReport { checked, healthy, repaired, unrecoverable }` as JSON.

### 5. CSV Column Mapping from `zinf.yaml`

The CSV header line is derived at runtime from the YAML schema:

```yaml
# zinf.yaml example
data_types:
  - name: sensor_t
    fields:
      - { name: temp,     type: float   }   # 4 bytes LE f32
      - { name: humidity, type: float   }   # 4 bytes LE f32
```

Produces CSV header: `sector,temp,humidity`

Wire format is the same as `sensor_to_wire()` in `api.c`: each `float` is 4 bytes LE. Future field types must be added to both `sensor_to_wire` and the Rust deserializer together.

---

## Deliverables Checklist

- [ ] `zinf-studio/` Tauri project initialized
- [ ] `build.rs` compiles C sources + generates bindings
- [ ] `hardware.rs` — `scan_zinf_devices`, `open_device`
- [ ] `commands.rs` — `extract_data`, `verify_integrity`, `generate_config`
- [ ] SvelteKit frontend — device sidebar, data table, settings drawer
- [ ] `zinf.yaml` parsed at runtime for column names
- [ ] Tauri events for progress streaming
