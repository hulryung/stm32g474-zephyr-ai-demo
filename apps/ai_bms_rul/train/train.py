#!/usr/bin/env python3
"""Train an MLP that PREDICTS REMAINING USEFUL LIFE (RUL) — i.e. how many
cycles until the cell's capacity drops below an EOL threshold.

This is a *prognostic* problem (predict the future) compared to ai_bms
(detect anomaly now) and ai_bms_soh (estimate current health). RUL
prediction is a step harder because the model has to learn that *trends*
in the discharge curve shape correlate with remaining cycles.

Data: NASA PCoE Battery Dataset. EOL = capacity < 1.5 Ah (chosen so all
four cells reach EOL within their recorded data, which the canonical 1.4
Ah threshold doesn't — B0007 stays right at 1.4).

Run:
    apps/ai_bms_rul/train/train.py

Outputs:
    model.tflite
    ../src/model.cpp / .hpp        — int8 model
    ../src/cycles_db.cpp / .h      — embedded sample cycles + ground-truth RUL
"""

from __future__ import annotations

import pathlib
import sys

import numpy as np
import scipy.io as sio
import tensorflow as tf

# ---------- config ----------------------------------------------------------

WINDOW       = 32
EOL_THR_AH   = 1.5     # capacity below this counts as end-of-life
TRAIN_CELLS  = ["B0005", "B0006", "B0007"]
HOLDOUT_CELL = "B0018"
EPOCHS       = 200
BATCH        = 32
SEED         = 42

V_MIN, V_MAX  = 2.4, 4.3
RUL_MAX       = 150          # normalize RUL to roughly [0, 1] then to [-1, +1]

HERE       = pathlib.Path(__file__).parent
DATA_DIR   = HERE.parent.parent.parent / "datasets" / "nasa-pcoe"
TFLITE_OUT = HERE / "model.tflite"
CPP_OUT    = HERE.parent / "src" / "model.cpp"
HPP_OUT    = HERE.parent / "src" / "model.hpp"
CYCLES_OUT = HERE.parent / "src" / "cycles_db.cpp"
CYCLES_HDR = HERE.parent / "src" / "cycles_db.h"

# ---------- data -----------------------------------------------------------

def load_cell(cell_id):
    """Return a list of (voltage_curve_resampled, capacity, cycle_idx) per
    discharge cycle, in order."""
    mat = sio.loadmat(DATA_DIR / f"{cell_id}.mat",
                       squeeze_me=True, struct_as_record=False)
    out = []
    di = 0
    for cy in mat[cell_id].cycle:
        if cy.type != "discharge":
            continue
        v = np.asarray(cy.data.Voltage_measured, dtype=np.float32)
        t = np.asarray(cy.data.Time, dtype=np.float32)
        cap = float(cy.data.Capacity)
        if v.size < 8:
            continue
        t_norm = (t - t[0]) / max(t[-1] - t[0], 1e-6)
        u = np.linspace(0.0, 1.0, WINDOW, dtype=np.float32)
        v_rs = np.interp(u, t_norm, v).astype(np.float32)
        out.append((v_rs, cap, di))
        di += 1
    return out

def first_eol(caps):
    """Return the cycle index where capacity first drops below EOL_THR_AH,
    or len(caps) if never (RUL = 0 from that point on)."""
    for i, c in enumerate(caps):
        if c < EOL_THR_AH:
            return i
    return len(caps)

def build_dataset(cells):
    """For each cell, label every cycle with its RUL. Returns:
       curves        : (N, WINDOW) float
       ruls          : (N,) float — cycles until EOL (saturates at 0)
       per_cell      : list of dicts for diagnostics
    """
    all_curves = []
    all_ruls   = []
    info = []
    for cid in cells:
        rows = load_cell(cid)
        caps = [r[1] for r in rows]
        eol_idx = first_eol(caps)
        for v_rs, cap, di in rows:
            rul = max(eol_idx - di, 0)
            all_curves.append(v_rs)
            all_ruls.append(rul)
        info.append({"cell": cid, "n_cycles": len(rows), "eol": eol_idx,
                     "min_cap": min(caps), "max_cap": max(caps)})
    return np.stack(all_curves), np.array(all_ruls, dtype=np.float32), info

