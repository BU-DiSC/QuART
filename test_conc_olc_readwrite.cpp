// test_conc_olc_readwrite.cpp — mixed read/write benchmark (paper §4.4, Fig 15) for the two
// concurrent QuART trees.  A/Bs QuART_conc_olc_art (no fast path) vs
// QuART_conc_olc_stail (per-thread stail fast-path tail) under a mix of
// concurrent lock-free lookups (olcLookup) and near-sorted inserts.
//
// Showcases OLC's whole point: lock-free versioned reads scale (the readers
// never write shared memory, so the root's version line stays SHARED across
// cores and never bounces), while the writer inserts still ride the fast path.
//
// Workload: W near-sorted BoDS streams, one per dedicated thread.  Each stream
// is split load_frac / (1-load_frac): the first part is WARM-LOADED into the
// tree (untimed) and is the read key-set; the rest are the NEW inserts issued
// during the timed mixed phase.  Per thread, for each new insert we interleave
// reads_per_write = r/(100-r) lookups of random already-loaded keys.  Both trees
// see the identical op sequence (same per-thread RNG seed).  Every key is
// verified present after join.  32-bit keys only.
//
// CSV: series,read_pct,W,warm,new,read_ops,write_ops,art_ns,stail_ns,
//      art_Mops,stail_Mops,stail_vs_art,rmiss_art,rmiss_stail
//
// Build: cmake --build build --target test_conc_olc_readwrite
// Run  : ./build/test_conc_olc_readwrite 8 95 --series K0_L0

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "ART.h"
#include "ArtNode.h"
#include "Helper.h"
#include "QuArtNodeBulkLoadMethods.cpp"
#include "trees/QuART_conc_olc_art.h"
#include "trees/QuART_conc_olc_stail.h"

using namespace std;
using namespace ART;

static void encodeKey(key_int_t k, uint8_t out[keyBytes]) { loadKey(k, out); }

// keys per BoDS stream file (workload_N<STREAM_N>_<series>_start<i>.bin);
// the paper uses 20M, QUART_STREAM_N overrides it for smoke tests.
static const long long STREAM_N =
    getenv("QUART_STREAM_N") ? atoll(getenv("QUART_STREAM_N")) : 20000000;
static constexpr long long STREAM_STEP = 40000000;  // start-offset stride

// One BoDS N=20M file per stream, at 40M start-offset steps (start=i*40M+1),
// filtered to its value window so streams stay disjoint.
static vector<vector<key_int_t>> buildBods(const string& dir,
                                           const string& series, size_t W,
                                           size_t kps) {
    vector<vector<key_int_t>> streams(W);
    for (size_t i = 0; i < W; i++) {
        const long long start = (long long)i * STREAM_STEP + 1;
        const key_int_t lo = (key_int_t)start;
        const key_int_t hi = (key_int_t)(start + STREAM_N);
        char path[512];
        snprintf(path, sizeof(path), "%s/workload_N%lld_%s_start%lld.bin",
                 dir.c_str(), STREAM_N, series.c_str(), start);
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) {
            perror(path);
            exit(1);
        }
        struct stat st;
        fstat(fd, &st);
        size_t count = (size_t)st.st_size / sizeof(key_int_t);
        size_t n = (kps == 0) ? count : min(kps, count);
        void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
        madvise(p, st.st_size, MADV_SEQUENTIAL);
        const key_int_t* data = reinterpret_cast<const key_int_t*>(p);
        auto& s = streams[i];
        s.reserve(n);
        for (size_t j = 0; j < n; j++) {
            key_int_t k = data[j];
            if (k >= lo && k <= hi) s.push_back(k);
        }
        munmap(p, st.st_size);
        ::close(fd);
        cerr << "  stream " << i << ": kept " << s.size() << " keys\n";
    }
    return streams;
}

