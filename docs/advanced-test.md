# Advanced Test Suite — Running and Interpreting Results

The advanced test (`tests/test_advanced.c`) is a structured regression suite covering seven
edge-case scenarios that the fuzzer and realistic lifecycle test do not exercise. Each test runs
as a self-contained scenario, asserts a set of invariants, and writes its full trace to an Excel
workbook (`advanced_results.xlsx`) — one worksheet per scenario plus a colour-coded summary sheet.

---

## What It Tests

| # | Test | Coverage gap addressed |
|---|---|---|
| 1 | StorageWipe | Full RAM wipe followed by reinit — proves storage is usable after total loss of content |
| 2 | DegradedWrite | Write with mirror-1 pre-blacklisted — exercises the `STORAGE_WARN_DEGRADED` code path |
| 3 | BlacklistOverflow | Attempt to mark more than `MAX_BAD_SECTORS` (16) — proves graceful saturation |
| 4 | MetadataCorruption | Corrupt sector 0 at 5 offsets — data sectors must be unaffected |
| 5 | VersionWrap | Force version counter 0xFFFE → 0xFFFF → 0x0000 — proves modular comparison is correct |
| 6 | FullRangeScrub | 30 faults scattered across early/mid/late 10% of 500 written sectors |
| 7 | Loopback | End-to-end byte verification using the linux block-device driver on a real image file |

---

## How Each Test Works

### 1. StorageWipe

1. Write 200 records to RAM storage (unique temp/humidity fingerprints)
2. Call `ram_driver_drop_buffer()` — zeroes every byte of every sector, including sector 0
3. Call `init_log_sector` — reinitialise the metadata header from scratch
4. Call `zinf_scrub` over the formerly-written range [2..200] — must complete without crash; all
   sectors are unrecoverable (zeroed data fails CRC), but no crash or assert is acceptable
5. Call `zinf_clear_bad_sectors` — reset the volatile blacklist (simulates MCU power-on after wipe)
6. Write 50 new records and read each one back; compare bytes exactly

**PASS criteria:** `init_log_sector` returns `STORAGE_OK`; `zinf_scrub` returns `STORAGE_OK`;
all 50 post-wipe reads match expected values.

**Key finding:** The scrub on zeroed storage fills the blacklist (every zeroed sector's CRC fails,
triggering `zinf_mark_bad_sector`). The blacklist saturates at `MAX_BAD_SECTORS = 16` and stops
accepting new entries — graceful degradation, not a crash. The subsequent
`zinf_clear_bad_sectors` call is necessary before new writes, mirroring the real MCU boot
sequence where the blacklist is lost on power-cycle.

---

### 2. DegradedWrite

- For 10 distinct sectors: blacklist mirror-1's physical address with `zinf_mark_bad_sector`,
  then call `raid_sensor_values`
- Assert `STORAGE_WARN_DEGRADED` is returned (write succeeded on mirror-0 only)
- Read back via `raid_read` and verify bytes match
- Call `zinf_check_sector` to confirm mirror-0 health is OK and mirror-1 is BLACKLIST

Excel columns include physical addresses for both mirrors, expected and actual return code, mirror
health codes (`OK=0 IO_ERR=1 CRC_FAIL=2 BLACKLIST=3`), and per-row Pass/Fail.

**PASS criteria:** All 10 writes return `WARN_DEGRADED`; all 10 reads return matching bytes;
`silent = 0`.

---

### 3. BlacklistOverflow

- Mark 20 distinct physical sectors as bad (one call to `zinf_mark_bad_sector` per sector)
- `MAX_BAD_SECTORS = 16`: the first 16 calls must return `STORAGE_OK`; calls 17–20 must return
  `STORAGE_ERR_PARAM`
- Verify `ctx.bad_sector_count` never exceeds 16

**PASS criteria:** Exactly 4 overflow attempts caught; `bad_sector_count == 16` after saturation;
no out-of-bounds write, no crash.

---

### 4. MetadataCorruption

1. Write 100 records
2. Read sector 0 raw, corrupt 5 random byte offsets via `ram_driver_corrupt(0, offset, val)`,
   recording the original byte value before each corruption
3. `zinf_clear_bad_sectors` + `zinf_scrub` over the written range
4. Read back all 100 data sector records; compare bytes

Excel captures the corrupted byte offset, original value, injected value, and the per-record
verify result.

**PASS criteria:** `zinf_scrub` returns `STORAGE_OK`; all 100 data records read back correctly
(`ok = 100`, `silent = 0`). Metadata corruption must not propagate to data sector content.

---

### 5. VersionWrap

- Write 3 records to establish version history
- Read sector 0 raw; patch all 3 copy-slot version fields to `0xFFFE`
- Write 3 more records — versions step `0xFFFE → 0xFFFF → 0x0000 → 0x0001`
- After each write, snapshot all 3 version fields (displayed as hex: `0xFFFF`, `0x0000`, etc.)
- Determine which slot was written using before/after comparison
- Read back all 6 records and verify data integrity across the wraparound boundary

The modular comparison `(uint16_t)(ver_a - ver_b) < 0x8000u` is the mechanism under test:
`(uint16_t)(0x0000 - 0xFFFF) = 0x0001 < 0x8000u`, so slot with version 0 is correctly
identified as newer than a slot at version 0xFFFF.

**PASS criteria:** All 6 reads match expected bytes; version counter progresses through
`0xFFFE → 0xFFFF → 0x0000 → 0x0001` without misidentifying the newest slot.

---

### 6. FullRangeScrub

1. Write 500 records
2. Inject 30 faults in three spatial zones:
   - 10 faults in the first 10% of the LBA range (early sectors)
   - 10 faults in the middle 10%
   - 10 faults in the last 10%
