# ai_bms_rul

Battery **Remaining Useful Life (RUL) prediction** on Cortex-M4 — a
*prognostic* problem: from one discharge curve, predict **how many cycles
the cell has left** before its capacity drops below an end-of-life
threshold.

This is the third complementary view of the same NASA PCoE data:

| App | Question it answers | ML problem |
|-----|---------------------|------------|
| `apps/ai_bms`     | "Is the current cycle anomalous?"        | Anomaly (AE)        |
| `apps/ai_bms_soh` | "What's the cell's current capacity?"   | Regression          |
| `apps/ai_bms_rul` | **"How many cycles until EOL?"**         | Prognostic (regression) |

A production BMS would run all three: anomaly for unknown failure modes,
SOH for the user-facing range estimate, RUL for warranty / maintenance
scheduling.

## Measured on this board (B0018, holdout cell)

EOL definition: capacity < 1.5 Ah (chosen so all 4 NASA cells reach EOL).

| Cycle | Capacity | True RUL | Predicted RUL | Error          |
|-------|----------|----------|---------------|----------------|
| early | 1.855 Ah | 69 cyc   | 47 cyc        | **-22** cycles |
| mid   | 1.522 Ah | 3 cyc    | 16 cyc        | **+13** cycles |
| aged  | 1.341 Ah | 0 cyc    | 2 cyc         | **+2** cycles  |

MAE on holdout = **12 cycles**. Notice the model is most accurate near
EOL — when the discharge curve clearly shows degradation. At early life,
when shape changes are subtle, predictions are noisier (the early-life
−22 cycle error reflects this).

Inference: **140 µs / prediction**, ~7,150 inferences/sec.

### Honest assessment

RUL prediction is the hardest of the three problems and the limits show:

- **Tiny training set**: 504 cycles across 3 cells is too few for robust
  RUL learning. Industry uses 100+ cells (e.g. Severson 2019 with 124
  LFP cells got ~9% RMSE — still hard).
- **Non-monotonic capacity**: real cells have measurement noise + recovery
  effects, so RUL labels jitter. The model can't be more precise than
  the labels.
- **Single-cycle input**: best practice is to use *trends across multiple
  cycles* (capacity slope, internal resistance trend) — far more
  informative than one curve in isolation.

The aged cycle's near-perfect prediction (+2 cycles error vs true RUL=0)
is the encouraging result: when the curve unmistakably shows EOL, the
model agrees. That's the bare minimum for a prognostic to be useful.

## Build & flash

Same setup as ai_bms / ai_bms_soh. Reuses NASA data already present.

```bash
.venv-ml/bin/python apps/ai_bms_rul/train/train.py
west build -p auto -b nucleo_g474re apps/ai_bms_rul -d build/ai_bms_rul
west flash -r openocd -d build/ai_bms_rul
```

VSCode tasks: `Zephyr: Build (ai_bms_rul)`,
`Zephyr: Train ai_bms_rul model`, `Zephyr: Flash (ai_bms_rul / openocd)`.

## Try it

```
g474> rul list
name    cell    idx   capacity   true RUL
early   B0018   0     1.8550 Ah  69 cycles
mid     B0018   66    1.5223 Ah  3 cycles
aged    B0018   131   1.3411 Ah  0 cycles

g474> rul scan
RUL prediction across all embedded cycles:
early   cap=1.855 Ah  true_RUL=69   predicted_RUL=47   error=-22 cycles
mid     cap=1.522 Ah  true_RUL=3    predicted_RUL=16   error=+13 cycles
aged    cap=1.341 Ah  true_RUL=0    predicted_RUL=2    error=+2 cycles
MAE: 12 cycles (over 3 samples)
```

## Commands

| Command                       | What it does                          |
|-------------------------------|---------------------------------------|
| `rul list`                    | List embedded cycles + ground-truth RUL |
| `rul info`                    | Model + arena info                    |
| `rul predict <cycle>`         | Predict RUL for one cycle             |
| `rul scan`                    | Predict for all + MAE                 |
| `rul bench [iterations]`      | Inference latency benchmark           |

## Data labeling — how RUL is computed

For each cell, find the cycle index where capacity first drops below the
EOL threshold (1.5 Ah). Call it `eol_idx`. Then for cycle `i`:

```
RUL(i) = max(eol_idx - i, 0)
```

So the very first cycle has the largest RUL; once the cell crosses below
1.5 Ah, RUL stays at 0 for the rest of the recorded data.

NASA EOL across the 4 cells (using 1.5 Ah threshold):

| Cell  | Total cycles | EOL @ |
|-------|--------------|-------|
| B0005 | 168          | 98    |
| B0006 | 168          | 75    |
| B0007 | 168          | 125   |
| B0018 | 132          | 69    |

## Memory footprint

| Region | Used   | %     |
|--------|--------|-------|
| FLASH  | 120 KB | 23 %  |
| RAM    | 23 KB  | 17 %  |

## Improvements for real prognostic systems

1. **Multi-cycle features**: feed the past N cycles' capacity *trend*, not
   just one curve. Capacity slope is much more predictive than a single
   point estimate.
2. **Internal resistance**: derive Rs from the IR drop at discharge start;
   it grows as the cell ages, often more reliably than capacity alone.
3. **Confidence intervals**: a Bayesian or quantile-regression head gives
   "RUL = 47 ± 12 cycles" instead of a single number — much more useful
   for maintenance planning.
4. **Larger dataset**: Severson 2019 (124 LFP cells, ~5 GB) is the
   standard for serious RUL benchmarks. Adapting this app to use it is a
   future task in `docs/roadmap.md`.

## File layout

```
apps/ai_bms_rul/
├── CMakeLists.txt
├── prj.conf
├── README.md
├── train/
│   └── train.py             # data + RUL labeling + training + export
└── src/
    ├── main.c
    ├── cmd_rul.c            # `rul list|info|predict|scan|bench`
    ├── tflm_rul.h / .cpp    # C++ TFLM wrapper
    ├── model.cpp / .hpp     # generated by train.py
    └── cycles_db.cpp / .h   # generated — embedded cycles + true RUL
```
