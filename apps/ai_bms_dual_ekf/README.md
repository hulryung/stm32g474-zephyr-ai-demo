# ai_bms_dual_ekf

**Dual EKF — SOC + capacity (Q) co-estimation across cell life.**

A real BMS that uses a fixed `Q_nominal` quietly accumulates SOC error as the
cell ages. Production solves this with a **second, slower EKF** that updates
the capacity estimate at the end of every full discharge cycle. After a few
cycles the SOC EKF is using the right denominator again.

This app demonstrates that on the live board: NASA B0005's 168 discharge
cycles are replayed in order, and the dual-EKF tracks the actual capacity
fade from 1.85 → 1.27 Ah.

## Measured on the live board

```
g474> dekf info
  SOC          : 100.00 %
  Q estimate   : 1.8500 Ah   (initial guess)
  SOH          : 100.0 %
  P_Q          : 0.0500

g474> dekf run
  cycle  Q_actual     Q_estim      error        SOH
  0      1.8565       1.8096       -0.0469      97.8%
  20     1.8474       1.7607       -0.0867      95.2%
  40     1.7679       1.7240       -0.0439      93.2%
  60     1.6849       1.6682       -0.0167      90.2%
  80     1.5598       1.5687       +0.0089      84.8%
  100    1.4804       1.4755       -0.0049      79.8%
  120    1.4383       1.4000       -0.0383      75.7%
  140    1.3442       1.3318       -0.0124      72.0%
  160    1.3034       1.2848       -0.0186      69.5%
  166    1.3090       1.2699       -0.0391      68.6%

Final: Q_estim=1.2699 Ah  Q_actual=1.3090 Ah  err=-0.0391 Ah
```

So the capacity tracker:
- Underestimates Q by ~5 % at the start (covariance still tight on initial
  guess of 1.85 Ah)
- Catches up by cycle 60-100 where its error drops below 2 %
- Stays within 4 % through end of life (cell at 1.30 Ah)
- Final SOH = 68.6 % of nominal — matches the lab measurement closely

## Why this matters

The first 6 BMS apps in this repo (`ai_bms`, `ai_bms_soh`, `ai_bms_rul`,
`ai_bms_soc`) all assumed a fixed Q_nominal. Useful for demos, but a real
deployment needs Q to track the cell. Dual-EKF is the standard pattern; this
app shows it working end-to-end on M4 with NASA data.

## How it works

```
                  ┌──────────────────────────────────────────┐
                  │ FAST EKF — every step                    │
   V, I, T  ────▶ │ state = [SOC, V_RC]                      │ ─▶ SOC, V_RC
                  │ Uses Q_estimate from slow EKF below      │
                  └─────────────┬────────────────────────────┘
                                │ accumulates Ah
                                ▼
                  ┌──────────────────────────────────────────┐
                  │ SLOW EKF — once per full discharge       │
   end-of-cycle  │ state = [Q]                              │ ─▶ Q_estimate, SOH
   trigger ────▶ │ measurement = (ΔSOC × Q_est / 100)        │
                  │            vs observed Ah                 │
                  └──────────────────────────────────────────┘
```

The slow EKF's "measurement equation" is the discrepancy between
predicted Ah (= ΔSOC × Q_estimate / 100) and observed Ah (the integrator
in `ah_passed_in_cycle`). After SOC drops by ~95% (a near-full discharge),
the slow EKF gets one strong update that nudges Q in the right direction.

## Shell commands

| Command | What it does |
|---------|--------------|
| `dekf info`        | Current state + covariances |
| `dekf reset`       | Back to fresh-cell defaults (Q=1.85 Ah, SOC=100 %) |
| `dekf run`         | Replay all 84 cycles, summary log |
| `dekf cycle <n>`   | Run only cycle n |
| `dekf table`       | Per-cycle Q_actual vs Q_estimate (84 rows) |

## Build

```bash
.venv-ml/bin/python apps/ai_bms_dual_ekf/train/extract.py
west build -p auto -b nucleo_g474re apps/ai_bms_dual_ekf -d build/ai_bms_dual_ekf
west flash -r openocd -d build/ai_bms_dual_ekf
```

NASA PCoE data must be at `datasets/nasa-pcoe/B0005.mat`.

## Footprint

| Region | Used   | %     |
|--------|--------|-------|
| FLASH  | 99 KB  | 19 %  |
| RAM    | 18 KB  | 14 %  |

## Production extensions

This is still a demo. Real Q-tracking adds:
1. **Skip partial cycles** — only update Q when ΔSOC is large (we do this).
2. **Outlier rejection** — discard a cycle if its Ah measurement is wildly
   off (e.g. user yanked the connector mid-discharge).
3. **Sliding window of recent Q estimates** — moving median resists single
   bad cycles dragging Q.
4. **Charge-side measurement** — observe Ah _into_ the cell during charge
   too; gives 2× the update rate.
