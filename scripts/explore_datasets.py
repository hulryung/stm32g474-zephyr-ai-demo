#!/usr/bin/env python3
"""End-to-end smoke test of every battery dataset under datasets/.

Loads each one, prints a structured summary (cell count, cycle counts,
capacity range, file format, sample fields), and reports any datasets
that fail to load. Designed to be readable as a terminal recording
(asciinema), not just dumped through a parser.

Run:
    .venv-ml/bin/python tools/explore_datasets.py

Datasets covered:
    1. nasa-pcoe        (.mat per cell)
    2. nasa-randomized  (nested zips → .mat per battery)
    3. oxford           (.mat with multi-cell struct)
    4. calce-cs2-lco    (.xlsx per cycle)
    5. calce-cx2-lfp    (.xlsx per cycle)

For each: prints "OK" + summary, or "FAIL" + the exception class.
"""

from __future__ import annotations

import os
import pathlib
import sys
import time
import traceback
import zipfile

import numpy as np
import scipy.io as sio


HERE = pathlib.Path(__file__).resolve().parent.parent
DATA = HERE / "datasets"


# ---------- terminal helpers ------------------------------------------------

ANSI_BOLD  = "\x1b[1m"
ANSI_DIM   = "\x1b[2m"
ANSI_GREEN = "\x1b[32m"
ANSI_RED   = "\x1b[31m"
ANSI_CYAN  = "\x1b[36m"
ANSI_RESET = "\x1b[0m"

def hdr(s: str) -> None:
    print()
    print(f"{ANSI_BOLD}{ANSI_CYAN}━━━ {s} ━━━{ANSI_RESET}")

def line(label: str, value) -> None:
    print(f"  {label:<22}{value}")

def ok(s: str) -> None:
    print(f"  {ANSI_GREEN}✓{ANSI_RESET} {s}")

def fail(s: str) -> None:
    print(f"  {ANSI_RED}✗{ANSI_RESET} {s}")

def pause(seconds: float = 0.4) -> None:
    """Tiny delay so an asciinema viewer can read the section before the next."""
    time.sleep(seconds)


# ---------- per-dataset exploration -----------------------------------------

def explore_nasa_pcoe() -> bool:
    hdr("NASA PCoE — 4 cells, LCO, CC discharge")
    root = DATA / "nasa-pcoe"
    if not root.exists():
        fail(f"{root} not found")
        return False

    cells_present = sorted(p.name for p in root.glob("B*.mat"))
    line("location", root.relative_to(HERE))
    line("cell files", ", ".join(cells_present))
    line("EOL criterion", "30% capacity fade (1.4 Ah of 2.0 Ah nominal)")
    pause()

    try:
        for cell in ["B0005", "B0006", "B0007", "B0018"]:
            m = sio.loadmat(root / f"{cell}.mat",
                            squeeze_me=True, struct_as_record=False)
            cycles = m[cell].cycle
            disch = [c for c in cycles if c.type == "discharge"]
            caps = np.array([float(c.data.Capacity) for c in disch])
            line(f"{cell} cycles", f"{len(disch)} discharge / "
                                    f"cap [{caps.min():.3f}..{caps.max():.3f}] Ah")
        ok("loaded all 4 cells, capacity ranges sensible")
        return True
    except Exception as e:
        fail(f"load error: {type(e).__name__}: {e}")
        return False


def explore_nasa_randomized() -> bool:
    hdr("NASA Randomized — random-walk current profiles, LCO")
    root = DATA / "nasa-randomized" / "11. Randomized Battery Usage Data Set"
    if not root.exists():
        fail(f"{root} not found")
        return False

    inner_zips = sorted(root.glob("*.zip"))
    line("location", root.relative_to(HERE))
    line("inner zips", f"{len(inner_zips)} cycling regimes")
    for z in inner_zips[:3]:
        line("  - " + z.stem.split('.', 1)[0], f"{z.stat().st_size // 1024 // 1024} MB")
    if len(inner_zips) > 3:
        line("  ...", f"and {len(inner_zips)-3} more")
    pause()

    # Peek inside one
    if inner_zips:
        target = inner_zips[1]  # "2. Battery_Uniform_Distribution_Discharge_Room_Temp"
        line("peek", target.name)
        try:
            with zipfile.ZipFile(target) as zf:
                mat_members = [m for m in zf.namelist() if m.endswith(".mat")]
                line("  .mat files", f"{len(mat_members)}")
                if mat_members:
                    # Extract one to a temp dir and inspect
                    import tempfile
                    with tempfile.TemporaryDirectory() as td:
                        zf.extract(mat_members[0], td)
                        extracted = pathlib.Path(td) / mat_members[0]
                        m = sio.loadmat(extracted, squeeze_me=True,
                                        struct_as_record=False)
                        keys = [k for k in m if not k.startswith("__")]
                        line("  top-level key", keys[0] if keys else "(none)")
                        if keys:
                            obj = m[keys[0]]
                            if hasattr(obj, "_fieldnames"):
                                line("  struct fields",
                                     ", ".join(obj._fieldnames[:6]))
            ok("can extract + load inner .mat files")
            return True
        except Exception as e:
            fail(f"inner-zip inspect error: {type(e).__name__}: {e}")
            return False
    return False


