# AI/ML on STM32G474RE — Findings Report

**Question:** Can we run neural-network inference on this Nucleo board, and
if so, what kind and how fast?

**Short answer:** Yes — TinyML class. Tested with TensorFlow Lite Micro +
CMSIS-NN: **46 µs per inference** for a small int8 MLP at 170 MHz. Up to
~21,500 inferences/sec. Real models fit in 16–22 % of available memory.

This report documents the hardware reality, the framework choice, the
implementation in `apps/ai_sine/`, and the actual measured results — so
future-you (or another AI agent) can decide quickly what's feasible without
re-discovering everything.

---

## 1. Hardware reality check

The STM32G474RE belongs to ST's "G" line — motor control / power
conversion. **Not** an AI-targeted MCU. The relevant specs vs typical AI
workloads:

| Resource | This board | TinyML model needs | Verdict |
|----------|-----------|--------------------|---------|
| CPU | Cortex-**M4F** @ 170 MHz | Cortex-M4+ with FPU/DSP | OK |
| RAM | 128 KB (32 KB CCMRAM) | model + arena commonly 10–80 KB | OK |
| Flash | 512 KB | weights typ. 5–200 KB | OK |
| **NPU** | **none** | optional accelerator | runs in software |
| Accelerators | CORDIC + FMAC (G4 specialty) | helpful for DSP front-end | bonus |

For comparison: ST has dedicated AI MCUs (STM32U5 with neural-ART, STM32N6
with NPU, Cortex-M55 with Helium). The G4 doesn't have any of those — so
all NN math is plain M4 instructions, optimized through CMSIS-NN.

**What this means in practice:** small classifiers, regressors, anomaly
detectors, gesture recognizers, keyword-spotting models all fit. Real-time
camera vision and any Transformer-class architecture do not.

---

## 2. Framework landscape

| Framework | License | Zephyr support | When to pick |
|-----------|---------|----------------|--------------|
| **TensorFlow Lite Micro (TFLM)** | Apache 2.0 | First-class Zephyr module | Default. FOSS, well-documented, integrates with the workspace's `west update` flow |
| STM32Cube.AI (X-CUBE-AI) | ST proprietary | Awkward outside CubeIDE | Only if you must use ST's optimizer or already in their toolchain |
| edge-impulse | Commercial (free dev tier) | C++ library export | End-to-end MLOps with on-device data collection |
| CMSIS-NN (alone) | Apache 2.0 | Used by TFLM | Only if you want to write the inference graph manually |

**This project picked TFLM** because (a) it's the upstream Zephyr-blessed
choice, (b) FOSS, (c) automatically uses CMSIS-NN as the M4-optimized
backend when `CONFIG_TENSORFLOW_LITE_MICRO_CMSIS_NN_KERNELS=y`.

### TFLM is in an *optional* west group

A practical gotcha: the TFLM source isn't pulled by default `west update`.
It lives under `submanifests/optional.yaml` with the `optional` group, which
is filtered out by the manifest's `group-filter: [-babblesim, -optional, -testing]`.

Enable once:

```bash
west config manifest.group-filter -- "+optional"
west update --narrow -o=--depth=1 tflite-micro
```

The source lands at `optional/modules/lib/tflite-micro/` (~44 MB shallow).

CMSIS-NN, by contrast, **is** in the default manifest at
`modules/lib/cmsis-nn/` — already present on a fresh setup.

---

## 3. The reference implementation: `apps/ai_sine`

A canonical TinyML "hello world": an int8-quantized fully-connected MLP that
approximates `sin(x)` on `x ∈ [0, 2π]`.

### What it demonstrates
- TFLM init/inference loop on Zephyr
- C/C++ interop pattern (TFLM is C++; shell commands are C)
- Shell-driven validation (`ai sine <x>`, `ai bench [n]`, `ai info`)
- Cycle-accurate microbenchmark using `k_cycle_get_32()`

### Architecture
```
       ┌──────────────────────────┐
       │   Zephyr shell (C)       │
       │   cmd_ai.c               │
       └──────────┬───────────────┘
                  │  tflm_sine_infer(x, &y)   ← extern "C"
       ┌──────────▼───────────────┐
       │   TFLM wrapper (C++)     │
       │   tflm_sine.cpp          │
       │   • interpreter setup    │
       │   • int8 quantize/deq    │
       └──────────┬───────────────┘
                  │
       ┌──────────▼───────────────┐
       │   TFLM + CMSIS-NN        │
       │   (Zephyr modules)       │
       └──────────────────────────┘
```

The wrapper isolates C++ from C — the rest of the app (main.c, shell
commands) stays C, only `tflm_sine.cpp` and `model.cpp` are C++.

---

## 4. Measured results (this board, this build)

Build: `west build -p auto -b nucleo_g474re apps/ai_sine`, default Kconfig
overlay (no debug build), CMSIS-NN kernels enabled.

### Memory footprint

| Region | Used | Region size | Used % |
|--------|------|-------------|--------|
| FLASH  | 115,112 B (~112 KB) | 512 KB | 21.96 % |
| RAM    | 20,608 B  (~20 KB)  | 128 KB | 15.72 % |
| Tensor arena | 2,000 B (within RAM) | — | — |

