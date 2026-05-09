#!/usr/bin/env python3
"""Train an autoencoder on NASA PCoE Battery Dataset discharge curves.

Strategy:
  - Treat early-life discharge curves (capacity > NORMAL_CAPACITY_THRESH)
    as "normal" — what the AE learns to reconstruct.
  - Test against late-life curves (capacity < ANOMALY_CAPACITY_THRESH).
    These should produce notably higher reconstruction error.
  - Bake a few representative cycles (one early, one mid, one late) into
    the firmware so the on-device demo can replay them.

Run:
    apps/ai_bms/train/train.py    # uses ../../../.venv-ml/bin/python

Outputs:
    model.tflite                — quantized model
    ../src/model.cpp / .hpp     — C array for the firmware
    ../src/cycles_db.cpp        — embedded sample cycles for demo replay

Dataset must be unpacked at:
    apps/ai_bms/train/data/5. Battery Data Set/B000{5,6,7,18}.mat
"""

from __future__ import annotations

import pathlib
import sys

import numpy as np
import scipy.io as sio
import tensorflow as tf

# ---------- config ----------------------------------------------------------

WINDOW                  = 32          # samples per discharge curve
TRAIN_CELLS             = ["B0005", "B0006", "B0007"]
HOLDOUT_CELL            = "B0018"     # not used for training; for evaluation
NORMAL_CAPACITY_THRESH  = 1.75        # Ah — only cycles with capacity ABOVE this train the AE
ANOMALY_CAPACITY_THRESH = 1.50        # Ah — eval set: cycles below this are 'aged'
BOTTLENECK              = 4
EPOCHS                  = 80
BATCH                   = 32
SEED                    = 42

HERE       = pathlib.Path(__file__).parent
DATA_DIR   = HERE.parent.parent.parent / "datasets" / "nasa-pcoe"
TFLITE_OUT = HERE / "model.tflite"
CPP_OUT    = HERE.parent / "src" / "model.cpp"
HPP_OUT    = HERE.parent / "src" / "model.hpp"
CYCLES_OUT = HERE.parent / "src" / "cycles_db.cpp"
CYCLES_HDR = HERE.parent / "src" / "cycles_db.h"

# ---------- data loading ----------------------------------------------------

def load_discharge_cycles(cell_id: str):
    """Yield (voltage_curve_resampled, capacity, cycle_index) per discharge cycle."""
    mat = sio.loadmat(DATA_DIR / f"{cell_id}.mat",
                       squeeze_me=True, struct_as_record=False)
    cycles = mat[cell_id].cycle
    disch_idx = 0
    for i, cy in enumerate(cycles):
        if cy.type != "discharge":
            continue
        v = np.asarray(cy.data.Voltage_measured, dtype=np.float32)
        t = np.asarray(cy.data.Time, dtype=np.float32)
        cap = float(cy.data.Capacity)
        if v.size < 8:
            continue   # skip degenerate cycles
        # Resample to WINDOW points uniformly across normalized time [0, 1]
        t_norm = (t - t[0]) / max(t[-1] - t[0], 1e-6)
        u = np.linspace(0.0, 1.0, WINDOW, dtype=np.float32)
        v_rs = np.interp(u, t_norm, v).astype(np.float32)
        yield v_rs, cap, disch_idx
        disch_idx += 1

def collect(cells):
    curves, caps, labels = [], [], []
    for cid in cells:
        n = 0
        for v, cap, _ in load_discharge_cycles(cid):
            curves.append(v)
            caps.append(cap)
            labels.append(cid)
            n += 1
        print(f"  {cid}: {n} discharge cycles "
              f"(cap range {min(c for v,c,_ in load_discharge_cycles(cid)):.3f} → "
              f"{max(c for v,c,_ in load_discharge_cycles(cid)):.3f} Ah)")
    return np.stack(curves), np.array(caps, dtype=np.float32), np.array(labels)

# ---------- normalize -------------------------------------------------------

# Voltage curves are in volts ([2.5..4.2] roughly). We normalize to [-1, 1] so
# the AE's input distribution roughly matches what it was trained on.
V_MIN, V_MAX = 2.4, 4.3

def normalize(v: np.ndarray) -> np.ndarray:
    return (v - V_MIN) / (V_MAX - V_MIN) * 2.0 - 1.0

def denormalize(x: np.ndarray) -> np.ndarray:
    return (x + 1.0) / 2.0 * (V_MAX - V_MIN) + V_MIN

