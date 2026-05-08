# STM32G474RE Nucleo + Zephyr RTOS

Reproducible Zephyr west workspace for the STM32G474RE Nucleo-64. VSCode +
Cortex-Debug + OpenOCD/ST-Link. macOS native toolchain.

This repo is a **template**: it holds only the user-managed pieces of the
workspace (VSCode configs, `debug.conf` overlay, helper scripts). The Zephyr
source tree, west modules, SDK, and Python venv are **not committed** — they
are regenerable from the steps below. This file is the canonical setup guide
for both humans and AI agents (Claude Code, Cursor, etc.).

---

## Target hardware

- **Board:** ST Nucleo-G474RE
- **MCU:** STM32G474RET6 — Cortex-M4F, 170 MHz, 512 KB Flash, 128 KB RAM
- **Debugger:** onboard ST-Link V3 (USB VID:PID `0483:374E`)
- **Zephyr board name:** `nucleo_g474re`
- **OpenOCD configs:** `interface/stlink.cfg` + `target/stm32g4x.cfg`
- **User LED:** LD2 on PA5 → mapped as `led0` alias in board DTS
- **Mass storage when connected:** `/Volumes/NOD_G474RE`
- **Serial (ST-Link VCP):** `/dev/cu.usbmodem*` @ 115200

## Host environment (verified)

- macOS 26.4 / Apple Silicon (arm64)
- Homebrew at `/opt/homebrew`
- VSCode + `code` CLI on PATH
- ~10 GB free disk (workspace ≈ 6.3 GB, SDK ≈ 1.5 GB)

---

## Quickstart for AI agents

**Before running any setup step**, run this state check first:

```bash
cd ~/dev/zephyr
echo "venv:    $(test -x .venv/bin/west && echo OK || echo MISSING)"
echo "manifest:$(test -d zephyr && echo OK || echo MISSING)"
echo "modules: $(test -d modules && test -d bootloader && echo OK || echo MISSING)"
echo "sdk:     $(test -d ~/zephyr-sdk-1.0.1 && echo OK || echo MISSING)"
echo "openocd: $(command -v openocd >/dev/null && echo OK || echo MISSING)"
```

- All `OK` → skip to **Daily use** below.
- Any `MISSING` → run only the corresponding section in **One-time setup**.

When flashing or debugging breaks, check **Gotchas** at the end before debugging
deeply — the issues there are not bugs, they are documented quirks of this
toolchain combination on macOS.

---

## One-time setup

### 1. Homebrew dependencies

```bash
brew install cmake ninja gperf python@3 ccache dtc libmagic wget openocd stlink
```

### 2. Python venv + west

```bash
cd ~/dev/zephyr
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install west
```

### 3. west workspace (manifest + modules)

This repo is itself the west workspace root (in-place layout — `zephyr/` is a
sibling of `.vscode/`, not nested under a `zephyrproject/` dir).

```bash
cd ~/dev/zephyr
.venv/bin/west init -m https://github.com/zephyrproject-rtos/zephyr --mr main .
.venv/bin/west update --narrow -o=--depth=1
```

`--narrow -o=--depth=1` keeps shallow clones — saves ~3 GB and ~10 minutes.

### 4. Zephyr Python deps

```bash
.venv/bin/west packages pip --install
```

### 5. Zephyr SDK — ARM toolchain only

```bash
.venv/bin/west sdk install -t arm-zephyr-eabi
```

Installs to `~/zephyr-sdk-1.0.1/`. Includes:
- GCC: `~/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-{gcc,gdb,objdump,nm}`
- OpenOCD (bundled): `~/zephyr-sdk-1.0.1/hosttools/usr/bin/openocd`
- OpenOCD scripts dir: `~/zephyr-sdk-1.0.1/hosttools/opt/openocd/share/openocd/scripts`

CMake auto-discovers the SDK via `~/.cmake/packages/Zephyr-sdk/`. **No PATH
manipulation needed** for west commands.

### 6. VSCode extensions

Already listed in `.vscode/extensions.json`. Install programmatically:

```bash
code --install-extension marus25.cortex-debug \
     --install-extension ms-vscode.cpptools \
     --install-extension mcu-debug.peripheral-viewer \
     --install-extension mcu-debug.rtos-views
```

---

## Daily use

### Activate environment in any shell

```bash
source ~/dev/zephyr/zephyr-env.sh
# Activates .venv, exports ZEPHYR_BASE and ZEPHYR_SDK_INSTALL_DIR
```

### Build + flash blinky

```bash
west build -p auto -b nucleo_g474re zephyr/samples/basic/blinky -d build/blinky
west flash -r openocd -d build/blinky
```

> **Always pass `-r openocd`.** See Gotcha A.

### Debug build (recommended for stepping through code)

