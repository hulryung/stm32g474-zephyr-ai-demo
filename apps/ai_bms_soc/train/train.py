#!/usr/bin/env python3
"""Train + calibrate everything ai_bms_soc needs.

Outputs:
    ../src/soc_calib.cpp / .h  - SOC-OCV table, EKF parameters, embedded test windows
    ../src/model_mlp.cpp / .hpp
    ../src/model_lstm.cpp / .hpp
    ../src/model_hybrid.cpp / .hpp

Approach:
- Use NASA PCoE discharge cycles from datasets/nasa-pcoe/
- Train cells: B0005, B0006, B0007 (different discharge cutoffs but same chemistry)
- Holdout cell: B0018
- Resample each discharge to fixed length, compute true SOC by coulomb counting
- Calibrate SOC-OCV (low-current segments) and ECM RC params (high-current segments)
- Train MLP/LSTM/Hybrid on (V,I,T)→SOC supervised
- Embed three test windows from B0018 (early/mid/aged) for on-board demo

Run:
    apps/ai_bms_soc/train/train.py
"""

from __future__ import annotations

import pathlib
import sys

import numpy as np
import scipy.io as sio
import scipy.optimize as opt
import tensorflow as tf

# ---------- config ----------------------------------------------------------

WINDOW          = 32          # samples per LSTM/MLP input window
TRAIN_CELLS     = ["B0005", "B0006", "B0007"]
HOLDOUT_CELL    = "B0018"
SEED            = 42
BATCH           = 64
EPOCHS_MLP      = 80
EPOCHS_LSTM     = 80
EPOCHS_HYBRID   = 60

# Voltage / current / temperature / SOC normalization ranges
V_MIN, V_MAX    = 2.4, 4.3
I_MIN, I_MAX    = -3.0, 3.0   # NASA discharge is ~-2A; charge ~+1.5A
T_MIN, T_MAX    = 20.0, 45.0
SOC_NORM        = 100.0       # SOC stored as 0..100 percent, model outputs normalized to [-1,+1]

HERE       = pathlib.Path(__file__).parent
DATA_DIR   = HERE.parent.parent.parent / "datasets" / "nasa-pcoe"
SRC_DIR    = HERE.parent / "src"

# ---------- data loading + ground-truth SOC ----------------------------------

def load_discharge_with_soc(cell_id: str):
    """For each discharge cycle of `cell_id`, return resampled
    (V, I, T, true_SOC) sequences of length WINDOW.

    True SOC is computed by coulomb counting against the cycle's recorded
    capacity, starting at 100 % and ending at 0 %. This is the cleanest
    ground-truth available from this dataset (NASA cells are fully charged
    before each discharge cycle, so SOC(t=0) = 100 %).
    """
    mat = sio.loadmat(DATA_DIR / f"{cell_id}.mat",
                      squeeze_me=True, struct_as_record=False)
    out = []
    for cy in mat[cell_id].cycle:
        if cy.type != "discharge":
            continue
        v = np.asarray(cy.data.Voltage_measured, dtype=np.float32)
        i = np.asarray(cy.data.Current_measured, dtype=np.float32)
        T = np.asarray(cy.data.Temperature_measured, dtype=np.float32)
        t = np.asarray(cy.data.Time, dtype=np.float32)
        Q = float(cy.data.Capacity)             # Ah delivered this cycle
        if v.size < 8:
            continue

        # Charge passed up to time t (in Ah, integrating |I|)
        # Note: NASA stores discharge current as negative in some cells, positive in others.
        # We use |I| and assume monotonic discharge (validated by Capacity field).
        dt = np.diff(t, prepend=t[0])
        Ah_passed = np.cumsum(np.abs(i) * dt) / 3600.0
        soc_pct = 100.0 * (1.0 - Ah_passed / max(Q, 1e-3))
        soc_pct = np.clip(soc_pct, 0.0, 100.0)

        # Resample to WINDOW points uniform in normalized time
        u = np.linspace(0.0, 1.0, WINDOW, dtype=np.float32)
        t_norm = (t - t[0]) / max(t[-1] - t[0], 1e-6)
        v_rs = np.interp(u, t_norm, v).astype(np.float32)
        i_rs = np.interp(u, t_norm, i).astype(np.float32)
        T_rs = np.interp(u, t_norm, T).astype(np.float32)
        soc_rs = np.interp(u, t_norm, soc_pct).astype(np.float32)

        out.append({"v": v_rs, "i": i_rs, "T": T_rs,
                    "soc": soc_rs, "capacity": Q})
    return out

