/// Tauri command implementations: device scanning, data extraction, integrity check, config.
use crate::hardware::{loop_backing_file, scan_zinf_devices as hw_scan, DeviceInfo};
use crate::zinf_ffi::{
    self, ScrubReport, STORAGE_OK,
};
use serde::{Deserialize, Serialize};
use std::ffi::CString;
use std::fs;
use std::io::{BufRead, Write as IoWrite};
use std::process::Command;
use std::sync::Mutex;
use tauri::{AppHandle, Emitter};

/// Global lock to serialise all C library calls (not thread-safe internally)
static ZINF_LOCK: Mutex<()> = Mutex::new(());

/// Return the path that linux_driver should open.
/// For loop devices the backing .img file is returned (user-readable without
/// root). For real block devices the device node itself is returned.
fn openable_path(device_path: &str) -> String {
    if device_path.contains("loop") {
        loop_backing_file(device_path).unwrap_or_else(|| device_path.to_owned())
    } else {
        device_path.to_owned()
    }
}

/// Wire linux_driver into zinf_ctx and call setup_storage.
///
/// This matches what the CLI does at zinf_main.c:493-494:
///   linux_driver_set_path(g_device);
///   zinf_ctx->driver = &linux_driver;
///
/// Must be called inside the ZINF_LOCK critical section.
/// `c_path` must remain alive for the duration of all subsequent FFI calls.
unsafe fn init_zinf_ctx(c_path: &std::ffi::CStr) -> Result<*mut zinf_ffi::ZinfCtx, String> {
    zinf_ffi::linux_driver_set_path(c_path.as_ptr());
    // Wire linux_driver into zinf_ctx->driver — platform_linux.c leaves it NULL.
    // The CLI does this at zinf_main.c:494: zinf_ctx->driver = &linux_driver;
    zinf_ffi::zinf_studio_use_linux_driver();
    let ctx = zinf_ffi::zinf_ctx;
    if ctx.is_null() {
        return Err("zinf_ctx is null".into());
    }
    let rc = zinf_ffi::setup_storage(ctx);
    if rc != STORAGE_OK {
        return Err(format!("setup_storage failed (rc={rc}) — check device path and permissions"));
    }
    // Fix mirror_offset: zinf_ctx_init_defaults falls back to 30 for regular files,
    // but the image was formatted using (total_sectors - metadata) / mirror_count.
    zinf_ffi::zinf_studio_fix_mirror_offset();
    Ok(ctx)
}

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
    #[serde(default = "default_header_size")]
    pub header_size: u8,
    #[serde(default = "default_max_bad_sectors")]
    pub max_bad_sectors: u32,
    pub data_types: Vec<YamlDataType>,
}

fn default_metadata_sectors() -> u8 { 2 }
fn default_header_size() -> u8 { 1 }
fn default_max_bad_sectors() -> u32 { 64 }

#[derive(Serialize, Deserialize)]
struct ZinfYamlRoot {
    zinf: ZinfYamlConfig,
}

/// Parse zinf config from either the nested `zinf: { ... }` format used by
/// zinf.yaml on disk, or the flat format used by the frontend's built-in fallback.
fn parse_zinf_config(yaml_text: &str) -> Result<ZinfYamlConfig, String> {
    if let Ok(root) = serde_yaml::from_str::<ZinfYamlRoot>(yaml_text) {
        return Ok(root.zinf);
    }
    // Fall back to flat format (frontend default / user-edited)
    serde_yaml::from_str::<ZinfYamlConfig>(yaml_text)
        .map_err(|e| format!("YAML parse error: {e}"))
}

// ---------------------------------------------------------------------------
// Tauri commands
// ---------------------------------------------------------------------------

/// Parse YAML text into a structured config object for the frontend.
#[tauri::command]
pub fn get_config(yaml_text: String) -> Result<ZinfYamlConfig, String> {
    parse_zinf_config(&yaml_text)
}

/// Serialize a config object back into a YAML string (nested under `zinf:`).
#[tauri::command]
pub fn serialize_config(config: ZinfYamlConfig) -> Result<String, String> {
    let root = ZinfYamlRoot { zinf: config };
    serde_yaml::to_string(&root).map_err(|e| format!("YAML serialization error: {e}"))
}

