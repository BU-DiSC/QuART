# QuART — Quick Ingestion to ART through Sortedness Awareness

This repository is the artifact of the paper *QuART: Quick Ingestion to ART
through Sortedness Awareness*. It contains the QuART implementation (a fork of
the Adaptive Radix Tree with a sortedness-aware fast path, grouped bulk loading
and a concurrent variant), the three system integrations of the paper as
submodules, and a script per figure and table so that every experimental
result in the paper can be regenerated.

The submission is double-anonymous: the repository and the three system forks
are published through anonymous.4open.science, which hides the hosting
organisation and the history; please do not try to identify the authors.

## What is here

| Path | Contents |
|---|---|
| `ART.h`, `ArtNode.h`, `QuArtNode*.cpp` | the base ART (nodes, insert, lookup, grouped `bulkLoad`) |
| `trees/QuART_{tail,lil,stail}.h` | the three fast-pointer policies of §3 (stail is the default QuART) |
| `trees/olc_base.h`, `trees/QuART_conc_olc_{art,stail}.h` | the concurrent ART-OLC baseline and QuART-OLC (§3.5, §4.4) |
| `run.cpp` | serial benchmark driver (§4.1–4.3) |
| `test_conc_olc.cpp`, `test_conc_olc_readwrite.cpp` | concurrent drivers (§4.4) |
| `experiments/` | one directory per figure/table: `run.sh` (measure) + `plot.py` (draw) + `results/paper/` (the paper's data) |
| `systems/{postgres,duckdb,redis}` | the three system integrations of §5, see [System integrations](#system-integrations-5) |

## Figures and tables

| Paper item | Directory | Measure | Draw |
|---|---|---|---|
| Fig 6 — fast-path vs root inserts | `experiments/3.3-insert-types` | `run.sh` (needs `-DQUART_FP_STATS`, built automatically) | `plot.py` |
| Fig 9 — ART vs QuART variants over K | `experiments/4.1-bods/quart-vs-art` | `run.sh` | `plot.py` |
| Fig 10 — stail vs ART (K×L heatmap) | `experiments/4.1-bods/stail-vs-art` | `run.sh` | `plot.py` |
| Fig 11 — stail vs QuIT (K×L heatmap) | `experiments/4.1-bods/stail-vs-quit` | `run.sh` | `plot.py` |
| Fig 12 — TPC-H-inspired workload | `experiments/4.2-tpch` | `run.sh` | `plot.py` |
| Fig 13 — bulk loading | `experiments/4.3-bulkload` | `run.sh` | `plot.py` |
| Fig 14, 15 — concurrent inserts, mixed read/write | `experiments/4.4-concurrent` | `run.sh` | `plot.py` |
| Table 2 — PostgreSQL VACUUM replay | `experiments/5.1-postgres` | `run.sh` | `table.py` |
| Fig 16 — DuckDB `CREATE INDEX` heatmap | `experiments/5.2-duckdb` | `run.sh` | `plot.py` |
| Fig 17 + in-server stream result — Redis | `experiments/5.3-redis` | `run.sh` | `plot.py` |

Figures 1–5, 7 and 8 and Table 1 are illustrations (Fig 1 plots samples of
public datasets and a sketch of the throughput trend), not measurements, and
are not produced here.

## System integrations (§5)

Each integration is a fork of the upstream project with the QuART work on the
fork's default branch (`master` for PostgreSQL, `v2.0-cyanoptera` for DuckDB,
`unstable` for Redis), kept in its own repository and checked out under
`systems/`. During anonymous review the forks are published through the links
below; `systems/` appears as three empty folders in the anonymized view of this
repository (the anonymizer cannot follow submodules), so download the fork you
need from its link and unpack it into the matching directory. Outside review,
`git clone --recursive` (or `git submodule update --init`) fills the same
directories.

| Section | Directory | Fork contents | Anonymous repository |
|---|---|---|---|
| §5.1 PostgreSQL | `systems/postgres` | stail fast path and guarded bulk loading in VACUUM's radix tree (`src/include/lib/quart.h`), the capture-and-replay harness `quart_bench/` | https://anonymous.4open.science/r/QuART-Postgres-8585/ |
| §5.2 DuckDB | `systems/duckdb` | grouped bulk loading in `CREATE INDEX` (`src/execution/index/art/art_grouped_builder.*`), benchmark `benchmark/quart_grouped_bulk_load/` | https://anonymous.4open.science/r/QuART-DuckDB-EB35/ |
| §5.3 Redis | `systems/redis` | stail inside `rax` (`src/rax_quart.*`, QuART-rax), the stream integration, harnesses `test-standalone/`, `test-redis/` | https://anonymous.4open.science/r/QuART-Redis-FB0D/ |

Each fork's `README.md` opens with a QuART section that maps the paper's section to its
files and experiments and gives the fork's own run instructions. The wrappers here build
each fork in place and drive its harness:

```shell
bash experiments/5.1-postgres/run.sh   # PG_ROWS="250000000 1000000000 5000000000" (paper); PG_ROWS=1000000 to try it
bash experiments/5.2-duckdb/run.sh     # SIZES="5000000 ... 500000000" THREADS="1 2 4 8 16 24" REPEAT=3
bash experiments/5.3-redis/run.sh      # KS="0 1 5 10 25 100" REPS=1 (paper: 2) INSERVER=1
```

### Regenerating the paper's figures without re-running anything

```shell
git clone --recursive <this repository>
python3 -m pip install -r experiments/requirements.txt     # matplotlib, pandas, numpy
QUART_RESULTS=paper bash experiments/make_figures.sh        # < 1 minute
```

This writes every figure (PDF) and the LaTeX body of Table 2 into
`experiments/figures/` from the measurements the paper used, which are kept in
each experiment's `results/paper/` with a `meta.txt` describing their origin.
Text is rendered with LaTeX and the Libertine font when `latex` and
`libertine.sty` are installed, exactly as in the paper; set `QUART_USETEX=0` to
use matplotlib's own text rendering instead. `pdfcrop` is used when present.

### Running an experiment

Every `run.sh` is self-contained: it sources `experiments/setup.sh`, which
clones and builds the two third-party tools, generates the workloads it needs,
and builds QuART. Results go to `experiments/<exp>/results/latest/` (`*.csv`,
`logs/`, `meta.txt`), which `plot.py` reads by default; `RUN_NAME=<name>`
keeps a run under another name and `QUART_RESULTS=<name>` plots it.

```shell
bash experiments/4.2-tpch/run.sh          # measure → results/latest/
python3 experiments/4.2-tpch/plot.py      # draw from results/latest/
bash experiments/make_figures.sh          # all figures from results/latest/ (paper data as fallback)
```

To reproduce every measured figure and table at the paper's settings, run the
scripts in this order (each is independent; the first one bootstraps the
dependencies and workloads for all of them):

```shell
bash experiments/setup.sh                              # BoDS + QuIT clones, N=500M workloads (~53 GB), QuART build
bash experiments/3.3-insert-types/run.sh               # Fig 6
bash experiments/4.1-bods/quart-vs-art/run.sh          # Fig 9
bash experiments/4.1-bods/stail-vs-art/run.sh          # Fig 10
bash experiments/4.1-bods/stail-vs-quit/run.sh         # Fig 11
REPEAT=200 bash experiments/4.2-tpch/run.sh            # Fig 12 (default REPEAT=10)
bash experiments/4.3-bulkload/run.sh                   # Fig 13
REPEAT=5 bash experiments/4.4-concurrent/run.sh        # Fig 14, 15 (default REPEAT=1)
bash experiments/5.1-postgres/run.sh                   # Table 2
bash experiments/5.2-duckdb/run.sh                     # Fig 16
REPS=2 bash experiments/5.3-redis/run.sh               # Fig 17 + in-server result
bash experiments/make_figures.sh                       # draw everything from results/latest/
```

For a quick end-to-end check of the pipeline, prefix the same commands with
`WORKLOAD_N=1000000 RUN_NAME=smoke` and shrink the per-experiment sizes
(`SIZES`, `PG_ROWS`, `N`, `THREADS`); every script then finishes in minutes.

Knobs are environment variables, documented in the header of each `run.sh`.
The common ones:

| Variable | Meaning |
|---|---|
| `WORKLOAD_DIR` | where the BoDS `.bin` workloads live (default `experiments/bods/workloads`); point it at pre-generated files to skip generation |
| `WORKLOAD_N` | keys per BoDS workload (default `500000000` as in the paper; e.g. `1000000` for a quick check) |
| `REPEAT` | timed repetitions per configuration; the average is recorded (defaults differ per experiment) |
| `RUN_NAME` | name of the results directory (default `latest`; e.g. `smoke` for a quick check) |
| `NUMACTL` | prefix for every timed command, e.g. `"numactl --cpunodebind=0 --membind=0"` to pin one socket |
| `JOBS` | parallel make jobs |

A full reproduction is long and large. Before starting one, note:

- **Workloads:** the BoDS grid at N=500M is 28 files of 1.9 GB (about 53 GB).
  The §4.4 experiment generates its own 20M-key per-stream files (48 files of
  80 MB for 16 threads).
- **Memory:** §4.3 at 2B keys needs about 40 GB; the DuckDB 500M-key table
  about 20 GB.
- **Time:** each §4.1 sweep is 20–50 runs of 500M inserts (hours); §4.4 and
  §5.2 take under an hour; the Redis sweep about an hour per repetition.
- **PostgreSQL:** capturing the VACUUM key streams at 5B rows needs about
  300 GB of disk and several hours per scale; set `PG_ROWS` to a subset
  (e.g. `PG_ROWS=250000000`) to try the pipeline. The replay itself is fast.
- **DuckDB:** the release build (`GEN=ninja make release`, done automatically)
  takes a while and needs `ninja`.

The paper's machine: two 24-core Intel Xeon Gold 6442Y (96 hardware
threads), 125 GB RAM, Rocky Linux 9. §4.1–4.3 use a single core; §4.4 and §5.2
one socket (`numactl --cpunodebind=0 --membind=0`). All binaries are built
with `-march=native`.

