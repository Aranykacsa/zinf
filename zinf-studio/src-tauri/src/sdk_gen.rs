/// SDK code generator — Tera-based template engine for guided ZINF integration.
/// Full implementation in Task 3.
use std::collections::HashMap;
use serde::{Deserialize, Serialize};
use tera::{Context, Tera};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SdkField {
    pub name: String,
    #[serde(rename = "type")]
    pub field_type: String,
    pub size: usize,
    pub mock_bytes: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct YamlField {
    pub name: String,
    #[serde(rename = "type")]
    pub field_type: String,
}

#[derive(Debug, Clone, Deserialize)]
struct YamlDataType {
    pub fields: Vec<YamlField>,
}

#[derive(Debug, Clone, Deserialize)]
struct ZinfYaml {
    pub data_types: Vec<YamlDataType>,
}

/// Render a Tera template string with the given context.
fn render_template(template_src: &str, ctx: &Context) -> Result<String, String> {
    let mut tera = Tera::default();
    tera.add_raw_template("t", template_src)
        .map_err(|e| format!("Template error: {e}"))?;
    tera.render("t", ctx).map_err(|e| format!("Render error: {e}"))
}

/// Map a C type name to its byte size and mock zero bytes.
fn type_to_mock(field_type: &str) -> (usize, Vec<String>) {
    match field_type {
        "float" | "f32" => (4, vec!["00".into(); 4]),
        "double" | "f64" => (8, vec!["00".into(); 8]),
        "int16_t" | "uint16_t" | "i16" | "u16" => (2, vec!["00".into(); 2]),
        "int32_t" | "uint32_t" | "i32" | "u32" => (4, vec!["00".into(); 4]),
        "uint8_t" | "int8_t" | "u8" | "i8" => (1, vec!["00".into(); 1]),
        _ => (1, vec!["00".into()]),
    }
}

/// Generate all SDK template files for a given sensor configuration.
#[tauri::command]
pub fn generate_sdk(
    sensor_name: String,
    bus_type: String,
    yaml_text: String,
) -> Result<HashMap<String, String>, String> {
    // Parse YAML to extract fields
    let config: ZinfYaml =
        serde_yaml::from_str(&yaml_text).map_err(|e| format!("YAML parse error: {e}"))?;

    let raw_fields = config
        .data_types
        .first()
        .map(|dt| dt.fields.clone())
        .unwrap_or_default();

    let fields: Vec<SdkField> = raw_fields
        .iter()
        .map(|f| {
            let (size, mock_bytes) = type_to_mock(&f.field_type);
            SdkField {
                name: f.name.clone(),
                field_type: f.field_type.clone(),
                size,
                mock_bytes,
            }
        })
        .collect();

    // Build Tera context
    let mut ctx = Context::new();
    ctx.insert("sensor_name", &sensor_name);
    ctx.insert("bus_type", &bus_type);
    ctx.insert("fields", &fields);

    let mut output: HashMap<String, String> = HashMap::new();

    // Render each template
    output.insert(
        "zinf_main.c".into(),
        render_template(MAIN_TEMPLATE, &ctx)?,
    );
    output.insert(
        format!("{sensor_name}_driver.c"),
        render_template(DRIVER_C_TEMPLATE, &ctx)?,
    );
    output.insert(
        format!("{sensor_name}_driver.h"),
        render_template(DRIVER_H_TEMPLATE, &ctx)?,
    );
    output.insert(
        "CMakeLists_zinf.txt".into(),
        render_template(CMAKE_TEMPLATE, &ctx)?,
    );
    output.insert(
        "sandbox.c".into(),
        render_template(SANDBOX_TEMPLATE, &ctx)?,
    );

    Ok(output)
}

// ---------------------------------------------------------------------------
// Embedded template sources (mirrors src-tauri/templates/)
// ---------------------------------------------------------------------------

const MAIN_TEMPLATE: &str = r#"#include "api.h"
#include "config.h"
#include "{{ sensor_name }}_driver.h"

// =========================================================================
// ZINF Integration Wizard — generated for {{ sensor_name }}
// Complete every STEP marked below before compiling.
// =========================================================================

// STEP 1: Include your MCU hardware headers here.
// Example: #include "pico/stdlib.h"

#ifndef HARDWARE_INITIALIZED
  #error "ZINF SDK STEP 1: Define HARDWARE_INITIALIZED after your hardware init is complete."
#endif

// STEP 2: Wire up the ZINF block driver.
static void zinf_driver_setup(void) {
    // TODO: assign ctx.driver to your platform's driver_t implementation.
    // Example: ctx.driver = &sd_driver;
    #error "ZINF SDK STEP 2: Assign ctx.driver to your platform driver."
}

int main(void) {
    // STEP 3: Initialize your hardware (clocks, SPI, GPIO, etc.)
    // hardware_init();  // uncomment and replace with your init call
    #error "ZINF SDK STEP 3: Call your hardware initialization here, then remove this line."

    zinf_ctx_t ctx = {0};
    zinf_driver_setup();
    zinf_ctx_init_defaults(&ctx);
    setup_storage(&ctx);
    init_log_sector(&ctx);

    // Startup scrub — recovers any degraded mirrors from previous run.
    zinf_scrub_report_t report;
    uint64_t last = 0;
    get_last_sector(&ctx, &last);
    zinf_scrub(&ctx, 1u, last, &report);

    while (1) {
        sensor_t data = {0};
        {{ sensor_name }}_read_sample(&data);
        raid_sensor_values(&ctx, &data, 1);
    }

    return 0;
}
"#;

const DRIVER_C_TEMPLATE: &str = r#"#include "{{ sensor_name }}_driver.h"

// Bus type hint: {{ bus_type }}
// STEP 4: Replace each TODO with your actual bus read for that field.
void {{ sensor_name }}_read_sample(sensor_t *out) {
{% for field in fields %}
    // {{ field.name }} ({{ field.type }})
    // out->{{ field.name }} = read_{{ field.name }}_from_bus();  // TODO
{% endfor %}
}
"#;

const DRIVER_H_TEMPLATE: &str = r#"#pragma once
#include "config.h"

void {{ sensor_name }}_read_sample(sensor_t *out);
"#;

const CMAKE_TEMPLATE: &str = r#"# ZINF integration snippet — add to your project's CMakeLists.txt

set(ZINF_ROOT "${CMAKE_CURRENT_LIST_DIR}/zinf")  # adjust path

target_sources({{ sensor_name }}_app PRIVATE
    ${ZINF_ROOT}/src/core/api/api.c
    ${ZINF_ROOT}/src/core/storage/storage.c
    ${ZINF_ROOT}/src/core/helper/helper.c
    ${ZINF_ROOT}/src/config/config.c
    {{ sensor_name }}_driver.c
    zinf_main.c
)

target_include_directories({{ sensor_name }}_app PRIVATE
    ${ZINF_ROOT}/src/core/api
    ${ZINF_ROOT}/src/core/storage
    ${ZINF_ROOT}/src/core/helper
    ${ZINF_ROOT}/src/config
    ${ZINF_ROOT}/src/drivers/embedded
)
"#;

const SANDBOX_TEMPLATE: &str = r#"#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "{{ sensor_name }}_driver.h"

// Simulated raw bus bytes — replace with bytes captured from your actual sensor.
static const uint8_t mock_bus_response[] = {
{% for field in fields %}    // {{ field.name }} ({{ field.type }}, {{ field.size }} bytes) — placeholder zeros
    {% for b in field.mock_bytes %}0x{{ b }}, {% endfor %}

{% endfor %}};

int main(void) {
    sensor_t data = {0};
    // Uncomment and test your {{ sensor_name }}_read_sample() logic here.
    // {{ sensor_name }}_read_sample(&data);
    printf("{{ sensor_name }} sample:\n");
{% for field in fields %}    printf("  {{ field.name }} = TODO\n");
{% endfor %}    return 0;
}

// Compile on host:
// gcc -I../zinf/src/config -I../zinf/src/core/api \
//     sandbox.c {{ sensor_name }}_driver.c ../zinf/src/config/config.c \
//     -o sandbox && ./sandbox
"#;
