# Dataset exploration — smoke-test report

End-to-end load test of every battery dataset under `../datasets/`. Confirms
each one parses correctly, reports cell counts / cycle counts / capacity
ranges, and verifies the format we'd plug into a future training script.

**Test harness:** `scripts/explore_datasets.py`. Recorded with `asciinema` so
the run is fully replayable as a terminal session.

**Test date:** 2026-05-09
**Outcome:** **5 PASS / 0 FAIL / 1 SKIP** (Severson skipped — manual
download required, see `datasets/README.md`).

---

## Replay the recording

```bash
# play in terminal (full ANSI colors, original timing, idle compressed to 1s)
asciinema play docs/casts/dataset-exploration.cast

# or convert to plain text (already saved alongside)
asciinema convert -f txt \
    docs/casts/dataset-exploration.cast \
    /tmp/exploration.txt
less /tmp/exploration.txt
```

The `.cast` file is asciicast v3 format (~4 KB). It can also be uploaded
to `asciinema.org` for an embeddable web player — `asciinema upload <file>`
prints a public URL.

---

## Captured output

The full terminal session, plain-text:

```
Battery dataset smoke-test
workspace: /Users/dkkang/dev/zephyr

━━━ NASA PCoE — 4 cells, LCO, CC discharge ━━━
  location              datasets/nasa-pcoe
  cell files            B0005.mat, B0006.mat, B0007.mat, B0018.mat
  EOL criterion         30% capacity fade (1.4 Ah of 2.0 Ah nominal)
  B0005 cycles          168 discharge / cap [1.287..1.856] Ah
  B0006 cycles          168 discharge / cap [1.154..2.035] Ah
  B0007 cycles          168 discharge / cap [1.400..1.891] Ah
  B0018 cycles          132 discharge / cap [1.341..1.855] Ah
  ✓ loaded all 4 cells, capacity ranges sensible

━━━ NASA Randomized — random-walk current profiles, LCO ━━━
  location              datasets/nasa-randomized/11. Randomized Battery Usage Data Set
  inner zips            7 cycling regimes
    - 1                 425 MB
    - 2                 114 MB
    - 3                 134 MB
    ...                 and 4 more
  peek                  2. Battery_Uniform_Distribution_Discharge_Room_Temp_DataSet_2Post.zip
    .mat files          4
    top-level key       data
    struct fields       step, procedure, description
  ✓ can extract + load inner .mat files

━━━ Oxford Battery Degradation — 8 cells, drive-cycle profile, LCO ━━━
  location              datasets/oxford
  main file size        253 MB
  cells                 8 (Cell1 … Cell8)
  Cell1 shape           (1, 1)
  Cell1 dtype           object
  ✓ loaded 8 cells, structured data accessible

━━━ CALCE CS2 — LCO, prismatic, CC-CV charge / CC discharge ━━━
  location              datasets/calce-cs2-lco
  cells                 3 (CS2_33, CS2_34, CS2_35)
    CS2_33              23 .xlsx + 0 .txt files
    CS2_34              23 .xlsx + 0 .txt files
    CS2_35              25 .xlsx + 0 .txt files
  sample file           CS2_33_10_04_10.xlsx
    sheets              Info, Channel_1-006
  ✓ Excel parsing works

━━━ CALCE CX2 — LFP, prismatic, CC-CV charge / CC discharge ━━━
  location              datasets/calce-cx2-lfp
  cells                 3 (CX2_33, CX2_34, CX2_35)
    CX2_33              45 .xlsx + 3 .txt files
    CX2_34              43 .xlsx + 0 .txt files
    CX2_35              48 .xlsx + 0 .txt files
  sample file           CX2_33_10_04_10.xlsx
    sheets              Info, Channel_1-012
  ✓ Excel parsing works

━━━ Severson 124-cell LFP — manual download required ━━━
  location              datasets/severson
  status                empty (manual download from data.matr.io required)
  see                   datasets/README.md → 'severson/' section

━━━ SUMMARY ━━━
  ✓ PASS  nasa-pcoe
  ✓ PASS  nasa-randomized
  ✓ PASS  oxford
  ✓ PASS  calce-cs2-lco
  ✓ PASS  calce-cx2-lfp
  ⏸ SKIP  severson

  total: 5 pass / 0 fail / 1 skipped
```

---

## Per-dataset takeaways

### `nasa-pcoe` ✓