# ---------- model -----------------------------------------------------------

def build_autoencoder() -> tf.keras.Model:
    inp = tf.keras.Input(shape=(WINDOW,), dtype=tf.float32)
    x = tf.keras.layers.Dense(24, activation="relu")(inp)
    x = tf.keras.layers.Dense(12, activation="relu")(x)
    z = tf.keras.layers.Dense(BOTTLENECK, activation="relu")(x)
    x = tf.keras.layers.Dense(12, activation="relu")(z)
    x = tf.keras.layers.Dense(24, activation="relu")(x)
    out = tf.keras.layers.Dense(WINDOW, activation="linear")(x)
    return tf.keras.Model(inp, out, name="bms_ae")

def to_tflite_int8(model, repr_data):
    def repr_gen():
        for i in range(min(500, len(repr_data))):
            yield [repr_data[i:i + 1].astype(np.float32)]
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = repr_gen
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    return converter.convert()

# ---------- emit C ----------------------------------------------------------

def write_model_c(tflite_bytes, cpp, hpp):
    n = len(tflite_bytes)
    cpp.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "/* Generated by apps/ai_bms/train/train.py — DO NOT EDIT */",
        "#include \"model.hpp\"",
        "",
        f"alignas(8) const unsigned char g_model[{n}] = {{",
    ]
    for i in range(0, n, 12):
        chunk = tflite_bytes[i:i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append(f"const unsigned int g_model_len = {n};")
    cpp.write_text("\n".join(lines) + "\n")
    hpp.write_text(
        "/* Generated — DO NOT EDIT */\n"
        "#ifndef MODEL_HPP\n#define MODEL_HPP\n"
        "extern const unsigned char g_model[];\n"
        "extern const unsigned int  g_model_len;\n"
        "#endif\n"
    )

def write_cycles_c(samples, cpp, hpp):
    """Bake the chosen sample cycles + their metadata into a flat C array.

    samples: list of dicts { 'name', 'cell', 'idx', 'capacity', 'curve_norm' (length WINDOW float) }
    """
    cpp.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "/* Generated by apps/ai_bms/train/train.py — DO NOT EDIT */",
        "#include \"cycles_db.h\"",
        "",
        f"const unsigned int g_cycle_count = {len(samples)};",
        "const struct bms_cycle g_cycles[] = {",
    ]
    for s in samples:
        lines.append("    {")
        lines.append(f"        .name = \"{s['name']}\",")
        lines.append(f"        .cell = \"{s['cell']}\",")
        lines.append(f"        .index = {s['idx']},")
        lines.append(f"        .capacity_ah = {s['capacity']:.4f}f,")
        lines.append("        .voltage = {")
        for v in s['curve_norm']:
            lines.append(f"            {v:+.6f}f,")
        lines.append("        },")
        lines.append("    },")
    lines.append("};")
    cpp.write_text("\n".join(lines) + "\n")

    hpp.write_text(
        "/* Generated — DO NOT EDIT */\n"
        "#ifndef CYCLES_DB_H\n#define CYCLES_DB_H\n"
        "#include <stddef.h>\n\n"
        "#define BMS_CYCLE_LEN 32\n\n"
        "struct bms_cycle {\n"
        "    const char *name;\n"
        "    const char *cell;\n"
        "    unsigned int index;\n"
        "    float capacity_ah;\n"
        "    float voltage[BMS_CYCLE_LEN];\n"
        "};\n\n"
        "extern const unsigned int g_cycle_count;\n"
        "extern const struct bms_cycle g_cycles[];\n"
        "#endif\n"
    )

# ---------- main ------------------------------------------------------------

