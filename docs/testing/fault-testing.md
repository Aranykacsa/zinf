# Fault Injection Test System

The fault test suite (`tests/test_fault_csv.c`) is a massive-scale continuous-state fuzzer that executes millions of randomized operations against the ZINF storage stack, injects hardware faults mid-run, and verifies that the RAID, CRC, blacklist, and recovery paths behave correctly under sustained adversarial conditions.

> **Note:** The previous scenario-driven mode (fixed `fault_scenarios.csv`, edge-case generation, xlsx output) was superseded by this fuzzer as part of Task 6. The unit test suite (`make run`, 28 tests) is unchanged and still runs without any prerequisites.

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
make csv          # build + 100k iterations, linux loopback driver → fault_results.csv.gz
make csv-fuzz     # build + 1M iterations, RAM driver, seed=42    → fault_results_fuzz.csv.gz
make clean        # removes binaries and *.csv.gz temp files
```

### Manual invocation

```
./run_fault_csv [options]
  -R           use RAM mock driver (default: linux loopback)
  -M           enable massive scale mode (required)
  -n <count>   iteration count (default: 1,000,000)
  -o <path>    output prefix for .csv.gz file (default: fault_results)
  -r <seed>    RNG seed (default: time())
  -h           show this help
```

**`-M` is required.** Running without it prints an error and exits.

**Examples:**

```bash
# Fast in-memory run, 1M iterations, deterministic seed
./run_fault_csv -M -R -n 1000000 -r 42 -o results_ram

# Realistic loopback I/O, 500k iterations
./run_fault_csv -M -n 500000 -o results_loopback

# Brutally long RAM run
./run_fault_csv -M -R -n 10000000 -r 1 -o results_brutal
```

---

## How the Fuzzer Works

The fuzzer runs a single `zinf_ctx_t` (3 mirrors) through a continuous loop of randomized operations. Each iteration independently picks one of five actions:

| Probability | Action | API called | Description |
|---|---|---|---|
| 40% | Write | `raid_sensor_values()` | Write one sensor record with random temperature |
| 30% | Read | `raid_read()` | Read a random LBA in the data region |
| 15% | Scrub | `zinf_scrub()` | Scrub logical sectors 1–10 |
| 10% | Corrupt Payload | `ram_driver_corrupt()` | Overwrite one random byte on a random data sector (RAM driver only) |
| 5% | Corrupt Metadata | `ram_driver_corrupt()` | Overwrite one random byte in the metadata header (sector 0, offset 0–40) |

Corruption actions are **no-ops** when using the linux loopback driver (`-R` not set) because `ram_driver_corrupt` is not available via the linux driver.

The fuzzer uses a **LCG random number generator** seeded with `-r`. Use `-r <fixed>` to reproduce any run exactly.

---

## Output Format

Results are written to `<output_prefix>.csv.gz` (gzip-compressed CSV).

```
Iter,Action,RC,Status
0,1,0,PASS
1,2,0,PASS
2,4,0,PASS
...
```

| Column | Values | Description |
|---|---|---|
| `Iter` | 0 … N-1 | Iteration index |
| `Action` | 1–5 | 1=Write, 2=Read, 3=Scrub, 4=CorruptPayload, 5=CorruptMetadata |
| `RC` | uint8 | Return code from the API call (`STORAGE_OK=0`, etc.) |
| `Status` | PASS / FAIL | PASS if `RC == STORAGE_OK` or `RC == STORAGE_WARN_DEGRADED` |

### Console progress

```
Starting massive fuzzing: 1000000 iterations
Progress: 0/1000000 (Pass: 0, Fail: 0)
Progress: 10000/1000000 (Pass: 9987, Fail: 13)
...
Massive fuzzing complete. Results written to fault_results_fuzz.csv.gz
```

Progress is printed every 10,000 iterations.

---

## Interpreting Results

### Pass vs Fail counts

A "Fail" is any return code other than `STORAGE_OK` or `STORAGE_WARN_DEGRADED`. Expected failures include:

- `STORAGE_ERR_UNRECOVERABLE` (4) on Read actions after enough corruption has accumulated — this is **correct behaviour**, not a bug.
- `STORAGE_ERR_LOG_FULL` (3) if the log fills up during a long write-heavy run.

A true bug would be a crash, a hang, or an unexpected return code on an action that had no preceding corruption.

### Analysing the csv.gz

```bash
# Quick pass/fail summary
zcat fault_results_fuzz.csv.gz | awk -F, 'NR>1 {c[$4]++} END {for(k in c) print k, c[k]}'

# Fails by action type
zcat fault_results_fuzz.csv.gz | awk -F, 'NR>1 && $4=="FAIL" {c[$2]++} END {for(k in c) print "action="k, c[k]}'

# First 20 failures
zcat fault_results_fuzz.csv.gz | awk -F, 'NR>1 && $4=="FAIL"' | head -20
```

---

## Driver Choice: RAM vs Linux Loopback

| | RAM driver (`-R`) | Linux loopback |
|---|---|---|
| Speed | Very fast (memcpy) | Slower (pread/pwrite + O_DIRECT) |
| Fault injection | Full (`ram_driver_corrupt`) | No corruption (corrupt actions are no-ops) |
| Typical use | Fuzz correctness of RAID/CRC/recovery logic | Validate actual I/O path |
| `make` target | `csv-fuzz` | `csv` |

For correctness testing use `-R`. For I/O-path validation use the loopback driver.

---

## Makefile Reference

```makefile
make csv        # 100k iterations, loopback driver → fault_results.csv.gz
make csv-fuzz   # 1M iterations, RAM driver, seed=42 → fault_results_fuzz.csv.gz
make clean      # remove binaries and *.csv.gz
```