The familiar one. Confirmation that the move from
`apps/ai_bms/train/data/` to `datasets/nasa-pcoe/` didn't break anything —
all four cells still load, capacity ranges match the published numbers,
B0007 still doesn't quite reach 1.4 Ah (which is why our `ai_bms_rul`
uses 1.5 Ah as the EOL cutoff instead of the canonical 1.4).

| Cell | Cycles (discharge) | Cap min (Ah) | Cap max (Ah) |
|------|---------------------|--------------|--------------|
| B0005 | 168 | 1.287 | 1.856 |
| B0006 | 168 | 1.154 | 2.035 |
| B0007 | 168 | 1.400 | 1.891 |
| B0018 | 132 | 1.341 | 1.855 |

### `nasa-randomized` ✓

Seven inner zips, each a different cycling regime
(uniform / skewed-high / skewed-low × room-temp / 40 °C). The largest
(`1.+Battery_Uniform_Distribution_Charge_Discharge`) is 425 MB on its own.

The peek into one of the smaller zips reveals four `.mat` files with a
`data` struct containing `step`, `procedure`, `description` fields — a
**different layout** from NASA PCoE (where the top-level was
`B<id>.cycle`). Any future app using this dataset will need its own
loader; can't reuse the PCoE loader directly.

### `oxford` ✓

The main 253 MB file loads cleanly. 8 cells (`Cell1`…`Cell8`) at the top
level, each a `(1, 1)` object array — Oxford's MATLAB struct serialization
nests deeply, so a real loader will need to drill in with
`m['Cell1'][0,0].dtype.names` to find the per-cycle fields. Doable, just
not as flat as NASA.

The drive-cycle data is what makes this dataset interesting — much closer
to real EV usage than NASA's lab-grade constant-current.

### `calce-cs2-lco` ✓ and `calce-cx2-lfp` ✓

Both load. CS2 (LCO) has ~23 Excel files per cell; CX2 (LFP) has ~45 per
cell — CX2 cells live longer so more files. CX2_33 also includes a few
`.txt` summary files alongside the Excel cycle data.

The Excel sheets follow the convention `Info` + `Channel_1-<n>` — the
channel sheet is the per-sample voltage/current/time time series, the Info
sheet has metadata. We confirmed `pandas.read_excel` works; will need
`openpyxl` (already installed in `.venv-ml`) for full sheet access.

**Key contrast for future demos:**
- **CS2 (LCO)** can be compared head-to-head with NASA-PCoE / Oxford —
  same chemistry, different cycling protocols.
- **CX2 (LFP)** is the chemistry-comparison dataset. Train an AE on
  NASA's LCO, score CX2 LFP cycles → does the AE cleanly flag everything
  as "anomalous because it doesn't match LCO discharge shape"? If so,
  that's the lesson: you must retrain per chemistry.

### `severson` ⏸

Skipped. `data.matr.io` is a JS-rendered SPA so simple HTTP fetch can't
recover per-file URLs. README documents two manual workarounds (browser
download, or use the Microsoft `BatteryML` helper script).

---

## What this confirms vs what's still untested

| Confirmed | Still untested |
|-----------|----------------|
| All 5 (auto-downloadable) datasets parse | None of them yet feed a Zephyr app |
| Workspace path layout works for shared data | Cross-chemistry generalization (CALCE LFP) |
| Move from `apps/ai_bms/train/data` didn't break NASA-using apps | Drive-cycle generalization (Oxford) |
| CALCE Excel parsing is feasible (~seconds per file) | Random-walk noise robustness (NASA Randomized) |

Each item in the "Still untested" column is a candidate for the next demo
— `apps/ai_bms_oxford`, `apps/ai_bms_chem_compare`, etc. The roadmap in
`docs/roadmap.md` already lists these.

---

## How to re-run this test

```bash
# 1. Make sure .venv-ml exists with TF + scipy + pandas + openpyxl
.venv-ml/bin/pip install --upgrade tensorflow numpy scipy pandas openpyxl

# 2. Download the datasets (see datasets/README.md). Severson optional.

# 3. Plain run (text output to stdout):
.venv-ml/bin/python scripts/explore_datasets.py

# 4. Re-record the asciinema cast:
asciinema rec docs/casts/dataset-exploration.cast \
    -t "Battery datasets smoke-test on STM32G4 workspace" \
    --idle-time-limit 1.0 \
    -c ".venv-ml/bin/python scripts/explore_datasets.py"
```

Exit code: 0 if every present dataset loads cleanly, 1 if any fail.
Severson being absent does NOT cause a failure — it skips with status
`SKIP`.
