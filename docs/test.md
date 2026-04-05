# Testing

## 1. Unit + fault-injection tests (no hardware needed)

```bash
cd tests
make -B run
```

Runs 66 tests covering CRC, RAID write/read, metadata versioning, bitflip recovery, torn writes, and majority voting. Uses a 2 MB image file in `/var/tmp/` — no loop device or sudo needed.

---

## 2. Interactive CLI (needs a loop device)

```bash
# Create a 10 MB test image and attach it
dd if=/dev/zero of=/tmp/test.img bs=1M count=10
sudo losetup /dev/loop0 /tmp/test.img

cd src
make
sudo ./zinf_cli cli
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

## 3. Benchmark

```bash
sudo ./zinf_cli bench
```

Outputs a CSV of throughput and latency across different chunk sizes.

---

## 4. Image inspection (no sudo)

```bash
./zinf_cli read /tmp/test.img
```

Shows sector geometry and computed RAID offset for any image file.

---

## 5. YAML codegen

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
