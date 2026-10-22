# QuART — Quick Ingestion to ART through Sortedness Awareness

QuART is a sortedness-aware Adaptive Radix Tree (ART). It avoids redundant
root-to-leaf traversals on near-sorted key streams by reusing cached insertion
state (a *fast-pointer*, `fp`) along shared prefix paths. This repo backs the
paper *"QuART: Quick Ingestion to ART through Sortedness Awareness"* and
reproduces its §4 figures.

## What's here

- **Serial fast paths** (`trees/`): `QuART_tail`, `QuART_lil` (êit), `QuART_stail`
  (super-tail) — three fast-pointer placement policies on top of the base `ART`.
- **Bulk loading** (`ART::bulkLoad`, `QuArtNodeBulkLoadMethods.cpp`): grouped,
  single-threaded construction from sorted keys (paper §3.4, Algorithm 2).
- **Concurrent QuART** (`trees/olc_base.h`, `trees/QuART_conc_olc_*.h`): an
  Optimistic-Lock-Coupling (OLC) design with a per-thread fast-pointer.
  `QuART_conc_olc_stail` (QuART-OLC) is A/B'd against `QuART_conc_olc_art`
  (plain ART-OLC, no fast path).

## Requirements

- **Linux x86-64**: the code uses x86 SSE/AVX intrinsics (`emmintrin.h`,
  `immintrin.h`) and POSIX `sys/time.h`. macOS / non-x86 will not compile.
- CMake ≥ 3.10, a C++17 compiler (GCC or Clang), `make`. Binaries are built with
  `-march=native` and may `SIGILL` on an older/different CPU.

## Build

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Targets: `run` (serial benchmark driver), `test_conc_olc` (concurrent insert
A/B), `test_conc_olc_readwrite` (concurrent mixed read/write A/B).

```shell
# serial driver: -t ART|QuART_tail|QuART_lil|QuART_stail ; --bulkload ; --synthetic
./build/run -t QuART_stail -f <workload.bin> -N 500000000
./build/run -t ART -N 1000000000 --synthetic --bulkload   # grouped bulk load
```

## Running Experiments

All experiments are self-contained and idempotent. Each `run.sh` invokes
`experiments/setup.sh` to clone dependencies (BoDS workload generator,
quick-insertion-tree), generate workloads, and build binaries as needed.

> **Disk warning:** `setup.sh` generates the §4.1/§4.2 workloads at N=500M
> (~1.9 GB each, ~49 GB total). Point `WORKLOAD_DIR` at pre-generated `.bin`
> files to skip, or run only §4.4 (which generates its own small N=20M files).

```shell
bash experiments/setup.sh        # one-time bootstrap (optional; run.sh calls it)
```

### §4.1 — BoDS sortedness experiments (N=500M)

Three sub-experiments over the BoDS K-L sortedness grid:

```shell
bash experiments/4.1-bods/quart-vs-art/run.sh    # Figure 9  — ART vs tail/lil/stail, throughput vs K
bash experiments/4.1-bods/stail-vs-art/run.sh    # Figure 10 — stail vs ART speedup heatmap (K,L)
bash experiments/4.1-bods/stail-vs-quit/run.sh   # Figure 11 — stail vs QuIT speedup heatmap (K,L)
```

Results: `experiments/4.1-bods/<sub>/results/results_<TIMESTAMP>.csv`.
Env: `WORKLOAD_DIR`, `REPEAT` (default 1–5; raise for paper-quality averages).

### §4.2 — TPC-H workload (Figure 12)

Insertion and query throughput of B+-tree, QuIT, ART, and the three QuART
variants on a TPC-H-inspired near-sorted `lineitem` workload (N=6M, K≈96.67, L≈0.1).

```shell
bash experiments/4.2-tpch/run.sh
```

Results: `experiments/4.2-tpch/results/results_<TIMESTAMP>.csv`.
Env: `WORKLOAD_FILE`, `REPEAT` (default 10; paper used 200).

### §4.3 — Bulk loading (Figure 13)

Standard repeated ART insertion vs QuART grouped bulk loading on fully sorted
keys, for input sizes 100M–2B.

```shell
bash experiments/4.3-bulkload/run.sh
```

Results: `experiments/4.3-bulkload/results/results_<TIMESTAMP>.csv`. Env: `REPEAT` (default 5).

### §4.4 — Concurrent QuART (Figures 14 & 15)

A/B of QuART-OLC (`QuART_conc_olc_stail`) vs ART-OLC (`QuART_conc_olc_art`):
concurrent insert throughput vs thread count over four workloads (Fig 14), and
mixed concurrent read/write (Fig 15). Each thread ingests its own near-sorted
BoDS N=20M stream; this experiment generates those small per-stream files itself.

```shell
bash experiments/4.4-concurrent/run.sh
```

Results: `experiments/4.4-concurrent/results/fig14_<TIMESTAMP>.csv` and
`fig15_<TIMESTAMP>.csv`. Env: `THREADS` (default `"1 2 4 8 16"`), `PER_STREAM`
(default 20000000), `WORKLOAD_DIR`.

> §5 (PostgreSQL VACUUM, Redis `rax`) is evaluated in separate repositories and
> is out of scope for this one.