def collect(cells):
    rows = []
    for c in cells:
        cs = load_discharge_with_soc(c)
        for r in cs:
            r["cell"] = c
        rows.extend(cs)
    return rows

# ---------- SOC-OCV table ---------------------------------------------------

def build_soc_ocv_table(train_rows, n_bins=21):
    """Average voltage at each SOC bucket, low-current points only.

    For NASA's 2A constant-current discharge there's no truly OCV-quality
    data, so we approximate: take all (V, SOC) pairs where |I| is below a
    threshold (rare during 2A discharge but happens at start/end), bin by
    SOC, average V per bin.

    Result: ocv[k] for SOC = k * (100/(n_bins-1)) percent.
    """
    Vs, SOCs = [], []
    for r in train_rows:
        # Use the very last samples of each discharge (low |I| as it tapers)
        # Plus the very first (rest before discharge start)
        v = r["v"]; i = r["i"]; soc = r["soc"]
        mask = np.abs(i) < 0.5   # below 0.5A
        Vs.extend(v[mask].tolist())
        SOCs.extend(soc[mask].tolist())
        # also include first sample as ~OCV start point
        Vs.append(float(v[0])); SOCs.append(float(soc[0]))
        Vs.append(float(v[-1])); SOCs.append(float(soc[-1]))
    Vs = np.array(Vs); SOCs = np.array(SOCs)

    soc_grid = np.linspace(0.0, 100.0, n_bins)
    ocv_table = np.zeros(n_bins, dtype=np.float32)
    for k, s in enumerate(soc_grid):
        # Choose nearest neighbors weighted by inverse distance (~LWR)
        d = np.abs(SOCs - s) + 1e-3
        w = 1.0 / d
        ocv_table[k] = float(np.sum(w * Vs) / np.sum(w))
    # Enforce monotonic non-decreasing in SOC (typical Li-ion)
    for k in range(1, n_bins):
        if ocv_table[k] < ocv_table[k-1]:
            ocv_table[k] = ocv_table[k-1]
    return soc_grid, ocv_table

# ---------- ECM (R0, R1, C1) least-squares fit ------------------------------

def fit_ecm_params(train_rows, soc_grid, ocv_table):
    """Fit R0, R1, C1 to the discharge voltage trajectories.

    Model:  V_term(t) = OCV(SOC(t)) - i(t)*R0 - V_RC(t)
            dV_RC/dt  = -V_RC/(R1*C1) + i/C1

    We approximate V_RC numerically from per-cycle data and least-squares
    the parameters that minimize prediction error across cycles. Coarse
    fit — production BMS would do this per-cycle; here we just want a
    reasonable single set of params for the EKF demo.
    """
    def predict_v(params, v_seq, i_seq, soc_seq, dt=1.0):
        R0, R1, C1 = params
        if R0 <= 0 or R1 <= 0 or C1 <= 0:
            return np.full_like(v_seq, 1e6)
        v_rc = 0.0
        v_pred = np.zeros_like(v_seq)
        for k in range(len(v_seq)):
            ocv = np.interp(soc_seq[k], soc_grid, ocv_table)
            v_pred[k] = ocv - i_seq[k] * R0 - v_rc
            # Update RC voltage for next step
            v_rc = v_rc + dt * (-v_rc/(R1*C1) + i_seq[k]/C1)
        return v_pred

    # Concatenate all training residuals into one objective
    def residual(params):
        err = []
        for r in train_rows[::5]:    # subsample for speed
            v_pred = predict_v(params, r["v"], r["i"], r["soc"])
            err.extend((r["v"] - v_pred).tolist())
        return np.array(err)

    # Starting point: typical 18650 LCO values
    x0 = np.array([0.05, 0.02, 1500.0])  # R0=50mΩ, R1=20mΩ, C1=1500F (-> τ=30s)
    bounds = ([0.005, 0.001, 100.0], [0.5, 0.2, 50000.0])
    result = opt.least_squares(residual, x0, bounds=bounds, max_nfev=200)
    R0, R1, C1 = result.x
    print(f"  ECM fit: R0={R0*1000:.1f}mΩ, R1={R1*1000:.1f}mΩ, "
          f"C1={C1:.0f}F (τ={R1*C1:.1f}s), final cost={np.sqrt(np.mean(result.fun**2)):.4f}V")
    return float(R0), float(R1), float(C1)

