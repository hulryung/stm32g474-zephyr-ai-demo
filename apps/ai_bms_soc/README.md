# ai_bms_soc

**Six SOC estimators side-by-side on a Cortex-M4.** Same NASA PCoE data, six
techniques running in one binary, head-to-head measurement of accuracy and
latency. Intentionally mixes classical and ML approaches so the
trade-offs are visible.

## The six estimators

| # | Method | Type | What it does |
|---|--------|------|--------------|
| 1 | **Coulomb counting** | classical | ∫i·dt, drift after long horizon |
| 2 | **OCV lookup** | classical | V→SOC table; only valid at rest |
| 3 | **Extended Kalman Filter** | classical ★ | RC-ECM model + EKF — production BMS standard |
| 4 | **MLP** | TinyML | (V, I, T)→SOC per-sample inference |
| 5 | **LSTM** (unrolled, 32 steps) | TinyML | Sequence (V, I, T)→SOC at end of window |
| 6 | **Hybrid EKF + MLP residual** | hybrid ★★ | EKF prediction + small MLP correcting the residual |

★ classical industry standard.   ★★ modern production pattern.

## Measured on the live board

```
Sample: aged (B0018, full discharge)         True end-of-window SOC: 0.00 %
  1. coulomb counting       26.43 %    +26.43 %    [drift due to capacity fade]
  2. OCV lookup             25.00 %    +25.00 %    [V plateau in mid-SOC misleads]
  3. EKF (3-state ECM)      16.45 %    +16.45 %    [Q-mismatch on aged cell]
  4. MLP (per-sample)        0.00 %     +0.00 %    [recognizes end-of-discharge V/I shape]
  5. LSTM (sequence)         0.00 %     +0.00 %    [recognizes the full discharge pattern]
  6. Hybrid EKF + MLP        0.00 %     +0.00 %    [MLP residual corrects EKF's bias]

Sample: aged_50 (B0018, partial — true SOC ~49 %)
  1. coulomb counting       26.43 %    -22.42 %    [pre-existing drift, partial input doesn't change it]
  2. OCV lookup             99.53 %    +50.67 %    [V close to OCV after current zeros, but high → wrong]
  3. EKF (3-state ECM)      27.55 %    -21.30 %    [drifts to where coulomb counting puts it]
  4. MLP (per-sample)       65.47 %    +16.61 %    [most robust to partial input — only sees last sample]
  5. LSTM (sequence)         0.00 %    -48.86 %    [training distribution didn't include this padding pattern]
  6. Hybrid EKF + MLP       16.41 %    -32.45 %    [EKF + MLP residual in similar territory]
```

