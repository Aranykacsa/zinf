# ZINF Researcher Edition: End-to-End Appliance Roadmap

## Executive Summary

To pivot ZINF from a developer-focused C library to a **boxed product for researchers**, we must eliminate friction for end-users while retaining flexibility for hardware developers. Researchers need bulletproof data logging without the burden of complex toolchains, whereas developers building for custom MCUs still need robust compilation support.

This roadmap details the architectural pivot to a **Boxed Appliance Model**, expanding upon the MVP features (F2: YAML config, F3: Desktop GUI). It centers around three pillars:
1. **ZINF Studio (Tauri App) & 1-Click Install:** A streamlined Linux-first desktop GUI (with future Windows/macOS support) for zero-code configuration, extraction, and seamless updates.
2. **Config-Injected Firmware (For Standard Users):** Eliminating MCU compilation steps for standard hardware by patching pre-compiled binaries via the GUI.
3. **Guided Integration SDK (For Developers):** A heavily scaffolded, hand-holding wizard for users who *do* need to integrate ZINF into their custom hardware codebases and compile it themselves.
4. **Massive-Scale Randomized Testing:** Scaling the reliability suite to 1,000,000+ iterations using a dual-path (RAM + Loopback) fuzzer to prove bulletproof durability.

---

## 1. ZINF Studio & One-Script Installation

ZINF Studio will replace the Linux CLI for the average researcher, ensuring a frictionless experience from day one.

### 1.1 The "One-Script" Installer
Researchers will only need to run a single installation script (or install the app package). This script will automatically handle all underlying dependencies, install the Tauri app, set up necessary udev rules, and prepare the environment. 
*   **Current Target:** Linux-first (optimized for standard research environments).
*   **Future Targets:** Windows and macOS support will follow once the Linux foundation is solid.
*   **Seamless Updates:** The installer and the Tauri app will feature built-in, easy update mechanisms to pull the latest ZINF features without requiring manual recompilation of tools.

### 1.2 Architecture & Technology Stack
*   **Frontend:** SvelteKit + TailwindCSS. Provides a responsive, dual-view interface (Visual Configurator ↔ Raw YAML Editor).
*   **Backend:** Rust (Tauri Core). Wraps the existing ZINF C library via FFI (Foreign Function Interface) to reuse `raid_read()`, `zinf_crc32()`, and the `zinf_ctx_t` parsing logic.
*   **Hardware Bridge:** Rust native libraries to detect ZINF-formatted block devices *before* the host OS attempts to mount them.

### 1.3 The YAML Configuration Core
The `zinf.yaml` file remains the absolute source of truth. The Visual Configurator acts as a friendly wrapper around it, allowing researchers to define payload structures (`sensor_t`), adjust `mirror_count`, and calculate `PAYLOAD_SIZE` visually, while preserving the raw YAML for advanced users and version control.

### 1.4 One-Click Data Extraction & Recovery
*   **Raw Sector Reads:** Direct block reads bypassing standard OS filesystems.
*   **RAID Verification:** Runs `raid_read()` across all mirrors applying majority voting.
*   **CSV Export:** Automatically serializes the wire format into timestamped `.csv` files.
*   **Scrubbing:** Exposes a "Verify Data Integrity" button that runs `zinf_scrub()`.

---

## 2. Zero-Toolchain Deployment (For Standard Hardware)

**The Problem:** Standard researchers shouldn't have to compile C code just to change a sampling rate or add a new field to `sensor_t`.

**The Solution:** For off-the-shelf development boards (e.g., RP2350), ZINF Studio will ship **pre-compiled base firmwares** and patch the `zinf.yaml` configuration into them directly.

### 2.1 On-the-Fly Binary Patching
1. The base firmware contains a predictable `config_blob` section in flash.
2. When the researcher clicks "Apply Config" in the GUI, the Rust backend serializes the YAML into a binary representation.
3. The app overwrites the placeholder bytes in the `.uf2` file and recalculates checksums.
4. **Drag-and-Drop Flashing:** The app copies the custom `.uf2` directly to the connected MCU bootloader drive. The MCU updates instantly.

---

## 3. The Guided Integration SDK (For Custom Hardware Developers)

