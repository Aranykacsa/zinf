---
id: getting-started
title: Getting Started
sidebar_label: Getting Started
sidebar_position: 2
description: Build, install, format a device, and run ZINF for the first time.
---

# Getting Started

## Why ZINF?

Research and data-acquisition systems must record large volumes of measurements in environments where power, memory, and storage reliability are severely constrained — think satellites, polar monitoring stations, or autonomous sensor platforms running without human supervision.

General-purpose filesystems (FAT32, ext4, LittleFS, SPIFFS) are unsuitable here for three reasons:

- **Non-deterministic timing** — garbage collection and journaling introduce unpredictable write latencies that break real-time guarantees.
- **Poor crash tolerance** — partial writes can leave metadata in an inconsistent state that requires expensive repair (`fsck`) or causes silent data loss.
- **Unnecessary complexity** — directory trees, permission bits, and large metadata structures waste RAM and flash on microcontrollers.

ZINF is designed around the opposite set of constraints: write latency is bounded by `mirror_count × T_sector_write`, every sector carries a CRC-32, and RAID-1 mirroring with majority voting makes single-sector bit-rot detectable and correctable without any offline repair step.

---

## Install

```bash
git clone <repo-url>
cd zinf
make
sudo make install
```

Installs `zinf` and `zinf-probe` to `/usr/local/bin` and registers the udev rule so ZINF-formatted SD cards are automatically detected when inserted.

**Prerequisites:** GCC (C11), GNU Make, Linux.

---

## Format a Device

```bash
# SD card or USB drive
sudo zinf format /dev/sdb

# Image file (useful for testing without hardware)
dd if=/dev/zero of=experiment.img bs=1M count=4
zinf format experiment.img
```

Both commands initialize ZINF metadata (magic header + RAID layout) in a single step.

---

## Inspect a Device

### Option A: Desktop GUI (Recommended)
Open **ZINF Studio** and select the device in the sidebar. The **Explorer** tab provides a real-time summary of sector geometry, RAID offset, and usable capacity.

### Option B: CLI
```bash
zinf info /dev/sdb
```

Output:
```
Image          : /dev/sdb
Sector size    : 512
Payload size   : 507
Mirrors        : 2
Total sectors  : 7744512
Usable sectors : 7744510
RAID offset    : 3872255
```

---

## Detect ZINF Devices

After `sudo make install`, inserting a ZINF-formatted SD card fires the udev rule automatically. **ZINF Studio** will automatically populate detected devices in the sidebar.

```bash
# Read udev properties (works out of the box after install)
udevadm info /dev/sdb | grep ID_FS
# → ID_FS_TYPE=zinf
# → ID_FS_VERSION=4

# lsblk with udev properties
lsblk --properties-by file,udev -f /dev/sdb
# → NAME  FSTYPE  FSVER ...
# → sdb   zinf    4
```

For standard `lsblk -f` and `blkid` recognition (patches the system libblkid — takes ~1 min, requires internet):

```bash
sudo make install-blkid
# then:
lsblk -f /dev/sdb        # → FSTYPE=zinf
blkid /dev/sdb           # → TYPE="zinf"
```

---

## Write and Verify Data

### 1. Write via CLI
One-shot sensor write:
```bash
sudo zinf -d /dev/sdb raid sensor 1 23.5 65.0
```

### 2. Verify via Studio
Open **ZINF Studio**, select the device, and click **[ Extract Data ]** in the **Explorer** tab. The new record will appear instantly in the "Retro Creamy" table view.

---

## Interactive Shell
For advanced CLI tasks:
```bash
sudo zinf shell /dev/sdb
> help
> raid sensor 2 23.5 65.0 24.1 62.3
> msg save 42
> cfg show
```

---

## Benchmark

```bash
sudo zinf bench /dev/sdb > results.csv
```

Outputs a CSV with throughput and latency columns for a range of payload sizes.
See [Benchmarking](testing/benchmarking.md) for details.

---

## Embedded (RP2350 / Pico)

A card formatted with `zinf format` can be inserted directly into the MCU — the on-disk layout is identical. See [Embedded Porting Guide](embedded-porting.md) for SPI wiring, CMakeLists.txt, and first-boot initialization code.

---

## Uninstall

```bash
sudo make uninstall
```

---

## Full Example (loopback test without hardware)

```bash
# Build
make

# Create and format a test image
dd if=/dev/zero of=test.img bs=1M count=8 status=none
./src/zinf format test.img

# Confirm ZINF magic
od -A x -t x1 test.img | head -1
# → 000000 5a 49 4e 46 04 00 ...

# Attach as loop device and verify detection
sudo losetup -f --show test.img     # → /dev/loopN
sudo udevadm trigger --action=add /dev/loopN && sudo udevadm settle
udevadm info /dev/loopN | grep ID_FS
# → ID_FS_TYPE=zinf
# → ID_FS_VERSION=4

# Cleanup
sudo losetup -d /dev/loopN
```
