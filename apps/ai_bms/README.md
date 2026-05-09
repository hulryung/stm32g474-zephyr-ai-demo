# ai_bms

Battery-cycle anomaly detection on Cortex-M4. The autoencoder is trained on
**real Li-ion discharge curves** from the NASA PCoE Battery Dataset
(B0005, B0006, B0007 healthy cycles) and successfully detects degradation in
a **fourth cell (B0018) it never saw during training**.

This is the BMS-specific specialization of the `ai_anomaly` foundation
pattern: same autoencoder architecture, but trained on real battery cycle
data and demonstrating cross-cell generalization.

## Measured on this board (B0018, holdout cell)

| Cycle | Capacity (Ah) | Health      | Score       | Verdict  |
|-------|---------------|-------------|-------------|----------|
| early | 1.855         | full        | 0.00300     | normal   |
| mid   | 1.522         | ~76% SOH    | 0.01974     | **★ ANOMALY** (~7×)  |
| aged  | 1.341         | ~67% SOH    | 0.02623     | **★ ANOMALY** (~9×)  |

Inference: **157 µs / cycle**, ~6,400 inferences/sec.

The model was never shown a single B0018 cycle during training — yet it
correctly flags the worn-down cycles. That is the test of whether the AE
learned the *generic shape* of a healthy discharge curve, not just memorized
the training cells.

## ⚠ Important — what this is NOT

This is a research/educational demo of an *augmenting* anomaly detection
layer. **It is not a substitute for hard-rule overvoltage / overcurrent /
overtemperature protection** on a production BMS. Production BMS
architecture is rules-first (UL 1973 / IEC 62619 certified), with ML
running alongside as an early-warning trend detector. Do not deploy this
file as your only safety check.

## Build & flash

Requires the Zephyr `tflite-micro` optional module (same as `ai_sine` and
`ai_anomaly`). Enable once per workspace:

```bash
west config manifest.group-filter -- "+optional"
west update --narrow -o=--depth=1 tflite-micro
```

### 1. Get the dataset

NASA PCoE Battery Dataset is a public dataset hosted by NASA. Download and
extract:

```bash
cd apps/ai_bms/train
mkdir -p data && cd data
curl -fL -o nasa.zip \
  "https://phm-datasets.s3.amazonaws.com/NASA/5.+Battery+Data+Set.zip"
unzip nasa.zip
unzip "5. Battery Data Set/1. BatteryAgingARC-FY08Q4.zip" \
      -d "5. Battery Data Set/"
```

After this, `apps/ai_bms/train/data/5. Battery Data Set/B000{5,6,7,18}.mat`
should exist. The raw data is **gitignored** — every developer downloads
their own copy.

### 2. Train the model + generate C sources

```bash
.venv-ml/bin/python apps/ai_bms/train/train.py
```

This re-emits `src/model.cpp` and `src/cycles_db.cpp`. Re-run any time you
change the architecture, capacity thresholds, or the chosen sample cycles.

### 3. Build + flash

```bash
west build -p auto -b nucleo_g474re apps/ai_bms -d build/ai_bms
west flash -r openocd -d build/ai_bms
```

Or use the VSCode tasks **`Zephyr: Build (ai_bms)`** /
**`Zephyr: Flash (ai_bms / openocd)`**.

## Try it from the shell

```
g474> bms list
name    cell    idx   capacity
early   B0018   0     1.8550 Ah
mid     B0018   66    1.5223 Ah
aged    B0018   131   1.3411 Ah

g474> bms scan
Scanning all embedded cycles (threshold=0.00500):
early   cap=1.855 Ah  score=0.00300
mid     cap=1.522 Ah  score=0.01974 ★ ANOMALY
aged    cap=1.341 Ah  score=0.02623 ★ ANOMALY

g474> bms cycle aged
aged    cap=1.341 Ah  score=0.02623 ★ ANOMALY

g474> bms bench 1000
iterations : 1000
avg        : 26670 cycles  (156.882 us)
min        : 26659 cycles  (156.817 us)
max        : 28353 cycles  (166.782 us)
throughput : ~6374 inf/sec
```

## Commands