static void pinToCore(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static inline uint64_t xorshift64(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

// Warm-load first `warm` of each stream (untimed), then timed mixed phase:
// per new insert, interleave reads_per_write random lookups of loaded keys.
template <typename TreeT>
static long long runReadWrite(size_t W, const vector<vector<key_int_t>>& streams,
                         const vector<vector<array<uint8_t, keyBytes>>>& enc,
                         const vector<size_t>& warm, size_t new_cap,
                         int numCores, double rpw, const char* label,
                         long long& read_miss_out, long long& read_ops_out,
                         long long& write_ops_out) {
    cerr << "  ── " << label << " ──\n";
    TreeT tree(static_cast<int>(W));

    // Warm-load (untimed): each thread inserts its first `warm[i]` keys.
    {
        vector<std::thread> vt;
        for (size_t i = 0; i < W; i++)
            vt.emplace_back([&, i]() {
                pinToCore((int)(i % numCores));
                for (size_t j = 0; j < warm[i]; j++)
                    tree.insert((int)i, (uint8_t*)enc[i][j].data(),
                                streams[i][j]);
            });
        for (auto& th : vt) th.join();
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<long long> tot_read{0}, tot_write{0}, tot_rmiss{0};
    vector<std::thread> threads;
    threads.reserve(W);
    for (size_t i = 0; i < W; i++) {
        threads.emplace_back([&, i]() {
            pinToCore((int)(i % numCores));
            const auto& e = enc[i];
            const auto& v = streams[i];
            const size_t nload = warm[i];
            uint64_t rng = (uint64_t)(i + 1) * 2654435761u + 12345u;
            double debt = 0.0;
            long long rd = 0, wr = 0, rmiss = 0;
            uint8_t qk[keyBytes];
            const size_t jend =
                new_cap ? std::min(v.size(), nload + new_cap) : v.size();
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
            }
            for (size_t j = nload; j < jend; j++) {
                debt += rpw;
                while (debt >= 1.0 && nload > 0) {
                    size_t idx = xorshift64(rng) % nload;
                    encodeKey(v[idx], qk);
                    ArtNode* leaf = tree.olcLookup(qk);
                    if (!leaf || !isLeaf(leaf) || getLeafValue(leaf) != v[idx])
                        rmiss++;
                    rd++;
                    debt -= 1.0;
                }
                tree.insert((int)i, (uint8_t*)e[j].data(), v[j]);
                wr++;
            }
            tot_read.fetch_add(rd, std::memory_order_relaxed);
            tot_write.fetch_add(wr, std::memory_order_relaxed);
            tot_rmiss.fetch_add(rmiss, std::memory_order_relaxed);
        });
    }
    while (ready.load(std::memory_order_acquire) < (int)W) {
    }
    auto t0 = chrono::high_resolution_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    auto t1 = chrono::high_resolution_clock::now();
    long long ns = chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count();

    read_ops_out = tot_read.load();
    write_ops_out = tot_write.load();
    read_miss_out = tot_rmiss.load();

    // Verify EVERY key present (post-join, plain lookup — no concurrent writers).
    std::atomic<long long> misses{0};
    {
        vector<std::thread> vt;
        for (size_t i = 0; i < W; i++)
            vt.emplace_back([&, i]() {
                pinToCore((int)(i % numCores));
                uint8_t key[keyBytes];
                long long lm = 0;
                const size_t jend =
                    new_cap ? std::min(streams[i].size(), warm[i] + new_cap)
                            : streams[i].size();
                for (size_t j = 0; j < jend; j++) {
                    key_int_t k = streams[i][j];
                    encodeKey(k, key);
                    ArtNode* leaf = tree.lookup(key);
                    if (!leaf || !isLeaf(leaf) || getLeafValue(leaf) != k) lm++;
                }
                misses.fetch_add(lm, std::memory_order_relaxed);
            });
        for (auto& th : vt) th.join();
    }
    if (misses.load()) {
        cerr << "  !!! VERIFY FAILED: " << misses.load() << " keys missing\n";
        abort();
    }
    cerr << "  ops: read=" << read_ops_out << " write=" << write_ops_out
         << " read_miss=" << read_miss_out << "  time=" << ns / 1e6 << " ms  ("
         << (read_ops_out + write_ops_out) / (ns / 1e3) << " Mops/s)\n";
    return ns;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <threads> [read_pct] [--series K_L] [--bods dir] "
                "[--load-frac F]\n",
                argv[0]);
        return 1;
    }
    size_t W = strtoul(argv[1], nullptr, 10);
    int read_pct = 50;
    if (argc >= 3 && argv[2][0] != '-') read_pct = atoi(argv[2]);
    string series = "K0_L0";
    // BoDS workload directory: --bods <dir>, else $WORKLOAD_DIR.
    string bodsDir = getenv("WORKLOAD_DIR") ? getenv("WORKLOAD_DIR") : "";
    double load_frac = 0.25;
    size_t new_cap = 1000000;  // max new inserts/stream in timed phase (0 = all)
    for (int i = 2; i < argc; i++) {
        string a = argv[i];
        if (a == "--series" && i + 1 < argc)
            series = argv[++i];
        else if (a == "--bods" && i + 1 < argc)
            bodsDir = argv[++i];
        else if (a == "--load-frac" && i + 1 < argc)
            load_frac = atof(argv[++i]);
        else if (a == "--new-cap" && i + 1 < argc)
            new_cap = strtoul(argv[++i], nullptr, 10);
    }
    if (read_pct < 0) read_pct = 0;
    if (read_pct > 99) read_pct = 99;
    double rpw = (double)read_pct / (100.0 - read_pct);  // reads per write

    int numCores = std::min((int)W, (int)std::thread::hardware_concurrency());
    fprintf(stderr,
            "═══ readwrite series=%s read=%d%% threads=%zu "
            "load_frac=%.2f cores=%d\n",
            series.c_str(), read_pct, W, load_frac, numCores);

    if (bodsDir.empty()) {
        cerr << "error: --bods <dir> is required (or set WORKLOAD_DIR)\n";
        return 1;
    }
    auto streams = buildBods(bodsDir, series, W, 0);
    size_t total_keys = 0, total_warm = 0, total_new = 0;
    vector<size_t> warm(W);
    for (size_t i = 0; i < W; i++) {
        warm[i] = (size_t)(streams[i].size() * load_frac);
        total_keys += streams[i].size();
        total_warm += warm[i];
        total_new += streams[i].size() - warm[i];
    }
    // Encode all keys upfront.
    vector<vector<array<uint8_t, keyBytes>>> enc(W);
    for (size_t i = 0; i < W; i++) {
        enc[i].resize(streams[i].size());
        for (size_t j = 0; j < streams[i].size(); j++)
            encodeKey(streams[i][j], enc[i][j].data());
    }

    long long rmiss_a, rmiss_s, rd_a, rd_s, wr_a, wr_s;
    long long art_ns =
        runReadWrite<QuART_conc_olc_art>(W, streams, enc, warm, new_cap, numCores,
                                    rpw, "OLC no-fp", rmiss_a, rd_a, wr_a);
    long long stail_ns =
        runReadWrite<QuART_conc_olc_stail>(W, streams, enc, warm, new_cap, numCores,
                                      rpw, "OLC+stail-fp", rmiss_s, rd_s, wr_s);

    double art_mops = (rd_a + wr_a) / (art_ns / 1e3);
    double stail_mops = (rd_s + wr_s) / (stail_ns / 1e3);
    double spd = art_ns > 0 ? (double)art_ns / stail_ns : 0.0;
    // stail vs art on the SAME op count is ~equal; normalize by ops to be exact.
    double spd_ops = stail_mops / (art_mops > 0 ? art_mops : 1.0);

    printf("%s,%d,%zu,%lld,%lld,%lld,%lld,%lld,%lld,%.2f,%.2f,%.3f,%lld,%lld\n",
           series.c_str(), read_pct, W, (long long)total_warm,
           (long long)total_new, rd_s, wr_s, art_ns, stail_ns, art_mops,
           stail_mops, spd_ops, rmiss_a, rmiss_s);
    (void)spd;
    return 0;
}
