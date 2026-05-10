# ai_bms_safety

**Layer 1 / Layer 2 BMS architecture — hard rules + ML advisory, in
separate threads.**

The hard rule that holds production BMS firmware together:

> ML never opens the contactor. Hard rules do. Nothing else.

This app shows the architecture: two Zephyr threads at very different
priorities, sharing only a small lock-protected snapshot of cell state.
Six injectable scenarios let you see exactly which conditions trigger
hard trips vs. ML advisories.

## Architecture

```
                ┌─────────────────────────────────────┐
                │ Shared cell snapshot (spinlocked)   │
                │  v[N], i_pack, t_max, timestamp     │
                └──────────┬──────────────────────────┘
                           │
   ┌───────────────────────┼───────────────────────────┐
   │                       │                           │
   ▼ priority 2 (high)     ▼ priority 10 (low)         ▼ shell (10)
┌────────────────┐   ┌────────────────────┐   ┌────────────────────┐
│ safety thread  │   │ ML advisory thread │   │ test command tree  │
│ 100 Hz         │   │ 10 Hz              │   │ inject scenarios,  │
│ HARD RULES:    │   │ ADVISORY ONLY:     │   │ read flags         │
│  trip_ov       │   │  imbalance         │   │                    │
│  trip_uv       │   │  thermal trend     │   │                    │
│  trip_oc       │   │  anomaly score     │   │                    │
│  trip_ot       │   │                    │   │                    │
│ → contactor    │   │ → log only         │   │                    │
└────────────────┘   └────────────────────┘   └────────────────────┘
```

Priority gap (2 → 10) is the keystone:
- Safety thread can preempt ML at any time.
- ML can never starve safety.
- If ML crashes, safety keeps running (separate stack, separate logic).

## Measured on the live board

After the demo runs, both threads have been ticking; the iter counter
shows their relative cadence:

```
g474> safety status
Iters: safety=412, ml=42  (separation = 10×)
```

Exactly as designed: 100 Hz / 10 Hz = 10×.

### Scenario 2: mild cell imbalance — advisory only

```
g474> safety scenario 2
Scenario 2: mild cell imbalance (cell 3 sagging) — advisory only

g474> safety status
Layer 1 (HARD RULES):
  trip_ov : no    trip_uv : no    trip_oc : no    trip_ot : no
Layer 2 (ML ADVISORY):
  advisory_imbalance : YES   advisory_anomaly : YES
  anomaly_score      : 0.2800
```

Cell 3 sagging by ~70 mV is well within hard-rule tolerance, but the ML
layer flags it. **Layer 1 untouched.** A real BMS would log this and
maybe schedule a balance-charge cycle but not interrupt operation.

### Scenario 4: cell over-voltage — HARD TRIP

```
g474> safety scenario 4
Scenario 4: cell 2 over-voltage 4.31 V — HARD TRIP expected

g474> safety status
Layer 1 (HARD RULES):
  trip_ov : YES ★    ← contactor would open
Layer 2 (ML ADVISORY):
  advisory_anomaly : YES   anomaly_score : 2.2600
```

Same cell-2 voltage triggers both layers, but the **hard trip is what
actually opens the contactor**. ML's 2.26 score is just a corroboration
in the log.

### Scenario 6: trend rising — advisory ahead of trip

```
g474> safety scenario 6
Scenario 6: trend rising — ML advisory but no hard trip yet

g474> safety status
Layer 1: all clear
Layer 2:
  advisory_imbalance : YES   advisory_anomaly : YES   score : 1.95
```

T = 52 °C, V at 4.20 V, mild imbalance — none of these alone trips a
hard rule, but together they're suspicious. ML flags it ~30 s before
the hard rule would catch it. **This is the actual product value of
the ML layer**: early warning while there's still time to act.

## Six built-in scenarios

| # | Description | Layer 1 expected | Layer 2 expected |
|---|---|---|---|
| 1 | healthy operation | clear | clear |
| 2 | mild cell imbalance | clear | imbalance + anomaly |
| 3 | thermal rise under heavy current | clear | thermal_trend + anomaly |
| 4 | cell-2 over-voltage 4.31 V | trip_ov | imbalance + anomaly |
| 5 | pack over-current 180 A | trip_oc | anomaly |
| 6 | combined trend rising | clear | imbalance + anomaly |

## Shell commands

| Command | What it does |
|---------|--------------|
| `cell sim <c0..c3> <i> <t_max>`   | Set the simulated snapshot |
| `cell show`                       | Read it back |
| `safety status`                   | Both threads' flags + iter counters |
| `safety reset`                    | Clear all flags (sticky after trip until reset) |
| `safety scenario <1..6>`          | Run a canned scenario |

## Hard-rule thresholds (UL/IEC equivalent)

```c
#define TRIP_V_OVER   4.25f   /* per-cell over-voltage */
#define TRIP_V_UNDER  2.50f   /* per-cell under-voltage */
#define TRIP_I_OVER   150.0f  /* pack over-current (|A|) */
#define TRIP_T_OVER   65.0f   /* over-temperature (°C) */
```

Each is a single-comparison hard rule. **No probabilities, no scores, no
neural networks.** This is what gets certified.

## Footprint

| Region | Used   | %     |
|--------|--------|-------|
| FLASH  | 69 KB  | 13 %  |
| RAM    | 24 KB  | 18 %  |

## Production extensions

Real safety architecture adds:
1. **Hardware watchdog** monitors safety thread heartbeat — if safety
   doesn't pet the dog within e.g. 50 ms, MCU resets.
2. **MPU-isolated stacks** — Zephyr supports running ML thread in
   user mode (G4 has MPU). ML crash (stack overflow, hard fault) gets
   isolated to its own region.
3. **CCMRAM for safety code** — STM32G4 has 32 KB CCMRAM. Putting the
   safety thread + lookup tables there gives deterministic, no-cache-
   miss execution.
4. **Independent voltage measurement** — production BMS reads cell V via
   the AFE (LTC6811/BQ76952) AND a backup ADC path, compares. If they
   disagree, hard trip on most-conservative reading.
5. **Trip latching** — once tripped, BMS refuses to re-engage contactor
   until operator clears the fault and confirms (UL 1973 requirement).
