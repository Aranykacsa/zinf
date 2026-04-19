# Task 6: Massive-Scale Randomized Fault Engine

**Context:**
To prove ZINF's robustness, we are scaling from 172 tests to 1,000,000+ randomized iterations. You will implement a dual-path execution engine: a high-speed In-Memory Mock Driver and a hardware-accurate Linux Loopback Driver.

**Step-by-Step Instructions for LLM:**

1. **In-Memory RAM Driver (Fast Path):**
   - Create `src/drivers/mock/ram_driver.c`.
   - Implement `driver_t`. `init` should `malloc` a buffer representing the disk.
   - `read_block` and `write_block` must use `memcpy`.
   - Add a private API: `ram_driver_corrupt(lba, offset, val)` to allow direct memory manipulation for fault injection.

2. **Continuous State Fuzzing Engine:**
   - Modify `tests/run_fault_csv.c` to support a `--fuzz-random` mode.
   - Implement a loop that executes 1,000,000 iterations.
   - In each iteration, randomly choose one action:
     - `WRITE`: Write a batch of randomized payload sizes.
     - `CORRUPT`: Randomly target Payload OR Metadata (Alpha/Omega) sectors.
     - `SCRUB`: Run `zinf_scrub()` to verify the blacklist.
     - `POWER_LOSS`: Randomly discard the RAM driver buffer and re-init from metadata.

3. **Metadata Attrition Targets:**
   - The fuzzer MUST explicitly target the 3 metadata copy slots.
   - Intentionally trigger version wraparounds (`0xFFFF -> 0x0000`) by artificially setting version counters high before a write batch.

4. **Paged Output Streamer:**
   - Implement a buffer that holds 100 results.
   - Every 100 iterations, flush the results to a `.csv.gz` file to prevent OOM.
   - If total runs > 1,000,000, only write the full detail trace to CSV, and generate a summary-only `.xlsx` with aggregation charts.

5. **Randomized Benchmarking:**
   - Update `src/apps/zinf_main.c` (bench command).
   - Add a `--random-payload` flag that uses a normal distribution for write sizes.
   - Add a `--jitter` flag that injects randomized `nanosleep()` between writes.
   - Add a `--pre-degraded <percentage>` flag that blacklists random sectors before starting the timer.