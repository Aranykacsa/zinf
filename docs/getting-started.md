# Getting Started

## Prerequisites

| Requirement | Notes |
|---|---|
| GCC (C11) | `gcc --version` should report 5.0+ |
| GNU Make | `make --version` |
| Linux kernel | Loopback device support (`CONFIG_BLK_DEV_LOOP`) |
| `dd` | Standard coreutils — for image creation and wipe |
| `sudo` | Required for `/dev/loop0` access and `O_DIRECT` raw I/O |

## Build

```bash
cd zinf/src
make          # produces ./zinf_cli
make clean    # remove build artifacts
```

The Makefile compiles all sources with:

```
-std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wundef
```

## Create a Test Loopback Device

ZINF targets `/dev/loop0` by default. The easiest way to test without real hardware is a loopback image.

```bash
# Create a 5 MB image (10240 sectors × 512 bytes)
dd if=/dev/zero of=testdisk.img bs=512 count=10240

# Attach it as a loopback device
sudo losetup --find --show testdisk.img
# Output: /dev/loop0

# Verify
sudo losetup -l
```

To detach when done:

```bash
sudo losetup -d /dev/loop0
```

> The image file is stored at `src/testdisk.img` by convention, but any path works as long as you attach it as `/dev/loop0`.

## Running the CLI

```bash
sudo ./zinf_cli <mode>
```

### Available Modes

| Mode | Description |
|---|---|
| `bench` | Run throughput/latency benchmark, print CSV |
| `cli` | Start interactive command prompt |
| `read <img>` | Print geometry info for an image file |
| `help` | Show usage |

### Interactive CLI

```
$ sudo ./zinf_cli cli
zinf> help
  help
  cfg show
  storage init
  log init
  msg save <0-255>
  msg test
  raid sensor <count> <v0> <v1> ...

zinf> storage init
zinf> log init
zinf> msg save 42
zinf> msg test
zinf> raid sensor 2 25.0 60.0 26.5 58.0
```

### One-Shot Command

You can pass CLI commands directly as arguments (no interactive prompt):

```bash
sudo ./zinf_cli storage init
sudo ./zinf_cli log init
sudo ./zinf_cli msg save 255
```

### Read Image Info

```bash
sudo ./zinf_cli read testdisk.img
```

Output example:

```
Image:          testdisk.img
Sector size:    512 bytes
Payload size:   507 bytes
Mirror count:   2
Total sectors:  10240
Usable sectors: 10238
RAID offset:    5119
```

## Benchmark

```bash
sudo ./zinf_cli bench
```

Outputs CSV to stdout:

```
PayloadSize,Throughput_KBps,MaxLatency_us,AvgLatency_us,SectorsWritten
507,1823.4,312,87,985
1014,3241.1,289,63,985
...
```

Redirect to a file for later analysis:

```bash
sudo ./zinf_cli bench > results.csv
```

See [Benchmarking](benchmarking.md) for details on metrics and interpretation.

## Full Example Session

```bash
# Build
cd zinf/src && make

# Set up loopback device
dd if=/dev/zero of=testdisk.img bs=512 count=10240
sudo losetup --find --show testdisk.img

# Initialize storage
sudo ./zinf_cli storage init
sudo ./zinf_cli log init

# Write some data
sudo ./zinf_cli msg save 100
sudo ./zinf_cli msg test         # writes 1024 bytes
sudo ./zinf_cli raid sensor 1 23.5 65.0

# Benchmark
sudo ./zinf_cli bench > bench.csv

# Cleanup
sudo losetup -d /dev/loop0
```
