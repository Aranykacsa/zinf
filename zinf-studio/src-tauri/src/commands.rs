/// Tauri command implementations: device scanning, data extraction, integrity check, config.
use crate::hardware::{scan_zinf_devices as hw_scan, DeviceInfo};
use crate::zinf_ffi::{
    self, ScrubReport, STORAGE_OK,
};
use serde::{Deserialize, Serialize};
use std::ffi::CString;
use std::fs;
use std::io::Write as IoWrite;
use std::process::Command;
use std::sync::Mutex;
use tauri::{AppHandle, Emitter};

/// Global lock to serialise all C library calls (not thread-safe internally)
static ZINF_LOCK: Mutex<()> = Mutex::new(());

// ---------------------------------------------------------------------------
// YAML config types
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct YamlField {
    pub name: String,
    #[serde(rename = "type")]
    pub field_type: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct YamlDataType {
    pub name: String,
    pub fields: Vec<YamlField>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ZinfYamlConfig {
    pub sector_size: u16,
    pub mirror_count: u8,
    #[serde(default = "default_metadata_sectors")]
    pub metadata_sectors: u8,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub header_size: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_bad_sectors: Option<u32>,
    pub data_types: Vec<YamlDataType>,
}

fn default_metadata_sectors() -> u8 { 2 }

// ---------------------------------------------------------------------------
// Tauri commands
// ---------------------------------------------------------------------------

/// Scan block devices for ZINF magic and return matching device list.
#[tauri::command]
pub fn scan_devices() -> Vec<DeviceInfo> {
    hw_scan()
}

/// Scrub report returned to the frontend.
#[derive(Debug, Serialize)]
pub struct ScrubResult {
    pub checked: u32,
    pub healthy: u32,
    pub repaired: u32,
    pub unrecoverable: u32,
}

/// Verify integrity of a ZINF device by running zinf_scrub over all sectors.
#[tauri::command]
pub fn verify_integrity(device_path: String) -> Result<ScrubResult, String> {
    let c_path = CString::new(device_path.as_str()).map_err(|e| e.to_string())?;
    let _guard = ZINF_LOCK.lock().map_err(|e| e.to_string())?;

    unsafe {
        zinf_ffi::linux_driver_set_path(c_path.as_ptr());
        let ctx = zinf_ffi::zinf_ctx;
        if ctx.is_null() {
            return Err("zinf_ctx is null".into());
        }
        let rc = zinf_ffi::setup_storage(ctx);
        if rc != STORAGE_OK {
            return Err(format!("setup_storage failed: {rc}"));
        }
        let mut last: u64 = 0;
        zinf_ffi::get_last_sector(ctx, &mut last);
        if last == 0 {
            return Ok(ScrubResult { checked: 0, healthy: 0, repaired: 0, unrecoverable: 0 });
        }
        let mut report = ScrubReport::default();
        zinf_ffi::zinf_scrub(ctx, 1, last, &mut report);
        Ok(ScrubResult {
            checked: report.checked,
            healthy: report.healthy,
            repaired: report.repaired,
            unrecoverable: report.unrecoverable,
        })
    }
}

/// Emitted event payload
#[derive(Clone, Serialize)]
struct ExtractProgress {
    current: u64,
    total: u64,
}

/// One CSV row of sensor data
#[derive(Clone, Serialize)]
pub struct SectorRow {
    pub sector: u64,
    pub values: Vec<f64>,
}

/// Read all logical sectors from a ZINF device, deserialise sensor fields,
/// write a CSV file, and emit progress events every 100 sectors.
#[tauri::command]
pub async fn extract_data(
    app: AppHandle,
    device_path: String,
    output_csv: String,
    yaml_text: String,
) -> Result<Vec<SectorRow>, String> {
    // Parse field schema from YAML
    let config: ZinfYamlConfig =
        serde_yaml::from_str(&yaml_text).map_err(|e| format!("YAML parse error: {e}"))?;
    let fields = config
        .data_types
        .first()
        .map(|dt| dt.fields.clone())
        .unwrap_or_default();

    let c_path = CString::new(device_path.as_str()).map_err(|e| e.to_string())?;
    let sector_size = config.sector_size as usize;
    let payload_size = sector_size - 1 - 4; // HEADER_SIZE=1, CRC=4

    // Initialise C library
    let _guard = ZINF_LOCK.lock().map_err(|e| e.to_string())?;
    let last_sector;
    unsafe {
        zinf_ffi::linux_driver_set_path(c_path.as_ptr());
        let ctx = zinf_ffi::zinf_ctx;
        if ctx.is_null() {
            return Err("zinf_ctx is null".into());
        }
        let rc = zinf_ffi::setup_storage(ctx);
        if rc != STORAGE_OK {
            return Err(format!("setup_storage failed: {rc}"));
        }
        let mut ls: u64 = 0;
        zinf_ffi::get_last_sector(ctx, &mut ls);
        last_sector = ls;
    }

    if last_sector == 0 {
        return Ok(vec![]);
    }

    // Build CSV header
    let header_names: Vec<&str> = fields.iter().map(|f| f.name.as_str()).collect();
    let csv_header = format!("sector,{}\n", header_names.join(","));

    let mut csv_content = csv_header;
    let mut rows: Vec<SectorRow> = Vec::with_capacity(last_sector as usize);
    let mut payload = vec![0u8; payload_size];

    for sector in 1..=last_sector {
        let rc = unsafe {
            zinf_ffi::raid_read(zinf_ffi::zinf_ctx, sector, payload.as_mut_ptr())
        };
        if rc != STORAGE_OK {
            continue;
        }

        // Deserialise fields (little-endian)
        let mut values: Vec<f64> = Vec::with_capacity(fields.len());
        let mut offset = 0usize;
        for field in &fields {
            let val = deserialise_field(&payload, &mut offset, &field.field_type);
            values.push(val);
        }

        let row_str: Vec<String> = values.iter().map(|v| format!("{v:.4}")).collect();
        csv_content.push_str(&format!("{},{}\n", sector, row_str.join(",")));
        rows.push(SectorRow { sector, values });

        if sector % 100 == 0 || sector == last_sector {
            let _ = app.emit("extract-progress", ExtractProgress {
                current: sector,
                total: last_sector,
            });
        }
    }

    // Write CSV file
    if !output_csv.is_empty() {
        let mut file = fs::File::create(&output_csv)
            .map_err(|e| format!("Cannot create CSV: {e}"))?;
        file.write_all(csv_content.as_bytes())
            .map_err(|e| format!("Write error: {e}"))?;
    }

    Ok(rows)
}

/// Deserialise a single field from payload bytes (little-endian).
fn deserialise_field(payload: &[u8], offset: &mut usize, field_type: &str) -> f64 {
    match field_type {
        "float" | "f32" => {
            if *offset + 4 <= payload.len() {
                let bytes = [payload[*offset], payload[*offset+1],
                             payload[*offset+2], payload[*offset+3]];
                *offset += 4;
                f32::from_le_bytes(bytes) as f64
            } else {
                *offset += 4;
                0.0
            }
        }
        "double" | "f64" => {
            if *offset + 8 <= payload.len() {
                let bytes: [u8; 8] = payload[*offset..*offset+8].try_into().unwrap_or([0;8]);
                *offset += 8;
                f64::from_le_bytes(bytes)
            } else {
                *offset += 8;
                0.0
            }
        }
        "int16_t" | "i16" => {
            if *offset + 2 <= payload.len() {
                let bytes = [payload[*offset], payload[*offset+1]];
                *offset += 2;
                i16::from_le_bytes(bytes) as f64
            } else {
                *offset += 2;
                0.0
            }
        }
        "uint16_t" | "u16" => {
            if *offset + 2 <= payload.len() {
                let bytes = [payload[*offset], payload[*offset+1]];
                *offset += 2;
                u16::from_le_bytes(bytes) as f64
            } else {
                *offset += 2;
                0.0
            }
        }
        "int32_t" | "i32" => {
            if *offset + 4 <= payload.len() {
                let bytes: [u8; 4] = payload[*offset..*offset+4].try_into().unwrap_or([0;4]);
                *offset += 4;
                i32::from_le_bytes(bytes) as f64
            } else {
                *offset += 4;
                0.0
            }
        }
        _ => {
            // Default: treat as u8
            let val = if *offset < payload.len() { payload[*offset] as f64 } else { 0.0 };
            *offset += 1;
            val
        }
    }
}

/// Read zinf.yaml from disk, near the binary or the repo root.
#[tauri::command]
pub fn load_yaml() -> Result<String, String> {
    // Try paths in order: next to binary, repo root
    for candidate in &[
        "zinf.yaml",
        "../../zinf.yaml",
        "../zinf.yaml",
    ] {
        if let Ok(text) = fs::read_to_string(candidate) {
            return Ok(text);
        }
    }
    Err("zinf.yaml not found".into())
}

/// Write zinf.yaml and re-run zinf_gen.py to regenerate config.h / config.c.
#[tauri::command]
pub fn generate_config(yaml_text: String) -> Result<String, String> {
    // Validate YAML parse first
    let _: ZinfYamlConfig = serde_yaml::from_str(&yaml_text)
        .map_err(|e| format!("Invalid YAML: {e}"))?;

    // Find zinf.yaml location
    let yaml_path = find_yaml_path()?;
    fs::write(&yaml_path, &yaml_text).map_err(|e| format!("Write error: {e}"))?;

    // Find zinf_gen.py
    let gen_py = find_gen_py()?;
    let config_dir = yaml_path
        .parent()
        .ok_or("No parent dir")?
        .join("src/config/");

    let out = Command::new("python3")
        .args([gen_py.to_str().unwrap(), yaml_path.to_str().unwrap(), config_dir.to_str().unwrap()])
        .output()
        .map_err(|e| format!("python3 error: {e}"))?;

    if !out.status.success() {
        return Err(format!(
            "zinf_gen.py failed:\n{}",
            String::from_utf8_lossy(&out.stderr)
        ));
    }

    Ok("Config regenerated successfully.".into())
}

fn find_yaml_path() -> Result<std::path::PathBuf, String> {
    for candidate in &["zinf.yaml", "../../zinf.yaml", "../zinf.yaml"] {
        let p = std::path::PathBuf::from(candidate);
        if p.exists() { return Ok(p); }
    }
    Err("zinf.yaml not found".into())
}

fn find_gen_py() -> Result<std::path::PathBuf, String> {
    for candidate in &["tools/zinf_gen.py", "../../tools/zinf_gen.py", "../tools/zinf_gen.py"] {
        let p = std::path::PathBuf::from(candidate);
        if p.exists() { return Ok(p); }
    }
    Err("tools/zinf_gen.py not found".into())
}
