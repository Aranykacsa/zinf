---
id: ekop
title: Research Directions (EKÖP 2026/2027)
sidebar_label: Research Directions
sidebar_position: 10
description: Future research tracks for ZINF — formal verification, SEU hardening, RTOS integration, and scientific data ecosystem.
---

# ZINF — Research Directions (EKÖP 2026/2027)

> This document serves as the scientific planning foundation for the EKÖP 2026/2027 (Egyetemi Kutatói Ösztöndíj Program) application at Pécsi Tudományegyetem. It presents four coordinated research tracks that extend ZINF from a working embedded file system into a scientifically validated, ecosystem-supported storage platform for mission-critical and space applications. Grant application text is prepared separately in Hungarian per program requirements.

---

## Scientific Context

ZINF is a deterministic, RAID-mirrored data logging file system designed for resource-constrained embedded systems. Its Format v4 has been empirically validated across 1,000,000+ randomized fault iterations with zero observed silent data corruption — a result that compares favorably to general-purpose embedded file systems (LittleFS, SPIFFS, YAFFS2) which make no such guarantees for the telemetry use case.

Despite this, four dimensions remain scientifically open:

1. **Formal guarantees** — empirical testing cannot cover all possible failure states; mathematical proof is required for safety certification.
2. **Radiation characterization** — real-world deployment in CanSat/CubeSat environments introduces ionizing-radiation fault patterns not modeled by the existing software fuzzer.
3. **Real-time integration** — ZINF is single-threaded today; deterministic WCET bounds in an RTOS context have not been established.
4. **Scientific accessibility** — data stored in ZINF binary format is not yet natively accessible from the scientific Python ecosystem used for post-mission analysis.

Each of the four tracks below directly addresses one of these gaps.

---

## Track 1 — Formal Crash-Consistency Verification

### Motivation

Empirical fuzzing demonstrates that ZINF never produced a silent corruption in 10M+ iterations, but empirical evidence cannot constitute a formal guarantee. Safety-critical applications — aerospace avionics, implantable medical devices, industrial control systems — increasingly require formal proofs of correctness for storage subsystems. ZINF's architecture is deliberately simple enough to be amenable to formal methods, unlike general-purpose file systems with complex metadata trees.

### Research Question

> *Can the crash-consistency and no-silent-corruption invariants of ZINF Format v4 be formally proven using model checking, and what is the minimal property set required for certification in safety-critical embedded environments?*

### Methodology

**Phase 1 — Formal specification (months 1–3)**
- Model the ZINF write/recovery protocol in **TLA+**: the 3-slot redundant metadata update, the 16-bit monotonic version counter (including wraparound), the RAID physical address computation, and the `zinf_scrub` recovery algorithm.
- Define key invariants formally:
  - *No-silent-corruption*: if `STORAGE_OK` is returned, the data bytes match the last committed write.
  - *Crash-consistency*: after any interrupted write sequence and subsequent `zinf_scrub`, the system is in a valid state.
  - *Version ordering*: the modular comparison `(V_new - V_old) mod 2^16 < 2^15` always selects the most recently committed slot.

**Phase 2 — Model checking (months 3–6)**
- Run **TLC** (TLA+ model checker) over the state space: enumerate all crash points, all possible version states, and all mirror failure combinations for `mirror_count ∈ {1, 2, 3, 5}`.
- Bound the state space: sector count is finite (parameterized), version counter wraps at 2^16.
- Verify the three invariants hold universally. Any counterexample found is a latent bug in the current implementation.

**Phase 3 — Comparison and publication (months 6–10)**
- Compare the ZINF formal model with LittleFS's informal consistency argument and SPIFFS's lack thereof.
- Document the proof methodology as a reusable template for embedded storage verification.

### Expected Deliverables

| Deliverable | Timeline |
|---|---|
| TLA+ specification of ZINF Format v4 protocol | Month 3 |
| TLC verification results for all three invariants | Month 5 |
| Conference paper draft | Month 7 |
| TDK paper (Hungarian, PTE conference) | Month 9 |

### Potential Publication Venues

- **IEEE Embedded Systems Letters** (Q2)
- **FMICS** (Formal Methods for Industrial Critical Systems, annual workshop)
- **RTSS** (IEEE Real-Time Systems Symposium) — work-in-progress track
- **TDK Konferencia** (PTE, required EKÖP output)

### Resources Needed

- TLA+ toolbox (free) + TLC model checker
- Access to ZINF source code (already available)
- No hardware required

### Feasibility

**Medium.** TLA+ modeling requires a learning investment (~2–4 weeks) and careful mapping from C semantics to TLA+. The state space for 3 copy slots × 2^16 version states is manageable. The main risk is underspecifying the model — mitigated by iterating against the existing test suite.

---

