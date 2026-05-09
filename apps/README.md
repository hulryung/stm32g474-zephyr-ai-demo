# apps/ — User applications

This directory holds **user-developed Zephyr applications**, separate from the
Zephyr source tree (`../zephyr/`) and west-managed modules. The Zephyr tree is
treated as a read-only dependency; new code goes here.

## Layout convention

```
apps/
├── README.md                # this file
└── <app_name>/              # one directory per application
    ├── CMakeLists.txt       # standard Zephyr app CMakeLists
    ├── prj.conf             # default Kconfig overlay
    ├── README.md            # what this app does + how to build/run
    ├── boards/              # (optional) board-specific overlays
    │   └── nucleo_g474re.overlay
    └── src/
        └── main.c
```

Each app is **self-contained** — it does not depend on other apps, only on
Zephyr and west modules. To add a new app, copy an existing one and rename.

## Building an app

From the workspace root (`~/dev/zephyr/`):

```bash
west build -p auto -b nucleo_g474re apps/<app_name> -d build/<app_name>
west flash -r openocd -d build/<app_name>
```

VSCode tasks for each app live in `../.vscode/tasks.json` with labels like
`Zephyr: Build (<app_name>)`. F5 launches debug from `../.vscode/launch.json`.

## Adding a debug build overlay

The workspace-level `../debug.conf` enables `-Og` and thread info for any app:

```bash
west build -p always -b nucleo_g474re apps/<app_name> -d build/<app_name> \
    -- -DEXTRA_CONF_FILE=$(pwd)/../debug.conf
```

Per-app debug overlays go in `apps/<app_name>/debug.conf` instead.

## Available apps

| App              | Purpose                                                   |
|------------------|-----------------------------------------------------------|
| `shell_monitor`  | UART shell on LPUART1 (ST-Link VCP) + system monitoring   |
| `ai_sine`        | TensorFlow Lite Micro int8 MLP that approximates sin(x).  |
|                  | ~46 µs / inference @ 170 MHz. Demonstrates TinyML on M4   |
|                  | (no NPU). Requires the optional west group — see          |
|                  | `apps/ai_sine/README.md` and `docs/ai-on-g474re.md`.      |
| `ai_anomaly`     | Autoencoder-based anomaly detection on a synthetic signal.|
|                  | Demonstrates the unsupervised TinyML pattern. Inject      |
|                  | faults via shell, watch reconstruction-error score spike  |
|                  | 20–200×. ~130 µs / inference. See `apps/ai_anomaly/README.md`. |
| `ai_bms`         | Battery-cycle anomaly detection trained on the NASA PCoE  |
|                  | Battery Dataset. AE generalizes to a held-out cell        |
|                  | (B0018) it never saw — flags aged cycles 7–9× higher than |
|                  | healthy. Real-data version of `ai_anomaly`. ~157 µs.      |
| `ai_bms_soh`     | Battery SOH (state-of-health) **regression** on the same  |
|                  | NASA data. Predicts capacity in Ah (not anomaly score).   |
|                  | MAE ~0.11 Ah on holdout cell. ~105 µs / inference.        |

## Why not just modify `samples/basic/blinky`?

Samples under `zephyr/samples/` belong to the upstream Zephyr tree and would
be lost on `west update`. User code lives here, in version-controlled
territory under our own repo.
