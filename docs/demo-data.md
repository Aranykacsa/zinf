---
id: demo-data
title: Demo Data
sidebar_label: Demo Data
sidebar_position: 10
description: Create a loopback device and populate it with synthetic sensor data for testing.
---

# Creating a ZINF Demo Loopback Device

This guide describes how to create a loopback device and populate it with synthetic sensor data for testing purposes.

## 1. Regenerate Configuration and Build
Ensure the C source files match the latest `zinf.yaml` configuration and build the `zinf` binary.
```bash
python3 tools/zinf_gen.py zinf.yaml src/config/
make
```

## 2. Create and Format an Image File
Create a blank 8MB image and initialize the ZINF metadata.
```bash
dd if=/dev/zero of=zinf_data.img bs=1M count=8
./src/zinf format zinf_data.img
```

## 3. Attach as a Loopback Device
Attach the image to a loop device. This requires `sudo` privileges.
```bash
sudo losetup -f --show zinf_data.img
# This will output the device path, e.g., /dev/loop0
```

## Status
The image file `zinf_data.img` has been successfully generated and formatted as ZINF v4.

## Writing Data
You can add data to the ZINF partition using one-shot commands or the interactive shell.

### Option A: One-shot CLI
The `raid sensor` command takes a `<count>` followed by `<count>` values.
```bash
# Write 5 records to the image file
./src/zinf -d zinf_data.img raid sensor 5 20 21 22 23 24
```

### Option B: Automatic Filling
Use the `fill` command to automatically populate the entire storage with synthetic sensor data.
```bash
./src/zinf -d zinf_data.img fill
```
This is the fastest way to prepare a device for large-scale testing or visualization.

## Interactive Shell
Use the shell for a more traditional CLI experience.
```bash
sudo ./src/zinf shell /dev/loop0
```

## Visualizing with `lsblk`
By default, `lsblk` may not show the ZINF filesystem type. To enable detection:

1. **Install udev rules and probe:**
   ```bash
   sudo make install
   ```
2. **Trigger udev detection for the loop device:**
   ```bash
   sudo udevadm trigger --action=add /dev/loop0
   sudo udevadm settle
   ```
3. **Verify with `udevadm`:**
   ```bash
   udevadm info /dev/loop0 | grep ID_FS
   ```

For full `lsblk -f` integration (shows `zinf` in the FSTYPE column), run:
```bash
sudo make install-blkid
```

## Cleanup
To detach the loopback devices:
```bash
sudo losetup -d /dev/loop0 /dev/loop1
```