## Track 2 — SEU Tolerance and Radiation Hardening Characterization

### Motivation

CanSat and CubeSat systems operate in environments where galactic cosmic rays, solar particle events, and trapped radiation in Van Allen belts cause **Single Event Upsets (SEU)** — random bitflips in SRAM and flash memory. ZINF's first production deployment (Mimike-II Rev-I, 2025) operated at near-space altitudes; the planned Rev-II mission extends this. No published study compares the data integrity of competing embedded file systems under realistic space radiation models.

### Research Question

> *How does ZINF's RAID-1 + CRC32 architecture compare to LittleFS, SPIFFS, and unprotected SD writes in terms of data recovery capability, silent corruption probability, and storage overhead under Single Event Upset injection patterns derived from low-Earth orbit cosmic ray flux models?*

### Methodology

**Phase 1 — SEU model construction (months 1–2)**
- Study published cosmic ray models: **CREME96**, **OMERE**, and **ECSS-E-ST-10-12C** (ESA standard for space radiation effects).
- Derive statistical distributions for: bitflip probability per sector per hour at 300 km LEO altitude, typical bitflip burst size, probability of two independent bitflips in the same sector within one write cycle.
- Parameterize the existing ZINF `ram_driver_corrupt` to inject bitflips according to these distributions (rather than the current pathological 15% rate).

**Phase 2 — Comparative fault injection study (months 2–6)**
- Implement a **LittleFS test harness** equivalent to the ZINF lifecycle test, using the same shadow-buffer verification methodology (every written record tracked, verified after each simulated power cycle).
- Run matched experiments: same PRNG seed, same fault rate, same write workload across ZINF Format v4, LittleFS v2, SPIFFS, and raw SD (no file system).
- Metrics: silent corruption probability, recovery rate, storage overhead, write latency.
- Sweep: fault rates from 0.001% (realistic LEO) to 5% (ZINF test suite worst case).

**Phase 3 — Mission correlation (months 6–10)**
- Cross-reference findings with Mimike-II Rev-I flight data (UV intensity, GNSS anomalies, temperature spikes during ascent through radiation belts).
- Estimate the probability that any stored byte was affected by a cosmic ray event during the Rev-I flight — and verify ZINF's redundancy was sufficient.
- Prepare a methodology document that future CanSat/CubeSat teams can use to select and configure their storage solution.

### Expected Deliverables

| Deliverable | Timeline |
|---|---|
| SEU injection framework (extended, open-source) | Month 3 |
| Comparative benchmark dataset (ZINF vs LittleFS vs SPIFFS vs raw) | Month 6 |
| Mission data correlation analysis (Mimike-II Rev-I) | Month 8 |
| Conference paper | Month 9 |
| TDK paper (PTE) | Month 10 |

### Potential Publication Venues

- **IEEE Transactions on Nuclear Science** (Q1 — SEU characterization section)
- **IAA Symposium on Small Satellites** (annual)
- **ESA ADCSS Workshop** (Avionics, Data, Control and Software Systems)
- **Embedded World Conference** (Nuremberg, annual)

### Resources Needed

- ZINF codebase (available) + LittleFS source (Apache 2.0)
- Mimike-II Rev-I flight data (already collected)
- RP2350 or ATSAMD21G18AU dev board for hardware timing validation
- Optional (Phase 3 extension): access to proton beam or gamma radiation facility via ESA ESAC or Hungarian nuclear research network

### Feasibility

**Low–Medium.** Software simulation and comparative benchmarking are straightforward extensions of existing ZINF infrastructure. The main research contribution is the SEU model parameterization and the systematic comparison methodology — neither requires exotic hardware. Radiation facility access would strengthen the work but is not required for the EKÖP period.

---

## Track 3 — RTOS Integration and Worst-Case Execution Time (WCET) Analysis

### Motivation

ZINF currently assumes single-threaded, exclusive access to `zinf_ctx_t`. Production embedded systems running FreeRTOS or Zephyr RTOS may have multiple tasks competing for storage access — a sensor acquisition task writing every 100 ms, a telemetry task bulk-reading for downlink, and a watchdog task checking integrity. Without a thread-safe wrapper and formal timing guarantees, ZINF cannot be safely used in these architectures. Furthermore, the claim that ZINF's write time is bounded by `T_max = M × T_sector_write` has been stated but not formally validated under real scheduling pressure.

### Research Question

> *What are the experimentally validated worst-case execution time (WCET) bounds for ZINF write operations on bare-metal Cortex-M targets, and can a thread-safe ZINF wrapper for FreeRTOS guarantee bounded write latency under concurrent multi-task sensor-write workloads?*

### Methodology