def main() -> int:
    if not DATA_DIR.exists():
        print(f"ERROR: dataset dir not found: {DATA_DIR}", file=sys.stderr)
        return 1

    np.random.seed(SEED)
    tf.random.set_seed(SEED)

    print("Loading discharge cycles…")
    train_curves, train_caps, train_labels = collect(TRAIN_CELLS)
    holdout_curves, holdout_caps, holdout_labels = collect([HOLDOUT_CELL])

    train_curves_n  = normalize(train_curves)
    holdout_curves_n = normalize(holdout_curves)

    # Split training data by capacity threshold ------------------------------
    normal_mask  = train_caps > NORMAL_CAPACITY_THRESH
    anomaly_mask = train_caps < ANOMALY_CAPACITY_THRESH

    print()
    print(f"Total training-cell cycles: {len(train_caps)}")
    print(f"  'normal'  (cap > {NORMAL_CAPACITY_THRESH} Ah): {normal_mask.sum()}")
    print(f"  'anomaly' (cap < {ANOMALY_CAPACITY_THRESH} Ah): {anomaly_mask.sum()}")
    print(f"  in-between (used for nothing): {(~normal_mask & ~anomaly_mask).sum()}")
    print(f"Holdout cell ({HOLDOUT_CELL}): {len(holdout_caps)} cycles, "
          f"cap range {holdout_caps.min():.3f} → {holdout_caps.max():.3f} Ah")

    x_train = train_curves_n[normal_mask]
    print(f"\nTraining set: {x_train.shape}")

    model = build_autoencoder()
    model.compile(optimizer=tf.keras.optimizers.Adam(1e-3), loss="mse")
    print(model.summary())

    print(f"Training for {EPOCHS} epochs…")
    history = model.fit(x_train, x_train,
                        validation_split=0.1,
                        epochs=EPOCHS, batch_size=BATCH, verbose=2)

    # Evaluate ---------------------------------------------------------------
    def per_curve_mse(model, x):
        recon = model.predict(x, verbose=0)
        return np.mean((x - recon) ** 2, axis=1)

    train_normal_mse  = per_curve_mse(model, train_curves_n[normal_mask])
    train_anomaly_mse = per_curve_mse(model, train_curves_n[anomaly_mask])
    holdout_normal_mse = per_curve_mse(
        model,
        holdout_curves_n[holdout_caps > NORMAL_CAPACITY_THRESH]
    )
    holdout_anomaly_mse = per_curve_mse(
        model,
        holdout_curves_n[holdout_caps < ANOMALY_CAPACITY_THRESH]
    )

    print()
    print("Reconstruction MSE (in normalized voltage units):")
    print(f"  TRAIN cells normal (cap>{NORMAL_CAPACITY_THRESH}): "
          f"mean={train_normal_mse.mean():.5f}  p95={np.quantile(train_normal_mse, 0.95):.5f}")
    print(f"  TRAIN cells aged   (cap<{ANOMALY_CAPACITY_THRESH}): "
          f"mean={train_anomaly_mse.mean():.5f}  p05={np.quantile(train_anomaly_mse, 0.05):.5f}")
    print(f"  HOLDOUT {HOLDOUT_CELL} normal: mean={holdout_normal_mse.mean():.5f}")
    print(f"  HOLDOUT {HOLDOUT_CELL} aged  : mean={holdout_anomaly_mse.mean():.5f}")
    print(f"  separation ratio (TRAIN aged / TRAIN normal): "
          f"{train_anomaly_mse.mean() / max(train_normal_mse.mean(), 1e-9):.1f}x")

    # Pick representative cycles to bake in ----------------------------------
    # Use holdout cell so the firmware demos cycles the AE never saw.
    selected = []
    cap_targets = [
        ("early",  holdout_caps[0],          0),
        ("mid",    holdout_caps[len(holdout_caps) // 2],  len(holdout_caps) // 2),
        ("aged",   holdout_caps[-1],         len(holdout_caps) - 1),
    ]
    for name, cap, idx in cap_targets:
        selected.append({
            "name":       name,
            "cell":       HOLDOUT_CELL,
            "idx":        int(idx),
            "capacity":   float(cap),
            "curve_norm": holdout_curves_n[idx].tolist(),
        })
    print()
    print("Selected cycles for firmware replay (from holdout cell):")
    for s in selected:
        print(f"  {s['name']:<6}  cell={s['cell']}  idx={s['idx']:3d}  cap={s['capacity']:.3f} Ah")

    # Quantize + emit --------------------------------------------------------
    print()
    print("Quantizing to int8 + converting to TFLite…")
    tflite_bytes = to_tflite_int8(model, x_train)
    TFLITE_OUT.write_bytes(tflite_bytes)
    print(f"  wrote {TFLITE_OUT}  ({len(tflite_bytes)} bytes)")

    print("Emitting C sources…")
    write_model_c(tflite_bytes, CPP_OUT, HPP_OUT)
    write_cycles_c(selected, CYCLES_OUT, CYCLES_HDR)
    print(f"  wrote {CPP_OUT}")
    print(f"  wrote {CYCLES_OUT}")
    print()
    print("Done. Build the firmware with:")
    print("  west build -p auto -b nucleo_g474re apps/ai_bms -d build/ai_bms")
    return 0

if __name__ == "__main__":
    sys.exit(main())
