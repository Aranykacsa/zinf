# Fault Injection Test System

The CSV-driven fault test suite is an integration-level harness that injects hardware faults into a ZINF image file and verifies that the storage layer detects, reports, and recovers from them correctly. It is separate from the 28-test unit suite (`make run`) and targets the full stack — RAID mirroring, CRC verification, blacklist enforcement, and recovery.

---

## Overview

Three scenario sources feed a single test loop:

```
fault_scenarios.csv  ──┐
gen_edge_cases()     ──┼──→  run_scenario_to_result() × K repeats  ──→  fault_results.xlsx
gen_fuzz_scenarios() ──┘
```

Each scenario:
1. Creates a fresh temporary image (`/var/tmp/zinf_csv_fault.img`)
2. Writes a sensor reading via `raid_sensor_values()`
3. Injects a corruption (or pre-blacklists mirrors)
4. Runs a health check, the test action, then another health check
5. Evaluates four pass/fail checkpoints
6. Tears down and deletes the image

Results accumulate in memory and are written to a four-sheet Excel workbook at the end.

---

## Running the Tests

### Prerequisites

```bash
sudo dnf install libxlsxwriter-devel   # Fedora / RHEL
# or: sudo apt install libxlsxwriter-dev  (Debian / Ubuntu)
```

### Make targets

```bash
cd tests
make csv          # 172 scenarios, writes fault_results.xlsx
make csv-fuzz     # 222 scenarios (100 fuzz × 5 repeats), writes fault_results_fuzz.xlsx
make clean        # removes both xlsx files and /var/tmp/zinf_csv_fault.img
```

### Manual invocation

```
./run_fault_csv [options]
  -s <file>    fixed scenarios CSV  (default: fault_scenarios.csv)
  -o <file>    output xlsx          (default: fault_results.xlsx)
  -n <N>       fuzz scenario count  (default: 50)
  -k <K>       fixed scenario repeat count  (default: 1)
  -K <K>       fuzz scenario repeat count   (default: 3)
  -E           skip edge-case generation
  -F           skip fuzz generation
  -r <seed>    RNG seed             (default: time())
  -v           verbose: print one line per run with failure detail
  -h           show this help
```

**Examples:**

```bash
./run_fault_csv -F -E                     # fixed scenarios only, once each
./run_fault_csv -n 200 -K 10 -v           # 200 fuzz scenarios × 10 repeats, verbose
./run_fault_csv -r 42 -o repro.xlsx       # deterministic seed, custom output file
./run_fault_csv -s custom.csv -E -F       # custom CSV, no generators
```

---

## Scenario Sources

### Fixed scenarios (`fault_scenarios.csv`)

80 hand-authored scenarios loaded from the CSV at startup. Each row fully specifies the fault and the expected outcome.

#### CSV column schema

| Col | Field | Type | Description |
|---|---|---|---|
| 1 | `scenario_id` | int | Unique ID (1–80) |
| 2 | `scenario_name` | string | Short camel-case name |
| 3 | `mirror_count` | int | Number of RAID mirrors (1–5) |
| 4 | `fault_type` | enum | What to inject (see table below) |
| 5 | `affected_mirrors` | string | Which mirrors to corrupt |
| 6 | `byte_offset` | int | Byte within the sector (0–511) |
| 7 | `byte_count` | int | Number of bytes to corrupt |
| 8 | `corruption_pattern` | hex/NONE | Fill byte (e.g. `0xDE`, `0x00`, `NONE`) |
| 9 | `test_action` | enum | What to call after fault injection |
| 10 | `expected_write_rc` | string | Return code constant for the write phase |
| 11 | `expected_action_rc` | string | Return code constant for the action phase |
| 12 | `expected_valid_before` | int | Valid mirrors expected before action |
| 13 | `expected_valid_after` | int | Valid mirrors expected after action |
| 14 | `description` | string | Human-readable explanation |

#### Fault types

| `fault_type` | What happens |
|---|---|
| `NONE` | No corruption — baseline health check |
| `SEU` | Single-event upset: `byte_count` bytes at `byte_offset` overwritten with `corruption_pattern` |
| `TORN_WRITE` | Power-loss simulation: same as SEU but semantically a partial write (typically zeroes second half of sector) |
| `ZERO_FILL` | Full sector erased: `byte_count` bytes filled with `0x00` (usually the whole 512-byte sector) |
| `BLACKLIST_BEFORE_WRITE` | Affected mirror LBAs are blacklisted *before* the write — the write must degrade gracefully |