**Phase 1 — WCET measurement and analysis (months 1–3)**
- Instrument the ZINF portable core with DWT cycle counters on RP2350; measure wall time for `raid_sensor_values()` across 1M+ iterations with:
  - Varying payload sizes (1–4096 sectors)
  - Pre-degraded mirrors (0, 1, M-1 blacklisted)
  - SD card at init speed (400 kHz) vs. full speed (25 MHz)
- Compare measured WCET against the theoretical `T_max = M × T_sector_write` bound.
- Identify sources of non-determinism: SD card FTL jitter, SPI bus contention, CRC lookup table cache effects.

**Phase 2 — Thread-safe wrapper design (months 3–6)**
- Design `zinf_rtos.c` / `zinf_rtos.h`: a mutex-protected wrapper over `zinf_ctx_t` compatible with FreeRTOS `xSemaphoreTake` / `xSemaphoreGive`.
- Implement a write queue (ring buffer) for scenarios where write latency must not block the calling task.
- Priority inheritance analysis: does acquiring the ZINF mutex cause priority inversion? Under what conditions?
- Port to Zephyr RTOS as a secondary target (uses different synchronization primitives but the same design pattern).

**Phase 3 — Formal WCET analysis (months 6–10)**
- Apply an automated WCET analysis tool (**OTAWA** or **AbsInt aiT**) to the ZINF portable core compiled for Cortex-M33 (RP2350).
- Compare tool-derived WCET bounds against empirically measured bounds from Phase 1.
- Document the gap between theoretical, tool-computed, and measured WCET — this is the primary scientific contribution.

### Expected Deliverables

| Deliverable | Timeline |
|---|---|
| WCET characterization dataset for RP2350 (open-source) | Month 3 |
| `zinf_rtos` FreeRTOS wrapper (merged into ZINF repo) | Month 5 |
| Zephyr port | Month 7 |
| WCET analysis tool comparison report | Month 8 |
| Conference paper | Month 9 |

### Potential Publication Venues

- **ECRTS** (Euromicro Conference on Real-Time Systems, A-level venue)
- **RTNS** (Real-Time Networks and Systems)
- **IEEE Embedded Systems Letters** (Q2)
- **FreeRTOS Community** (technical article, broader reach)

### Resources Needed

- RP2350 / Raspberry Pi Pico 2 dev board (available)
- SPI SD card module (available)
- FreeRTOS (MIT license) + Zephyr RTOS (Apache 2.0)
- OTAWA WCET tool (academic license, free)
- Logic analyzer for SPI timing verification

### Feasibility

**Medium.** The instrumentation and wrapper implementation are well within scope for the EKÖP period. The formal WCET tool analysis (Phase 3) is the riskiest component — the tools have steep learning curves and may produce overly conservative bounds. This risk is mitigated by the empirical measurements in Phase 1, which are valuable independently.

---

## Track 4 — Scientific Data Ecosystem (zinf-py)

### Motivation

ZINF stores sensor data in a compact, CRC-protected binary format defined by `zinf.yaml`. Post-mission analysis of CanSat and CubeSat data currently requires either the ZINF Studio GUI or custom C code — both are inaccessible to domain scientists (atmospheric physicists, biomedical researchers, agricultural engineers) who work in Python. Providing a `zinf-py` library would open ZINF's data to the standard scientific Python ecosystem (NumPy, pandas, matplotlib, Jupyter, Zarr) and enable reproducible, citable open datasets — increasingly required by journals and space agencies.

### Research Question

> *How should a schema-driven, FAIR-compliant Python toolchain for ZINF binary data be designed to enable reproducible scientific analysis workflows, and what metadata standards are needed to make ZINF-formatted datasets citeable and interoperable with existing space science data archives?*

### Methodology

**Phase 1 — zinf-py library design and implementation (months 1–4)**
- Implement `zinf-py`: a pure Python library for reading ZINF-formatted image files.
  - Parse metadata sectors (magic, version, copy slots), verify CRC32, decode sensor data according to the `zinf.yaml` schema.
  - Output formats: `pandas.DataFrame`, `numpy.ndarray`, CSV, Parquet (Apache Arrow).
  - Streaming mode: process files larger than available RAM sector-by-sector.
  - Write support: generate valid ZINF-formatted images for testing (without requiring C compilation).
- Publish as a **PyPI package** (`zinf-py`) with full type annotations and pytest test suite.

**Phase 2 — Jupyter integration and reproducible workflow (months 4–7)**
- Develop a **Jupyter notebook case study** using Mimike-II Rev-I flight data:
  - Load raw ZINF image → extract sensor streams → plot temperature, UV intensity, GNSS altitude vs. time → statistical analysis of measurement quality.
  - Demonstrate cross-correlation with external data (e.g., NOAA atmospheric models, ESA space weather data).
