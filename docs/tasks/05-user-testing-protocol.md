# Task 5: User Testing Execution Protocol

**Context:**
Conduct user testing with 3 researchers and 3 developers to validate the appliance model. This task covers test preparation, execution, and evaluation criteria. It depends on Tasks 1–4 being complete.

**Prerequisites:**
- ZINF Studio installed via `tools/install.sh` on the test machines.
- A ZINF-formatted SD card (or loop device image) with synthetic sensor data loaded.
- The Guided SDK Generator (Task 3) producing downloadable code packages.

---

## Preparation

### Generate Mock Test Images

Create 3 synthetic ZINF images with known sensor data for the researcher sessions. Each image should contain at least 500 logical sectors of sensor readings.

```bash
# Create a test image with synthetic data
dd if=/dev/zero of=/tmp/zinf_test_N.img bs=1M count=8 status=none
zinf format /tmp/zinf_test_N.img

# Attach as loop device and write synthetic sensor data via zinf shell:
sudo losetup -f --show /tmp/zinf_test_N.img   # → /dev/loopX
sudo zinf shell /dev/loopX
> raid sensor 500 23.5 65.0   # write 500 sensor readings
> exit
sudo losetup -d /dev/loopX
```

Produce 3 such images with slightly different data ranges (vary temperature 20–28°C, humidity 55–75%).

### Skeleton MCU Project for Developer Sessions

Provide a skeleton project directory at `tests/sdk_integration_skeleton/`:
```
sdk_integration_skeleton/
├── CMakeLists.txt      # references ZINF root but leaves driver wiring empty
├── main.c              # empty main(), no ZINF calls yet
└── README.md           # "Integrate ZINF using the Studio SDK Generator"
```

---

## Protocol A: Researcher Session (N = 3)

**Goal:** Read sensor data from a ZINF device and export it to CSV using ZINF Studio. No CLI knowledge required.

**Setup per participant:**
1. Provide a fresh Linux machine (or VM) with ZINF Studio installed.
2. Attach one of the three synthetic test images as a loop device so it appears as `/dev/loopX`.

**Task sequence (facilitator reads aloud, no hints):**
1. "Open ZINF Studio."
2. "Find the connected ZINF device."
3. "Extract the sensor data to a CSV file on your Desktop."
4. "Open the CSV file and tell me what the average temperature reading is." *(forces verification)*

**Metrics to record (stopwatch + observation notes):**
| Metric | How to measure |
|---|---|
| Time to first device detected | Stopwatch from app launch to device appearing in sidebar |
| Time to CSV saved | From Step 3 start to file saved confirmation |
| Total clicks | Count all mouse clicks during the session |
| Errors / confusion points | Note any moment the participant hesitates > 15 seconds or takes a wrong action |
| Success | Did they correctly identify the average temperature from the CSV? |

**Pass criteria:** All 3 participants complete CSV export without facilitator assistance.

---

## Protocol B: Developer Session (N = 3)

**Goal:** Integrate ZINF into a skeleton MCU project using the Guided SDK Generator. Reach successful compilation.

**Setup per participant:**
1. Provide a Linux machine with ZINF Studio installed, the skeleton project at `~/sdk_integration_skeleton/`, and a toolchain (`arm-none-eabi-gcc` for RP2350 or `gcc` for desktop sandbox).
2. Participants may use the generated `sandbox.c` to compile on the host if cross-compilation is unavailable.

**Task sequence:**
1. "Open ZINF Studio and go to the SDK Generator."
2. "Define a sensor with two fields: temperature (float) and pressure (float)."
3. "Download the generated SDK."
4. "Copy the generated files into the skeleton project and make it compile — use the compiler errors as your guide."

**Metrics to record:**
| Metric | How to measure |
|---|---|
| Time to first successful compilation | Stopwatch from SDK download to `gcc`/`make` exits 0 |
| Number of compiler errors before success | Count each unique `#error` pragma triggered |
| `#error` clarity rating | Participant rates each guard message 1–5 for clarity |
| Makefile integration friction | Did they find and use `CMakeLists_zinf.txt` without prompting? |
| Success | Binary or sandbox executable produced without facilitator edits to the generated code |

**Pass criteria:** All 3 developers produce a compiling binary. No facilitator edits to generated code.

---

## Evaluation Summary Sheet

After both sessions fill in:

| Session | Participant | Success | Total time (min) | Clicks / Errors | Notes |
|---|---|---|---|---|---|
| Researcher | R1 | | | | |
| Researcher | R2 | | | | |
| Researcher | R3 | | | | |
| Developer | D1 | | | | |
| Developer | D2 | | | | |
| Developer | D3 | | | | |

**Pass threshold for the boxed product milestone:** ≥ 5/6 participants succeed without facilitator assistance.
