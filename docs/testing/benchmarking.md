---
id: benchmarking
title: Benchmarking
sidebar_label: Benchmarking
sidebar_position: 2
description: Running the ZINF benchmark and interpreting throughput and latency results.
---

# Benchmarking

## Running the Benchmark

```bash
# Run (outputs CSV to stdout)
sudo zinf bench /dev/sdb

# Save results
sudo zinf bench /dev/sdb > results.csv

# Use RAM mock driver (no device needed)
zinf -R bench

# Randomized payload sizes (normal distribution instead of fixed steps)
sudo zinf bench /dev/sdb --random-payload

# Add jitter between writes (simulates scheduling noise)
sudo zinf bench /dev/sdb --jitter 500   # 500 µs max random sleep

# Pre-degrade N sectors before timing starts
sudo zinf bench /dev/sdb --pre-degraded 32
```

### Flags

| Flag | Description |
|---|---|
| `-d <device>` | Block device path (default `/dev/loop0`) |
| `-R` | Use the in-memory RAM mock driver (no block device needed) |
| `--random-payload` | Randomize write chunk counts (normal distribution) instead of the fixed step list |
| `--jitter <us>` | Inject a random `nanosleep()` up to `<us>` microseconds between each write call |
| `--pre-degraded <count>` | Blacklist `<count>` random sectors before the timing loop starts |

The benchmark automatically:
1. Wipes the device with `dd`
2. Recomputes `RAID_OFFSET`
3. Initializes storage and log sectors
4. Runs a write sweep across increasing payload sizes
5. Prints one CSV row per configuration

---

## Tested Payload Sizes

The benchmark tests the following chunk counts (each chunk = 507 bytes of payload = 1 sector):

```
1, 2, 4, 6, 8, 10, 12, 14, 16, 32, 1024, 2048, 4096 sectors
```

For each configuration, approximately 500 KB of total data is written, split into batches of the given chunk size.

---

## Metrics

| Column | Unit | Description |
|---|---|---|
| `PayloadSize` | bytes | Payload bytes per write call (`chunks × 507`) |
| `Throughput_KBps` | KB/s | Total data written ÷ total elapsed time |
| `MaxLatency_us` | µs | Worst-case single-call latency |
| `AvgLatency_us` | µs | Mean per-call latency across all iterations |
| `SectorsWritten` | count | Total sectors written during the run |

### Latency Measurement

Timing is measured with `clock_gettime(CLOCK_MONOTONIC)` at nanosecond resolution around each `raid_sensor_values()` call. Both mirror writes are included in the measurement.

---

## CSV Output Format

```
PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten
507,1823.4,312,87,985
1014,3241.1,289,63,985
2028,4102.7,251,54,985
...
```

The header line is always present. Values use `.1f` floating-point precision.

---

## Synthetic Sensor Data

The benchmark generates realistic-looking sensor data to avoid trivially compressible patterns:

### Temperature

```
period = 2000 ticks
phase  = tick % period
if phase < period/2:
    temp = 18.0 + (12.0 * phase) / (period/2)
else:
    temp = 30.0 - (12.0 * (phase - period/2)) / (period/2)
temp += noise * 0.5   (noise in range [-1, 1])
```

Range: 18–30°C, triangular wave, period 2000 iterations.

### Humidity

```
period = 2600 ticks
phase  = tick % period
if phase < period/2:
    humidity = 35.0 + (60.0 * phase) / (period/2)
else:
    humidity = 95.0 - (60.0 * (phase - period/2)) / (period/2)
humidity += noise * 1.0
```

Range: 35–95%, triangular wave, period 2600 iterations.

### PRNG (Noise)

XORshift32 algorithm, seeded with `0xDEADBEEF`:

```c
state ^= state << 13;
state ^= state >> 17;
state ^= state << 5;
noise = (float)(state & 0xFFFF) / 32768.0f - 1.0f;  // range [-1, 1]
```

---

## Interpreting Results

### Throughput

Higher is better. Throughput generally increases with larger payload sizes because:
- Per-call overhead (CRC computation, metadata read/write) is amortized over more data
- Fewer metadata updates relative to data written

Small payloads (1–4 sectors) are I/O overhead dominated. Large payloads (1024+ sectors) approach raw device throughput.

### Latency

Latency is per `raid_sensor_values()` call and includes:
- Sensor packing (negligible)
- CRC32 computation for each sector
- Two mirror writes to the block device (the dominant factor)

**Max latency** spikes often indicate OS scheduling jitter or page-cache writeback. `O_DIRECT` reduces but does not eliminate these.

**Avg latency** is a better metric for comparing configurations under steady load.

### Expected Behavior

| Payload Size | Expected Throughput | Expected Avg Latency |
|---|---|---|
| 1 sector (507 B) | Low (< 5 MB/s typical) | High (per-call metadata overhead dominates) |
| 16 sectors (~8 KB) | Medium | Medium |
| 4096 sectors (~2 MB) | High (approaches device limit) | Low per byte, high absolute |

On a loopback device (RAM-backed), throughput is typically in the 50–500 MB/s range depending on kernel version and RAM speed.

