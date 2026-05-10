# STM32G474RE Nucleo + Zephyr — TinyML demo

**TensorFlow Lite Micro on a Cortex-M4 with no NPU.** Six runnable apps,
all backed by real public battery datasets, with measured numbers from the
live board for every one of them. Built on top of the
[stm32g474-zephyr-template](https://github.com/hulryung/hulryung/stm32g474-zephyr-template)
base setup.

---

## Headline result

A standard Nucleo-G474RE (Cortex-M4F at 170 MHz, 512 KB Flash, 128 KB RAM,
**no NPU, no Helium**) runs a meaningful TinyML stack — int8-quantized
inference on real Li-ion battery data — at **0.05 % CPU @ 1 Hz** if all
three battery models run together.

| App | Question answered | Inference (avg) | Holdout result on B0018 |
|-----|-------------------|-----------------|--------------------------|
| `apps/ai_sine`         | "What is sin(x)?"             | **46 µs**     | 4 % error at π/2 (canonical TinyML hello-world) |
| `apps/ai_anomaly`      | "Is this signal corrupted?"   | **130 µs**    | 20× / 215× score separation (synthetic) |
| `apps/ai_bms`          | "Is this cycle anomalous?"    | **157 µs**    | 7×–9× separation aged vs healthy |
| `apps/ai_bms_soh`      | "What is the cell capacity?"  | **105 µs**    | MAE 0.109 Ah (~5.9 %) |
| `apps/ai_bms_rul`      | "Cycles until end-of-life?"   | **140 µs**    | MAE 12 cycles |
| `apps/ai_bms_soc`      | **6 SOC estimators side-by-side** (CC / OCV / EKF / MLP / LSTM / Hybrid) | 3-200 µs each | classical vs ML head-to-head, honest |
| `apps/ai_bms_dual_ekf` | **Dual EKF — SOC + Q co-estimation** | (84 cycles, ~ms each) | tracks 1.85 → 1.27 Ah on NASA B0005 |
| `apps/ai_bms_persistence` | **NVS save/restore + OCV recalibration** | µs | real flash storage + rest-time policy |
| `apps/ai_bms_safety`   | **Layer 1/2 thread separation** | 100Hz/10Hz | hard rule + ML advisory, 6 scenarios |
| `apps/ai_bms_live`     | **DAC→ADC stream pipeline** | 1 kHz sampler | jumper PA4↔PA0, ring buffer + worker |
| `apps/shell_monitor`   | (UART shell + system stats)   | —             | foundation for all the others |

Numbers reproducible — full live-board capture in
[`docs/experiment-results.md`](docs/experiment-results.md).

---

## Watch them run

Three short asciinema recordings of the live board, captured by driving
the Zephyr shell over `tether` (multiplexing serial daemon). The GIFs
below are auto-generated from the cast files in `docs/casts/`. Each
playthrough corresponds to a real flash → run → measure cycle.

### `ai_bms` — autoencoder catches degraded battery cycles on a held-out cell

![ai_bms scan demo](docs/gifs/demo-ai-bms.gif)

Three cycles from B0018 (the cell the AE has never seen):
`early` (1.855 Ah, healthy) scores 0.003; `mid` and `aged` exceed the
0.005 threshold and are flagged with `★ ANOMALY`. Score grows monotonically
with cell age — `aged` cycles ~9× higher than healthy.

> Cast: [`docs/casts/demo-ai-bms.cast`](docs/casts/demo-ai-bms.cast)
> · Replay: `asciinema play docs/casts/demo-ai-bms.cast`

### `ai_anomaly` — fault injection from the shell, score reacts immediately

![ai_anomaly inject demo](docs/gifs/demo-ai-anomaly.gif)

Baseline score is ~0.001 (normal periodic signal). Injecting a 3-sample
**pulse** through the shell pushes the score 20× higher; a constant
**drift** corrupts every sample and pushes it 200× higher. The same
inference loop, just different input distribution.

> Cast: [`docs/casts/demo-ai-anomaly.cast`](docs/casts/demo-ai-anomaly.cast)

### `ai_bms_dual_ekf` — capacity tracking across cell life (production pattern)

![ai_bms_dual_ekf demo](docs/gifs/demo-ai-bms-dual-ekf.gif)

Dual EKF — fast SOC estimator + slow capacity (Q) estimator. Replays
NASA B0005's 168 discharge cycles in order; the slow EKF tracks capacity
fade from 1.85 Ah down to 1.27 Ah, ending at SOH = 68.6 %. Final error
just 39 mAh. **The pattern every production BMS uses to keep SOC
calibrated as the cell ages.**

### `ai_bms_persistence` — NVS save/restore + OCV recalibration policy

![ai_bms_persistence demo](docs/gifs/demo-ai-bms-persistence.gif)

Real flash storage via Zephyr's settings subsystem. Demonstrates the
**rest-time policy** that decides whether to trust saved SOC after
boot: short rest → use saved value; long rest → blend with OCV reading
(0.3 saved + 0.7 OCV). Versioned schema, factory-reset path, drift
detector all included.

### `ai_bms_safety` — Layer 1 hard rules + Layer 2 ML advisory thread

![ai_bms_safety demo](docs/gifs/demo-ai-bms-safety.gif)

Two Zephyr threads. **Safety thread** runs at 100 Hz priority 2,
evaluates hard rules (OV/UV/OC/OT) only — would open the contactor.
**ML advisory thread** runs at 10 Hz priority 10, computes anomaly score
+ imbalance / thermal trend warnings — **never** touches the contactor.
Six injectable scenarios. The architecture every UL/IEC-certified BMS
has to use.

### `ai_bms_soc` — six SOC estimators side-by-side, classical vs ML

![ai_bms_soc demo](docs/gifs/demo-ai-bms-soc.gif)

Six different ways to answer "what's the cell's SOC right now": coulomb
counting, OCV lookup, EKF (production-grade), MLP, LSTM, and a hybrid
EKF+MLP. On a full-discharge window all three ML methods nail it (0 %
error); EKF makes a 16 % error from capacity-fade mismatch. On a
partial-discharge window with artificial padding, **MLP is the most
robust** because it only looks at the last sample. Honest comparison
of classical and ML approaches on the same data — no winner takes all.

> Cast: [`docs/casts/demo-ai-bms-soc.cast`](docs/casts/demo-ai-bms-soc.cast)

### `ai_bms_rul` — predicting cycles until end-of-life

![ai_bms_rul demo](docs/gifs/demo-ai-bms-rul.gif)

Same NASA data as `ai_bms`, different question: "how many cycles until
the cell crosses below 1.5 Ah?" Notice the model is most accurate on the
**aged** cycle (predicted 2 vs true 0 — within margin of error) where
degradation is unmistakable, and least accurate on **early** cycles where
the signal is subtle. Inference: 140 µs per cycle.

> Cast: [`docs/casts/demo-ai-bms-rul.cast`](docs/casts/demo-ai-bms-rul.cast)

---

## What this repo demonstrates

1. **TinyML is real on no-NPU MCUs.** Cortex-M4 + DSP + FPU + CMSIS-NN
   gets you small NN inference in tens to a few hundred microseconds.
   You don't need an Ethos-U or a Helium-class core to run useful models.

2. **One dataset, multiple operational outputs.** The same NASA PCoE
   battery cycle data drives anomaly detection (`ai_bms`), capacity
   estimation (`ai_bms_soh`), and remaining-life prediction
   (`ai_bms_rul`) — three different ML problem types from one source.
   This is the practical multi-task pattern for industrial BMS.

3. **Cross-cell generalization works.** All three battery models train
   on B0005-B0007 and are tested on B0018 — a held-out cell the model
   has never seen. They still detect anomaly / estimate capacity /
   predict RUL on the unseen cell, which is the test of whether the
   model learned generic battery health vs cell-specific quirks.

4. **End-to-end reproducibility.** Every result above can be re-derived
   from this repo: training notebooks under `apps/<name>/train/`, public
   data download instructions in `datasets/README.md`, build/flash via
   the same `west build && west flash` for every app. No hidden model
   weights or proprietary blobs.

---

## The six apps

```
apps/
├── shell_monitor/   ── UART shell on LPUART1 (ST-Link VCP) + system stats
├── ai_sine/         ── int8 MLP that approximates sin(x)               [TinyML hello-world]
├── ai_anomaly/      ── autoencoder anomaly detection on synthetic signal [unsupervised pattern]
├── ai_bms/          ── autoencoder on NASA PCoE battery discharge data  [real-data anomaly]
├── ai_bms_soh/      ── MLP regressor — predicts capacity in Ah          [supervised regression]
└── ai_bms_rul/      ── MLP prognostic — cycles remaining until EOL      [prognostic]
```

Each app is self-contained with its own `train/train.py`, model bytes,
TFLM C++ wrapper, shell command tree, README, and VSCode build/launch
config. Per-app details: see each app's `README.md`.

---

## Battery datasets — already on hand

```
datasets/
├── nasa-pcoe/         ── 4 cells, LCO, CC discharge          (250 MB)
├── nasa-randomized/   ── 7 cycling regimes, random walk      (1.0 GB)
├── oxford/            ── 8 cells, LCO, drive-cycle profile   (256 MB)
├── calce-cs2-lco/     ── 3 cells (CS2_33/34/35), LCO         (257 MB)
├── calce-cx2-lfp/     ── 3 cells (CX2_33/34/35), LFP         (524 MB)
└── severson/          ── (manual download, ~5 GB, 124 cells, LFP)
```

All five auto-downloadable datasets are smoke-tested as loadable —
[`docs/dataset-exploration.md`](docs/dataset-exploration.md) is the
report (with an asciinema replay). Reproduce instructions and citations
are in [`datasets/README.md`](datasets/README.md). Data files themselves
are gitignored (raw data is large and public).

The dataset variety lets future demos isolate specific axes:

| Axis | Demonstrated by |
|------|-----------------|
| **Chemistry** (LCO vs LFP) | NASA / Oxford / CALCE-CS2 (LCO) ↔ CALCE-CX2 (LFP) |
| **Profile** (CC vs drive-cycle vs random) | NASA-PCoE ↔ Oxford ↔ NASA-Randomized |
| **Scale** (4 cells vs 8 vs 124) | NASA ↔ Oxford ↔ Severson |

---

## Documentation

| Path                                  | What's in it                                     |
|---------------------------------------|--------------------------------------------------|
| [`docs/ai-on-g474re.md`](docs/ai-on-g474re.md)        | TinyML feasibility report — hardware reality, framework choice, measured per-app latency / footprint, capacity for other model classes |
| [`docs/experiment-results.md`](docs/experiment-results.md) | Live-board test report for the 3 BMS apps — full tether output for every command, cross-app comparison, caveats |
| [`docs/dataset-exploration.md`](docs/dataset-exploration.md) | Smoke-test of all 5 datasets with asciinema recording — confirms each one parses + summarizes structure |
| [`docs/casts/`](docs/casts/) + [`docs/gifs/`](docs/gifs/) | Asciinema casts + GIFs of the demo runs above (replayable in any terminal) |
| [`docs/roadmap.md`](docs/roadmap.md)                  | Planned future apps — chemistry generalization, drive-cycle robustness, KWS, MNIST CNN, etc. |
| [`apps/<name>/README.md`](apps/)                       | Per-app: how to build, train, flash, and use     |
| [`datasets/README.md`](datasets/README.md)            | Per-dataset: source, format, citations, reproduce commands |

---

## How to run any of this

### Quick state check (for AI agents resuming work)

```bash
cd ~/dev/zephyr
echo "venv:    $(test -x .venv/bin/west && echo OK || echo MISSING)"
echo "venv-ml: $(test -x .venv-ml/bin/python && echo OK || echo MISSING)"
echo "manifest:$(test -d zephyr && echo OK || echo MISSING)"
echo "modules: $(test -d modules && test -d bootloader && echo OK || echo MISSING)"
echo "tflm:    $(test -d optional/modules/lib/tflite-micro && echo OK || echo MISSING)"
echo "sdk:     $(test -d ~/zephyr-sdk-1.0.1 && echo OK || echo MISSING)"
echo "openocd: $(command -v openocd >/dev/null && echo OK || echo MISSING)"
echo "data:    $(test -f datasets/nasa-pcoe/B0005.mat && echo OK || echo MISSING)"
```

If everything is `OK`, you can build and flash any app:

```bash
source ./zephyr-env.sh
west build -p auto -b nucleo_g474re apps/ai_bms -d build/ai_bms
west flash -r openocd -d build/ai_bms
```

If anything is `MISSING`, see **First-time setup** below.

### Driving an app via tether (no human needed)

`tether` lets a script run shell commands on the board and capture
output non-interactively — the same harness the test reports use.

```bash
# (one-time) start the daemon
tetherd -D /dev/cu.usbmodem* -b 115200 &

# every command + reply captured deterministically
SOCK=$(ls /tmp/tetherd*.sock | head -1)
tether -s "$SOCK" run --until 'g474> $' --newline crlf 'bms scan'
tether -s "$SOCK" run --until 'g474> $' --newline crlf 'bms bench 1000'
```

### VSCode debug

`code ~/dev/zephyr`, then **Run-and-Debug** → pick any
`Cortex-Debug: <app> (debug build)` config, F5. Auto-builds with `-Og`
debug overlay, flashes via OpenOCD/ST-Link, breaks at `main`.

---

## First-time setup

### 1. Homebrew dependencies

```bash
brew install cmake ninja gperf python@3 ccache dtc libmagic wget openocd stlink
```

### 2. Two Python venvs — west and ML training are kept separate

```bash
cd ~/dev/zephyr
# build venv (west, Zephyr deps)
python3 -m venv .venv && .venv/bin/pip install --upgrade pip west

# ML training venv (TF, scipy, pandas) — Python 3.12 because TF doesn't
# yet ship for 3.14 wheels at time of writing
python3.12 -m venv .venv-ml
.venv-ml/bin/pip install tensorflow numpy scipy pandas openpyxl matplotlib
```

### 3. west workspace + TFLM optional module

```bash
.venv/bin/west init -m https://github.com/zephyrproject-rtos/zephyr --mr main .
.venv/bin/west update --narrow -o=--depth=1
.venv/bin/west config manifest.group-filter -- "+optional"
.venv/bin/west update --narrow -o=--depth=1 tflite-micro
.venv/bin/west packages pip --install
.venv/bin/west sdk install -t arm-zephyr-eabi
```

### 4. Battery datasets

```bash
# Each dataset has its own download command — see datasets/README.md
# Quickest start (just NASA PCoE — what the 3 BMS apps use):
mkdir -p datasets/nasa-pcoe && cd datasets/nasa-pcoe
curl -fL -o nasa.zip "https://phm-datasets.s3.amazonaws.com/NASA/5.+Battery+Data+Set.zip"
unzip nasa.zip && unzip "5. Battery Data Set/1. BatteryAgingARC-FY08Q4.zip" \
    -d "5. Battery Data Set/"
mv "5. Battery Data Set"/* . && rmdir "5. Battery Data Set" && rm nasa.zip
cd ../..
```

### 5. VSCode extensions

```bash
code --install-extension marus25.cortex-debug \
     --install-extension ms-vscode.cpptools \
     --install-extension mcu-debug.peripheral-viewer \
     --install-extension mcu-debug.rtos-views
```

### 6. Verify

```bash
.venv-ml/bin/python scripts/explore_datasets.py    # all dataset checks PASS?
.venv/bin/west build -p auto -b nucleo_g474re apps/ai_sine -d build/ai_sine
.venv/bin/west flash -r openocd -d build/ai_sine   # LED blinks, ai sine works
```

---

## Target hardware

- **Board:** ST Nucleo-G474RE
- **MCU:** STM32G474RET6 — Cortex-M4F, 170 MHz, **512 KB Flash, 128 KB RAM**
- **Debugger:** onboard ST-Link V3 (USB VID:PID `0483:374E`)
- **Zephyr board name:** `nucleo_g474re`
- **OpenOCD configs:** `interface/stlink.cfg` + `target/stm32g4x.cfg`
- **User LED:** LD2 on PA5 → mapped as `led0` alias in board DTS
- **Mass storage on connect:** `/Volumes/NOD_G474RE`
- **Serial (ST-Link VCP):** `/dev/cu.usbmodem*` @ 115200

## Host environment (verified)

- macOS 26.4 / Apple Silicon (arm64)
- Homebrew at `/opt/homebrew`
- VSCode + `code` CLI on PATH
- `~12 GB free disk` (workspace ≈ 6.3 GB, SDK ≈ 1.5 GB, datasets ≈ 2.3 GB,
  ML venv ≈ 1.5 GB)

## Repository layout

| Path              | Purpose                                                       |
|-------------------|---------------------------------------------------------------|
| `apps/`           | Six self-contained Zephyr applications (5 TinyML + 1 shell)   |
| `datasets/`       | Public battery datasets (raw data gitignored, README tracked) |
| `docs/`           | Feasibility report, experiment results, dataset exploration, roadmap |
| `scripts/`        | Cross-cutting Python tools (e.g. `explore_datasets.py`)        |
| `.vscode/`        | tasks, launch, settings, extensions for nucleo_g474re         |
| `debug.conf`      | Build overlay for `-Og` + thread info (debugging)             |
| `zephyr-env.sh`   | Sources venv + sets `ZEPHYR_BASE`, `ZEPHYR_SDK_INSTALL_DIR`   |

Not in repo (regenerable): `zephyr/`, `modules/`, `bootloader/`, `tools/`,
`optional/` (west update), `.west/` (west init), `.venv/`, `.venv-ml/`
(python venv), `build/` (cmake/ninja), `~/zephyr-sdk-1.0.1/` (SDK install),
and the actual data files under `datasets/<name>/`.

---

## Caveats — read before using any of this on a real product

1. **None of this is a substitute for hard-rule BMS protection.** All
   three battery apps are *advisory*. Production firmware must implement
   classical OV / OT / OC hard-trip thresholds in the foreground; ML
   layers run alongside as monitoring/early-warning, not as primary
   safety. UL 1973 / IEC 62619 certification is rules-based, not
   ML-based.
2. **NASA PCoE has 4 cells.** That's tiny. Every BMS app's accuracy
   ceiling on the published numbers is bounded by the training data,
   not the architecture. To go from `ai_bms_soh`'s ~5–6 % MAE to a
   sub-1 % production estimator you need a Severson-class dataset (124
   cells) and Kalman / particle filtering layered on top of the
   single-cycle inference loop documented here.
3. **No live cell wiring.** Every "cycle" the firmware scores is a
   pre-recorded curve baked into flash from a public dataset. Replacing
   this with live ADC readings is the next step — each app's README
   lists the changes required.

---

## Gotchas (from real failures during setup)

These are the macOS-specific traps. Each cost real time during initial
bring-up.

### A. Default flash runner is `stm32cubeprogrammer` (not installed)

`zephyr/boards/st/nucleo_g474re/board.cmake` sets `stm32cubeprogrammer` as
the default. Always pass `-r openocd` to `west flash`. The `.vscode/`
tasks already do this.

### B. `runToEntryPoint: main` shows source in `device.h:57 compiler_barrier()`

Default `-Os` builds inline aggressively, so the first PC of `main`'s
prologue maps to an inlined `compiler_barrier()` line. The CALL STACK
panel still says `main@…` — you ARE at main, just looking at the wrong
source line. Fix: build with `debug.conf` (`CONFIG_DEBUG_OPTIMIZATIONS=y`)
to disable inlining. Use the `(debug build, …)` launch configs.

### C. `"rtos": "Zephyr"` in launch.json crashes OpenOCD on macOS

OpenOCD 0.12.0's Zephyr RTOS plugin has a symbol-resolution timing bug on
macOS that makes the GDB server quit. Removed from all our launch configs;
use the `mcu-debug.rtos-views` extension's **XRTOS** panel instead — it
reads the same offsets via GDB after the session is running.

### D. Two Python venvs

`.venv/` is for west (Zephyr build). `.venv-ml/` is for ML training (TF,
pandas, scipy). Don't mix them — TF won't install in the west venv on
Python 3.14.

### E. `west update` is heavy

First-time clones ~60 modules (~3 GB even with shallow clones, ~6.3 GB
total). Allow 5–10 min on a fast connection. The optional TFLM module
adds another ~44 MB.

### F. Severson dataset can't be auto-downloaded

`data.matr.io` is a JavaScript SPA, so simple HTTP fetch can't recover
per-file URLs. `datasets/README.md` documents two manual workarounds.
The other 5 battery datasets do download cleanly via `curl`.

---

## Pinning notes for reproducibility

- **Zephyr SDK:** 1.0.1 (`west sdk install` defaults). `.vscode/launch.json`
  paths assume `~/zephyr-sdk-1.0.1/`; bump if you upgrade.
- **Zephyr manifest:** `--mr main`. For a stable release, replace with
  `--mr v4.0.0` and re-run `west update`.
- **TFLM:** version pinned to whatever the optional submanifest pulls
  (commit `fcc760a` at time of writing).
- **CMSIS-NN:** version pinned to whatever the main manifest pulls
  (commit `d20117c` at time of writing).
- **Python ML stack:** TF 2.21.0, NumPy 2.4, Python 3.12.13.

If reproducing exact numbers from `docs/experiment-results.md`, also pin
the train.py random seed (already 42 in every script) and use the same
TF version.
