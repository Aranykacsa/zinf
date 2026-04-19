# Task 3: Guided Integration SDK Generator

**Context:**
Developers integrating ZINF into custom hardware codebases need extreme hand-holding. Build a templating engine in ZINF Studio that generates heavily guarded C code, ensuring they cannot skip vital initialization or hardware setup steps.

---

## Templates Overview

Four template files live in `src-tauri/templates/`. Each is a Tera (or Handlebars) template rendered with variables derived from `zinf.yaml`.

| Template file | Output file | Variables injected |
|---|---|---|
| `main_template.c` | `zinf_main.c` | `sensor_name`, `fields[]` |
| `driver_template.c` | `{sensor_name}_driver.c` | `sensor_name`, `bus_type`, `fields[]` |
| `driver_template.h` | `{sensor_name}_driver.h` | `sensor_name`, `fields[]` |
| `CMakeLists_snippet.txt` | `CMakeLists_zinf.txt` | `sensor_name` |
| `sandbox_template.c` | `sandbox.c` | `sensor_name`, `fields[]`, mock byte values |

---

## Step-by-Step Instructions for LLM

### 1. Templating Engine Setup

- Add to `zinf-studio/src-tauri/Cargo.toml`:
  ```toml
  [dependencies]
  tera = "1"
  serde_yaml = "0.9"
  ```
- Create `src-tauri/templates/` directory with the five template files listed above.
- In `src-tauri/src/sdk_gen.rs`, implement `fn render_template(name: &str, ctx: &tera::Context) -> Result<String, String>`.

### 2. `main_template.c` — with Compiler Guards

```c
#include "api.h"
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
// Call this from your main() before zinf_ctx_init_defaults().
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
```

### 3. `driver_template.c` — Custom Sensor Driver

Generated from `zinf.yaml` `data_types[0].fields`. Each field gets a commented placeholder read:

```c
#include "{{ sensor_name }}_driver.h"

// STEP 4: Replace each TODO with your actual bus read for that field.
void {{ sensor_name }}_read_sample(sensor_t *out) {
{% for field in fields %}
    // {{ field.name }} ({{ field.type }})
    // out->{{ field.name }} = read_{{ field.name }}_from_bus();  // TODO
{% endfor %}
}
```

### 4. `driver_template.h`

```c
#pragma once
#include "config.h"

void {{ sensor_name }}_read_sample(sensor_t *out);
```

### 5. `CMakeLists_snippet.txt`

```cmake
# ZINF integration snippet — add to your project's CMakeLists.txt

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
```

### 6. `sandbox_template.c` — Desktop Mock Environment

Lets the developer compile and test their parsing logic on a Linux PC before flashing:

```c
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "{{ sensor_name }}_driver.h"

// Simulated raw bus bytes — replace with bytes captured from your actual sensor.
static const uint8_t mock_bus_response[] = {
{% for field in fields %}
    // {{ field.name }} ({{ field.type }}, {{ field.size }} bytes) — placeholder zeros
    {% for b in field.mock_bytes %}0x{{ b }}, {% endfor %}
{% endfor %}
};

int main(void) {
    sensor_t data = {0};
    // Uncomment and test your {{ sensor_name }}_read_sample() logic here.
    // {{ sensor_name }}_read_sample(&data);
    printf("{{ sensor_name }} sample:\n");
{% for field in fields %}
    printf("  {{ field.name }} = TODO\n");
{% endfor %}
    return 0;
}
```

Compile sandbox on host:
```bash
gcc -I../zinf/src/config -I../zinf/src/core/api \
    sandbox.c {{ sensor_name }}_driver.c ../zinf/src/config/config.c \
    -o sandbox && ./sandbox
```

### 7. Tauri Command

```rust
#[tauri::command]
fn generate_sdk(sensor_name: String, bus_type: String, yaml_text: String)
    -> Result<HashMap<String, String>, String>
```

- Parse `yaml_text` to extract `data_types[0].fields`.
- Render all five templates with `tera::Context`.
- Return a `HashMap<filename, content>` so the frontend can display each file in a tabbed code viewer and offer a "Download All as ZIP" button.

Supported `bus_type` values: `"spi"`, `"i2c"`, `"uart"`, `"custom"`. The value is injected as a comment hint in the driver template.

---

## Deliverables Checklist

- [ ] `tera` dependency added to `Cargo.toml`
- [ ] `src-tauri/templates/` directory with all five template files
- [ ] `sdk_gen.rs` — `render_template` + `generate_sdk` Tauri command
- [ ] Frontend: tabbed code viewer with generated file contents
- [ ] Frontend: "Download All as ZIP" button (use `tauri-plugin-fs` to write files)