#### Sector byte layout

```
 Offset   Size   Field
 ──────   ────   ─────
   0       1 B   Header byte (message type tag, written by log_raid_u8bit_values)
   1     507 B   Payload (sensor wire format, zero-padded to 507 bytes)
 508       4 B   CRC32 of bytes [0..507], little-endian
```

CRC covers the header and the full payload region. Corrupting *any* byte in [0..507] causes a CRC mismatch. Corrupting bytes [508..511] directly corrupts the stored CRC.

#### `affected_mirrors` encoding

| Value | Meaning |
|---|---|
| `NONE` | No mirrors affected (used with `NONE` fault type) |
| `ALL` | All mirrors in the current configuration |
| `0` | Mirror 0 only |
| `0:1` | Mirrors 0 and 1 |
| `0:1:2` | Mirrors 0, 1, and 2 |

Mirror indices are 0-based. Physical LBA for mirror *m* = `logical_sector + m × mirror_offset`.

#### Test actions

| `test_action` | API called | What it does |
|---|---|---|
| `CHECK_ONLY` | `zinf_check_sector()` | Read and CRC-verify all mirrors; report health. No writes. |
| `RECOVER` | `zinf_recover_sector()` | Repair bad mirrors by copying from the first valid mirror. Re-read and re-verify after write. Blacklists any mirror that still fails. |
| `RAID_READ` | `raid_read()` | Read all mirrors, apply majority voting. Returns payload if majority agrees; otherwise `STORAGE_ERR_UNRECOVERABLE`. |

#### Fixed scenario coverage

| Group | IDs | Count | What is tested |
|---|---|---|---|
| 2-mirror baselines and single-mirror SEU | 1–11 | 11 | Healthy state, per-byte bitflips (header, payload, CRC), blacklist write-degradation |
| 3-mirror recovery and majority voting | 12–25 | 14 | 1-of-3, 2-of-3, all-3 corrupt; torn write, zero-fill, blacklist |
| 1-mirror (no redundancy) | 26–29 | 4 | Any fault → unrecoverable |
| 4-mirror scaling | 30–37 | 8 | 1-of-4 through 4-of-4 corrupt; torn write, zero-fill, blacklist |
| 5-mirror scaling | 38–45 | 8 | 1-of-5 through 5-of-5 corrupt; majority edge (2-valid ≠ majority) |
| RAID_READ action | 46–51 | 6 | 2-mirror and 3-mirror, 0 to all mirrors bad |
| Boundary byte offsets | 52–59 | 8 | Offsets 0, 1, 253, 506, 507, 508, 510, 511 |
| Pattern and burst variations | 60–65 | 6 | 0x55, 0xAA alternating patterns; 32 B, 128 B, 506 B bursts; compound corruption |
| 3-mirror gap coverage | 66–71 | 6 | Mirrors 1+2 bad (not 0), header corruption, 2-mirror blacklist, torn two mirrors |
| Edge extras | 72–80 | 9 | Both-mirrors-blacklisted write failure, 4-mirror CHECK_ONLY, 5-mirror majority threshold, multi-mirror zero-fill |

---

### Edge-case generation (skip with `-E`)

Produced by `gen_edge_cases()`. Sweeps all combinations of:

- **mirror_count** ∈ {2, 3}
- **fault_type** ∈ {SEU, TORN_WRITE, ZERO_FILL}
- **byte_offset** ∈ {0, 1, 253, 506, 507, 508, 511}

