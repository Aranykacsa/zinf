# Task 2: Zero-Toolchain UF2 Config-Injection

**Context:**
Researchers must be able to change their data schema (`zinf.yaml`) and flash their MCU without compiling C code. We achieve this by patching a pre-compiled `.uf2` firmware binary on-the-fly using the ZINF Studio Rust backend.

> **Status:** Pre-compiled base UF2 firmware images do not yet exist. Steps 1–3 (C modification, Rust UF2 parser, binary serialization) should be implemented fully. Step 4 (patching and flashing) should implement the patching logic but the actual write-to-MCU-drive step is a stub — it will be wired up once base firmware images are available.

---

## `config_blob` Binary Layout

The blob is exactly 64 bytes, placed at a known section in flash so the Rust patcher can find it by magic:

```
Offset  Size  Type      Field
------  ----  --------  -----
0       4     u8[4]     Magic: 0x5A 0x49 0x4E 0x46  ("ZINF")
4       1     u8        Blob format version: 0x01
5       1     u8        mirror_count (e.g. 2 or 3)
6       2     u16_le    sector_size  (e.g. 512)
8       2     u16_le    metadata_sectors (e.g. 2)
10      2     u16_le    payload_size (e.g. 507) — derived, for MCU validation
12      4     u32_le    raid_offset  (computed by host from device total_sectors)
16      48    u8[48]    reserved / zero-padded
```

Total: 64 bytes. The MCU runtime calls `zinf_ctx_init_defaults()` which checks the magic; if valid, it reads `mirror_count`, `sector_size`, `metadata_sectors`, and `raid_offset` from the blob instead of compile-time constants.

---

## Step-by-Step Instructions for LLM

### 1. C Code Modifications (`src/config/config.c`)

- Add the blob struct at file scope, section-mapped so the linker places it at a predictable flash address:
  ```c
  #include <stdint.h>

  __attribute__((section(".zinf_config_blob"), used))
  volatile const uint8_t config_blob[64] = {
      0x5A, 0x49, 0x4E, 0x46,  /* magic */
      0x01,                    /* blob version */
      RAID_MIRRORS,            /* mirror_count */
      (uint8_t)(SECTOR_SIZE & 0xFF), (uint8_t)(SECTOR_SIZE >> 8),
      0x02, 0x00,              /* metadata_sectors = 2 */
      (uint8_t)(PAYLOAD_SIZE & 0xFF), (uint8_t)(PAYLOAD_SIZE >> 8),
      0x00, 0x00, 0x00, 0x00,  /* raid_offset placeholder (patched by host) */
      /* 48 bytes reserved */
  };
  ```
- Update `zinf_ctx_init_defaults(zinf_ctx_t *ctx)` in `config.c`:
  ```c
  extern volatile const uint8_t config_blob[64];

  void zinf_ctx_init_defaults(zinf_ctx_t *ctx) {
      if (config_blob[0] == 0x5A && config_blob[1] == 0x49 &&
          config_blob[2] == 0x4E && config_blob[3] == 0x46) {
          ctx->mirror_count      = config_blob[5];
          ctx->sector_size       = (uint16_t)config_blob[6] | ((uint16_t)config_blob[7] << 8);
          ctx->metadata_sectors  = (uint16_t)config_blob[8] | ((uint16_t)config_blob[9] << 8);
          ctx->mirror_offset     = (uint32_t)config_blob[12]
                                 | ((uint32_t)config_blob[13] << 8)
                                 | ((uint32_t)config_blob[14] << 16)
                                 | ((uint32_t)config_blob[15] << 24);
      } else {
          /* Fall back to compile-time constants */
          ctx->mirror_count      = RAID_MIRRORS;
          ctx->sector_size       = SECTOR_SIZE;
          ctx->metadata_sectors  = 2;
          ctx->mirror_offset     = ctx->raid_offset;
      }
      ctx->log_sector       = 0;
      ctx->bad_sector_count = 0;
  }
  ```