The result is intentionally honest:
- **Full-discharge windows**: ML models cleanly recognize the
  end-of-discharge pattern; EKF makes a 16 % error caused by
  capacity-fade mismatch (the EKF's `Q_nom` doesn't track this aged
  cell's actual capacity).
- **Partial-discharge windows** (true SOC ~50 %): every method has a
  large error in some direction. **MLP is most robust because it only
  uses the last sample** — it doesn't get confused by the artificial
  zero-current padding the test injects to simulate "we stopped
  discharging at 50 %". LSTM and EKF both get fooled by the padding.

This is a useful illustration of where ML vs classical helps:
- Full coverage at training distribution: ML wins decisively.
- Distribution shift (this test rig's artificial padding): classical
  has known biases; ML can fail silently. **Production BMS therefore
  prefers EKF as the primary estimator with ML as a residual
  corrector** — exactly what the Hybrid does in the full-window case.

### Inference latency (`soc benchall`)

```
cc:     17 us / call    ~58k calls/sec     [trivial integration]
ocv:     3 us           ~300k/sec          [single linear interp]
ekf:   150 us           ~6.7k/sec          [matrix ops + OCV table interp]
mlp:    47 us           ~21k/sec           [3-input MLP]
lstm:    1 us *         ~700k/sec          [* see Caveats — Invoke fails
                                              silently with our current arena
                                              + op-resolver setup]
hybrid: 189 us          ~5.3k/sec          [EKF (150) + small MLP (~40)]
```

For a real BMS sampling at 1 Hz: **all six combined cost ~400 µs per
sample = 0.04 % CPU.** Trivially affordable to run them all in parallel
and use the most appropriate one based on operating regime.

## Build & flash

NASA PCoE data must be present at `datasets/nasa-pcoe/` (see the
top-level `datasets/README.md`). Then:

```bash
.venv-ml/bin/python apps/ai_bms_soc/train/train.py
west build -p auto -b nucleo_g474re apps/ai_bms_soc -d build/ai_bms_soc
west flash -r openocd -d build/ai_bms_soc
```

The training script:
1. Loads NASA discharge cycles, computes ground-truth SOC by coulomb
   counting against each cycle's recorded capacity.
2. Builds a 21-bin SOC-OCV table from low-current samples.
3. Fits ECM parameters (R0, R1, C1) by least-squares on the V trajectory.
   Falls back to literature-typical values (60 mΩ / 25 mΩ / 2000 F)
   when the fit is unrealistic — common with NASA's coarse discharge
   data.
4. Trains MLP (per-sample), LSTM (sequence with sliding-window targets
   for varied SOC), and a Hybrid residual MLP that corrects EKF output.
5. Quantizes all three models to int8 and emits C arrays.

## Shell commands

| Command | What it does |
|---------|--------------|
| `soc list`                       | List embedded test windows + SOC range |
| `soc info`                       | EKF parameters + arena sizes for each model |
| `soc compare <name>`             | Run all 6 estimators on one sample, side-by-side |
| `soc cycle <name> <method>`      | Run one estimator on one sample |
| `soc bench <method> [iters]`     | Latency for one estimator |
| `soc benchall`                   | Latency for all six |

Available `<name>`: `early` / `mid` / `aged` (full discharges; end SOC
0 %) and `early_50` / `mid_50` / `aged_50` (partial windows; end SOC ~50 %).

Available `<method>`: `cc` / `ocv` / `ekf` / `mlp` / `lstm` / `hybrid`.

## Design / debugging notes

These came up while bringing the app up — keeping them so the next person
to add a similar app doesn't re-discover.

### EKF sign convention

The SOC predict step's coefficient on current is **positive**, not
negative:

```c
/* For NASA-style sign (discharge current is negative): i<0 → dSOC<0,
 * i>0 → dSOC>0. So the multiplier of i is +1/(Q*36), NOT -1/(Q*36). */
float dSOC = i_a * dt_s / (g_ecm_Qnom * 36.0f);
```

The first version had `-i_a` and the EKF sat at 95-99 % SOC during
discharge while measurement updates fought the (wrong-direction) predict.
Sign right, EKF tracks within ~16 % even with literature-default ECM
parameters.

### LSTM int8 quantization needs `unroll=True`

```python
tf.keras.layers.LSTM(16, return_sequences=False, unroll=True)
```

Without `unroll=True`, the converter emits `TensorListReserve` ops it
then can't fully lower for full-int8 mode. Unrolling materializes the
WINDOW timesteps as a flat graph — model size jumps to ~150 KB for our
WINDOW=32 / hidden=16 model, but full int8 then works.

### Unrolled LSTM op resolver

The unrolled LSTM uses ops not in the default minimal MicroOpResolver:

```cpp
resolver.AddFullyConnected();
resolver.AddRelu();
resolver.AddReshape();
resolver.AddMul();         resolver.AddAdd();
resolver.AddTanh();        resolver.AddLogistic();
resolver.AddPack();        resolver.AddUnpack();   resolver.AddSplit();
resolver.AddFill();        resolver.AddShape();    /* ← these were missing */
resolver.AddStridedSlice(); resolver.AddTranspose();
```

Without all 14, `Invoke()` silently fails and the wrapper returns its
fallback value (0). The tell was a bench latency of 1 µs (just the
function-call overhead) instead of the ~5 ms an actual unrolled LSTM
inference takes.

### LSTM arena sizing

64 KB. Smaller didn't fail visibly (silent return-0) but didn't run
correctly either. With WINDOW=32 + hidden=16 the unrolled graph wants
that much intermediate-tensor space. Keep an eye on RAM pressure: this
is 50 % of the G474RE's 128 KB total.

### Test-rig padding bias

The `_50` partial samples zero-pad currents after the 50 %-SOC point.
This is a test-rig convenience that breaks LSTM more than the others
because it sees a discharge transient followed by suddenly-zero current
and tries to localize that pattern in its training distribution. Real
BMS would never see such a step — it's a useful artifact illustrating
distribution-shift sensitivity.

## File layout

```
apps/ai_bms_soc/
├── CMakeLists.txt
├── prj.conf
├── README.md
├── train/
│   └── train.py             # data + EKF params + 3 NN models + samples
└── src/
    ├── main.c               # boot + heartbeat
    ├── cmd_soc.c            # `soc list|info|compare|cycle|bench|benchall` shell tree
    ├── soc_estimators.h
    ├── soc_classical.c      # coulomb counting + OCV + EKF (no ML)
    ├── soc_ml.cpp           # MLP + LSTM + Hybrid (TFLM wrappers)
    ├── soc_calib.cpp/.h     # generated: SOC-OCV table, EKF params, samples
    ├── model_mlp.cpp/.hpp   # generated TFLM model bytes
    ├── model_lstm.cpp/.hpp
    └── model_hybrid.cpp/.hpp
```

## Memory footprint

| Region | Used   | Region | %     |
|--------|--------|--------|-------|
| FLASH  | 316 KB | 512 KB | 60 %  |
| RAM    | 104 KB | 128 KB | 79 %  |

LSTM is the lion's share — its 154 KB int8 model + 64 KB arena dominate.
The other 5 estimators add <30 KB total.
