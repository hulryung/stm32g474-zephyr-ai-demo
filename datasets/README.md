# `datasets/` — public battery datasets

Shared store of public Li-ion battery datasets used by the apps under
`../apps/`. Raw data is **not committed** (it's large and the sources are
public); each subdirectory has either an extracted-on-this-machine copy
or instructions to download.

`apps/<name>/train/train.py` references these paths via
`HERE.parent.parent.parent / "datasets" / "<name>"`.

---

## What's here

| Directory               | Source / cells           | Chemistry | Cycling profile           | Format       | Used by                 | On disk |
|-------------------------|--------------------------|-----------|---------------------------|--------------|-------------------------|---------|
| `nasa-pcoe/`            | NASA PCoE — 4 cells      | LCO       | CC discharge (mixed cutoffs) | `.mat`       | `ai_bms`, `ai_bms_soh`, `ai_bms_rul` | ~250 MB |
| `nasa-randomized/`      | NASA — random walk 1     | LCO       | randomized current profiles | `.mat` (in zips) | (none yet)            | ~1.0 GB |
| `oxford/`               | Oxford ORA — 8 cells     | LCO       | drive-cycle (real EV-like)  | `.mat`       | (none yet)              | ~256 MB |
| `calce-cs2-lco/`        | UMD CALCE — CS2_33,34,35 | **LCO**   | CC-CV charge, CC discharge  | `.xlsx` per cycle | (none yet)         | ~257 MB |
| `calce-cx2-lfp/`        | UMD CALCE — CX2_33,34,35 | **LFP**   | CC-CV charge, CC discharge  | `.xlsx` per cycle | (none yet)         | ~524 MB |
| `severson/`             | MIT/Stanford 124-cell    | LFP       | fast-charging              | `.mat`       | (manual download — see below) | ~5 GB if downloaded |

---

## Why these specific datasets

The selection covers four practical ML diversity axes:

| Axis | NASA PCoE | NASA Random | Oxford | CALCE CS2 | CALCE CX2 | Severson |
|------|-----------|-------------|--------|-----------|-----------|----------|
| Chemistry diversity | LCO | LCO | LCO | LCO | **LFP** | LFP |
| Profile diversity | CC | **random** | **drive-cycle** | CC-CV | CC-CV | fast-charge |
| Cell count | 4 | many | 8 | 3 (subset) | 3 (subset) | 124 |
| Useful for | anomaly / SOH / RUL | anomaly under noise | profile generalization | LCO comparison | **chemistry generalization** | RUL benchmark |

When building a new app under `apps/`, pick the dataset that exercises the
question you're asking. For instance, "does our anomaly detector trained on
LCO still work on LFP?" → use `calce-cx2-lfp/`.

---

## Per-dataset details

### `nasa-pcoe/` — NASA Prognostics Center of Excellence Battery Dataset

