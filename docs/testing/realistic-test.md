---
id: realistic-test
title: Realistic Lifecycle Test
sidebar_label: Lifecycle Test
sidebar_position: 5
description: End-to-end data integrity test with power cycles, scrub recovery, and fault injection.
---

# Realistic Lifecycle Test — Running and Interpreting Results

The realistic test (`tests/test_realistic.c`) simulates a full embedded device lifetime: continuous
sensor writes, periodic power-loss events, scrub-based recovery on each reboot, and randomised
hardware faults injected mid-run. It is the closest thing to a production workload in the test
suite.

---

## What It Tests

| Property | How it is tested |
|---|---|
| Data integrity end-to-end | Every written record is read back after each scrub and compared byte-for-byte |
| Power-cycle recovery | Blacklist is cleared before each scrub, forcing full re-discovery from disk |
| RAID repair | Single-mirror corruptions are repaired by `zinf_scrub`; the fix is verified on the next read |
| Silent corruption absence | `STORAGE_OK` returned with wrong bytes = immediate test failure |
| Capacity management | Auto re-format at `RESET_THRESHOLD` sectors; write counter is monotonic across formats |

---

## How It Works

**Shadow buffer** — a `shadow_t[]` array (up to 20,000 entries) records every written record's
LBA, expected `temp`, and expected `humidity`. `temp` is set to `(float)write_counter` and
`humidity` to `(float)(write_counter % 100)`, giving each record a unique, verifiable fingerprint
that survives any byte-level comparison.

**Write batch** — each iteration writes 10–`max_batch` sensor records. After each write, with
probability `error_rate`, one or two mirrors of a random previously-written sector are silently
corrupted via `ram_driver_corrupt` — simulating a cosmic-ray bitflip or partial erase.

**Power cycle** (every 3–7 batches):
1. `zinf_clear_bad_sectors` — clears the in-RAM blacklist (simulates MCU losing volatile state)
2. `zinf_scrub` — rebuilds the blacklist by scanning all mirrors; repairs single-mirror failures
3. Read back every record in the shadow buffer; compare to expected values

**Verdict per record:**
- `verified_ok` — bytes match exactly
- `verified_lost` — `STORAGE_ERR_UNRECOVERABLE` returned; all mirrors corrupted — data is gone and ZINF said so
- `verified_silent` — `STORAGE_OK` returned but bytes are wrong — **this is a bug**; exits with code 1

---

## Running the Tests

```bash
cd tests
make realistic          # 1000 power cycles, 1% fault rate, verbose
make realistic-brutal   # 5000 power cycles, 5% fault rate, verbose
```

### Manual invocation

```
./run_realistic [options]
  -n <cycles>   number of power cycles (default: 1000)
  -e <rate>     fault injection rate 0.0–1.0 (default: 0.01 = 1%)
  -b <size>     max write batch size per cycle (default: 100)
  -r <seed>     RNG seed (default: time())
  -v            verbose: print each power cycle result
  -h            show this help
```

**Examples:**

```bash
./run_realistic -n 1000 -v              # 1000 power cycles, verbose
./run_realistic -n 5000 -e 0.05 -v      # brutal: 5000 cycles, 5% fault rate
./run_realistic -n 500  -e 0.20 -v      # extreme: 20% fault rate
./run_realistic -n 2000 -r 42           # deterministic seed for reproducibility
```

---

## Reading the Per-Cycle Output

```
[CYCLE #  17] scrub: chk=4634 rep=5 unrec=22 | verify: ok=4613 lost=22 silent=0 | shadow=4635
```

| Field | Meaning |
|---|---|
| `chk=N` | Sectors scanned by scrub this cycle |
| `rep=N` | Degraded sectors repaired (one mirror bad, reconstructed from others) |
| `unrec=N` | Cumulative sectors with all mirrors destroyed — permanently unrecoverable |
| `ok=N` | Shadow records read back with matching bytes |
| `lost=N` | Shadow records that returned `STORAGE_ERR_UNRECOVERABLE` |
| `silent=N` | `STORAGE_OK` returned but bytes wrong — **must always be 0** |
| `shadow=N` | Total records currently tracked in the shadow buffer |

---

## Interpreting the Summary

### 1% fault rate — standard run (`make realistic`)

```
=== REALISTIC FUZZ SUMMARY ===
Error injection rate:     1.00%
Power cycles:             206
Format cycles:            2

Writes attempted:         55157
  Succeeded (OK):         55157
  Succeeded (degraded):   0
  Failed:                 0

Faults injected:          568
Repaired by scrub:        283
Unrecoverable:            9494

Verified OK:              1932182
Verified LOST:            9494
Silent corruption:        0  <-- must be 0

Data integrity:           99.511041%
No silent corruption. ZINF never returned OK with wrong data.
```

~48% of injected faults are repaired by scrub (single-mirror hits). The remaining ~52% are
double-mirror hits on the same sector — unrecoverable by design. Data integrity stays above 99.5%.

### 5% fault rate — brutal run (`make realistic-brutal`)

```
=== REALISTIC FUZZ SUMMARY ===
Error injection rate:     5.00%
Power cycles:             1014
Format cycles:            14

Writes attempted:         273597
  Succeeded (OK):         273597
  Succeeded (degraded):   0
  Failed:                 0

Faults injected:          13599
Repaired by scrub:        6555
Unrecoverable:            229263

Verified OK:              9310708
Verified LOST:            229392
Silent corruption:        0  <-- must be 0

Data integrity:           97.595497%
No silent corruption. ZINF never returned OK with wrong data.
```

At 5× the standard rate, 14 format cycles were triggered (capacity recycled 14 times). Still
273,597 successful writes with zero failures — the blacklist correctly routes new writes away from
known-bad sectors rather than silently writing to them.

---

## Why `Verified LOST` Is Cumulative

`Verified LOST: 229,392` across 1014 power cycles does not mean 229k distinct records are gone.
The shadow buffer is verified in full on every power cycle. A sector lost in cycle 10 is counted
again in cycles 11, 12, 13, and so on. The actual number of distinct unrecoverable sectors is
`scrub_unrecoverable` from the final cycle — roughly 200–300 at 5% fault rate.

---

## The Only Number That Must Be Zero

```
Silent corruption:        0  <-- must be 0
```

A non-zero here means ZINF returned `STORAGE_OK` while handing back corrupted bytes with no
indication to the caller. This has never been observed across any run at any fault rate.

The test exits with code 1 if silent corruption is detected, making it CI-friendly:

```bash
make realistic && echo "PASS" || echo "FAIL — silent corruption detected"
```

---

## Limitations

- **RAM driver only** — fault injection requires `ram_driver_corrupt`; the linux loopback driver does not support it.
- **Single-threaded** — does not test concurrent access.
- **No power-loss mid-write** — `ram_driver_drop_buffer` is not yet wired in; power loss is simulated only as blacklist reset, not as a partial sector write.
- **Shadow buffer capped at 20,000 entries** — auto re-formats before overflow; the write counter is monotonic across formats so verifiability is maintained.