# ---------- ML models -------------------------------------------------------

def normalize_inputs(v, i, T):
    vn = (v - V_MIN) / (V_MAX - V_MIN) * 2 - 1
    in_ = (i - I_MIN) / (I_MAX - I_MIN) * 2 - 1
    Tn = (T - T_MIN) / (T_MAX - T_MIN) * 2 - 1
    return vn, in_, Tn

def normalize_soc(s):
    return s / SOC_NORM * 2 - 1

def denormalize_soc(y):
    return (y + 1) * 0.5 * SOC_NORM

def build_mlp():
    """Simple per-sample MLP: (V, I, T) → SOC."""
    inp = tf.keras.Input(shape=(3,), dtype=tf.float32)
    x = tf.keras.layers.Dense(16, activation="relu")(inp)
    x = tf.keras.layers.Dense(8,  activation="relu")(x)
    out = tf.keras.layers.Dense(1, activation="linear")(x)
    return tf.keras.Model(inp, out, name="soc_mlp")

def build_lstm():
    """Sequence LSTM: (WINDOW, 3) → SOC at end of window.

    `unroll=True` is required for clean TFLite int8 export — without it,
    the converter emits TensorListReserve ops it then can't fully lower.
    Unrolling materializes the WINDOW timesteps as a flat graph, which
    raises model size but lets full-int8 quantization work on TFLM.
    """
    inp = tf.keras.Input(shape=(WINDOW, 3), dtype=tf.float32)
    x = tf.keras.layers.LSTM(16, return_sequences=False, unroll=True)(inp)
    x = tf.keras.layers.Dense(8, activation="relu")(x)
    out = tf.keras.layers.Dense(1, activation="linear")(x)
    return tf.keras.Model(inp, out, name="soc_lstm")

def build_hybrid_residual():
    """Tiny MLP that takes (V, I, T, SOC_ekf_estimate) → residual to add.

    Trained on the EKF residual after we run the EKF on the training data.
    """
    inp = tf.keras.Input(shape=(4,), dtype=tf.float32)
    x = tf.keras.layers.Dense(8, activation="relu")(inp)
    x = tf.keras.layers.Dense(4, activation="relu")(x)
    out = tf.keras.layers.Dense(1, activation="linear")(x)
    return tf.keras.Model(inp, out, name="soc_hybrid_residual")

def to_tflite_int8(model, repr_data):
    def repr_gen():
        for i in range(min(500, len(repr_data))):
            yield [repr_data[i:i+1].astype(np.float32)]
    cv = tf.lite.TFLiteConverter.from_keras_model(model)
    cv.optimizations = [tf.lite.Optimize.DEFAULT]
    cv.representative_dataset = repr_gen
    cv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8,
                                    tf.lite.OpsSet.TFLITE_BUILTINS]
    cv.inference_input_type  = tf.int8
    cv.inference_output_type = tf.int8
    return cv.convert()

# ---------- EKF reference impl (for residual training) -----------------------