def normalize_v(v):
    return (v - V_MIN) / (V_MAX - V_MIN) * 2.0 - 1.0

def normalize_rul(r):
    return (r / RUL_MAX) * 2.0 - 1.0

def denormalize_rul(y):
    return (y + 1.0) * 0.5 * RUL_MAX

# ---------- model ----------------------------------------------------------

def build_regressor():
    inp = tf.keras.Input(shape=(WINDOW,), dtype=tf.float32)
    x = tf.keras.layers.Dense(48, activation="relu")(inp)
    x = tf.keras.layers.Dense(24, activation="relu")(x)
    x = tf.keras.layers.Dense(12, activation="relu")(x)
    out = tf.keras.layers.Dense(1, activation="linear")(x)
    return tf.keras.Model(inp, out, name="bms_rul_mlp")

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

# ---------- emit C ---------------------------------------------------------

def write_model_c(tflite_bytes, cpp, hpp):
    n = len(tflite_bytes)
    cpp.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "/* Generated by apps/ai_bms_rul/train/train.py — DO NOT EDIT */",
        "#include \"model.hpp\"",
        "",
        f"alignas(8) const unsigned char g_model[{n}] = {{",
    ]
    for i in range(0, n, 12):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in tflite_bytes[i:i + 12]) + ",")
    lines.append("};")
    lines.append(f"const unsigned int g_model_len = {n};")
    cpp.write_text("\n".join(lines) + "\n")
    hpp.write_text(
        "/* Generated — DO NOT EDIT */\n#ifndef MODEL_HPP\n#define MODEL_HPP\n"
        "extern const unsigned char g_model[];\nextern const unsigned int g_model_len;\n"
        "#endif\n"
    )

def write_cycles_c(samples, cpp, hpp):
    cpp.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "/* Generated — DO NOT EDIT */",
        "#include \"cycles_db.h\"",
        "",
        f"const unsigned int g_cycle_count = {len(samples)};",
        "const struct rul_cycle g_cycles[] = {",
    ]
    for s in samples:
        lines.append("    {")
        lines.append(f"        .name = \"{s['name']}\",")
        lines.append(f"        .cell = \"{s['cell']}\",")
        lines.append(f"        .index = {s['idx']},")
        lines.append(f"        .capacity_ah = {s['cap']:.4f}f,")
        lines.append(f"        .true_rul = {s['rul']},")
        lines.append("        .voltage_norm = {")
        for v in s['curve']:
            lines.append(f"            {v:+.6f}f,")
        lines.append("        },")
        lines.append("    },")
    lines.append("};")
    cpp.write_text("\n".join(lines) + "\n")
    hpp.write_text(
        "/* Generated — DO NOT EDIT */\n#ifndef CYCLES_DB_H\n#define CYCLES_DB_H\n"
        "#include <stddef.h>\n\n"
        "#define RUL_WINDOW 32\n\n"
        "struct rul_cycle {\n"
        "    const char  *name;\n"
        "    const char  *cell;\n"
        "    unsigned int index;\n"
        "    float        capacity_ah;\n"
        "    unsigned int true_rul;     /* ground truth: cycles remaining */\n"
        "    float        voltage_norm[RUL_WINDOW];\n"
        "};\n\n"
        "extern const unsigned int g_cycle_count;\n"
        "extern const struct rul_cycle g_cycles[];\n"
        "#endif\n"
    )

# ---------- main ----------------------------------------------------------

