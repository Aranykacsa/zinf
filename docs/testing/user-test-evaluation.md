---
id: user-test-evaluation
title: User Testing Evaluation Sheet
sidebar_label: User Testing
sidebar_position: 7
unlisted: true
---

# ZINF User Testing — Evaluation Sheet

**Date:** _______________  
**Facilitator:** _______________  
**Studio version:** _______________  
**Test image used:** `zinf_test_1.img` / `zinf_test_2.img` / `zinf_test_3.img` *(circle one per session)*

---

## Protocol A — Researcher Session

**Goal:** Export sensor data to CSV using ZINF Studio, with no CLI knowledge required.

**Setup checklist (facilitator):**
- [ ] ZINF Studio installed via `tools/install.sh`
- [ ] Test image attached: `sudo losetup -f --show /tmp/zinf_test_N.img`
- [ ] Participant has a fresh desktop with no terminal open
- [ ] Stopwatch ready

**Task sequence (read aloud — no hints):**

1. "Open ZINF Studio."
2. "Find the connected ZINF device."
3. "Extract the sensor data to a CSV file on your Desktop."
4. "Open the CSV file and tell me what the average temperature reading is."

**Metrics:**

| Metric | R1 | R2 | R3 |
|--------|----|----|-----|
| Time to device detected (sec) | | | |
| Time to CSV saved (sec) | | | |
| Total clicks | | | |
| Number of hesitations (>15 s) | | | |
| Wrong actions taken | | | |
| Average temp stated correctly | ☐ Y ☐ N | ☐ Y ☐ N | ☐ Y ☐ N |
| Completed without facilitator help | ☐ Y ☐ N | ☐ Y ☐ N | ☐ Y ☐ N |

**Confusion points / facilitator notes:**

R1: _______________________________________________________________

R2: _______________________________________________________________

R3: _______________________________________________________________

**Pass criteria:** All 3 participants complete CSV export without facilitator assistance.

Result: ☐ PASS ☐ FAIL

---

## Protocol B — Developer Session

**Goal:** Integrate ZINF into the skeleton MCU project using the SDK Generator. Reach successful compilation.

**Setup checklist (facilitator):**
- [ ] ZINF Studio installed
- [ ] `tests/sdk_integration_skeleton/` copied to `~/sdk_integration_skeleton/` on participant machine
- [ ] Toolchain available: `gcc` (desktop sandbox) or `arm-none-eabi-gcc` (MCU)
- [ ] Terminal open in `~/sdk_integration_skeleton/`

**Task sequence (read aloud — no hints):**

1. "Open ZINF Studio and go to the SDK Generator."
2. "Define a sensor with two fields: temperature (float) and pressure (float)."
3. "Download the generated SDK."
4. "Copy the generated files into the skeleton project and make it compile — use the compiler errors as your guide."

**Metrics:**

| Metric | D1 | D2 | D3 |
|--------|----|----|-----|
| Time to first successful compile (min) | | | |
| Number of `#error` pragmas hit before success | | | |
| `#error` clarity rating (1–5 avg) | | | |
| Found CMakeLists_zinf.txt without prompting | ☐ Y ☐ N | ☐ Y ☐ N | ☐ Y ☐ N |
| Used sandbox.c path | ☐ Y ☐ N | ☐ Y ☐ N | ☐ Y ☐ N |
| Compiled without facilitator edits to generated code | ☐ Y ☐ N | ☐ Y ☐ N | ☐ Y ☐ N |

**`#error` clarity ratings per pragma (1 = confusing, 5 = crystal clear):**

| Pragma | D1 | D2 | D3 |
|--------|----|----|-----|
| STEP 1 — Define HARDWARE_INITIALIZED | | | |
| STEP 2 — Assign ctx.driver | | | |
| STEP 3 — Call hardware init | | | |
| STEP 4 — Implement bus reads | | | |

**Confusion points / facilitator notes:**

D1: _______________________________________________________________

D2: _______________________________________________________________

D3: _______________________________________________________________

**Pass criteria:** All 3 developers produce a compiling binary. No facilitator edits to generated code.

Result: ☐ PASS ☐ FAIL

---

## Summary

| Session | Participant | Success | Total time (min) | Clicks / Errors | Notes |
|---------|-------------|---------|-----------------|-----------------|-------|
| Researcher | R1 | ☐ Y ☐ N | | | |
| Researcher | R2 | ☐ Y ☐ N | | | |
| Researcher | R3 | ☐ Y ☐ N | | | |
| Developer  | D1 | ☐ Y ☐ N | | | |
| Developer  | D2 | ☐ Y ☐ N | | | |
| Developer  | D3 | ☐ Y ☐ N | | | |

**Overall result:** _____ / 6 participants succeeded without facilitator assistance.

**Boxed product milestone threshold:** ≥ 5 / 6  →  ☐ PASS ☐ FAIL

---

## Post-session debrief (open questions)

1. What was the most confusing part of the interface?
2. What would you change first?
3. Would you use this tool in your own research workflow?