def run_ekf_python(v_seq, i_seq, soc0, ekf):
    """Python reference EKF — same equations the C version will run.

    State:    x = [SOC, V_RC]
    Measure:  V_term = OCV(SOC) - i*R0 - V_RC
    """
    R0, R1, C1, soc_grid, ocv_table, Q_nom = ekf
    dt = 1.0
    n = len(v_seq)
    x = np.array([soc0, 0.0])           # state
    P = np.eye(2) * 1.0                  # covariance
    Qproc = np.diag([0.001, 0.0001])     # process noise
    Rmeas = 0.01                         # voltage measurement noise (V²)

    soc_log = np.zeros(n)
    for k in range(n):
        # Predict
        F = np.array([
            [1.0,           0.0],
            [0.0,           1.0 - dt/(R1*C1)],
        ])
        u = i_seq[k]
        x_pred = np.array([
            x[0] - u * dt / (Q_nom * 36.0),    # SOC drops by Ah/Q*100
            x[1] + dt * (-x[1]/(R1*C1) + u/C1),
        ])
        P_pred = F @ P @ F.T + Qproc

        # Measure
        # h(x) = OCV(SOC) - u*R0 - V_RC
        ocv = np.interp(x_pred[0], soc_grid, ocv_table)
        # dOCV/dSOC numerically
        dsoc = 0.5
        ocv_p = np.interp(x_pred[0]+dsoc, soc_grid, ocv_table)
        ocv_m = np.interp(x_pred[0]-dsoc, soc_grid, ocv_table)
        H = np.array([(ocv_p - ocv_m)/(2*dsoc), -1.0])
        z_pred = ocv - u * R0 - x_pred[1]
        innov = v_seq[k] - z_pred

        S = H @ P_pred @ H.T + Rmeas
        K = (P_pred @ H) / S
        x = x_pred + K * innov
        P = (np.eye(2) - np.outer(K, H)) @ P_pred

        x[0] = np.clip(x[0], 0.0, 100.0)
        soc_log[k] = x[0]
    return soc_log

# ---------- C output --------------------------------------------------------

def write_calibration_c(soc_grid, ocv_table, R0, R1, C1, Q_nom, samples,
                         cpp, hpp):
    cpp.parent.mkdir(parents=True, exist_ok=True)
    n = len(soc_grid)
    n_samp = len(samples)
    lines = [
        "/* Generated by apps/ai_bms_soc/train/train.py — DO NOT EDIT */",
        "#include \"soc_calib.h\"",
        "",
        f"const unsigned int g_sococv_n = {n};",
        f"const float g_sococv_soc[{n}] = {{",
        "    " + ", ".join(f"{s:.2f}f" for s in soc_grid),
        "};",
        f"const float g_sococv_ocv[{n}] = {{",
        "    " + ", ".join(f"{v:.5f}f" for v in ocv_table),
        "};",
        "",
        f"/* ECM parameters (single-RC: R0, R1, C1) — fit on training cells. */",
        f"const float g_ecm_R0 = {R0:.6f}f;   /* ohms */",
        f"const float g_ecm_R1 = {R1:.6f}f;   /* ohms */",
        f"const float g_ecm_C1 = {C1:.2f}f;     /* farads */",
        f"const float g_ecm_Qnom = {Q_nom:.4f}f;  /* nominal Ah */",
        "",
        f"const unsigned int g_sample_count = {n_samp};",
        "const struct soc_sample g_samples[] = {",
    ]
    for s in samples:
        lines.append("    {")
        lines.append(f"        .name = \"{s['name']}\",")
        lines.append(f"        .cell = \"{s['cell']}\",")
        lines.append(f"        .capacity_ah = {s['capacity']:.4f}f,")
        lines.append(f"        .v = {{{', '.join(f'{x:+.5f}f' for x in s['v'])}}},")
        lines.append(f"        .i = {{{', '.join(f'{x:+.5f}f' for x in s['i'])}}},")
        lines.append(f"        .T = {{{', '.join(f'{x:+.4f}f' for x in s['T'])}}},")
        lines.append(f"        .soc_true = {{{', '.join(f'{x:+.4f}f' for x in s['soc'])}}},")
        lines.append("    },")
    lines.append("};")
    cpp.write_text("\n".join(lines) + "\n")

    hpp.write_text(f"""/* Generated — DO NOT EDIT */
#ifndef SOC_CALIB_H
#define SOC_CALIB_H

#define SOC_WINDOW {WINDOW}

#ifdef __cplusplus
extern "C" {{
#endif

/* SOC-OCV lookup table (sorted by SOC in 0..100 %). */
extern const unsigned int g_sococv_n;
extern const float        g_sococv_soc[];
extern const float        g_sococv_ocv[];

/* Single-RC ECM parameters fit from training cells. */
extern const float g_ecm_R0;
extern const float g_ecm_R1;
extern const float g_ecm_C1;
extern const float g_ecm_Qnom;

/* Embedded test sample windows (from B0018 holdout cell). */
struct soc_sample {{
    const char  *name;
    const char  *cell;
    float        capacity_ah;
    float        v[SOC_WINDOW];
    float        i[SOC_WINDOW];
    float        T[SOC_WINDOW];
    float        soc_true[SOC_WINDOW];
}};

extern const unsigned int g_sample_count;
extern const struct soc_sample g_samples[];

/* Normalization constants used by the int8 ML models (must match train.py). */
#define SOC_V_MIN  {V_MIN}f
#define SOC_V_MAX  {V_MAX}f
#define SOC_I_MIN  {I_MIN}f
#define SOC_I_MAX  {I_MAX}f
#define SOC_T_MIN  {T_MIN}f
#define SOC_T_MAX  {T_MAX}f
#define SOC_NORM   100.0f

#ifdef __cplusplus
}}
#endif
#endif
""")