While standard users get a zero-toolchain experience, **we acknowledge that developers building for custom MCUs absolutely require complex toolchains** to compile code for their specific hardware. 

For these users, we will provide a heavily scaffolded, "hand-holding" guide that makes integrating the ZINF C library into their existing codebase impossible to fail.

### 3.1 Step-by-Step Code Integration Wizard
Instead of just throwing the library at the developer, ZINF Studio will generate an integration template tailored to their project. 
*   **How to compile:** The wizard provides exact Makefile/CMake snippets to link `api.c`, `storage.c`, and generated `config.c` alongside the developer's code.
*   **How to use APIs:** It generates a commented `main.c` example showing exactly how to initialize `zinf_ctx_t`, wire up the block driver callbacks, and call `raid_sensor_values()`.

### 3.2 Hand-Holding Code Generation
If the developer needs to integrate ZINF into their main application or write a custom sensor driver, the wizard outputs a template packed with compiler `#error` guards to ensure every necessary step is completed.

```c
#include "zinf_api.h"

// =========================================================================
// 🛑 STOP! READ THIS FIRST 🛑
// Welcome to the ZINF Integration Wizard. Fill in the areas marked with "STEP X".
// =========================================================================

// STEP 1: Include your hardware-specific SPI/I2C headers here.
// #include "my_mcu_spi.h"

#ifndef MY_HARDWARE_READY
  #error "ZINF SDK: You forgot to set up your hardware includes in STEP 1!"
#endif

// STEP 2: Implement your sensor reading logic here.
void read_custom_sensor_and_log(zinf_ctx_t *ctx) {
    sensor_t my_data = {0};

    // -> Read from your bus here and map it to the fields in zinf.yaml
    // my_data.temp = read_temperature();

    // STEP 3: Write to the ZINF RAID storage
    uint8_t rc = raid_sensor_values(ctx, &my_data, 1);
    if (rc != STORAGE_OK) {
        // Handle error...
    }
}
```

### 3.3 The "Driver Sandbox" (Desktop Mocking)
To prevent tedious hardware compile-flash-debug cycles, the generated SDK includes a desktop-compilable `sandbox.c`. Developers can input raw hex bytes simulating their sensor's bus response and verify their parsing/bit-shifting logic directly on their Linux machine before cross-compiling for the MCU.

---

## 4. Maintenance, Recovery, and Seamless Portability

Hardware in the field gets corrupted, power-loss happens, and SD cards die. The appliance model makes recovery foolproof.

### 4.1 The "Nuke and Pave" Rescue Mode
If an SD card becomes unreadable, ZINF Studio provides a "Factory Reset" flow:
1. **Low-Level Wipe:** Writes `0x00` directly to the first 10,000 sectors.
2. **Metadata Regeneration:** Sets up the Format v4 metadata header and copy slots natively from the desktop.
3. **Base Reflash:** Prompts the user to push the latest verified firmware.

### 4.2 The `.zinf_project` Archive
ZINF Studio stores all configurations (`zinf.yaml`), custom `.c` drivers, and Makefiles in a single compressed `.zinf_project` archive. This makes it trivial to version control the setup, share it with other researchers, and instantly restore the environment on a new machine.

---

## 5. Massive-Scale Reliability & Randomized Benchmarking

To ensure the "Boxed Appliance" is bulletproof, we are scaling our testing infrastructure.

### 5.1 Dual-Path Fuzzing (RAM + Hardware)
We will implement an **In-Memory RAM Driver** that executes 1,000,000+ logical fault tests (SEU, metadata corruption) per minute. Parallel to this, we will run hardware-accurate tests via the Linux loopback driver to verify POSIX block guarantees.

### 5.2 Deep State Machine Fuzzing
Tests will no longer be single-shot. The fuzzer will simulate years of field attrition by randomly chaining:
*   Interleaved writes and scrubs.
*   Power-loss simulations (instant buffer drops).
*   Targeted metadata attrition (zeroing copy slots, forcing version wraparounds).

### 5.3 Real-World Randomized Benchmarking
`zinf bench` will be updated to use randomized payload sizes and injected timing jitter, providing researchers with throughput and latency metrics that match their actual sensor sampling rates.
