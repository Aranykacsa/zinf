# ZINF — Minimalist filesystem for IoT and embedded systems

ZINF (Zinf Is Not FAT) is a lightweight, RAID-mirrored data logging library designed for IoT and embedded systems where data integrity and resource efficiency are paramount.

## Key Features
- **RAID-1 Mirroring:** Dual on-disk copies for single-failure tolerance.
- **CRC32 Integrity:** Every sector is checksummed for error detection.
- **Pluggable Drivers:** Easy porting to SD cards (SPI), EEPROMs, or custom block devices.
- **Low-RAM Footprint:** Designed for microcontrollers (Cortex-M, RP2350, etc.).
- **ZINF Studio:** Modern GUI for data extraction, configuration, and SDK generation.

---

## 1. ZINF Studio Installation

ZINF Studio is a cross-platform (Tauri-based) desktop application for researchers and developers. It allows for "zero-code" data extraction and hardware configuration.

### Prerequisites
- **Node.js & npm** (v18+)
- **Rust & Cargo** (Latest stable)
- **Linux Build Tools** (gcc, make, pkg-config, etc.)

### Build and Install
Run the following command from the repository root:
```bash
make install-studio
```
This will:
1. Build the Tauri application.
2. Install the `zinf-studio` binary to `/usr/local/bin`.
3. Configure `udev` rules to grant your user (`plugdev` group) access to ZINF-formatted devices without `sudo`.

---

## 2. C Library Integration

To integrate ZINF into your embedded C project, follow these steps:

### Files to Include
Add these portable core files to your build system (Makefile, CMake, etc.):
- `src/core/api/api.c`
- `src/core/storage/storage.c`
- `src/core/helper/helper.c`
- `src/config/config.c` (Generated from `zinf.yaml`)
- `src/drivers/sd/sd_driver.c` (If using SPI SD cards)

### Include Paths
Ensure your compiler can find the following directories:
```bash
-Isrc/core/api -Isrc/core/storage -Isrc/core/helper -Isrc/config -Isrc/drivers/sd
```

### Driver Setup & Wiring
Initialize the ZINF context and wire up your hardware driver in your `main()`:

```c
#include "api.h"
#include "config.h"
#include "sd_driver.h"

// 1. Define the ZINF runtime context
zinf_ctx_t zinf_ctx;

void setup_zinf(void) {
    // 2. Platform-specific SPI hardware init (e.g., Pico SDK)
    my_spi_init();
    
    // 3. Bind SPI callbacks to the SD driver
    sd_driver_init_spi(NULL, &my_spi_ops);
    
    // 4. Assign driver and init context
    zinf_ctx.driver = &sd_driver;
    zinf_ctx_init_defaults(&zinf_ctx);
    
    // 5. Hardware-level driver init (queries card size)
    if (zinf_ctx.driver->init(zinf_ctx.driver) == DRIVER_OK) {
        // 6. Compute RAID offset based on total sectors
        uint64_t usable = zinf_ctx.driver->total_sectors - zinf_ctx.metadata_sectors;
        zinf_ctx.mirror_offset = usable / zinf_ctx.mirror_count;
        zinf_ctx.raid_offset = zinf_ctx.mirror_offset;
        
        // 7. Initialize storage (only if new or formatted)
        setup_storage(&zinf_ctx);
        init_log_sector(&zinf_ctx);
    }
}
```

---

## Repository Structure

```
zinf/
├── src/
│   ├── core/
│   │   ├── api/        # High-level Storage API
│   │   ├── storage/    # RAID & Sector Engine
│   │   └── helper/     # CRC32 & Driver Interface
│   ├── drivers/        # Hardware Backends (SD, Linux, Mock)
│   └── config/         # Codegen settings (from zinf.yaml)
├── zinf-studio/        # Tauri Desktop Application
├── tools/              # udev rules, zinf_gen.py, zinf-probe
├── tests/              # Comprehensive Unit & Fault Tests
└── docs/               # Technical Documentation
```

## Documentation
For detailed guides on benchmarking, fault injection, and porting, see the [docs/](docs/) directory:
- [Architecture](docs/architecture.md)
- [API Reference](docs/api-reference.md)
- [Embedded Porting Guide](docs/embedded-porting.md)
- [Benchmarking](docs/benchmarking.md)