- **Source:** Saha & Goebel, NASA Ames PCoE
  ([data set page](https://www.nasa.gov/intelligent-systems-division/discovery-and-systems-health/pcoe/pcoe-data-set-repository/))
- **Direct download (used here):**
  https://phm-datasets.s3.amazonaws.com/NASA/5.+Battery+Data+Set.zip
- **Cells:** B0005, B0006, B0007, B0018 (4 × 18650, ~2 Ah, LCO)
- **Cycling protocol:** CC-CV charge → CC discharge to per-cell cutoff
  (2.7 / 2.5 / 2.2 / 2.5 V), repeated until 30 % capacity fade
- **Format:** MATLAB v5 `.mat` per cell. Top-level struct `B000<id>.cycle`
  is an array of cycle records; each record has `type` (charge / discharge /
  impedance) and `data` (voltage, current, temperature, time, capacity).
- **Caveat:** the four cells use different discharge cutoffs. Late-life
  curve shapes are NOT directly comparable across cells — train per-cell
  or use only normalized-time features.

### `nasa-randomized/` — NASA Randomized Battery Usage

- **Source:** Bole, Kulkarni, Daigle (NASA Ames PCoE)
- **Direct download:**
  https://phm-datasets.s3.amazonaws.com/NASA/11.+Randomized+Battery+Usage+Data+Set.zip
- **Contents:** 7 inner zips, each a different cycling regime
  (uniform discharge / uniform charge-discharge / skewed-high / skewed-low
  / room-temp / 40 °C). Each contains a `data/Matlab/` directory with
  `RW<n>.mat` files.
- **Why valuable:** the random current profiles stress-test models trained
  only on smooth CC profiles (like NASA PCoE). Useful for evaluating how
  robust the BMS apps are to less idealized usage.
- **Status:** extracted, not yet wired into any app.

### `oxford/` — Oxford Battery Degradation Dataset 1

- **Source:** Birkl & Howey, University of Oxford (2017)
  ([ORA page](https://ora.ox.ac.uk/objects/uuid:03ba4b01-cfed-46d3-9b1a-7d4a7bdf6fac))
- **Files:**
  - `Oxford_Battery_Degradation_Dataset_1.mat` (254 MB) — 8 cells,
    characterization tests every 100 cycles
  - `ExampleDC_C1.mat` (71 KB) — first charge/discharge sample
  - `Readme.txt`
- **Cells:** 8 × Kokam SLPB533459H4 740 mAh pouch cells (LCO/NMC)
- **Cycling:** drive-cycle profile based on Artemis urban duty cycle
  + characterization (capacity test, pulse test, EIS) every 100 cycles
- **Why valuable:** drive-cycle current profile is much closer to real EV
  usage than the lab-grade CC profiles in NASA. A model trained on NASA
  but applied to Oxford data is a real test of profile generalization.
- **Top-level keys:** `Cell1` … `Cell8` (8 numpy arrays of cycle data)
- **Status:** extracted, not yet wired into any app.

### `calce-cs2-lco/` & `calce-cx2-lfp/` — UMD CALCE Battery Group

- **Source:** [CALCE Battery Group](https://calce.umd.edu/battery-data),
  Univ. of Maryland Center for Advanced Life Cycle Engineering
- **Direct download (per cell):**
  https://web.calce.umd.edu/batteries/data/CS2_33.zip (and CS2_34, CS2_35)
  https://web.calce.umd.edu/batteries/data/CX2_33.zip (and CX2_34, CX2_35)
- **CS2:** 1.1 Ah prismatic LCO cells, CC-CV charge to 4.2 V, CC
  discharge to 2.7 V
- **CX2:** prismatic **LFP** cells (chemistry comparison vs CS2/NASA)
- **Format:** Excel `.xlsx` per cycle (one file per few-day batch). Each
  file has multiple sheets:
  - `Info` — test metadata
  - `Channel_1-<n>` — voltage/current/time per sample
  - `Statistics_1-<n>` — per-cycle aggregates
- **Why we picked 3 cells per series:** CALCE has 6+ cells per series, but
  3 already gives chemistry coverage at ~250-500 MB total per series. Add
  more cells (CS2_36, CS2_37, CX2_36, CX2_38, etc.) by re-running the
  download script if needed.
- **Caveat:** Excel parsing is slow (`pd.read_excel` ~seconds per file).
  Cache parsed cycles in a single `.npy` or `.parquet` artifact for
  training speed.
- **Status:** extracted, not yet wired into any app.

### `severson/` — MIT/Stanford 124-cell LFP fast-charging dataset

- **Source:** Severson, Attia et al., *Nature Energy* 2019
  [paper](https://web.mit.edu/braatzgroup/Severson_NatureEnergy_2019.pdf) /
  [GitHub](https://github.com/rdbraatz/data-driven-prediction-of-battery-cycle-life-before-capacity-degradation)
- **Cells:** 124 commercial 18650 LFP/graphite cells (A123 APR18650M1A),
  cycled at 30 °C with various fast-charge protocols, ~5 GB
- **Why we'd want it:** the canonical RUL prediction benchmark; cycle
  lives 150 → 2,300 cycles, so the dataset is large and varied enough to
  train RUL models with much lower MAE than `apps/ai_bms_rul` achieves on
  4-cell NASA.

#### Manual download required

The dataset is hosted at <https://data.matr.io/1/projects/5c48dd2bc625d700019f3204>,
which is a JavaScript-rendered application — the per-file URLs aren't
recoverable via simple HTTP. Two options:

1. **Web browser:** open the matr.io link, navigate to each batch, click
   the `.mat` download button. Files are typically:
   - `2017-05-12_batchdata_updated_struct_errorcorrect.mat` (~1.7 GB)
   - `2017-06-30_batchdata_updated_struct_errorcorrect.mat` (~1.0 GB)
   - `2018-04-12_batchdata_updated_struct_errorcorrect.mat` (~0.9 GB)
   - Plus optional batch 4 from the related project
   Place them in `datasets/severson/`.
2. **Microsoft BatteryML helper:** the
   [microsoft/BatteryML](https://github.com/microsoft/BatteryML) repo has
   a download script that handles the matr.io session. Clone and follow
   their `dataprepare.md`.

When the files are present, an `apps/ai_bms_rul_severson/` app could use
them — see `docs/roadmap.md`.

---

## Reproducing the downloads

For everything except Severson, the exact commands used to fetch and
extract are below. Copy-paste into a fresh shell at the workspace root.

```bash
mkdir -p datasets/{nasa-pcoe,nasa-randomized,oxford,calce-cs2-lco,calce-cx2-lfp}

# --- NASA PCoE (already done — kept for reference) -------------------------
cd datasets/nasa-pcoe
curl -fL -o nasa-pcoe.zip \
  "https://phm-datasets.s3.amazonaws.com/NASA/5.+Battery+Data+Set.zip"
unzip nasa-pcoe.zip
unzip "5. Battery Data Set/1. BatteryAgingARC-FY08Q4.zip"
mv "5. Battery Data Set"/* . && rmdir "5. Battery Data Set"
rm nasa-pcoe.zip
cd ../..

# --- NASA Randomized ------------------------------------------------------
cd datasets/nasa-randomized
curl -fL -o source.zip \
  "https://phm-datasets.s3.amazonaws.com/NASA/11.+Randomized+Battery+Usage+Data+Set.zip"
unzip source.zip && rm source.zip
# Inner zips left intact — extract per use case
cd ../..

# --- Oxford ---------------------------------------------------------------
cd datasets/oxford
curl -fL -o Oxford_Battery_Degradation_Dataset_1.mat \
  "https://ora.ox.ac.uk/objects/uuid:03ba4b01-cfed-46d3-9b1a-7d4a7bdf6fac/files/m5ac36a1e2073852e4f1f7dee647909a7"
curl -fL -o ExampleDC_C1.mat \
  "https://ora.ox.ac.uk/objects/uuid:03ba4b01-cfed-46d3-9b1a-7d4a7bdf6fac/files/me9fc40a60ac98708f1b73f3a836548e9"
curl -fL -o Readme.txt \
  "https://ora.ox.ac.uk/objects/uuid:03ba4b01-cfed-46d3-9b1a-7d4a7bdf6fac/files/m43cc05e7c5f1245f4895d9dbd495e52f"
cd ../..

# --- CALCE CS2 (LCO) ------------------------------------------------------
cd datasets/calce-cs2-lco
for f in CS2_33 CS2_34 CS2_35 ; do
  curl -fL -o "${f}.zip" "https://web.calce.umd.edu/batteries/data/${f}.zip"
  unzip -o "${f}.zip" -d "${f}"
done
cd ../..

# --- CALCE CX2 (LFP) ------------------------------------------------------
cd datasets/calce-cx2-lfp
for f in CX2_33 CX2_34 CX2_35 ; do
  curl -fL -o "${f}.zip" "https://web.calce.umd.edu/batteries/data/${f}.zip"
  unzip -o "${f}.zip" -d "${f}"
done
cd ../..
```

Total disk after extraction: **~2.3 GB** (excluding Severson).

---

## Citations

If you publish results based on these datasets, cite:

- **NASA PCoE / Randomized**: B. Saha & K. Goebel, "Battery Data Set",
  NASA Ames Prognostics Data Repository (2007); B. Bole, C. Kulkarni,
  M. Daigle, "Randomized Battery Usage Data Set", NASA Ames PDR (2014).
- **Oxford**: Birkl, C. R. & Howey, D. A. *Oxford Battery Degradation
  Dataset 1*. University of Oxford (2017).
- **CALCE**: Y. Xing, E. W. M. Ma, K. L. Tsui, M. Pecht, "An ensemble
  model for predicting the remaining useful performance of lithium-ion
  batteries", *Microelectronics Reliability* (2013).
- **Severson**: K. A. Severson, P. M. Attia, et al. "Data-driven
  prediction of battery cycle life before capacity degradation",
  *Nature Energy* 4, 383–391 (2019).
