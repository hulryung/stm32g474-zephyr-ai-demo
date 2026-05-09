# ai_anomaly

Autoencoder-based anomaly detection on Cortex-M4. The board synthesizes a
"normal" periodic signal continuously; an int8-quantized autoencoder scores
how well it reconstructs each window. Reconstruction error stays low for
genuine normal windows and **spikes 20–200× higher** when you corrupt the
signal via shell commands.

## Measured on this board

| Condition          | Score (avg) | Ratio vs normal |
|--------------------|-------------|-----------------|
| Normal             | 0.0012      | 1×              |
| Pulse injection 0.6| 0.024       | **20×**         |
| Drift  injection 0.5| 0.258      | **215×**        |
| Inference latency  | **130 µs**  | (~7,650 inf/sec)|

## Build & flash

Requires the optional west group (TFLM source). Enable once:

```bash
west config manifest.group-filter -- "+optional"
west update --narrow -o=--depth=1 tflite-micro
```

Train the model and emit `src/model.cpp`:

```bash
.venv-ml/bin/python apps/ai_anomaly/train/train.py
```

(Re-run any time you change the architecture or the synthetic signal.)

Build + flash the firmware:

```bash
west build -p auto -b nucleo_g474re apps/ai_anomaly -d build/ai_anomaly
west flash -r openocd -d build/ai_anomaly
```

Or via VSCode tasks: **`Zephyr: Build (ai_anomaly)`** then
**`Zephyr: Flash (ai_anomaly / openocd)`**.

## Try it

Connect to the board's shell (`screen /dev/cu.usbmodem* 115200` or via
`tether`). Type `anomaly <Tab>` for completion.

```
g474> anomaly state
injection: none  amplitude: 0.000
threshold: 0.02000
stream   : off (period 1000 ms)
arena    : 4096 bytes

g474> anomaly score          # one window, one inference
score    : 0.00127
threshold: 0.02000

g474> anomaly inject pulse 0.6
g474> anomaly score
score    : 0.02500 ★ ANOMALY

g474> anomaly inject drift 0.5
g474> anomaly score
score    : 0.25849 ★ ANOMALY

g474> anomaly stream on 500   # auto-print every 500 ms; great for watching
                              # the score react as you flip injection on/off

g474> anomaly bench 1000      # measure inference latency
iterations : 1000
avg        : 22202 cycles  (130.600 us)
min        : 22146 cycles  (130.270 us)
max        : 23844 cycles  (140.258 us)
throughput : ~7656 inf/sec
```

## Commands

| Command                            | What it does                                  |
|------------------------------------|-----------------------------------------------|
| `anomaly score`                    | Generate one window, run inference, print score |
| `anomaly bench [n]`                | Measure inference latency over n iterations   |
| `anomaly state`                    | Show current injection / threshold / stream   |
| `anomaly inject none`              | Clear injected fault                          |
| `anomaly inject pulse <amp>`       | Inject 3-sample pulse in window center        |
| `anomaly inject drift <amp>`       | Add constant offset to all samples            |
| `anomaly inject noise <amp>`       | Add extra random noise on top                 |
| `anomaly stream on [period_ms]`    | Auto-print score every period (default 1 s)   |
| `anomaly stream off`               | Stop streaming                                |
| `anomaly threshold [value]`        | Show or set the warning threshold (advisory)  |

## How it works

The training script in `train/train.py` generates ~8000 windows of synthetic
"normal" signal:

```
y(t) = sin(2π·1·t + φ₁) + 0.3·sin(2π·3·t + φ₂) + 0.05·noise
```

…with random phases per window so the autoencoder learns the *shape* rather
than memorizing one waveform. The model is a tiny dense AE
(32 → 16 → 8 → 4 → 8 → 16 → 32) trained for 60 epochs; final validation
loss ≈ 0.0024. The bottleneck of 4 forces it to compress meaningful
structure rather than passing inputs through unchanged.

After training, the model is int8-quantized using a representative dataset
(both input and output as int8 — no float runtime path on the MCU) and
written out as a C array (`src/model.cpp`).

On the board (`src/tflm_anomaly.cpp`):
1. `tflm_anomaly_init()` parses the model and allocates tensors in a 4 KB
   arena.
2. `tflm_anomaly_score(window, &score)` quantizes a float window into the
   model's int8 input, runs `Invoke()`, dequantizes the output, and returns
   the mean squared error between input and reconstruction.

`src/signal_gen.c` produces normal windows on demand and applies whatever
fault the user has injected from the shell.

## File layout

```
apps/ai_anomaly/
├── CMakeLists.txt
├── prj.conf
├── README.md
├── train/
│   ├── train.py             # training + quantization + C-array export
│   └── model.tflite         # generated artifact (gitignored — small enough to commit if desired)
└── src/
    ├── main.c               # boot, init TFLM + signal_gen, heartbeat
    ├── cmd_anomaly.c        # `anomaly score|bench|state|inject|stream|threshold` shell tree
    ├── signal_gen.h / .c    # synthetic signal + injection state
    ├── tflm_anomaly.h       # extern "C" inference API
    ├── tflm_anomaly.cpp     # C++ TFLM glue
    ├── model.cpp / .hpp     # generated by train.py
```

## Memory footprint

| Region | Used   | Region | %     |
|--------|--------|--------|-------|
| FLASH  | 125 KB | 512 KB | 24 %  |
| RAM    |  25 KB | 128 KB | 19 %  |

The model itself is ~7.5 KB (int8). The rest is TFLM runtime + CMSIS-NN +
Zephyr+shell baseline.

## Why this is interesting

`ai_sine` showed the network can approximate something we already know how
to compute. **`ai_anomaly` shows the network detecting things it has never
seen**: the model was trained only on the normal distribution, but
correctly flags any deviation as anomalous via the reconstruction error
signal. This is the unsupervised pattern that makes TinyML practical for
real industrial monitoring (vibration, current signature, BMS, …) where
labeled fault data is scarce or non-existent.

See `docs/ai-on-g474re.md` for how this generalizes, and `docs/roadmap.md`
for the planned `ai_bms` follow-on that applies the same pattern to real
battery cycle data.