## Building QuART by itself

Linux x86-64 only (SSE/AVX intrinsics). CMake ≥ 3.10, a C++17 compiler, `make`.

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/run -t QuART_stail -f <workload.bin> -N 500000000     # -t ART | QuART_tail | QuART_lil | QuART_stail
./build/run -t ART -N 1000000000 --synthetic --bulkload        # grouped bulk load of keys 1..N
./build/test_conc_olc 16 20000000 --no-serial --bods <dir> --series K5_L5
./build/test_conc_olc_readwrite 16 50 --series K5_L5 --bods <dir>
```

`run` prints `insertion_ns,query_ns` (queries = N/100 random lookups). CMake
options: `QUART_KEY_64` (64-bit keys), `QUART_FP_STATS` (serial fast-path
counter, used by Fig 6), `QUART_CONC_STATS` / `QUART_STAIL_FP_STATS`
(concurrent lock and fast-path counters).

## Dependencies

- **BoDS** (the sortedness workload generator) and the **quick-insertion-tree**
  (the QuIT and B⁺-tree baselines) are published tools; `experiments/setup.sh`
  clones them at pinned commits into `experiments/bods` and
  `experiments/4.1-bods/quick-insertion-tree`.
- The three system forks under `systems/`: see [System integrations](#system-integrations-5).
- Python 3.8+ with `matplotlib`, `pandas`, `numpy` for the plots. Optional:
  LaTeX with `libertine`, `pdfcrop`, `numactl`.

## Notes on the paper's data

`results/paper/meta.txt` in each experiment records where its data came from
and any deviation from the paper's figure. In particular: the Fig 9 reference
data is the unedited run behind the figure (the plotted values had been
touched up by a few tenths of a Mops); the Fig 6 data assumes L=1 for the K
sweep; for Figs 14–15 only the medians exist, not the per-run driver rows.

`scripts/check_anonymity.sh [--submodules]` is the release gate: it fails if a
tracked file or the forks' QuART-touched files contain an identifying string
or a date, and reports identifying commit metadata.
