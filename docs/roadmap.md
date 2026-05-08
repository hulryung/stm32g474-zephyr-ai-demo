# Planned apps — roadmap

A running list of apps that should eventually live under `apps/` to make this
template a well-rounded TinyML + embedded reference. Each entry is a
self-contained scope that can be picked up independently. Ordering is rough
priority, not strict dependency.

## Status legend

- 🟢 **Done**
- 🔵 **Next** — clearly worth doing, no blockers
- ⚪ **Candidate** — useful but not yet committed
- 🔴 **Held** — known blocked or out of scope for this board

---

## Already shipped

| App | Pattern demonstrated | Status |
|-----|----------------------|--------|
| `apps/shell_monitor` | UART shell, custom shell command tree, system monitoring | 🟢 |
| `apps/ai_sine` | TFLM hello-world: int8 MLP regression, ~46 µs/inference | 🟢 |

---

## TinyML pattern coverage (the main gap)

Goal: cover the 4 fundamental ML patterns at this scale — **regression**,
**anomaly detection**, **classification (CNN)**, **sensor-driven
classification**. We have regression. The rest:

### 🔵 `apps/ai_anomaly` — autoencoder anomaly detection

Why: most "real" TinyML use cases on motor-control / industrial MCUs are
anomaly detection on vibration, current, or audio. Educationally, this fills
the **unsupervised** slot none of our other apps cover.

Concept:
- Autoencoder trained on a synthetic clean signal (e.g. sin + harmonics).
- Inference computes reconstruction error.
- Shell command `anomaly score` returns current error; `anomaly inject
  <amplitude>` corrupts the input to demonstrate detection.

External HW: **none** — generate the signal on-device (DAC loopback or
plain math).

Effort: medium. Need to write the training notebook ourselves (no upstream
sample). Inference code follows the `ai_sine` pattern.

Educational value: ⭐⭐⭐⭐ (most "real" feeling demo)

### 🔵 `apps/ai_mnist` — int8 CNN classifier

Why: covers the **CNN/classification** slot. Tests TFLM Conv2D, MaxPool,
Softmax kernels (ai_sine only exercises FullyConnected).

Concept:
- Pretrained tiny MNIST classifier (~50–80 KB model).
- 5–10 hardcoded test images stored as flash arrays.
- `ai mnist <index>` runs inference on `images[index]`, prints predicted
  digit + confidence.
- `ai mnist bench` like ai_sine bench, expect ~10–30 ms / inference.

External HW: **none**.

Effort: low–medium. Use upstream MNIST training scripts (TFLM has examples).
Adapt `ai_sine` skeleton; mainly extend the op resolver.

Watch: stays well within 128 KB RAM, but be careful with arena size — CNN
activations are bigger than MLP.

Educational value: ⭐⭐⭐ (most concrete "AI does a thing" moment for newcomers)

### ⚪ `apps/ai_gesture` — accelerometer gesture recognition

Why: sensor-driven classification with real hardware in the loop. Most
visually impressive demo — wave the board, see the classification.

Concept: port the upstream Zephyr `samples/modules/tflite-micro/magic_wand`
sample into our `apps/<name>/` convention with shell wrapping. Add I²C
accelerometer (MPU6050 ~$3 or LSM6DSL).

External HW: I²C accelerometer (~$3).

Effort: medium. Magic Wand sample exists; main work is sensor wiring +
shell integration (`gesture state`, `gesture stream`).

Educational value: ⭐⭐⭐⭐ (best wow factor)

### 🔴 `apps/ai_kws` — keyword spotting ("yes/no")

Why: completes the audio-pipeline demo class. Real-world relevant
(wake-word).

Concept: port upstream `tensorflow/lite/micro/examples/micro_speech`. Audio
framing → MFCC features → small classifier. ~20 KB model.

External HW: PDM/I²S microphone (~$5, e.g. INMP441).

Effort: **high**. Audio framing + MFCC are non-trivial; not a
copy-paste of a Zephyr sample.

Educational value: ⭐⭐⭐ but cost-of-effort is high. Defer.

### 🔴 `apps/ai_person` — person detection from camera

Why: completes the vision-pipeline class.

Why held: model alone is ~250 KB; we have 128 KB RAM — tight or impossible
once activations are added. Camera (OV7670) wiring is non-trivial. Skip
unless we move to a bigger STM32.

---

## Non-AI apps that round out peripheral coverage

These are not TinyML but cover Zephyr fundamentals not yet shown:

### ⚪ `apps/button_led` — GPIO interrupt + work queue

User button (B1, PC13) → GPIO IRQ → debounce via `k_work_delayable` →
toggle LD2.

Pattern: **interrupt-driven GPIO + deferred work**. Foundation for any
hardware-event-driven app.

External HW: none (B1 is on the board).

Effort: low (~30 min).

### ⚪ `apps/adc_temp` — internal temperature sensor

Read STM32G4's internal temperature sensor + Vrefint via ADC1, convert to
°C using factory calibration values from System Memory.

Pattern: **ADC subsystem + sensor data + factory cal**.

Shell: `sys temp` (extends shell_monitor's command tree, or new `apps/`
entry).

Effort: low–medium (Vrefint scaling is the tricky bit).

### ⚪ `apps/pwm_led` — PWM dimming via TIM2

LD2 (PA5) has TIM2_CH1 alternate function. Drive it as PWM.

Pattern: **PWM subsystem + devicetree pinctrl alt-function override**.

Shell: `pwm set <duty>` 0–100 %.

Effort: low.

### ⚪ `apps/nvs_counter` — boot counter in flash

NVS (non-volatile storage) demo: increment a counter in flash on each boot,
print it on startup. Survives reset.

Pattern: **flash partition + NVS subsystem**.

External HW: none.

Effort: low.

### ⚪ `apps/fdcan_loopback` — CAN-FD self-test

STM32G4 has FDCAN (CAN with flexible data rate). The transceiver isn't
populated on Nucleo, but **internal loopback mode** lets the controller
send/receive to itself without external wiring.

Pattern: **G4 specialty peripheral**.

External HW: none for loopback. External CAN bus needs a transceiver
(MCP2562FD ~$2).

Effort: medium. Driver bring-up is non-trivial.

---

## Suggested next-pickup order

If you're an agent (or human) coming back to this list cold:

1. **`apps/button_led`** — fast win, 30 min, fills basic GPIO IRQ pattern
2. **`apps/ai_anomaly`** — highest TinyML educational value, no HW needed
3. **`apps/ai_mnist`** — completes CNN coverage, no HW needed
4. **`apps/ai_gesture`** — best demo, requires accelerometer purchase
5. **`apps/adc_temp`** — fills ADC pattern, real sensor data

After 1–3: this template covers regression, anomaly, classification, GPIO
IRQ, shell, monitoring — a solid Zephyr + TinyML reference for STM32G4.

---

## How to add a new app

```bash
cp -r apps/ai_sine apps/<new_name>
# edit CMakeLists.txt project() name
# edit prj.conf (probably remove TFLM stuff if not needed)
# rewrite src/main.c and src/cmd_<x>.c
```

Then update three places to register the app:

1. `apps/README.md` — add a row in the "Available apps" table
2. `.vscode/tasks.json` — copy the four task entries (Build, Debug build,
   Flash, Clean) and replace `<old_name>` with `<new_name>`
3. `.vscode/launch.json` — add a new Cortex-Debug entry mirroring the
   existing pattern

(A scaffolding script could automate this — that itself is a small future
task, ⚪ `tools/new-app.sh`.)