The model bytes themselves are ~3 KB. The remaining flash is TFLM runtime
(~80 KB), CMSIS-NN library, and Zephyr+shell baseline (~30 KB).

### Inference latency

`ai bench` results, 5,000 iterations sweeping `x` across `[0, 2π]`:

| Metric | Cycles | Microseconds @ 170 MHz |
|--------|--------|------------------------|
| avg    | 7,875  | 46.323 |
| min    | 7,840  | 46.117 |
| max    | 9,487  | 55.805 |
| **throughput** | — | **~21,587 inferences/sec** |

The min/avg gap of 0.2 % indicates the workload is essentially
deterministic — no significant cache effects or scheduler interference at
this model size. The max outlier (~10 µs above avg) appears once or twice
in 5 k iterations, likely a tick interrupt servicing.

### Accuracy spot-check

Three reference points, comparing model prediction vs `sinf()`:

| x | model → ŷ | actual sin(x) | error |
|---|-----------|---------------|-------|
| 0       | 0.00000 | 0.00000 | 0.000 |
| π/2 ≈ 1.5708 | 1.04206 | 1.00000 | +0.042 |
| π ≈ 3.1416  | 0.00847 | -0.00001 | +0.008 |

The 4 % error at the peak (`x = π/2`) is normal for this 3-layer model with
int8 quantization. Adding another hidden layer or going `int16` would close
that gap at the cost of ~2× memory and ~2–3× latency.

### What CMSIS-NN actually buys

Not benchmarked in this report, but documented in upstream TFLM literature:
disabling `CONFIG_TENSORFLOW_LITE_MICRO_CMSIS_NN_KERNELS` typically slows
int8 matrix-vector ops 3–10× because TFLM falls back to portable C
reference kernels. **This is the optimization, not the M4 itself.** If your
inference is too slow, check this Kconfig first.

---

## 5. What this implies for other model classes

Extrapolating from the 46 µs / 3 KB model figure (and known TFLM/CMSIS-NN
benchmarks on similar M4 silicon):

| Model class | Approx params | Approx weights | Approx inference @ 170 MHz | Verdict |
|-------------|---------------|----------------|----------------------------|---------|
| Tiny MLP (sine) | ~600 | 3 KB | 46 µs | ✅ measured here |
| Anomaly detector (autoencoder, 32→16→32) | ~3 K | ~10 KB | < 200 µs | ✅ comfortable |
| Gesture/IMU classifier (1D CNN small) | ~10 K | ~30 KB | 1–3 ms | ✅ comfortable |
| Keyword spotting (DS-CNN small) | ~25 K | ~70 KB | 5–15 ms | ✅ fits, real-time at 1 Hz |
| MNIST CNN (tiny) | ~50 K | ~80–150 KB | 10–30 ms | ⚠️ tight but feasible |
| Person-detect MobileNet v1 8-bit | ~250 K | ~250 KB | 200–500 ms | ⚠️ flash OK, not real-time |
| Anything Transformer-based | — | — | — | ❌ wrong tool |

The hard ceiling on this board is **128 KB RAM** — once activations +
arena exceed that, you can't run the model regardless of speed. Flash is
rarely the bottleneck up to a few hundred KB.

---

## 6. Recommended next experiments

In rough order of educational value and effort:

1. **`ai_anomaly`** — autoencoder on synthetic 1D signal. Inject anomalies
   via shell. No external HW. Educational about reconstruction-error
   detection.
2. **`ai_mnist`** — pre-trained CNN, hardcoded test images in flash.
   `ai mnist <index>` runs inference. Tests CNN ops in TFLM.
3. **`ai_gesture`** — adapt upstream `magic_wand` sample. Needs an MPU6050
   (~$3 on I²C). Most "wow" factor — wave the board and watch classification.
4. **`ai_kws`** — keyword spotting. Needs a PDM/I²S microphone (~$5).
   Most complex pipeline (audio framing, MFCC features, classifier).

---

## 7. Reproducibility / how this report was generated

All numbers in section 4 came from running:

```bash
# build
west build -p auto -b nucleo_g474re apps/ai_sine -d build/ai_sine

# flash
west flash -r openocd -d build/ai_sine

# verify (via tether daemon for non-interactive serial access)
SOCK=$(ls /tmp/tether-*.sock | head -1)
tether -s "$SOCK" run --until 'g474> $' --newline crlf 'ai info'
tether -s "$SOCK" run --until 'g474> $' --newline crlf 'ai sine 1.5708'
tether -s "$SOCK" run --until 'g474> $' --newline crlf 'ai bench 5000'
```

The full transcript and methodology are in `apps/ai_sine/README.md`.

---

## 8. References

- Zephyr TFLM module: `zephyr/modules/tflite-micro/`
- TFLM source (after `+optional`): `optional/modules/lib/tflite-micro/`
- CMSIS-NN: `modules/lib/cmsis-nn/`
- Upstream Zephyr samples: `zephyr/samples/modules/tflite-micro/{hello_world,magic_wand}`
- ARM CMSIS-NN docs: https://arm-software.github.io/CMSIS-NN/latest/
- TFLM: https://www.tensorflow.org/lite/microcontrollers
