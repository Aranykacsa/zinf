mod commands;
mod hardware;
mod sdk_gen;
mod zinf_ffi;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .invoke_handler(tauri::generate_handler![
            commands::scan_devices,
            commands::extract_data,
            commands::verify_integrity,
            commands::generate_config,
            commands::load_yaml,
            commands::get_config,
            commands::serialize_config,
            commands::preview_csv,
            sdk_gen::generate_sdk,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