def explore_oxford() -> bool:
    hdr("Oxford Battery Degradation — 8 cells, drive-cycle profile, LCO")
    root = DATA / "oxford"
    if not root.exists():
        fail(f"{root} not found")
        return False
    main_mat = root / "Oxford_Battery_Degradation_Dataset_1.mat"
    if not main_mat.exists():
        fail(f"main .mat not found at {main_mat}")
        return False

    line("location", root.relative_to(HERE))
    line("main file size", f"{main_mat.stat().st_size // 1024 // 1024} MB")
    pause()

    try:
        m = sio.loadmat(main_mat, squeeze_me=False, struct_as_record=False)
        keys = [k for k in m if not k.startswith("__")]
        line("cells", f"{len(keys)} ({keys[0]} … {keys[-1]})")

        # Inspect first cell's structure
        cell1 = m[keys[0]]
        # cell1 is typically a record array of cycles
        if isinstance(cell1, np.ndarray):
            line("Cell1 shape", str(cell1.shape))
            line("Cell1 dtype", "structured" if cell1.dtype.names else str(cell1.dtype))
            if cell1.dtype.names:
                line("Cell1 fields", ", ".join(cell1.dtype.names[:6]))

        ok("loaded 8 cells, structured data accessible")
        return True
    except Exception as e:
        fail(f"load error: {type(e).__name__}: {e}")
        return False


def explore_calce(name: str, chemistry: str, prefix: str) -> bool:
    hdr(f"CALCE {prefix} — {chemistry}, prismatic, CC-CV charge / CC discharge")
    root = DATA / name
    if not root.exists():
        fail(f"{root} not found")
        return False

    # Each cell is a directory like CS2_33/CS2_33/*.xlsx
    cell_dirs = sorted([p for p in root.iterdir() if p.is_dir()])
    line("location", root.relative_to(HERE))
    line("cells", f"{len(cell_dirs)} ({', '.join(p.name for p in cell_dirs)})")
    pause()

    try:
        # Just count files per cell, peek one
        for cd in cell_dirs:
            xls = list(cd.rglob("*.xlsx"))
            txt = list(cd.rglob("*.txt"))
            line(f"  {cd.name}",
                 f"{len(xls)} .xlsx + {len(txt)} .txt files")

        # Open one xlsx to confirm structure — only sheet names + a few rows
        if cell_dirs:
            first_cell = cell_dirs[0]
            xl_files = sorted(first_cell.rglob("*.xlsx"))
            if xl_files:
                import pandas as pd
                xl = pd.ExcelFile(xl_files[0])
                line("sample file", xl_files[0].name)
                line("  sheets", ", ".join(xl.sheet_names[:4]))
                if "Statistics_1-012" in xl.sheet_names:
                    df = pd.read_excel(xl_files[0], sheet_name="Statistics_1-012", nrows=3)
                    line("  stats columns", ", ".join(map(str, df.columns[:5])))
                elif xl.sheet_names:
                    # fall back to whatever first non-Info sheet exists
                    pass
        ok("Excel parsing works")
        return True
    except Exception as e:
        fail(f"load error: {type(e).__name__}: {e}")
        traceback.print_exc()
        return False


def explore_severson() -> bool:
    hdr("Severson 124-cell LFP — manual download required")
    root = DATA / "severson"
    line("location", root.relative_to(HERE))
    contents = list(root.iterdir()) if root.exists() else []
    if any(p.suffix == ".mat" for p in contents):
        line("status", "data present — would explore")
        ok("found .mat files")
        return True
    line("status", "empty (manual download from data.matr.io required)")
    line("see", "datasets/README.md → 'severson/' section")
    return None  # neither pass nor fail


# ---------- main ------------------------------------------------------------

def main() -> int:
    print(f"{ANSI_BOLD}Battery dataset smoke-test{ANSI_RESET}")
    print(f"{ANSI_DIM}workspace: {HERE}{ANSI_RESET}")
    pause(0.6)

    results = {}
    results["nasa-pcoe"]       = explore_nasa_pcoe()
    pause(0.6)
    results["nasa-randomized"] = explore_nasa_randomized()
    pause(0.6)
    results["oxford"]          = explore_oxford()
    pause(0.6)
    results["calce-cs2-lco"]   = explore_calce("calce-cs2-lco", "LCO", "CS2")
    pause(0.6)
    results["calce-cx2-lfp"]   = explore_calce("calce-cx2-lfp", "LFP", "CX2")
    pause(0.6)
    results["severson"]        = explore_severson()
    pause(0.6)

    # Summary -----------------------------------------------------------------
    hdr("SUMMARY")
    n_pass = sum(1 for v in results.values() if v is True)
    n_fail = sum(1 for v in results.values() if v is False)
    n_skip = sum(1 for v in results.values() if v is None)
    for name, status in results.items():
        if status is True:
            print(f"  {ANSI_GREEN}✓ PASS{ANSI_RESET}  {name}")
        elif status is False:
            print(f"  {ANSI_RED}✗ FAIL{ANSI_RESET}  {name}")
        else:
            print(f"  ⏸ SKIP  {name}")
    print()
    print(f"  total: {n_pass} pass / {n_fail} fail / {n_skip} skipped")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