- Add a linker script section entry for RP2350 target in `CMakeLists.txt` (embedded target only):
  ```
  KEEP(*(.zinf_config_blob))
  ```

### 2. Rust UF2 Parser (`src-tauri/src/uf2.rs`)

UF2 block structure (512 bytes total):
```
[0..3]   Magic 1: 0x0A324655 ("UF2\n")
[4..7]   Magic 2: 0x9E5D5157
[8..11]  Flags
[12..15] Target address in flash
[16..19] Payload size (always 256)
[20..23] Block number (0-indexed)
[24..27] Total blocks
[28..31] Family ID / file size
[32..287] 256 bytes of payload data
[288..507] Padding (zeros)
[508..511] Magic 3: 0x0AB16F30
```

Implement:
- `fn parse_uf2_blocks(bytes: &[u8]) -> Vec<Uf2Block>` — splits into 512-byte blocks, validates all three magic values, returns `Vec<Uf2Block>` (fields: `target_addr`, `payload: [u8; 256]`, `block_no`).
- `fn find_config_blob(blocks: &[Uf2Block]) -> Option<(usize, usize)>` — scans block payloads for the 4-byte magic `0x5A494E46`; returns `(block_index, byte_offset_within_payload)`.

### 3. Binary Serialization of GUI State (`src-tauri/src/config_blob.rs`)

```rust
#[repr(C, packed)]
pub struct ZinfConfigBlob {
    magic: [u8; 4],       // always [0x5A, 0x49, 0x4E, 0x46]
    blob_version: u8,     // always 0x01
    mirror_count: u8,
    sector_size: u16,     // LE
    metadata_sectors: u16, // LE
    payload_size: u16,    // LE — derived: sector_size - HEADER_SIZE - 4
    raid_offset: u32,     // LE — passed in from device total_sectors
    _reserved: [u8; 48],
}

impl ZinfConfigBlob {
    pub fn from_config(cfg: &ZinfYamlConfig, total_sectors: u64) -> Self { ... }
    pub fn to_bytes(&self) -> [u8; 64] { ... }
}
```

`ZinfYamlConfig` is the Rust struct deserialized from `zinf.yaml` via `serde_yaml`:
- `mirror_count: u8`
- `sector_size: u16`
- `metadata_sectors: u16`

### 4. Patching Command (Partial — Stub Flash Step)

Implement Tauri command `patch_firmware(base_uf2_path: String, config: ZinfYamlConfig, device_total_sectors: u64) -> Result<Vec<u8>, String>`:
1. Load and parse `base_uf2_path` via `parse_uf2_blocks`.
2. Call `find_config_blob` to locate the blob position.
3. Serialize the new config via `ZinfConfigBlob::from_config(&config, device_total_sectors).to_bytes()`.
4. Overwrite the 64 bytes at the found position in the relevant UF2 block payload.
5. Recalculate any affected block fields (no CRC in UF2 format — just rewrite the payload bytes).
6. Return the modified UF2 bytes as `Vec<u8>`.

**Stub flash step** (implement as TODO placeholder):
```rust
#[tauri::command]
async fn flash_to_device(uf2_bytes: Vec<u8>, target_drive: String) -> Result<(), String> {
    // TODO: Write uf2_bytes to target_drive once base firmware images exist.
    // Example target_drive: "/run/media/user/RPI-RP2/"
    Err("Flash step not yet implemented — base firmware images pending".to_string())
}
```

---

## Deliverables Checklist

- [ ] `config_blob[64]` section added to `src/config/config.c`
- [ ] `zinf_ctx_init_defaults` reads from blob when magic is valid
- [ ] `uf2.rs` — `parse_uf2_blocks`, `find_config_blob`
- [ ] `config_blob.rs` — `ZinfConfigBlob`, `from_config`, `to_bytes`
- [ ] `patch_firmware` Tauri command returns modified UF2 bytes
- [ ] `flash_to_device` Tauri command stubbed with clear TODO comment

## Blocked By

- Pre-compiled RP2350 base firmware `.uf2` images (does not yet exist — needed to test end-to-end flashing).
