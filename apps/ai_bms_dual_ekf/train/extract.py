#!/usr/bin/env python3
"""Extract per-cycle data from NASA B0005 across its full life and emit
a C array of (V_seq, I_seq, T_seq, Q_actual) — one entry per discharge cycle.

The on-board demo replays these cycles in order. The dual-EKF tracks
capacity Q as the cell ages; we want to show Q_estimated converging to
Q_actual over the full ~150 cycles.
"""
from __future__ import annotations
import pathlib, sys
import numpy as np
import scipy.io as sio

WINDOW = 32
HERE = pathlib.Path(__file__).parent
DATA_DIR = HERE.parent.parent.parent / "datasets" / "nasa-pcoe"
SRC_DIR  = HERE.parent / "src"
CELL = "B0005"

def load(cell):
    m = sio.loadmat(DATA_DIR / f"{cell}.mat", squeeze_me=True, struct_as_record=False)
    out = []
    for cy in m[cell].cycle:
        if cy.type != "discharge": continue
        v = np.asarray(cy.data.Voltage_measured, dtype=np.float32)
        i = np.asarray(cy.data.Current_measured, dtype=np.float32)
        T = np.asarray(cy.data.Temperature_measured, dtype=np.float32)
        t = np.asarray(cy.data.Time, dtype=np.float32)
        Q = float(cy.data.Capacity)
        if v.size < 8: continue
        u = np.linspace(0,1,WINDOW,dtype=np.float32)
        tn = (t - t[0]) / max(t[-1]-t[0], 1e-6)
        out.append({
            "v": np.interp(u, tn, v).astype(np.float32),
            "i": np.interp(u, tn, i).astype(np.float32),
            "T": np.interp(u, tn, T).astype(np.float32),
            "t_total_s": float(t[-1] - t[0]),
            "Q_ah": Q,
        })
    return out

def main():
    cycles = load(CELL)
    print(f"loaded {len(cycles)} discharge cycles for {CELL}")
    print(f"  Q range: {min(c['Q_ah'] for c in cycles):.3f} → {max(c['Q_ah'] for c in cycles):.3f} Ah")
    print(f"  duration range: {min(c['t_total_s'] for c in cycles):.0f}s → {max(c['t_total_s'] for c in cycles):.0f}s")

    # Subsample to keep flash size sane: take every other cycle
    sel = cycles[::2]
    print(f"  emitting every-other-cycle ({len(sel)}) to keep flash sane")

    cpp = SRC_DIR / "cycles_db.cpp"
    hpp = SRC_DIR / "cycles_db.h"
    cpp.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "/* Generated — DO NOT EDIT */",
        "#include \"cycles_db.h\"",
        "",
        f"const unsigned int g_cycle_count = {len(sel)};",
        "const struct cycle_record g_cycles[] = {",
    ]
    for idx, c in enumerate(sel):
        lines.append("    {")
        lines.append(f"        .index = {idx*2},")
        lines.append(f"        .duration_s = {c['t_total_s']:.1f}f,")
        lines.append(f"        .Q_actual = {c['Q_ah']:.4f}f,")
        lines.append(f"        .v = {{{', '.join(f'{x:+.5f}f' for x in c['v'])}}},")
        lines.append(f"        .i = {{{', '.join(f'{x:+.5f}f' for x in c['i'])}}},")
        lines.append(f"        .T = {{{', '.join(f'{x:+.4f}f' for x in c['T'])}}},")
        lines.append("    },")
    lines.append("};")
    cpp.write_text("\n".join(lines) + "\n")

    hpp.write_text(f"""/* Generated — DO NOT EDIT */
#ifndef CYCLES_DB_H
#define CYCLES_DB_H

#define CYCLE_WINDOW {WINDOW}

#ifdef __cplusplus
extern "C" {{
#endif

struct cycle_record {{
    unsigned int index;       /* 0..n original cycle number in B0005 */
    float        duration_s;
    float        Q_actual;    /* Ah measured by the lab — ground truth */
    float        v[CYCLE_WINDOW];
    float        i[CYCLE_WINDOW];
    float        T[CYCLE_WINDOW];
}};

extern const unsigned int        g_cycle_count;
extern const struct cycle_record g_cycles[];

#ifdef __cplusplus
}}
#endif
#endif
""")
    print(f"  wrote {cpp}")
    print(f"  wrote {hpp}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
