/// Block device enumeration and raw sector I/O.
use serde::Serialize;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};

/// ZINF metadata magic bytes (META_MAGIC_B0..B3 in config.h)
const ZINF_MAGIC: [u8; 4] = [0x5A, 0x49, 0x4E, 0x46]; // "ZINF"

#[derive(Debug, Clone, Serialize)]
pub struct DeviceInfo {
    /// Path used to open the device (always /dev/loopX or /dev/sdX …)
    pub path: String,
    /// Detected ZINF format version (bytes [4..6] of sector 0, LE)
    pub version: u16,
}

/// Walk /sys/block/ and return every block device whose sector 0 carries
/// the ZINF magic.
///
/// For loop devices the backing file (readable without root) is tried first;
/// the /dev/loopX node is used as the reported path so that C-library FFI
/// calls (which go through linux_driver) still work.
pub fn scan_zinf_devices() -> Vec<DeviceInfo> {
    let mut devices: Vec<DeviceInfo> = Vec::new();
    let mut seen: std::collections::HashSet<String> = std::collections::HashSet::new();

    let block_dir = match fs::read_dir("/sys/block") {
        Ok(d) => d,
        Err(_) => return devices,
    };

    for entry in block_dir.flatten() {
        let name = entry.file_name();
        let name_str = name.to_string_lossy().into_owned();
        let sysfs_path = entry.path(); // /sys/block/<name>
        let dev_path   = format!("/dev/{name_str}");

        if seen.contains(&dev_path) {
            continue;
        }

        let mut buf = [0u8; 512];

        if name_str.starts_with("loop") {
            // ----------------------------------------------------------------
            // Loop device: try backing file first (no root needed).
            // Backing file path lives in /sys/block/loopX/loop/backing_file.
            // If that file is empty the loop device is unattached — skip it.
            // ----------------------------------------------------------------
            let bf_path = sysfs_path.join("loop/backing_file");
            let backing = match fs::read_to_string(&bf_path) {
                Ok(s) => s.trim().to_owned(),
                Err(_) => continue, // unattached / not a loop
            };
            if backing.is_empty() {
                continue;
            }

            // Read sector 0 from the backing image file (user-readable)
            if read_first_sector(&backing, &mut buf).is_err() {
                // Fall back to reading the block device directly
                if read_first_sector(&dev_path, &mut buf).is_err() {
                    continue;
                }
            }
        } else {
            // ----------------------------------------------------------------
            // Regular block device (/dev/sdb, /dev/mmcblk0, …).
            // Requires plugdev group membership (set up by install.sh + udev).
            // ----------------------------------------------------------------
            if read_first_sector(&dev_path, &mut buf).is_err() {
                continue;
            }
        }

        if buf[0..4] != ZINF_MAGIC {
            continue;
        }

        let version = u16::from_le_bytes([buf[4], buf[5]]);
        seen.insert(dev_path.clone());
        devices.push(DeviceInfo { path: dev_path, version });
    }

    devices
}

/// Read the first `buf.len()` bytes from `path` starting at offset 0.
fn read_first_sector(path: &str, buf: &mut [u8]) -> Result<(), String> {
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
    let n = file.read(buf).map_err(|e| e.to_string())?;
    if n < buf.len() {
        return Err(format!("short read: {n} bytes"));
    }
    Ok(())
}

/// Return the backing file path for a loop device, or None.
/// Used by commands.rs so that `extract_data` can also open the image
/// file directly when the /dev/loopX node isn't readable.
pub fn loop_backing_file(dev_path: &str) -> Option<String> {
    // dev_path is e.g. /dev/loop0 → name is loop0
    let name = dev_path.trim_start_matches("/dev/");
    let bf = format!("/sys/block/{name}/loop/backing_file");
    let s = fs::read_to_string(&bf).ok()?;
    let trimmed = s.trim().to_owned();
    if trimmed.is_empty() { None } else { Some(trimmed) }
}
