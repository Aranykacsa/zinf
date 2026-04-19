# Testing

## 1. Unit + fault-injection tests (no hardware needed)

```bash
cd tests
make -B run
```

Runs **28 tests** across three suites — no loop device, no sudo needed. Two image files are created and cleaned up automatically under `/var/tmp/`. For the larger CSV-driven integration suite see [Section 2](#2-csv-fault-injection-test-suite-no-hardware-needed).

> **All test suites (`make run`, `make csv`) are host-only** — they use the Linux block driver and, for the CSV suite, `libxlsxwriter`. Neither can run on an MCU. See the [Embedded Porting Guide](embedded-porting.md#testing-on-mcu) for MCU testing strategy.

| Suite | File | Count | Covers |
|---|---|---|---|
| CRC | `test_crc.c` | 5 | `zinf_crc32()` correctness and bit sensitivity |
| Storage | `test_storage.c` | 8 | Write/read round-trip, message log, metadata versioning, wraparound |
| Fault | `test_fault.c` | 15 | Bitflip recovery, torn write, majority voting, **blacklist**, **health-check**, **recover**, **scrub** |

### Fault test breakdown

| Test | What it checks |
|---|---|
| `fault_single_mirror_bitflip` | Corrupt mirror 0 → `raid_read` falls back to mirror 1 |
| `fault_torn_write_mirror0` | Zero second half of mirror 0 sector → recovers from mirror 1 |
| `fault_all_mirrors_bad` | Both mirrors corrupt → `STORAGE_ERR_UNRECOVERABLE` |
| `fault_majority_voting_3mirrors` | 3 mirrors, 1 bad → majority wins |
| `fault_metadata_partial_corruption` | 2 of 3 metadata copy slots corrupt → third slot wins |
| `blacklist_mark_and_check` | `zinf_mark_bad_sector` / `zinf_is_bad_sector` basic round-trip |
| `blacklist_overflow` | Fill to `MAX_BAD_SECTORS` → next mark returns `STORAGE_ERR_PARAM` |
| `blacklist_clear` | Mark sectors, `zinf_clear_bad_sectors`, verify none remain |
| `check_sector_all_ok` | Write → `zinf_check_sector` → `valid_count == 2`, both `ZINF_MIRROR_OK` |
| `check_sector_one_corrupt` | Corrupt mirror 0 → `zinf_check_sector` → `status[0] == ZINF_MIRROR_CRC_FAIL` |
| `check_sector_blacklisted_mirror` | Blacklist mirror 0 LBA → `status[0] == ZINF_MIRROR_BLACKLIST` |
| `recover_repairs_bad_mirror` | Corrupt mirror 0 → `zinf_recover_sector` → both mirrors OK after |
| `recover_unrecoverable` | All mirrors corrupt → `STORAGE_ERR_UNRECOVERABLE` + both LBAs blacklisted |
| `write_skips_blacklisted_mirror` | Blacklist mirror 0 LBA → `raid_sensor_values` returns `STORAGE_WARN_DEGRADED`, mirror 0 untouched |
| `scrub_range` | Write 4 sectors, corrupt 2 → `zinf_scrub` → `repaired=2, healthy=2, unrecoverable=0` |

---

## 2. CSV fault-injection test suite (no hardware needed)

```bash
cd tests
make csv          # standard run
make csv-fuzz     # extended fuzz run
```

Runs a matrix of fault scenarios drawn from three sources — no loop device, no sudo needed. Results are written to an Excel workbook with colour-coded pass/fail rows, per-mirror status cells, and four embedded charts. See [fault-testing.md](fault-testing.md) for the full reference.

| Command | Scenarios | Runs | Output |
|---|---|---|---|
| `make csv` | 80 fixed + 42 edge + 50 fuzz = 172 | ×1 fixed, ×1 edge, ×3 fuzz | `fault_results.xlsx` |
| `make csv-fuzz` | 80 fixed + 42 edge + 100 fuzz = 222 | ×1 fixed, ×1 edge, ×5 fuzz | `fault_results_fuzz.xlsx` |

**Prerequisite:** `libxlsxwriter-devel` must be installed before building `run_fault_csv`.

```bash
sudo dnf install libxlsxwriter-devel   # Fedora
```

---

## 3. Interactive CLI (needs a loop device)

```bash
# Create a 10 MB test image and attach it
dd if=/dev/zero of=/tmp/test.img bs=1M count=10
sudo losetup /dev/loop0 /tmp/test.img

cd src
make
sudo zinf shell /dev/loop0
```

Then type commands:

```
> storage init
> log init
> cfg show
> msg save 42
> raid sensor 1 25
> help
```

---

## 4. Benchmark

```bash
sudo zinf bench /dev/loop0
```

Outputs a CSV of throughput and latency across different chunk sizes.

---

## 5. Image inspection (no sudo)

```bash
zinf info /tmp/test.img
```

Shows sector geometry and computed RAID offset for any image file.

---

## 6. YAML codegen

```bash
# Edit zinf.yaml (change mirror_count, add data types, etc.)
nano zinf.yaml

# Regenerate config.h and config.c
python3 tools/zinf_gen.py zinf.yaml src/config/

# Rebuild
cd src && make
```

---

## Cleanup

```bash
sudo losetup -d /dev/loop0
rm /tmp/test.img
```

## 7. User testing

See [`05-user-testing-protocol.md`](../tasks/05-user-testing-protocol.md) for the full protocol.

### Researchers
At least 3 researchers must participate. Each reads measurement data from a ZINF-formatted device and exports it to CSV using ZINF Studio.

### Developers
At least 3 developers must participate. Each integrates ZINF into a predefined reference project from scratch.