- Design and document a **FAIR data packaging standard for ZINF datasets**:
  - `zinf.yaml` as machine-readable metadata
  - Dataset versioning (ZINF Format v4 version field as dataset provenance)
  - ORCID and mission attribution fields
  - Target: compatibility with Zenodo and ESA's Open Space Data Hub

**Phase 3 — Community and publication (months 7–10)**
- Submit `zinf-py` to the **Journal of Open Source Software (JOSS)** — peer review is code-quality focused, achievable within the EKÖP period.
- Present the reproducible data pipeline at a scientific conference.
- Connect with the broader CanSat/CubeSat community (ESA Education, REXUS/BEXUS) to pilot the standard.

### Expected Deliverables

| Deliverable | Timeline |
|---|---|
| `zinf-py` v1.0 on PyPI (MIT license) | Month 4 |
| Mimike-II Rev-I Jupyter notebook case study | Month 6 |
| FAIR data standard draft for ZINF datasets | Month 7 |
| JOSS paper submission | Month 8 |
| TDK paper (PTE) + conference presentation | Month 10 |

### Potential Publication Venues

- **Journal of Open Source Software (JOSS)** — software paper, quick turnaround
- **Scientific Data** (Nature portfolio) — data descriptor paper
- **COSPAR Scientific Assembly** (space research community)
- **pyOpenSci community** (Python scientific software ecosystem)

### Resources Needed

- Mimike-II Rev-I flight data binary images (available)
- Python 3.11+ development environment
- Zenodo account (free, for dataset archival)
- No specialized hardware required

### Feasibility

**Low.** The library implementation is straightforward Python engineering with no hardware dependencies. The main research contribution is the FAIR data standard design, which requires literature review of existing space data standards (CCSDS, FITS, HDF5) and adaptation to ZINF's append-only model. Lowest risk of all four tracks.

---

## EKÖP Deliverables Summary

| Track | Primary deliverable | Publication venue | EKÖP required output |
|---|---|---|---|
| 1 — Formal verification | TLA+ spec + TLC results | IEEE ESL / FMICS | TDK paper + conference |
| 2 — SEU characterization | Comparative benchmark dataset + paper | IEEE TNS / IAA SmallSat | Conference paper + TDK |
| 3 — RTOS / WCET | FreeRTOS wrapper + WCET dataset | ECRTS / RTNS | Conference paper + TDK |
| 4 — zinf-py ecosystem | PyPI package + JOSS paper | JOSS / Scientific Data | Software release + TDK |

All four tracks contribute a **TDK paper** (required for MSc category) and at least one **external conference or journal presentation** (required for both MSc and PhD categories).

---

## Open Scientific Questions

These are the core research questions in proposal-ready form. Each maps to one track.

**Track 1 — Formal verification:**
> "Is the crash-consistency of ZINF Format v4 formally provable, and what is the minimum set of protocol invariants that must hold for safety certification in aerospace applications?"

**Track 2 — SEU hardening:**
> "Does ZINF's RAID-1 architecture provide statistically sufficient protection against Single Event Upset rates characteristic of low-Earth orbit, and how does this compare quantitatively to LittleFS and unprotected flash access?"

**Track 3 — RTOS / WCET:**
> "What are the empirically validated and tool-computed worst-case execution time bounds for ZINF write operations on Cortex-M33, and can these bounds be maintained under concurrent multi-task access in FreeRTOS?"

**Track 4 — Scientific ecosystem:**
> "What design principles for a schema-driven, FAIR-compliant Python data toolchain maximize the reproducibility and interoperability of embedded sensor datasets produced by resource-constrained space platforms?"

---

## Further Directions (Post-EKÖP)

The following directions are noted for completeness but are not proposed for the EKÖP period. They represent a natural PhD continuation or Horizon Europe proposal scope.

- **Heterogeneous multi-type schemas** — multiple different sensor structs in a single ZINF partition, with type dispatch at read time.
- **Transparent compression layer** — LZ4-Embedded or custom delta coding for temporally correlated telemetry (temperature, pressure), reducing storage requirements by 30–60% for slowly changing signals.
- **Energy-aware write scheduling** — defer non-critical writes when battery SoC is below threshold; journal to RAM and flush on charge. Relevant for energy-harvesting IoT nodes.
- **Post-quantum authenticated storage** — CRYSTALS-Kyber or SPHINCS+ signature layer over ZINF sectors for long-lifetime (10+ year) space asset data integrity, when classical CRC32 is insufficient against adversarial models.
- **ESA / ECSS standardization** — propose ZINF Format v4 as a profile under ECSS-E-ST-10C (Space Engineering — Testing) for small satellite data recorders.
- **FPGA co-processor** — offload CRC32 computation and mirror writes to a dedicated hardware accelerator (e.g., on RP2350's PIO or an external iCE40), enabling write rates limited only by bus bandwidth.