All edge cases use `ACTION_RECOVER` on mirror 0. Expected values are derived automatically (see [Expected-values derivation](#expected-values-derivation)).

**Output:** 42 scenarios with IDs 1000–1041, labelled `edge_<mc>m_<fault>_off<offset>`.

---

### Fuzz generation (skip with `-F`)

Produced by `gen_fuzz_scenarios()`. Each scenario is randomised:

| Parameter | Range |
|---|---|
| `mirror_count` | 2, 3, 4, or 5 |
| `fault_type` | any of the 5 types |
| `affected_mirrors` | 1 to all mirrors, randomly selected |
| `byte_offset` | 0–511 |
| `byte_count` | 1 to (512 − offset) |
| `corruption_pattern` | 0x00–0xFF |
| `action` | CHECK_ONLY, RECOVER, or RAID_READ (BLACKLIST and NONE always use CHECK_ONLY) |

Expected values are derived automatically. Use `-r <seed>` for a reproducible run:

```bash
./run_fault_csv -r 12345 -v   # same scenarios every time
```

**Output:** N scenarios (default 50, override with `-n`) with IDs 2000+, labelled `fuzz_<####>`.

---

## Console Output

Without `-v`, one line per **scenario** (not per run):

```
[  1/172] baseline_2m_healthy
[  2/172] seu_m0_payload_byte2
...
Writing fault_results.xlsx ...

=== Results: 272/272 runs passed (172 unique scenarios) ===
```

With `-v`, one line per **run**, including failure detail:

```
  [ OK ] sc=2 run=1 seu_m0_payload_byte2
  [FAIL] sc=75 run=1 seu_5m_majority_vote_3bad
         action_rc: exp STORAGE_OK got STORAGE_ERR_UNRECOVERABLE
```

A failing scenario prints the specific checkpoint mismatch(es) on the next line.

---

## Excel Output — Interpreting the Results

Open `fault_results.xlsx` in LibreOffice Calc or Excel. The workbook has four sheets.

### Colour coding

#### Row backgrounds

| Colour | Hex | Meaning |
|---|---|---|
| Light green | `#C6EFCE` | All checkpoints passed; no degraded write expected |
| Light yellow | `#FFEB9C` | All checkpoints passed but scenario expects `STORAGE_WARN_DEGRADED` |
| Light red | `#FFC7CE` | One or more checkpoints failed |
| Light blue | `#D9E1F2` | Fuzz-generated scenario (overrides status colour in Details) |
| Light grey | `#EDEDED` | Edge-generated scenario (overrides status colour in Details) |

#### Mirror status cells (columns `Bef_M0`–`Bef_M4`, `Aft_M0`–`Aft_M4`)

| Colour | Hex | Status code | Cause |
|---|---|---|---|
| White | — | `ZINF_MIRROR_OK` (0) | CRC verified — mirror is good |
| Orange | `#F4B183` | `ZINF_MIRROR_CRC_FAIL` (2) | CRC mismatch — bitflip or partial overwrite detected |
| Grey | `#BFBFBF` | `ZINF_MIRROR_BLACKLIST` (3) | LBA is in the bad-sector list — reads and writes skip this mirror |
| Red | `#FFC7CE` | `ZINF_MIRROR_IO_ERR` (1) | `read_sector` returned a driver error |
| Light grey | `#EDEDED` | N/A | Mirror index ≥ `mirror_count` — not used in this configuration |

#### Pass/fail checkpoint cells

| Colour | Meaning |
|---|---|
| Dark green bold (`#375623`) | Checkpoint passed (actual == expected) |
| Dark red bold (`#9C0006`) | Checkpoint failed (actual ≠ expected) |

#### Status cell (Overview, column 16)

| Text | Text colour | Row background |
|---|---|---|
| `PASS` | Dark green `#375623` | Light green |
| `WARN` | Dark amber `#7F6000` | Light yellow |
| `FAIL` | Dark red `#9C0006` | Light red |

---

### Overview sheet

One row per **unique scenario**, aggregated across all K repeats. Row 1 is the frozen header; auto-filter is active.

| Col | Header | Description |
|---|---|---|
| A | ID | Scenario ID (1–80 fixed, 1000+ edge, 2000+ fuzz) |
| B | Name | Scenario name |
| C | Type | `Fixed`, `Edge`, or `Fuzz` |
| D | FaultType | Fault type enum name |
| E | Mirrors | `mirror_count` |
| F | Action | `CHECK_ONLY`, `RECOVER`, or `RAID_READ` |
| G | WriteRC_Exp | Expected write return code |
| H | ActionRC_Exp | Expected action return code |
| I | ValidBefore_Exp | Expected valid mirrors before action |
| J | ValidAfter_Exp | Expected valid mirrors after action |
| K | Runs | Total runs for this scenario |
| L | Passed | Runs where all 4 checkpoints passed |
| M | Failed | Runs where at least one checkpoint failed |
| N | Warn/Degraded | Runs that expect `STORAGE_WARN_DEGRADED` |
| O | Pass% | `100 × Passed / Runs` |
| P | Status | `PASS`, `WARN`, or `FAIL` (bold, coloured) |

**How to read it:**
- A scenario that fails intermittently shows `Passed < Runs` with `Pass% < 100`. This indicates a race condition or non-deterministic behaviour.
- `WARN` rows (yellow) are expected-degraded writes — the write committed to fewer than `mirror_count` mirrors because some were blacklisted. This is correct behaviour.
- Sort by **Pass%** ascending to find the worst-performing scenarios first.

---

### Details sheet

One row per **run** (scenario × repeat number). Row 1 is the frozen header; auto-filter is active.

| Col | Header | Description |
|---|---|---|
| A | RunID | Sequential run index (1, 2, 3, …) |
| B | ScenID | Scenario ID |
| C | ScenName | Scenario name |
| D | Type | `Fixed`, `Edge`, or `Fuzz` |
| E | FaultType | Fault type enum |
| F | Mirrors | `mirror_count` |
| G | Action | Test action |
| H | Run# | Repeat number within the scenario (1..K) |
| I | WriteRC_Exp | Expected write return code |
| J | WriteRC_Act | Actual write return code |
| K | WriteRC_Pass | `PASS` or `FAIL` (green or red) |
| L | ValidBef_Exp | Expected valid mirrors before action |
| M | ValidBef_Act | Actual valid mirrors before action |
| N | ValidBef_Pass | `PASS` or `FAIL` |
| O | ActionRC_Exp | Expected action return code |
| P | ActionRC_Act | Actual action return code |
| Q | ActionRC_Pass | `PASS` or `FAIL` |
| R | ValidAft_Exp | Expected valid mirrors after action |
| S | ValidAft_Act | Actual valid mirrors after action |
| T | ValidAft_Pass | `PASS` or `FAIL` |
| U–Y | Bef_M0…Bef_M4 | Per-mirror status before action (colour-coded) |
| Z–AD | Aft_M0…Aft_M4 | Per-mirror status after action (colour-coded) |
| AE | BadSectors | Number of LBAs in the blacklist after the run |
| AF | BadLBAs | Semicolon-separated blacklisted LBAs (e.g. `1;2049`) |
| AG | Overall | `PASS` or `FAIL` — all four checkpoints AND-ed together |
| AH | FailureDetail | `NONE` or a description of each mismatched checkpoint |

**Reading `FailureDetail`:** The field concatenates each failing checkpoint with the expected and actual value. Example:

```
action_rc: exp STORAGE_OK got STORAGE_ERR_UNRECOVERABLE; valid_after: exp 3 got 1
```

This tells you the action returned the wrong code *and* the mirror count after recovery was lower than expected.

**Reading mirror columns:**
- If a mirror column shows `N/A` (light grey), that mirror index is not in use for this scenario's `mirror_count`.
- An orange `CRC_FAIL` cell *before* the action and white `OK` *after* the action confirms a successful repair.
- A grey `BLACKLIST` cell indicates the LBA was pre-blacklisted (intentional in blacklist scenarios) or blacklisted by a failed repair attempt.

---

### _ChartData sheet (hidden)

A hidden support sheet that holds four aggregation tables used as data sources for the chart series. You normally do not need to look here, but you can unhide it to inspect the raw numbers.

| Table | Rows | Content |
|---|---|---|
| A | 1–8 | Pass/fail counts by fault type (NONE, SEU, TORN_WRITE, ZERO_FILL, BLACKLIST, Edge, Fuzz) |
| B | 10–15 | Pass rate by mirror count (mirrors 1–5) |
| C | 18–47 | Average valid-mirror count before/after, for the first 30 unique scenarios |
| D | 51–55 | Failure checkpoint breakdown (which checkpoint fails most) |

---

### Charts sheet

Four embedded charts built from the `_ChartData` tables.

#### Chart 1 — Pass / Fail by Fault Type (clustered bar)

Shows how many runs passed and failed for each fault type and generator category. A tall red bar for a specific type (e.g. `SEU`) means that class of fault is regularly producing unexpected results.

#### Chart 2 — Pass Rate by Mirror Count (column)

Percentage of passing runs for each mirror count (1–5). A dip at a particular mirror count points to a recovery threshold issue — for example, majority voting requires `floor(mirror_count/2) + 1` valid mirrors, so 3-of-5 corrupt will fail even though 2 mirrors are intact.

#### Chart 3 — Avg Valid Mirrors Before vs After (clustered bar, first 30 scenarios)

Side-by-side comparison of average valid mirror counts before and after the test action. For `RECOVER` scenarios, the "after" bar should reach `mirror_count`. For `CHECK_ONLY`, both bars should be equal. A shorter "after" bar indicates incomplete repair.

#### Chart 4 — Failure Checkpoint Breakdown (pie)

Proportion of checkpoint failures across all runs. The slices are:

| Slice | What it means |
|---|---|
| `write_rc_mismatch` | Write phase returned the wrong code |
| `action_rc_mismatch` | Action phase returned the wrong code |
| `valid_before_low` | Fewer valid mirrors than expected before action |
| `valid_after_low` | Fewer valid mirrors than expected after action |
| `all_passed` | Runs with no failures (makes the pie meaningful) |

A dominant `action_rc_mismatch` slice suggests the expected return codes in the CSV need updating (e.g. majority voting threshold logic changed). A dominant `valid_before_low` slice suggests corruption is not being injected as expected.

---

## Expected-Values Derivation

For edge-case and fuzz scenarios the runner derives expected values automatically via `derive_expected()`. The rules match the actual API behaviour:

### `FAULT_NONE`

```
expected_write_rc     = STORAGE_OK
expected_valid_before = mirror_count
expected_action_rc    = STORAGE_OK
expected_valid_after  = mirror_count
```

### `FAULT_SEU` / `FAULT_TORN_WRITE` / `FAULT_ZERO_FILL`

Let *n* = number of affected mirrors, *M* = `mirror_count`.

```
expected_write_rc     = STORAGE_OK
expected_valid_before = M − n
```

For `RECOVER`:
```
if n < M:   expected_action_rc = STORAGE_OK,               expected_valid_after = M
else:        expected_action_rc = STORAGE_ERR_UNRECOVERABLE, expected_valid_after = 0
```

For `RAID_READ`:
```
valid = M − n
if valid == 0:                  expected_action_rc = STORAGE_ERR_UNRECOVERABLE
elif valid == 1 or M < 3:       expected_action_rc = STORAGE_OK        # no majority voting
else:
    threshold = M / 2 + 1
    if valid >= threshold:       expected_action_rc = STORAGE_OK
    else:                        expected_action_rc = STORAGE_ERR_UNRECOVERABLE
expected_valid_after = expected_valid_before   # RAID_READ does not repair
```

For `CHECK_ONLY`:
```
expected_action_rc   = STORAGE_OK
expected_valid_after = expected_valid_before   # no repair
```

### `FAULT_BLACKLIST_BEFORE_WRITE`

```
if n >= M:   expected_write_rc = STORAGE_ERR_DRIVER,      expected_valid_before = 0
else:        expected_write_rc = STORAGE_WARN_DEGRADED,   expected_valid_before = M − n
expected_action_rc   = STORAGE_OK
expected_valid_after = expected_valid_before
```

> **Fuzz false positives:** When `corruption_byte = 0x00` and `byte_offset` falls in the zero-padded payload region (bytes 9–507, since the 8-byte sensor wire format leaves the rest as zeros), the sector content does not change and the CRC remains valid. The fuzz runner may predict a failure that does not occur. This is an inherent property of the payload sparsity — it is not a bug in the tested code.

---

## Makefile Reference

```makefile
make csv          # build run_fault_csv + run with defaults → fault_results.xlsx
make csv-fuzz     # build run_fault_csv + run with 100 fuzz × 5 repeats → fault_results_fuzz.xlsx
make clean        # remove binaries, xlsx files, and /var/tmp/zinf_csv_fault.img
```

The build rule links `-lxlsxwriter` via `pkg-config` with a fallback:

```makefile
XLSX_LIBS := $(shell pkg-config --libs xlsxwriter 2>/dev/null || echo -lxlsxwriter)
```

Temporary image file used during runs: `/var/tmp/zinf_csv_fault.img` (4096 sectors, 2 MB). It is created and deleted for each scenario run and cleaned up by `make clean` in case a run was interrupted.
