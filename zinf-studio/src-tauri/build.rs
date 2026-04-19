use std::path::PathBuf;
use std::process::Command;

fn main() {
    tauri_build::build();

    // Paths relative to this build.rs (zinf-studio/src-tauri/)
    let repo_root = PathBuf::from("../../");
    let src_root = repo_root.join("src");

    // Re-run when zinf.yaml or any C source changes
    println!("cargo:rerun-if-changed=../../zinf.yaml");
    println!("cargo:rerun-if-changed=../../src/core/api/api.c");
    println!("cargo:rerun-if-changed=../../src/core/storage/storage.c");
    println!("cargo:rerun-if-changed=../../src/core/helper/helper.c");
    println!("cargo:rerun-if-changed=../../src/config/config.c");
    println!("cargo:rerun-if-changed=../../src/drivers/linux/linux_driver.c");
    println!("cargo:rerun-if-changed=../../src/platform/platform_linux.c");

    // Step 1: Regenerate config.h / config.c from zinf.yaml
    let gen_status = Command::new("python3")
        .args([
            repo_root.join("tools/zinf_gen.py").to_str().unwrap(),
            repo_root.join("zinf.yaml").to_str().unwrap(),
            src_root.join("config/").to_str().unwrap(),
        ])
        .status();

    match gen_status {
        Ok(s) if s.success() => {}
        Ok(s) => eprintln!("warning: zinf_gen.py exited with {s}"),
        Err(e) => eprintln!("warning: could not run zinf_gen.py: {e}"),
    }

    // Step 2: Compile C sources into a static library
    cc::Build::new()
        .std("c11")
        .flag("-O2")
        .flag("-Wall")
        .flag("-Wno-unused-parameter")
        // Source files
        .file(src_root.join("core/api/api.c"))
        .file(src_root.join("core/storage/storage.c"))
        .file(src_root.join("core/helper/helper.c"))
        .file(src_root.join("config/config.c"))
        .file(src_root.join("drivers/linux/linux_driver.c"))
        .file(src_root.join("platform/platform_linux.c"))
        .file("zinf_studio_helpers.c")
        // Include paths
        .include(src_root.join("core/api"))
        .include(src_root.join("core/storage"))
        .include(src_root.join("core/helper"))
        .include(src_root.join("config"))
        .include(src_root.join("drivers/linux"))
        .include(src_root.join("platform"))
        .compile("zinf");
}
