# ai_sine

TinyML on Cortex-M4: a TensorFlow Lite Micro int8-quantized MLP that
approximates `sin(x)`. Demonstrates that meaningful neural-network inference
is feasible on the STM32G474RE (no NPU, just M4 + DSP + FPU + CMSIS-NN
optimized kernels).

Measured on this board: **~46 µs per inference at 170 MHz**, ~21k
inferences/sec.

## Build & flash

This app requires the Zephyr `tflite-micro` module from the optional manifest
group. Enable once per workspace:

```bash
cd ~/dev/zephyr
.venv/bin/west config manifest.group-filter -- "+optional"
.venv/bin/west update --narrow -o=--depth=1 tflite-micro
```

Then build:

```bash
west build -p auto -b nucleo_g474re apps/ai_sine -d build/ai_sine
west flash -r openocd -d build/ai_sine
```

Or via VSCode: **`Zephyr: Build (ai_sine)`** then
**`Zephyr: Flash (ai_sine / openocd)`**.

## Try it

Connect to the shell (`screen /dev/cu.usbmodem* 115200` or via `tether`),
then:

```
g474> ai info
Model     : sine (int8-quantized FullyConnected MLP)
Framework : TensorFlow Lite Micro + CMSIS-NN
Arena size: 2000 bytes

g474> ai sine 1.5708           # π/2
x = 1.57080
predicted sin(x) = 1.04206
actual    sin(x) = 1.00000
error            = +0.04206

g474> ai bench 5000
iterations : 5000
cpu freq   : 170000000 Hz
avg        : 7875 cycles  (46.323 us)
min        : 7840 cycles  (46.117 us)
max        : 9487 cycles  (55.805 us)
throughput : ~21587 inf/sec
```

## What's where

```
apps/ai_sine/
├── CMakeLists.txt        # builds C + C++ sources together
├── prj.conf              # CONFIG_TENSORFLOW_LITE_MICRO=y, CMSIS_NN_KERNELS=y, FPU=y
├── README.md
└── src/
    ├── main.c            # heartbeat + LOG_INF on boot
    ├── cmd_ai.c          # `ai sine|bench|info` shell commands (C)
    ├── tflm_sine.h       # extern "C" wrapper API
    ├── tflm_sine.cpp     # interpreter init + inference (C++ — TFLM is C++)
    ├── model.cpp         # int8-quantized model bytes (from upstream sample)
    └── model.hpp
```

## Why these numbers matter

The Cortex-M4 in the STM32G474RE has no NPU. The performance comes from:
- Single-cycle MAC, hardware FPU, DSP extensions
- **CMSIS-NN** optimized kernels (enabled via
  `CONFIG_TENSORFLOW_LITE_MICRO_CMSIS_NN_KERNELS=y`) replacing TFLM reference
  ops with hand-tuned ARM assembly for `int8` matrix-vector multiplies

Disabling CMSIS-NN (set the Kconfig to `n`) typically increases inference
time 3–10× on the same model — the kernels are the optimization, not magic
hardware.

## Memory footprint

| Region | Used   | Total | %     |
|--------|--------|-------|-------|
| FLASH  | 115 KB | 512 KB | 22 % |
| RAM    |  21 KB | 128 KB | 16 % |

Most flash is TFLM runtime + CMSIS-NN. The model itself is ~3 KB.

## Adding your own model

1. Train a model in TensorFlow / Keras (offline, on a real machine).
2. Convert + int8-quantize:
   ```python
   converter = tf.lite.TFLiteConverter.from_keras_model(m)
   converter.optimizations = [tf.lite.Optimize.DEFAULT]
   converter.representative_dataset = repr_data_gen
   converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
   converter.inference_input_type  = tf.int8
   converter.inference_output_type = tf.int8
   tfl = converter.convert()
   ```
3. Convert to a C array: `xxd -i model.tflite > model.cpp`
4. Update `tflm_sine.cpp` resolver to add the ops your model uses
   (e.g. `resolver.AddConv2D(); resolver.AddSoftmax();`).
5. Bump `kArenaSize` if `AllocateTensors()` fails.

The upstream Zephyr sample at `zephyr/samples/modules/tflite-micro/hello_world`
includes a Python training notebook in `train/`.

## References

- Zephyr TFLM module: `zephyr/modules/tflite-micro/`
- Upstream samples: `zephyr/samples/modules/tflite-micro/{hello_world,magic_wand}`
- TFLM source: `optional/modules/lib/tflite-micro/` (after enabling optional group)
- CMSIS-NN: `modules/lib/cmsis-nn/`
