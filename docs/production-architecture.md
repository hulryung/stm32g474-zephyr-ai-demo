# From demo algorithms to a real BMS

How the four `ai_bms_*` "production-pattern" apps in this repo map onto a
real battery management system, and what's still missing.

> **Read this if** you've gone through `ai_bms`, `ai_bms_soh`,
> `ai_bms_rul`, `ai_bms_soc` and want to understand how those algorithms
> would be wired into actual deployed firmware.

## The full picture

A production BMS pulls data from analog front-end ICs at 1 kHz, runs
hard-rule safety in microsecond budgets, performs SOC/SOH/RUL estimation
at 10–100 Hz, persists state across reboots, and survives 5–10 years of
duty cycles. None of that is in the algorithm-only demos.

These four apps each take one slice of that production stack and
demonstrate it on the live STM32G474RE:

| Slice | Demo app | What it shows |
|---|---|---|
| **Capacity tracking** | `apps/ai_bms_dual_ekf` | Slow EKF updates Q every full cycle; SOC EKF stays calibrated as the cell ages |
| **State persistence** | `apps/ai_bms_persistence` | Real NVS save/restore + rest-time-driven OCV recalibration policy |
| **Safety architecture** | `apps/ai_bms_safety` | Two Zephyr threads at very different priorities — hard rules separate from ML |
| **Live data pipeline** | `apps/ai_bms_live` | DAC→ADC loop driving a real 1 kHz sampler + ring buffer + worker thread |

## How they compose

A real product would combine all four (plus the algorithm apps) into a
single firmware:

```
                         ┌───────────────────────────────────────────┐
                         │ Layer 1 — safety thread, priority 2        │
                         │ from apps/ai_bms_safety                    │
                         │ - hard rules (OV/UV/OC/OT)                 │
                         │ - opens contactor, latches faults          │
                         │ - hardware watchdog monitors heartbeat     │
                         └───────────────┬───────────────────────────┘
                                         │ owns the safety GPIO,
                                         │ never blocks
                                         │
                         ┌───────────────▼───────────────────────────┐
                         │ Layer 2 — sampler thread, priority 4       │
                         │ from apps/ai_bms_live                      │
                         │ - 1 kHz ADC reads, DMA-driven in real HW   │
                         │ - ring_buf to worker                        │
                         └───────────────┬───────────────────────────┘
                                         │
                         ┌───────────────▼───────────────────────────┐
                         │ Layer 3 — SOC worker thread, priority 8    │
                         │ from apps/ai_bms_dual_ekf                  │
                         │ - 100 Hz: dual_ekf_step() per cell         │
                         │ - 1 Hz: ML model (ai_bms / ai_bms_soh)     │
                         │ - publishes SOC, Q, SOH                    │
                         └───────────────┬───────────────────────────┘
                                         │
                         ┌───────────────▼───────────────────────────┐
                         │ Layer 4 — ML advisory thread, priority 10 │
                         │ from apps/ai_bms_safety                    │
                         │ - reads SOC + raw V/I/T snapshot          │
                         │ - imbalance + thermal trend + anomaly     │
                         │ - LOG ONLY                                 │
                         └────────────────────────────────────────────┘
                                         │
                         ┌───────────────▼───────────────────────────┐
                         │ Layer 5 — persistence task                 │
                         │ from apps/ai_bms_persistence               │
                         │ - save state every 60 s + on key-off       │
                         │ - restore + OCV recalibrate at boot        │
                         └────────────────────────────────────────────┘
```

## What's still demo-only

Each of the four "production pattern" apps stops short of full
production-grade in specific ways. The next step on each:

### `ai_bms_dual_ekf`
- Replays pre-recorded NASA cycles instead of live data → wire to
  `apps/ai_bms_live`'s ring buffer.
- Discharge-only ground truth → also observe Ah _into_ the cell during
  charge (NASA charge curves available, just not used here).
- Single-cell → loop over 16 cells, one `struct dual_ekf` per cell.

### `ai_bms_persistence`
- Rest time comes from shell command → use STM32G4's RTC running off
  VBAT to compute true wall-clock rest at boot.
- Save on shell command only → save on every key-off interrupt + every
  60 s while running.
- No P-matrix decay during long rest → multiply saved P₀₀ by 1.5× at
  boot if rest > 1 hour.

### `ai_bms_safety`
- Cell snapshot fed by shell → fed by ADC sampler in production.
- No real contactor GPIO → wire `trip_*` flags to a GPIO that drives an
  external solid-state relay.
- No watchdog → enable IWDG at boot, pet from safety thread only.

### `ai_bms_live`
- Toy IIR + linear SOC → call `dual_ekf_step()` from worker_loop.
- DAC simulates cell voltage → wire SPI to an LTC6811 / BQ76952
  analog front-end IC and read real cell voltages.
- Single channel → multi-channel ADC scan, 16 cells × V + 8 × T.

Each of these is a 1–3 day extension. None changes the architecture —
only adds more channels / drivers / accessors on top of the patterns the
demos establish.

## Memory budget if you compose all of them

Rough sum of the largest 2 in each layer:

| Region | Used | Headroom |
|---|---|---|
| FLASH | ~400 KB | 100 KB |
| RAM | ~80 KB | 50 KB |

Most of the FLASH is ML model weight (TFLM) + Zephyr+TFLM runtime. If
you only deploy a subset (e.g. EKF + Safety + Persistence, no ML), you
fit comfortably in 200 KB FLASH and 30 KB RAM.

## Certification considerations

The architecture in these demos is deliberately structured to **make
certification easier**:

- **Layer 1 (safety)** is small, deterministic, no probabilistic
  decisions. Single-comparison hard rules. Auditable line-by-line.
  This is the part that goes through UL 1973 / IEC 62619 / ISO 26262.
- **Layer 4 (ML advisory)** is logged but does not influence
  Layer 1's contactor behavior. Out of scope for the safety
  certification (advisory only).
- **Layer 5 (persistence)** uses standard NVS / settings subsystem,
  versioned schema, fail-safe wear leveling.

Production-grade certification beyond this means:
- Memory protection unit (MPU) enforcing thread separation.
- Independent voltage measurement path (AFE + backup ADC, compared).
- Hardware watchdog with separate clock domain.
- FMEDA documentation linking each fault mode to the rule that catches
  it.

The demos here aren't certified. They're the **architectural skeleton**
that survives UL review.

## See also

- [`docs/ai-on-g474re.md`](ai-on-g474re.md) — TinyML feasibility report
- [`docs/experiment-results.md`](experiment-results.md) — live-board
  measurements for the algorithm apps
- [`docs/dataset-exploration.md`](dataset-exploration.md) — battery
  dataset audit
- [`docs/roadmap.md`](roadmap.md) — what comes next