/// Read first 1000 lines of a ZINF-exported CSV to preview offline.
#[tauri::command]
pub fn preview_csv(path: String) -> Result<(Vec<String>, Vec<SectorRow>), String> {
    let file = fs::File::open(&path).map_err(|e| format!("Cannot open CSV: {e}"))?;
    let reader = std::io::BufReader::new(file);
    let mut lines = reader.lines();

    // Read header
    let header_line = lines.next()
        .ok_or("CSV is empty")?
        .map_err(|e| e.to_string())?;
    let headers: Vec<String> = header_line.split(',')
        .skip(2) // Skip sector, index
        .map(|s| s.to_string())
        .collect();

    let mut rows = Vec::with_capacity(1000);
    for line_res in lines.take(1000) {
        let line = line_res.map_err(|e| e.to_string())?;
        let parts: Vec<&str> = line.split(',').collect();
        if parts.len() < 2 { continue; }
        
        let sector = parts[0].parse::<u64>().unwrap_or(0);
        let index = parts[1].parse::<usize>().unwrap_or(0);
        let values: Vec<f64> = parts.iter().skip(2)
            .map(|s| s.trim().parse::<f64>().unwrap_or(0.0))
            .collect();
            
        rows.push(SectorRow { sector, index, values });
    }

    Ok((headers, rows))
}

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
    let open_path = openable_path(&device_path);
    let c_path = CString::new(open_path.as_str()).map_err(|e| e.to_string())?;
    let _guard = ZINF_LOCK.lock().map_err(|e| e.to_string())?;

    unsafe {
        let ctx = init_zinf_ctx(&c_path)?;
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
    pub index: usize,
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
    // Parse field schema from YAML (handles both nested zinf: and flat formats)
    let config = parse_zinf_config(&yaml_text)?;
    let fields = config
        .data_types
        .first()
        .map(|dt| dt.fields.clone())
        .unwrap_or_default();

    let open_path = openable_path(&device_path);
    let c_path = CString::new(open_path.as_str()).map_err(|e| e.to_string())?;
    let sector_size = config.sector_size as usize;
    let payload_size = sector_size - 1 - 4; // HEADER_SIZE=1, CRC=4

    // Initialise C library
    let _guard = ZINF_LOCK.lock().map_err(|e| e.to_string())?;
    let last_sector;
    unsafe {
        let ctx = init_zinf_ctx(&c_path)?;
        let mut ls: u64 = 0;
        zinf_ffi::get_last_sector(ctx, &mut ls);
        last_sector = ls;
    }

    if last_sector == 0 {
        return Ok(vec![]);
    }

    // Build CSV header
    let header_names: Vec<&str> = fields.iter().map(|f| f.name.as_str()).collect();
    let csv_header = format!("sector,index,{}\n", header_names.join(","));

    // Calculate record size
    let record_size: usize = fields.iter().map(|f| match f.field_type.as_str() {
        "float" | "f32" | "int32_t" | "i32" | "uint32_t" | "u32" => 4,
        "double" | "f64" => 8,
        "int16_t" | "i16" | "uint16_t" | "u16" => 2,
        _ => 1,
    }).sum();
    
    if record_size == 0 {
        return Err("Invalid YAML: total record size is zero".into());
    }
    let records_per_sector = payload_size / record_size;

    // Open CSV file early to stream writes
    let mut file = if !output_csv.is_empty() {
        let mut f = fs::File::create(&output_csv)
            .map_err(|e| format!("Cannot create CSV: {e}"))?;
        f.write_all(csv_header.as_bytes())
            .map_err(|e| format!("Write error: {e}"))?;
        Some(f)
    } else {
        None
    };

    let mut rows: Vec<SectorRow> = Vec::with_capacity(1000);
    let mut payload = vec![0u8; payload_size];

    for sector in 1..=last_sector {
        let rc = unsafe {
            zinf_ffi::raid_read(zinf_ffi::zinf_ctx, sector, payload.as_mut_ptr())
        };
        if rc != STORAGE_OK {
            continue;
        }

        for i in 0..records_per_sector {
            let chunk_offset = i * record_size;
            let chunk = &payload[chunk_offset..chunk_offset + record_size];

            // Skip entirely zeroed chunks (padding)
            if chunk.iter().all(|&b| b == 0) {
                continue;
            }

            // Deserialise fields (little-endian)
            let mut values: Vec<f64> = Vec::with_capacity(fields.len());
            let mut field_offset = 0usize;
            for field in &fields {
                let val = deserialise_field(chunk, &mut field_offset, &field.field_type);
                values.push(val);
            }

            // Stream to file
            if let Some(f) = file.as_mut() {
                let row_str: Vec<String> = values.iter().map(|v| format!("{v:.4}")).collect();
                f.write_all(format!("{},{},{}\n", sector, i, row_str.join(",")).as_bytes())
                    .map_err(|e| format!("Write error: {e}"))?;
            }

            // Add to UI preview (limit to 1000 rows to prevent frontend freeze)
            if rows.len() < 1000 {
                rows.push(SectorRow { sector, index: i, values });
            }
        }

        if sector % 100 == 0 || sector == last_sector {
            let _ = app.emit("extract-progress", ExtractProgress {
                current: sector,
                total: last_sector,
            });
        }
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
    // Validate YAML parse first (handles both nested zinf: and flat formats)
    let _ = parse_zinf_config(&yaml_text)?;

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