```bash
west build -p always -b nucleo_g474re zephyr/samples/basic/blinky \
    -d build/blinky -- -DEXTRA_CONF_FILE=$(pwd)/debug.conf
west flash -r openocd -d build/blinky
```

`debug.conf` enables `CONFIG_DEBUG_OPTIMIZATIONS=y` (`-Og`) and thread info.
Required for sane source-line debugging — see Gotcha B.

### VSCode debug

1. Open this folder in VSCode (`code ~/dev/zephyr`).
2. Run-and-Debug panel → pick one of:
   - **`Cortex-Debug: blinky (OpenOCD + ST-Link)`** — fast build, breaks at
     `main` (note: source view will show `device.h:57` due to inlining;
     CALL STACK shows `main@…` — see Gotcha B).
   - **`Cortex-Debug: blinky (debug build, stop at main.c first line)`** —
     builds with `debug.conf`, breaks at `main.c:32` cleanly.
   - **`Cortex-Debug: blinky (attach, no flash)`** — attach to a board that
     already has firmware on it.
3. F5 to launch. F10 step-over, F11 step-into, Shift+F5 stop.
4. RTOS thread view: in the left sidebar, **XRTOS** panel from the
   `mcu-debug.rtos-views` extension. Or in Debug Console: `-exec info threads`.

---

## Repository contents

| Path              | Purpose                                                       |
|-------------------|---------------------------------------------------------------|
| `apps/`           | User-developed Zephyr applications (see `apps/README.md`)     |
| `.vscode/`        | tasks, launch, settings, extensions for nucleo_g474re         |
| `debug.conf`      | Build overlay for `-Og` + thread info (debugging)             |
| `zephyr-env.sh`   | Sources venv + sets `ZEPHYR_BASE`, `ZEPHYR_SDK_INSTALL_DIR`   |
| `.gitignore`      | Excludes west trees, venv, build, SDK                         |

### Application layout

User applications live under `apps/<name>/`, completely separate from the
upstream Zephyr tree (`zephyr/`, `modules/`, `bootloader/`, `tools/`) which
is treated as a read-only dependency managed by `west`. This keeps user code
in version control while letting `west update` freely rewrite the dependency
trees.

Bundled apps:
- **`apps/shell_monitor/`** — UART shell on LPUART1 (ST-Link VCP) with
  built-in monitoring commands (`kernel thread list`, `device list`, …)
  plus a custom `sys info|uptime|led` command tree. LD2 1 Hz heartbeat.
- **`apps/ai_sine/`** — TensorFlow Lite Micro on Cortex-M4. Int8-quantized
  MLP approximating `sin(x)`. Measured ~46 µs / inference @ 170 MHz with
  CMSIS-NN. See `docs/ai-on-g474re.md` for the full feasibility report.

To add a new app, copy an existing one and rename. See `apps/README.md` for
the convention.

### Documentation

| Path                          | What's in it                              |
|-------------------------------|-------------------------------------------|
| `apps/<name>/README.md`       | How to build, flash, and use that app     |
| `apps/README.md`              | The `apps/` convention itself             |
| `docs/ai-on-g474re.md`        | TinyML feasibility report with measured numbers |

### Not in repo (regenerable)

| Path                       | Created by                                         |
|----------------------------|----------------------------------------------------|
| `zephyr/`, `modules/`,     | `west update`                                      |
| `bootloader/`, `tools/`    |                                                    |
| `.west/`                   | `west init`                                        |
| `.venv/`                   | `python3 -m venv .venv`                            |
| `build/`                   | `west build` / cmake                               |
| `~/zephyr-sdk-1.0.1/`      | `west sdk install` (lives outside repo)            |

---

## Gotchas

These are documented from real failures hit during initial setup. Each one is a
real "wasted 30 minutes" trap on macOS — read before debugging.

### A. Default flash runner is `stm32cubeprogrammer` (not installed)

`zephyr/boards/st/nucleo_g474re/board.cmake` sets `stm32cubeprogrammer` as the
default flash runner. STM32CubeProgrammer is not installed on this machine,
so `west flash` without `-r openocd` fails with:

```
FATAL ERROR: required program /Applications/STMicroelectronics/.../STM32_Programmer_CLI not found
```

**Fix:** always `west flash -r openocd …`. The `.vscode/tasks.json` task
`Zephyr: Flash (blinky / openocd)` does this; the launch configs use OpenOCD
directly via Cortex-Debug.

### B. `runToEntryPoint: main` shows source in `device.h:57 compiler_barrier()`

With default `-Os` (size optimization) builds, GCC inlines `gpio_is_ready_dt()`
→ `device_is_ready()` (which contains `compiler_barrier()`) into `main()`. The
**first PC of `main`'s prologue** has its DWARF line-table entry mapped to the
inlined `compiler_barrier()` call site, so the source view jumps to
`include/generated/zephyr/syscalls/device.h:57` even though you are stopped at
the entry of user `main`.

