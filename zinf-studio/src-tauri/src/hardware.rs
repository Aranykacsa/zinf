/// Block device enumeration and raw sector I/O.
use serde::Serialize;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};

/// ZINF metadata magic bytes (same as META_MAGIC_B0..B3 in config.h)
const ZINF_MAGIC: [u8; 4] = [0x5A, 0x49, 0x4E, 0x46]; // "ZINF"

#[derive(Debug, Clone, Serialize)]
pub struct DeviceInfo {
    pub path: String,
    pub version: u16,
}

/// Walk /sys/block/, open each /dev/<name>, read sector 0,
/// and check for ZINF magic. Returns only ZINF-formatted devices.
pub fn scan_zinf_devices() -> Vec<DeviceInfo> {
    let mut devices = Vec::new();

    let block_dir = match fs::read_dir("/sys/block") {
        Ok(d) => d,
        Err(_) => return devices,
    };

    for entry in block_dir.flatten() {
        let name = entry.file_name();
        let name_str = name.to_string_lossy();

        // Skip loop devices that aren't set up, ram devices, etc.
        let dev_path = format!("/dev/{name_str}");

        // Try to read the first 512 bytes
        let mut buf = [0u8; 512];
        if read_first_sector(&dev_path, &mut buf).is_err() {
            continue;
        }

        // Check magic bytes
        if buf[0..4] != ZINF_MAGIC {
            continue;
        }

        // Format version is at bytes [4..6] as u16 LE
        let version = u16::from_le_bytes([buf[4], buf[5]]);

        devices.push(DeviceInfo { path: dev_path, version });
    }

    // Also scan /dev/loop* explicitly (common for development with loopback images)
    let dev_dir = match fs::read_dir("/dev") {
        Ok(d) => d,
        Err(_) => return devices,
    };

    for entry in dev_dir.flatten() {
        let name = entry.file_name();
        let name_str = name.to_string_lossy();
        if !name_str.starts_with("loop") || name_str.contains('p') {
            continue;
        }
        let dev_path = format!("/dev/{name_str}");
        if devices.iter().any(|d| d.path == dev_path) {
            continue;
        }

        let mut buf = [0u8; 512];
        if read_first_sector(&dev_path, &mut buf).is_err() {
            continue;
        }
        if buf[0..4] != ZINF_MAGIC {
            continue;
        }
        let version = u16::from_le_bytes([buf[4], buf[5]]);
        devices.push(DeviceInfo { path: dev_path, version });
    }

    devices
}

/// Read the first `buf.len()` bytes from a block device or image file.
fn read_first_sector(path: &str, buf: &mut [u8]) -> Result<(), String> {
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
    let n = file.read(buf).map_err(|e| e.to_string())?;
    if n < buf.len() {
        return Err(format!("short read: {n} bytes"));
    }
    Ok(())
}