| Command                          | What it does                                |
|----------------------------------|---------------------------------------------|
| `bms list`                       | List embedded sample cycles                 |
| `bms info`                       | Model info, arena size, threshold           |
| `bms cycle <name>`               | Score one cycle (`early` / `mid` / `aged`)  |
| `bms scan`                       | Score all embedded cycles                   |
| `bms bench [iterations]`         | Inference latency benchmark                 |
| `bms threshold [value]`          | Show or set anomaly threshold (advisory)    |

## How the data is processed

The training pipeline (in `train/train.py`) does the following:

1. **Load** all four .mat files (B0005/6/7/18) using `scipy.io.loadmat`.
2. **Filter** to discharge cycles only (each cell has 168 of them).
3. **Resample** the discharge voltage curve to 32 points uniformly across
   normalized time (so curves of different lengths/capacities become
   directly comparable).
4. **Normalize** to [-1, +1] using a fixed voltage range
   (`V_MIN=2.4, V_MAX=4.3`), matching the input range that the int8
   quantizer expects.
5. **Split** training-cell cycles by capacity:
   - `cap > 1.75 Ah` → "normal" (used to train the AE)
   - `cap < 1.50 Ah` → "anomaly" (used only for evaluation)
6. **Train** a tiny dense AE: `Dense(24→12→4→12→24→32)`, ReLU activations,
   80 epochs, MSE loss, Adam(1e-3).
7. **Quantize** to int8 with a representative dataset of healthy curves.
8. **Bake** three representative cycles from B0018 (the holdout cell) into
   the firmware: `early` (cycle 0), `mid` (cycle 66), `aged` (cycle 131).

## Why this generalizes

The autoencoder doesn't memorize specific cells — it learns the
**reconstructable shape** of a healthy Li-ion discharge curve: smooth
decline from ~4.2V, knee around 3.6V, sharper drop near cutoff. Aged cells
have a slightly different shape:

- Less time spent in the upper plateau (capacity loss visible as lower mAh
  delivered before voltage drop)
- Steeper internal-resistance-driven voltage drops at high current
- Shifted knee position

These deviations cause the AE to mis-reconstruct the curve, and the
mean squared error (MSE) goes up. That MSE is the score.

## File layout

```
apps/ai_bms/
├── CMakeLists.txt
├── prj.conf
├── README.md
├── train/
│   ├── train.py             # data loading, preprocessing, training, export
│   ├── data/                # gitignored — user downloads NASA .mat files here
│   └── model.tflite         # generated, gitignored
└── src/
    ├── main.c               # boot + heartbeat
    ├── cmd_bms.c            # `bms list|info|cycle|scan|bench|threshold`
    ├── tflm_bms.h           # extern "C" inference API
    ├── tflm_bms.cpp         # C++ TFLM glue
    ├── model.cpp / .hpp     # generated by train.py — TFLM model bytes
    └── cycles_db.cpp / .h   # generated by train.py — embedded sample cycles
```

## Memory footprint

| Region | Used   | Region | %     |
|--------|--------|--------|-------|
| FLASH  | 122 KB | 512 KB | 23 %  |
| RAM    |  23 KB | 128 KB | 17 %  |

## Next steps for a real BMS application

1. **Replace synthetic on-device cycles with real cell measurements.** Wire
   up an analog front-end (cell voltage ADC, current shunt amplifier,
   thermistor) and feed live discharge curves to `tflm_bms_score()`.
2. **Multi-channel input.** Train on a vector of [voltage_curve,
   temperature_curve, current_curve] concatenated — then a single anomaly
   covers electrical and thermal failure modes.
3. **Multi-cell.** Score each cell of a pack separately; flag any cell
   whose score diverges from siblings even if all are individually under
   threshold.
4. **Real labels.** Once you have ground-truth fault data, switch to
   supervised classification or hybrid (AE for unknown faults, classifier
   for known ones).

Each of these is a real project on its own. The pattern in this app — load
real public data, train AE, quantize, deploy, score from shell — is the
template you'd reuse.

## References

- NASA PCoE Battery Dataset: <https://www.nasa.gov/intelligent-systems-division/discovery-and-systems-health/pcoe/pcoe-data-set-repository/>
- Direct download (used here): <https://phm-datasets.s3.amazonaws.com/NASA/5.+Battery+Data+Set.zip>
- Saha & Goebel (2007), "Battery Data Set", NASA Ames Prognostics Data Repository
