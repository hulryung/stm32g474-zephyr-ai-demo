# ai_bms_persistence

**State persistence + OCV recalibration policy for a BMS.**

A real BMS reboots — at every key cycle, every brown-out, every firmware
update. State (SOC, capacity, EKF covariances, cycle count) must survive
those reboots, and the system must know **when not to trust the saved
state** and re-anchor itself from a fresh OCV reading instead.

This app demonstrates both halves on real STM32 flash.

## What's in here

1. **NVS persistence** via Zephyr's `settings` subsystem (NVS backend).
   Saves a versioned `bms_persisted_state` struct to the board's
   `storage` partition — the same flash area a production BMS uses.
2. **OCV recalibration policy** — when the board has been off for more
   than `REST_THRESHOLD_SEC` (10 minutes), the OCV reading at boot is
   blended with the saved SOC. Below the threshold, trust the saved
   value (cell hasn't relaxed enough for OCV to be meaningful).
3. **Schema versioning** — refuses to load saved state from an older
   schema, so firmware upgrades don't get confused.

## Measured on the live board

```
g474> persist save 73.5 1.65 142
saved: SOC=73.50% Q=1.6500Ah

g474> persist info
  schema version : 1
  SOC (saved)    : 73.50 %
  Q estimate     : 1.6500 Ah
  cycles done    : 142
  uptime at save : 4175 ms

g474> persist load 60                  # short rest
  rest 60 s < threshold 600 s → trust saved SOC
  active SOC = 73.50 %                 (unchanged)

g474> persist load 3600 3.732          # long rest + V terminal
  rest 3600 s ≥ threshold 600 s → recalibrate from OCV
  V terminal at rest = 3.7320 V → OCV-derived SOC = 45.00 %
  saved SOC = 73.50 %
  blended SOC (0.3*saved + 0.7*OCV) = 53.55 %
  saved-vs-OCV diff = +28.50 % (drift detector)
```

The blend coefficient (0.3 saved + 0.7 OCV) is a tunable. Tighter blend
toward OCV gives faster recovery from drift but worse rejection of
single-shot OCV measurement noise.

## Boot flow

```
power-on
   │
   ▼
settings_subsys_init
   │
   ▼
read bms_persisted_state from NVS
   │
   ├── version mismatch? → discard, treat as first boot
   │
   ▼
boot_count++
   │
   ▼
how long was the BMS off?            ◀── from RTC (we don't have one
   │                                      here — demo uses shell input)
   ├── < 10 min : trust saved SOC
   ├── 10 min – 1 day : blend (0.3 saved + 0.7 OCV)
   └── > 1 day : OCV only — saved Q kept but SOC re-anchored
   │
   ▼
EKF init with chosen SOC + saved Q + saved P matrix (×1.5 — let it
                                                       loosen up after rest)
```

## Shell commands

| Command | What it does |
|---------|--------------|
| `persist info`                           | Show saved state + boot count |
| `persist save <soc%> <Q_ah> [cycles]`    | Save synthetic state |
| `persist load <rest_sec> [V_now]`        | Simulate key-on after rest |
| `persist clear`                          | Erase saved state (factory reset) |
| `persist demo`                           | Walk through the full policy |

## Storage partition

Re-uses the board's existing `storage_partition` from the upstream
`nucleo_g474re.dts` — 6 KB at flash offset `0x7e800`. NVS handles wear
leveling within that area; you can save/clear thousands of times per
day for the lifetime of the cell without flash wear-out concerns.

## Footprint

| Region | Used   | %     |
|--------|--------|-------|
| FLASH  | 78 KB  | 15 %  |
| RAM    | 18 KB  | 14 %  |

## Production extensions

Real-world additions:
1. **RTC-based rest timer** — STM32G4 has an RTC that runs from VBAT.
   Save (uptime, wallclock) at shutdown, compare to current wallclock at
   boot to compute true rest duration without a host saying "it's been
   1 hour".
2. **Crash-safe save** — current code saves on shell command. Production
   saves on every `key-off` interrupt and periodically (every 60 s)
   while running. Use NVS sector-rotation to survive a brown-out
   mid-write.
3. **Saved EKF P matrix decay** — after a long rest, the EKF should
   *forget* part of its certainty (reset P₀₀ from saved value back
   toward 1.0). We don't apply that here.
4. **Cell-swap detection** — sudden Q_estimate jump on boot is suspect.
   Compare saved last-known V_terminal with current V; if they differ
   massively without a key cycle in between, flag a service event.
