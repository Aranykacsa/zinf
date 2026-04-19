# ZINF Project Memory

## Current State (as of 2026-04-19)
ZINF (Zinf Is Not FAT) is a minimalist, RAID-mirrored filesystem optimized for sensor data logging in resource-constrained environments (IoT, CanSat).

### Recent Achievements
- **README Overhaul:** Refactored for clarity, including installation and integration guides.
- **Studio Extraction Fix:** Optimized `extract_data` command in ZINF Studio. It now handles multiple records per sector (batching), streams data directly to CSV to prevent memory issues, and limits the UI preview to 1000 rows to prevent freezes.
- **Project Renaming:** Officially transitioned to "ZINF – Minimalista fájlrendszer IoT és űripari környezetekhez".

### Architecture
- **Core:** C11 portable library (API, Storage, Helper, Config).
- **Drivers:** Linux Loopback, SPI SD Card, RAM Mock.
- **Studio Backend:** Rust (Tauri 2), FFI to C library.
- **Studio Frontend:** SvelteKit + TailwindCSS.

### Current Objective: Studio UI/UX Overhaul
Refactoring ZINF Studio based on `docs/plans/studio_fixes.md`:
- **Theme:** Single dark theme using custom palette (Dark Navy, Slate Blue, Muted Teal, Pastel Pink).
- **UI Standard:** GNOME HIG (Header Bar, Segmented Controls, rounded-xl).
- **Frameworks:** Integration of `shadcn-svelte`.
- **Layout:** SPA with top-level tabs (Data Explorer, Configuration, SDK Generator).
- **Features:** 
    - Advanced Visual Configuration (full YAML mapping).
    - Import/Export (CSV offline preview, YAML loading/saving, SDK source export).

## Technical Notes
- **Payload Logic:** MCU batches up to 63 `sensor_t` (8 bytes) into one 507-byte sector payload. Studio must iterate through these chunks and skip `0x00` padding.
- **FFI Boundary:** C global `zinf_ctx` is used; Rust backend uses a Mutex lock to serialize access since the C library isn't thread-safe.

## Pending Tasks
- [ ] Initialize `shadcn-svelte` in `zinf-studio`.
- [ ] Define custom Tailwind color palette.
- [ ] Refactor `+page.svelte` into a tabbed SPA layout.
- [ ] Expand Visual Configurator to handle nested `data_types` and all YAML fields.
- [ ] Implement YAML file loading and "Save As" functionality.
- [ ] Implement Offline CSV preview.