def write_model_c(tflite_bytes, name, cpp, hpp):
    n = len(tflite_bytes)
    cpp.parent.mkdir(parents=True, exist_ok=True)
    sym = f"g_model_{name}"
    lines = [
        "/* Generated — DO NOT EDIT */",
        f"#include \"model_{name}.hpp\"",
        "",
        f"alignas(8) const unsigned char {sym}[{n}] = {{",
    ]
    for i in range(0, n, 12):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in tflite_bytes[i:i+12]) + ",")
    lines.append("};")
    lines.append(f"const unsigned int {sym}_len = {n};")
    cpp.write_text("\n".join(lines) + "\n")
    hpp.write_text(
        f"/* Generated — DO NOT EDIT */\n#ifndef MODEL_{name.upper()}_HPP\n"
        f"#define MODEL_{name.upper()}_HPP\n"
        f"extern const unsigned char {sym}[];\n"
        f"extern const unsigned int  {sym}_len;\n"
        "#endif\n"
    )

# ---------- main ------------------------------------------------------------

def main() -> int:
    if not DATA_DIR.exists():
        print(f"ERROR: NASA data not at {DATA_DIR}")
        return 1
    np.random.seed(SEED); tf.random.set_seed(SEED)

    print("Loading NASA discharge cycles…")
    train_rows = collect(TRAIN_CELLS)
    holdout_rows = collect([HOLDOUT_CELL])
    print(f"  train: {len(train_rows)} cycles ({TRAIN_CELLS})")
    print(f"  holdout: {len(holdout_rows)} cycles ({HOLDOUT_CELL})")

    print("\nBuilding SOC-OCV table…")
    soc_grid, ocv_table = build_soc_ocv_table(train_rows)
    print(f"  table {len(soc_grid)} bins, OCV [{ocv_table.min():.3f}..{ocv_table.max():.3f}] V")

    print("\nFitting ECM parameters…")
    R0_fit, R1_fit, C1_fit = fit_ecm_params(train_rows, soc_grid, ocv_table)
    # Sanity check: NASA 18650 LCO real-world: R0 ~ 30-100 mΩ. If fit
    # collapses to the lower bound (often happens with our coarse data),
    # fall back to typical values from the literature so the EKF actually
    # gets a meaningful measurement update.
    if R0_fit < 0.020 or R1_fit < 0.005:
        print(f"  ⚠ fit unrealistic (R0={R0_fit*1000:.1f}mΩ, R1={R1_fit*1000:.1f}mΩ);"
              f" using literature defaults instead")
        R0, R1, C1 = 0.060, 0.025, 2000.0     # 60mΩ / 25mΩ / 2000F
    else:
        R0, R1, C1 = R0_fit, R1_fit, C1_fit
    print(f"  ECM in use: R0={R0*1000:.1f}mΩ, R1={R1*1000:.1f}mΩ, "
          f"C1={C1:.0f}F (τ={R1*C1:.1f}s)")
    Q_nom = float(np.mean([r["capacity"] for r in train_rows[:30]]))   # approx 1.85 Ah

    # ----- build training arrays for ML --------------------------------------
    print("\nAssembling ML training data…")
    # Per-sample dataset for MLP (and hybrid)
    Xs_per, Ys_per = [], []
    # Sequence dataset for LSTM. Each cycle's WINDOW points already span the
    # whole SOC range from 100% down to 0%. To give the LSTM varied targets
    # we synthesize sub-sequences: take a left-padded sliding view ending at
    # each timestep, target = SOC at that step. So end-step in {1..WINDOW}
    # gives WINDOW different SOC targets per cycle.
    Xs_seq, Ys_seq = [], []
    for r in train_rows:
        vn, in_, Tn = normalize_inputs(r["v"], r["i"], r["T"])
        soc_n = normalize_soc(r["soc"])
        # per-sample (already covers full SOC range — MLP fine)
        for k in range(WINDOW):
            Xs_per.append([vn[k], in_[k], Tn[k]])
            Ys_per.append(soc_n[k])
        # sequence: for each end-step e in [WINDOW//4 .. WINDOW), build a
        # WINDOW-long input by repeating the first sample for the missing
        # prefix + actual samples up to e. Target = SOC at e.
        # (We don't bother with e<WINDOW//4 since 8-sample real history
        # gives weak training signal.)
        full = np.stack([vn, in_, Tn], axis=1)   # (WINDOW, 3)
        for e in range(WINDOW // 4, WINDOW):
            n_real = e + 1
            n_pad  = WINDOW - n_real
            # pad with the first sample (helps the LSTM stabilize state)
            pad = np.tile(full[0:1], (n_pad, 1))
            seq = np.concatenate([pad, full[:n_real]], axis=0)
            Xs_seq.append(seq)
            Ys_seq.append(soc_n[e])
        # Also include the canonical full-window-end (= SOC 0%) so the
        # endpoint is anchored.
        Xs_seq.append(full)
        Ys_seq.append(soc_n[-1])
    Xs_per = np.array(Xs_per, dtype=np.float32)
    Ys_per = np.array(Ys_per, dtype=np.float32)
    Xs_seq = np.array(Xs_seq, dtype=np.float32)
    Ys_seq = np.array(Ys_seq, dtype=np.float32)
    print(f"  per-sample: {Xs_per.shape} → {Ys_per.shape}")
    print(f"  sequence:   {Xs_seq.shape} → {Ys_seq.shape}")

    # ----- MLP --------------------------------------------------------------
    print("\nTraining MLP…")
    mlp = build_mlp()
    mlp.compile(optimizer="adam", loss="mse", metrics=["mae"])
    mlp.fit(Xs_per, Ys_per, validation_split=0.1, epochs=EPOCHS_MLP,
            batch_size=BATCH, verbose=0)
    mlp_val = mlp.evaluate(Xs_per, Ys_per, verbose=0)
    print(f"  final loss={mlp_val[0]:.4f}  mae={mlp_val[1]:.4f} (normalized)")

    # ----- LSTM -------------------------------------------------------------
    print("\nTraining LSTM…")
    lstm = build_lstm()
    lstm.compile(optimizer="adam", loss="mse", metrics=["mae"])
    lstm.fit(Xs_seq, Ys_seq, validation_split=0.1, epochs=EPOCHS_LSTM,
             batch_size=BATCH, verbose=0)
    lstm_val = lstm.evaluate(Xs_seq, Ys_seq, verbose=0)
    print(f"  final loss={lstm_val[0]:.4f}  mae={lstm_val[1]:.4f} (normalized)")

    # ----- Hybrid: train on EKF residuals -----------------------------------
    print("\nRunning EKF on training data to compute residuals (for hybrid)…")
    ekf_pack = (R0, R1, C1, soc_grid, ocv_table, Q_nom)
    Xs_hyb, Ys_hyb = [], []
    for r in train_rows[::3]:    # subsample
        ekf_soc = run_ekf_python(r["v"], r["i"], soc0=100.0, ekf=ekf_pack)
        for k in range(WINDOW):
            true_s = r["soc"][k]
            ekf_s  = ekf_soc[k]
            residual = (true_s - ekf_s) / SOC_NORM    # in normalized units
            vn = (r["v"][k] - V_MIN)/(V_MAX-V_MIN)*2-1
            in_ = (r["i"][k] - I_MIN)/(I_MAX-I_MIN)*2-1
            Tn = (r["T"][k] - T_MIN)/(T_MAX-T_MIN)*2-1
            ekf_n = ekf_s/SOC_NORM*2-1
            Xs_hyb.append([vn, in_, Tn, ekf_n])
            Ys_hyb.append(residual * 2)       # scale residual to [-1,1] approximately
    Xs_hyb = np.array(Xs_hyb, dtype=np.float32)
    Ys_hyb = np.array(Ys_hyb, dtype=np.float32)
    print(f"  hybrid training set: {Xs_hyb.shape}")

    print("Training hybrid residual MLP…")
    hybrid = build_hybrid_residual()
    hybrid.compile(optimizer="adam", loss="mse", metrics=["mae"])
    hybrid.fit(Xs_hyb, Ys_hyb, validation_split=0.1,
               epochs=EPOCHS_HYBRID, batch_size=BATCH, verbose=0)
    hybrid_val = hybrid.evaluate(Xs_hyb, Ys_hyb, verbose=0)
    print(f"  final loss={hybrid_val[0]:.5f}  mae={hybrid_val[1]:.5f}")

    # ----- Quantize + emit C ------------------------------------------------
    print("\nQuantizing models to int8…")
    mlp_tflite    = to_tflite_int8(mlp,    Xs_per)
    lstm_tflite   = to_tflite_int8(lstm,   Xs_seq)
    hybrid_tflite = to_tflite_int8(hybrid, Xs_hyb)
    print(f"  MLP    : {len(mlp_tflite)} bytes")
    print(f"  LSTM   : {len(lstm_tflite)} bytes")
    print(f"  Hybrid : {len(hybrid_tflite)} bytes")

    # ----- Pick test samples from holdout B0018 -----------------------------
    # We embed 6 windows: 3 full discharges (early/mid/aged → end SOC ~0%)
    # and 3 partial windows that end at ~50% SOC. The partial windows are
    # constructed by zero-padding the LATER half (post-50% portion) so the
    # WINDOW-length input still has 32 samples but the "end of meaningful
    # data" lands at SOC ~50%.  This gives the LSTM/MLP/EKF a non-trivial
    # SOC to actually predict, instead of all tests ending at the trivial 0%.
    samples = []
    n = len(holdout_rows)
    for name, idx in [("early", 0), ("mid", n//2), ("aged", n-1)]:
        r = holdout_rows[idx]
        samples.append({
            "name": name, "cell": HOLDOUT_CELL, "capacity": r["capacity"],
            "v": r["v"].tolist(), "i": r["i"].tolist(),
            "T": r["T"].tolist(), "soc": r["soc"].tolist(),
        })
        # Partial window — find the index where SOC first crosses below 50,
        # use samples[0..mid] padded with samples[0] for [mid+1..WINDOW-1].
        soc_arr = r["soc"]
        mid_idx = int(np.argmax(soc_arr < 50.0)) if (soc_arr < 50.0).any() else WINDOW//2
        if mid_idx < 4: mid_idx = WINDOW // 2
        v_p = list(r["v"][:mid_idx+1]) + [float(r["v"][0])] * (WINDOW - mid_idx - 1)
        i_p = list(r["i"][:mid_idx+1]) + [0.0] * (WINDOW - mid_idx - 1)   # zero current after
        T_p = list(r["T"][:mid_idx+1]) + [float(r["T"][mid_idx])] * (WINDOW - mid_idx - 1)
        soc_p = list(r["soc"][:mid_idx+1]) + [float(r["soc"][mid_idx])] * (WINDOW - mid_idx - 1)
        samples.append({
            "name": f"{name}_50",
            "cell": HOLDOUT_CELL,
            "capacity": r["capacity"],
            "v": v_p, "i": i_p, "T": T_p, "soc": soc_p,
        })
    print("\nEmbedded test samples:")
    for s in samples:
        print(f"  {s['name']:<6} cap={s['capacity']:.3f} Ah  V[{min(s['v']):.2f}..{max(s['v']):.2f}]  "
              f"SOC[{min(s['soc']):.1f}..{max(s['soc']):.1f}]")

    # ----- Write all C outputs ----------------------------------------------
    print("\nWriting C source files…")
    write_calibration_c(soc_grid, ocv_table, R0, R1, C1, Q_nom, samples,
                         SRC_DIR / "soc_calib.cpp", SRC_DIR / "soc_calib.h")
    write_model_c(mlp_tflite,    "mlp",    SRC_DIR / "model_mlp.cpp",    SRC_DIR / "model_mlp.hpp")
    write_model_c(lstm_tflite,   "lstm",   SRC_DIR / "model_lstm.cpp",   SRC_DIR / "model_lstm.hpp")
    write_model_c(hybrid_tflite, "hybrid", SRC_DIR / "model_hybrid.cpp", SRC_DIR / "model_hybrid.hpp")
    print(f"  → {SRC_DIR}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
