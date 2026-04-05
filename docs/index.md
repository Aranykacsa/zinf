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
| [Getting Started](getting-started.md) | Build, loopback device setup, and first run |
| [Architecture](architecture.md) | Layer design, RAID layout, and data flows |
| [API Reference](api-reference.md) | All public functions with signatures and return codes |
| [Data Formats](data-formats.md) | Sector, metadata, and sensor wire layouts |
| [Drivers](drivers.md) | `driver_t` interface and platform implementations |
| [Configuration](configuration.md) | Constants, structs, and runtime settings |
| [Benchmarking](benchmarking.md) | Running the benchmark and interpreting results |

## Repository Layout

```
zinf/
├── src/
│   ├── apps/           # CLI / benchmark entry point (zinf_main.c)
│   ├── config/         # Global constants and config struct
│   ├── core/
│   │   ├── api/        # High-level storage API
│   │   ├── storage/    # RAID write/read engine
│   │   └── helper/     # CRC32 + driver interface definition
│   ├── drivers/
│   │   ├── linux/      # Raw block device driver (O_DIRECT)
│   │   └── embedded/   # Stub for custom embedded targets
│   └── platform/       # Platform selection (active_driver, log_sector)
├── tests/              # Functional tests and benchmark CSV/charts
├── tools/              # Stress and endurance test programs
└── docs/               # This documentation
```

## Quick Start

```bash
# Build
cd src && make

# Create a 5 MB test image and attach it
dd if=/dev/zero of=testdisk.img bs=512 count=10240
sudo losetup --find --show testdisk.img   # usually /dev/loop0

# Run interactive CLI
sudo ./zinf_cli cli

# Run benchmarks
sudo ./zinf_cli bench
```

See [Getting Started](getting-started.md) for the full walkthrough.