`compiler_barrier()` itself (defined at `include/zephyr/toolchain/gcc.h:619`)
is a macro emitting `__asm__ __volatile__("" ::: "memory")` — **zero machine
instructions**, just a compiler memory-reorder hint. It is not a function and
the debugger is not really "stopped at" a barrier; the line table simply
points there.

Verification that you're actually at `main`:
- CALL STACK panel top frame shows `main@0x080004a0` (or similar).
- VARIABLES shows `main`'s locals (`ret`, `led_state`).
- Stepping (F10) eventually lands on real `main.c` lines.

**Fix (recommended):** build with `debug.conf` (`CONFIG_DEBUG_OPTIMIZATIONS=y`)
to disable inlining; use the `(debug build, stop at main.c first line)` launch
config.

### C. `"rtos": "Zephyr"` in launch.json causes OpenOCD to quit

OpenOCD 0.12.0 (the version bundled with Zephyr SDK 1.0.1) has a Zephyr RTOS
plugin that, on macOS, prints

```
Please build Zephyr with CONFIG_OPENOCD option set
```

at GDB connect time **even when** the build correctly exports
`_kernel_thread_info_offsets`, `_kernel_openocd_offsets`, etc. (verified via
`arm-zephyr-eabi-nm zephyr.elf | grep _kernel_openocd`). Shortly after, the
GDB server quits with `OpenOCD: GDB Server Quit Unexpectedly`.

The misleading message is misleading: there is no `CONFIG_OPENOCD` Kconfig in
modern Zephyr (the relevant option is `CONFIG_DEBUG_THREAD_INFO`, which is
already enabled via `debug.conf`). The plugin appears to fail at GDB-connect
time due to symbol-resolution timing.

**Fix:** remove `"rtos": "Zephyr"` from all launch configs. Use the
`mcu-debug.rtos-views` extension's **XRTOS** panel for thread visualization
instead — it reads the same offsets via GDB after the session is running, and
does not interact with OpenOCD's RTOS plugin.

### D. Zephyr SDK toolchain is not on PATH

Tools at `~/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/` are **not** added to
shell PATH. west invokes them via the CMake package registry
(`~/.cmake/packages/Zephyr-sdk/`). For manual use (e.g. `arm-zephyr-eabi-nm`,
`arm-zephyr-eabi-objdump`), reference them by absolute path, or add to PATH
yourself. The `zephyr-env.sh` helper deliberately does **not** add them, to
mirror west's invocation environment.

### E. Workspace is in-place at repo root, not under `zephyrproject/`

Standard Zephyr docs use `~/zephyrproject/` as the workspace root with
`zephyr/` nested below. **This repo is itself the workspace root** —
`.west/`, `zephyr/`, `modules/` are all siblings of `.vscode/`. When following
upstream Zephyr docs, treat `~/dev/zephyr` as their `~/zephyrproject`.

### F. `west update` is heavy

First-time `west update` clones ~60 modules (~3 GB even with shallow clones,
~6.3 GB workspace total). Allow 5–10 minutes on a fast connection. Use
`--narrow -o=--depth=1` to skip historical commits.

---

## Recreating from a fresh clone

```bash
git clone <repo-url> ~/dev/zephyr
cd ~/dev/zephyr

# 1. brew deps (skip if already installed)
brew install cmake ninja gperf python@3 ccache dtc libmagic wget openocd stlink

# 2. venv + west
python3 -m venv .venv
.venv/bin/pip install --upgrade pip west

# 3. workspace — note: NOT `west init -l zephyr` because zephyr/ doesn't exist yet
.venv/bin/west init -m https://github.com/zephyrproject-rtos/zephyr --mr main .
.venv/bin/west update --narrow -o=--depth=1

# 4. python deps + SDK
.venv/bin/west packages pip --install
.venv/bin/west sdk install -t arm-zephyr-eabi

# 5. verify by building blinky
source ./zephyr-env.sh
west build -p auto -b nucleo_g474re zephyr/samples/basic/blinky -d build/blinky
west flash -r openocd -d build/blinky    # LD2 should blink at 1 Hz
```

If the LED blinks, you are done.

---

## Pinning notes for reproducibility

- Zephyr SDK: 1.0.1 (installed by `west sdk install` at the time of this
  setup). To pin, replace step 5 with a specific version download from
  https://github.com/zephyrproject-rtos/sdk-ng/releases.
- Zephyr manifest: `--mr main`. For a stable release, replace with e.g.
  `--mr v4.0.0` and re-run `west update`. The `.vscode/launch.json` paths
  assume `~/zephyr-sdk-1.0.1/`; bump the version in `settings.json` and
  `launch.json` if you upgrade the SDK.
