# ZINF — Zinf is not FAT

ZINF is a lightweight, RAID-mirrored data logging library for IoT and embedded systems. It provides reliable persistent storage with built-in redundancy and CRC32 data integrity for sensor data and arbitrary byte sequences.

## Key Features

- **RAID-1 mirroring** — two on-disk copies of every write for single-failure tolerance
- **CRC32 integrity** — each sector is checksummed before being written
- **Pluggable driver interface** — swap out the storage backend per platform
- **Low-RAM fallback** — sector-by-sector writes when heap is constrained
- **Benchmark tooling** — built-in throughput and latency measurement

## Documentation

| Document | Description |
|---|---|
| [Getting Started](getting-started.md) | Build, install, format a device, and first run |
| [Architecture](architecture.md) | Layer design, RAID layout, and data flows |
| [API Reference](api-reference.md) | All public functions with signatures and return codes |
| [Data Formats](data-formats.md) | Sector, metadata, and sensor wire layouts |
| [Drivers](drivers.md) | `driver_t` interface and platform implementations |
| [Configuration](configuration.md) | Constants, structs, and runtime settings |
| [Benchmarking](benchmarking.md) | Running the benchmark and interpreting results |
| [Testing Overview](testing/test.md) | All test suites at a glance — unit tests, fault injection, CLI, benchmark |
| [Fault Testing](testing/fault-testing.md) | Massive-scale fuzz engine — running and understanding results |
| [Fuzz Analysis](testing/fuzz-analysis.md) | What the failure rate means, baseline results, how to spot real bugs |
| [Realistic Lifecycle Test](testing/realistic-test.md) | End-to-end data integrity test with power cycles, scrub recovery, and fault injection |
| [Advanced Test Suite](testing/advanced-test.md) | 8 targeted edge-case scenarios: storage wipe, degraded write, blacklist overflow, metadata corruption, version wraparound, full-range scrub, loopback I/O, repair cycle |
| [Embedded Porting Guide](embedded-porting.md) | SPI wiring, CMakeLists.txt, and first-boot init for RP2350/Pico |

## Repository Layout

```
zinf/
├── src/
│   ├── apps/           # CLI / benchmark entry point (zinf_main.c)
│   ├── config/         # Global constants and config struct (generated from zinf.yaml)
│   ├── core/
│   │   ├── api/        # High-level storage API
│   │   ├── storage/    # RAID write/read engine
│   │   └── helper/     # CRC32 + driver_t interface definition
│   ├── drivers/
│   │   ├── linux/      # Raw block device driver (O_DIRECT, pread/pwrite)
│   │   ├── sd/         # SPI-mode SD/SDHC/SDXC driver (embedded targets)
│   │   ├── mock/       # In-memory RAM driver for tests and fuzz engine
│   │   └── embedded/   # Compilable stub — starting point for new drivers
│   └── platform/       # platform_linux.c / platform_embedded.c
├── tests/              # 28 unit tests + 172 CSV fault-injection scenarios
├── tools/              # zinf_gen.py, zinf-probe.c, udev rules, libblkid patch
├── zinf.yaml           # Master configuration (sector size, mirror count, data types)
└── docs/               # This documentation
```

## Quick Start

```bash
# Build and install
make && sudo make install

# Format a test image
dd if=/dev/zero of=test.img bs=1M count=8 status=none
zinf format test.img

# Interactive shell
sudo zinf shell /dev/loop0

# Benchmark
sudo zinf bench /dev/loop0
```

See [Getting Started](getting-started.md) for the full walkthrough.