3. Run `zinf_scrub` over the full [2..last_lba] range
4. Read back all 500 records; count OK / lost / silent

Excel labels each row with its zone (`early` / `mid` / `late`), includes the corrupted mirror
index, byte offset, and injected value for each fault row.

**PASS criteria:** `silent = 0` (the only hard invariant); `rep.repaired + rep.unrecoverable`
consistent with 30 injected faults (single-mirror hits repaired, double-mirror hits
unrecoverable).

---

### 7. Loopback

- Create `/var/tmp/zinf_advanced_loopback.img` (65536 × 512 bytes via `ftruncate`)
- Open with the linux block-device driver (O_DIRECT path, not the RAM driver)
- Write 100 records with unique temp/humidity values
- Read each record back immediately; compare bytes exactly; record temp diff and humidity diff

This test validates that the linux driver's `pread`/`pwrite` path does not mangle sector
content — a byte written must be a byte read back.

**PASS criteria:** All 100 records match exactly (temp diff < 0.001, humidity diff < 0.001).
The image file is deleted on completion.

---

## Running the Tests

```bash
cd tests
make advanced          # build + run, output → advanced_results.xlsx
```

### Manual invocation

```
./run_advanced [options]
  -o <prefix>   output file prefix (default: advanced_results)
  -h            show this help
```

```bash
./run_advanced                      # default output: advanced_results.xlsx
./run_advanced -o my_results        # output: my_results.xlsx
```

---

## Reading the Terminal Output

```
ZINF advanced tests
  [1/7] StorageWipe           PASS
  [2/7] DegradedWrite         PASS
  [3/7] BlacklistOverflow     PASS
  [4/7] MetadataCorruption    PASS
  [5/7] VersionWrap           PASS
  [6/7] FullRangeScrub        PASS
  [7/7] Loopback              PASS

Results → advanced_results.xlsx
Overall: PASS
```

The executable exits with code 0 if all tests pass, 1 if any test fails — making it CI-friendly:

```bash
make advanced && echo "PASS" || echo "FAIL"
```

---

## Reading the Excel Workbook

Open `advanced_results.xlsx`. The first sheet is **Summary**:

| Column | Meaning |
|---|---|
| # | Test index |
| Test | Test name |
| Description | What the test verifies |
| Result | PASS or FAIL |
| Total Assertions | Number of individual boolean checks run |
| Passed | Assertions that evaluated true |
| Failed | Assertions that evaluated false (0 on a clean run) |
| Metric | Key counters in condensed form (see per-test notes below) |

All rows are green on a passing run. Any red row identifies the failing test and its first failed
assertion.

### Per-sheet structure

Each detail sheet has a **Phase** column as the first field:

| Phase label | Meaning |
|---|---|
| `PRE_WIPE_WRITE` | Data written before the storage wipe (StorageWipe) |
| `WIPE` | `ram_driver_drop_buffer()` event row (amber) |
| `REINIT` | `init_log_sector` call and its return code |
| `SCRUB` | `zinf_scrub` call with checked / repaired / unrecoverable counts |
| `CLEAR_BLACKLIST` | `zinf_clear_bad_sectors` event (amber) |
| `POST_WIPE_WRITE` | Writes after reinit |
| `POST_WIPE_VERIFY` | Reads after reinit with expected vs actual byte comparison |
| `WRITE` | Normal data write row |
| `INJECT` | Fault injection row (amber) — shows mirror, offset, original byte, injected byte |
| `VERIFY` | Read-back row with expected and actual values |

**Row colours:**
- Dark blue header row: column labels
- Light green: assertion passed
- Light red: assertion failed
- Amber/bold: event row (wipe, inject, blacklist clear) — not an assertion
- Light gray: write-only data row (no assertion yet, data recorded for later verify)

---

## Metrics Reference

### StorageWipe
```
pre_writes=200 post_writes=50 assertions=52 passed=52
```
52 assertions: 1 REINIT ok + 1 SCRUB ok + 50 post-wipe read verifications.

### DegradedWrite
```
degraded_seen=10/10 silent=0 assertions=30 passed=30
```
30 assertions: 10 RC checks + 10 data matches + 10 mirror-health checks.

### BlacklistOverflow
```
overflow_caught=4/4 max_count=16 assertions=24 passed=24
```
24 assertions: 16 "mark succeeded" + 4 "overflow caught" + 4 "count at limit" checks.

### MetadataCorruption
```
ok=100 lost=0 silent=0 assertions=101 passed=101
```
101 assertions: 1 scrub RC + 100 per-record verify.

### VersionWrap
```
total_writes=6 silent=0 assertions=6 passed=6
```
6 assertions: one read-back verification per write.

### FullRangeScrub
```
faults=30 rep=N unrec=M ok=P lost=Q silent=0 assertions=501 passed=501
```
501 assertions: 1 (silent must be 0) + 500 per-record verify. `rep + unrec` should sum to ~30
(minor variation due to duplicate fault targets on the same sector).

### Loopback
```
100/100 matched assertions=100 passed=100
```
100 assertions: one byte-match check per record.

---

## The Only Number That Must Be Zero

Across all tests, the key invariant is:

```
silent = 0
```

A non-zero here means ZINF returned `STORAGE_OK` while handing back incorrect bytes. This was
never observed across any test at any scenario. Tests 2, 5, 6 explicitly track and assert `silent`.

---

## Limitations

- **Single-threaded** — does not test concurrent access.
- **RAM driver for most tests** — fault injection requires `ram_driver_corrupt`; only the Loopback
  test uses the linux block-device driver.
- **Fixed seed** — the LCG starts at `0xDEADBEEF`; results are fully deterministic.
- **No mid-write power-loss** — `ram_driver_drop_buffer` is used post-write only; partial-sector
  writes are not yet exercised.