def main():
    if not DATA_DIR.exists():
        print(f"ERROR: NASA data not found at {DATA_DIR}", file=sys.stderr)
        return 1

    np.random.seed(SEED)
    tf.random.set_seed(SEED)

    print(f"EOL threshold: capacity < {EOL_THR_AH} Ah\n")
    print("Loading + labeling cycles for training cells…")
    train_v, train_rul, train_info = build_dataset(TRAIN_CELLS)
    for inf in train_info:
        print(f"  {inf['cell']}: {inf['n_cycles']} cycles, "
              f"cap [{inf['min_cap']:.3f}..{inf['max_cap']:.3f}] Ah, EOL@{inf['eol']}")

    print(f"\nLoading + labeling holdout cell {HOLDOUT_CELL}…")
    holdout_v, holdout_rul, holdout_info = build_dataset([HOLDOUT_CELL])
    for inf in holdout_info:
        print(f"  {inf['cell']}: {inf['n_cycles']} cycles, EOL@{inf['eol']}")

    train_v_n = normalize_v(train_v)
    holdout_v_n = normalize_v(holdout_v)
    train_y = normalize_rul(train_rul)

    print(f"\nTraining set: {train_v_n.shape}  RUL range [{train_rul.min():.0f}..{train_rul.max():.0f}]")
    model = build_regressor()
    model.compile(optimizer=tf.keras.optimizers.Adam(1e-3), loss="mse",
                  metrics=["mae"])
    print(model.summary())

    print(f"Training {EPOCHS} epochs…")
    model.fit(train_v_n, train_y,
              validation_split=0.1,
              epochs=EPOCHS, batch_size=BATCH, verbose=2)

    # Evaluate on holdout cell -------------------------------------------
    pred_n = model.predict(holdout_v_n, verbose=0).flatten()
    pred = denormalize_rul(pred_n)
    pred = np.clip(pred, 0, RUL_MAX)
    err = pred - holdout_rul
    mae = float(np.mean(np.abs(err)))
    rmse = float(np.sqrt(np.mean(err ** 2)))

    print()
    print(f"Holdout {HOLDOUT_CELL} RUL prediction:")
    print(f"  MAE  : {mae:.1f} cycles")
    print(f"  RMSE : {rmse:.1f} cycles")
    print(f"  RUL ground-truth range: [{holdout_rul.min():.0f}..{holdout_rul.max():.0f}] cycles")

    # Pick demo cycles spanning life --------------------------------------
    n = len(holdout_rul)
    selected = []
    for name, idx in [("early", 0),
                      ("mid",   n // 2),
                      ("aged",  n - 1)]:
        selected.append({
            "name":  name,
            "cell":  HOLDOUT_CELL,
            "idx":   int(idx),
            "cap":   float(holdout_v[idx][0] * 0 + holdout_info[0]["min_cap"]
                           if False else  # placeholder unused
                           0.0),  # we'll recompute below
            "rul":   int(holdout_rul[idx]),
            "curve": holdout_v_n[idx].tolist(),
        })
    # recompute capacity per selected cycle
    raw = load_cell(HOLDOUT_CELL)
    for s in selected:
        s["cap"] = float(raw[s["idx"]][1])

    print()
    print("Embedded sample cycles for the firmware:")
    for s in selected:
        print(f"  {s['name']:<6} idx={s['idx']:3d}  cap={s['cap']:.4f} Ah  true_RUL={s['rul']}")

    # Quantize + emit ------------------------------------------------------
    print("\nQuantizing to int8 + converting to TFLite…")
    tflite_bytes = to_tflite_int8(model, train_v_n)
    TFLITE_OUT.write_bytes(tflite_bytes)
    print(f"  wrote {TFLITE_OUT}  ({len(tflite_bytes)} bytes)")

    write_model_c(tflite_bytes, CPP_OUT, HPP_OUT)
    write_cycles_c(selected, CYCLES_OUT, CYCLES_HDR)
    print(f"  wrote {CPP_OUT}")
    print(f"  wrote {CYCLES_OUT}")
    print()
    print("Done. Build:")
    print("  west build -p auto -b nucleo_g474re apps/ai_bms_rul -d build/ai_bms_rul")
    return 0

if __name__ == "__main__":
    sys.exit(main())
