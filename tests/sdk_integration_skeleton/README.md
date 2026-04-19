# ZINF SDK Integration Skeleton

This skeleton project is the starting point for **Protocol B (Developer Session)** user testing.
Your goal: integrate ZINF into a custom hardware project using the **Studio SDK Generator**.

---

## Prerequisites

- ZINF Studio installed (`zinf-studio`)
- A C toolchain:
  - **Desktop sandbox**: `gcc` (any modern version)
  - **RP2350 MCU**: `arm-none-eabi-gcc` + CMake + Pico SDK

---

## Step-by-step

### 1. Generate your SDK

1. Open **ZINF Studio**
2. Navigate to the **SDK Generator** tab
3. Enter your sensor name (e.g. `bme280`) and bus type (e.g. `i2c`)
4. Click **[ Generate SDK ]**
5. Download all generated files into this directory

### 2. Desktop sandbox (fastest path)

Compile and run on your Linux PC before touching any MCU:

```bash
gcc -I../../src/config -I../../src/core/api \
    sandbox.c <sensor_name>_driver.c ../../src/config/config.c \
    -o sandbox && ./sandbox
```

Follow the `#error` messages — each one is a numbered step that tells you exactly what to fill in.

### 3. CMake build (MCU or desktop)

```bash
mkdir build && cd build
cmake .. -DZINF_ROOT=../..
make
```

If cross-compiling for RP2350, pass your toolchain file:
```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/arm-none-eabi.cmake -DZINF_ROOT=../..
```

### 4. What counts as success

A compiling binary (exit code 0 from gcc/make) with **no facilitator edits** to the generated files.
