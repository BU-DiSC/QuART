// test_conc_olc.cpp — robust, parameter-free concurrent QuART.
//
// Exercises trees/olc_base.h + trees/QuART_conc_olc_stail.h, which make NO
// assumption about where (or whether) streams share key bytes: no fork-depth,
// no regionBytes, no pre-warm. The same tree must stay correct and extract
// parallelism whether streams are root-disjoint, share a top byte and fork
// deeper, fully overlap, or are an arbitrary MIX of these.
//
// Modes (each stream is inserted by its own thread, in ascending order):
//   • disjoint : stream i is a dense band in its own top-byte region.
//   • deepfork : every stream shares byte 0; stream i forks at byte F (--fork-
//                depth F, default 1) and fills a band below F.  Shares a top
//                byte.
//   • overlap  : all streams interleave inside ONE small region (residue by W),
//                so every thread mutates shared subtrees.
//   • mixed    : the hard case — even-indexed streams are root-disjoint bands,
//                odd-indexed streams all share ONE top byte and fork at byte 1.
//                So the tree simultaneously contains root forks and deep forks,
//                and the index must handle both with no configuration.
//
// For each run: a single-threaded baseline (plain ART), then parallel insert
// (W threads, no pre-warm) into BOTH concurrent tree types —
// QuART_conc_olc_stail (per-thread stail fast-path tail) and QuART_conc_olc_art
// (plain OLC ART, no fast path, = ART-OLC) — and verify EVERY inserted key by
// lookup after all joins (misses ⇒ abort).  CSV row:
//   mode,W,per_stream,total,serial_art_ns,olc_nofp_ns,olc_stail_ns,
//   nofp_Mips,stail_Mips,nofp_xART,stail_xART,stail_vs_nofp
//
// Build: cmake --build build --target test_conc_olc
// Run  : ./build/test_conc_olc 8 40000 --mode mixed
// 32-bit keys only.

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

// ── Stream sources ───────────────────────────────────────────────────────────
// disjoint: W dense ascending bands, one per top-byte region.
static vector<vector<key_int_t>> buildDisjoint(size_t W, size_t per_stream) {
    const uint64_t band = (uint64_t(1) << 32) / W;
    if (per_stream > band - 1) {
        cerr << "ERROR: keys_per_stream " << per_stream << " exceeds band "
             << (band - 1) << " for W=" << W << ".\n";
        exit(1);
    }
    vector<vector<key_int_t>> s(W);
    for (size_t i = 0; i < W; i++) {
        const uint64_t base = uint64_t(i) * band;
        s[i].resize(per_stream);
        for (size_t j = 0; j < per_stream; j++)
            s[i][j] = static_cast<key_int_t>(base + j + 1);
    }
    return s;
}

// deepfork: all streams share byte 0 (=0); stream i sets byte F to i and fills
// a dense band below F.  Shares the top byte, forks at depth F.
static vector<vector<key_int_t>> buildDeepFork(size_t W, size_t per_stream,
                                               unsigned F) {
    const unsigned kb = keyBytes - 1;  // 4 for 32-bit
    if (F < 1 || F >= kb) {
        cerr << "ERROR: fork-depth must be in [1," << (kb - 1) << "].\n";
        exit(1);
    }
    const unsigned bandBits = (kb - 1 - F) * 8;
    const uint64_t band = (uint64_t(1) << bandBits);
    if (W > 256 || per_stream > band) {
        cerr << "ERROR: deepfork needs W<=256 and keys_per_stream<=" << band
             << " for fork-depth " << F << ".\n";
        exit(1);
    }
    const unsigned shift = (kb - 1 - F) * 8;
    vector<vector<key_int_t>> s(W);
    for (size_t i = 0; i < W; i++) {
        const uint64_t base = (uint64_t)i << shift;
        s[i].resize(per_stream);
        for (size_t j = 0; j < per_stream; j++)
            s[i][j] = static_cast<key_int_t>(base + j);
    }
    return s;
}

// overlap: all W streams inside one small region, interleaved by residue.
static vector<vector<key_int_t>> buildOverlap(size_t W, size_t per_stream) {
    const uint64_t total = (uint64_t)W * per_stream;
    if (total > (uint64_t(1) << 32) - 2) {
        cerr << "ERROR: W*keys_per_stream too large for 32-bit keys.\n";
        exit(1);
    }
    vector<vector<key_int_t>> s(W);
    for (size_t i = 0; i < W; i++) {
        s[i].resize(per_stream);
        for (size_t j = 0; j < per_stream; j++)
            s[i][j] = static_cast<key_int_t>(1 + i + (uint64_t)j * W);
    }
    return s;
}

// mixed: even streams are root-disjoint bands (each its own top byte in the
// upper half of the key space); odd streams ALL share top byte 0 and fork at
// byte 1 (bands below byte 1).  Exercises root forks and deep forks at once.
static vector<vector<key_int_t>> buildMixed(size_t W, size_t per_stream) {
    if (per_stream > 60000) {
        cerr << "ERROR: mixed mode caps keys_per_stream at 60000 (byte-1 "
                "band).\n";
        exit(1);
    }
    vector<vector<key_int_t>> s(W);
    for (size_t i = 0; i < W; i++) {
        s[i].resize(per_stream);
        if (i % 2 == 0) {
            // Root-disjoint band: distinct top byte in [128,255].
            const uint32_t top = 128 + (uint32_t)(i / 2);
            if (top > 255) {
                cerr
                    << "ERROR: too many even streams for distinct top bytes.\n";
                exit(1);
            }
            const uint64_t base = (uint64_t)top << 24;
            for (size_t j = 0; j < per_stream; j++)
                s[i][j] = static_cast<key_int_t>(base + j + 1);
        } else {
            // Shared top byte 0, fork at byte 1: byte1 = odd-stream index.
            const uint32_t b1 = (uint32_t)(i / 2);
            const uint64_t base = (uint64_t)b1 << 16;
            for (size_t j = 0; j < per_stream; j++)
                s[i][j] = static_cast<key_int_t>(base + j);
        }
    }
    return s;
}

// bods: real BoDS N=20M workload files, one per stream, at 40M start-offset
// steps (start = i*40M+1).  Each stream is filtered to its nominal value window
// [start, start+N], which drops stray generation artifacts (e.g. a lone 0 in a
// K1_L1 file) so the streams stay disjoint and duplicate-free.  keys_per_stream
// 0 = full file.  conc_olc makes no disjointness assumption, but a clean window
// keeps the single-threaded ART baseline apples-to-apples and avoids ART's
// duplicate-key hazard.
// keys per BoDS stream file (workload_N<STREAM_N>_<series>_start<i>.bin);
// the paper uses 20M, QUART_STREAM_N overrides it for smoke tests.
static const long long STREAM_N =
    getenv("QUART_STREAM_N") ? atoll(getenv("QUART_STREAM_N")) : 20000000;
static constexpr long long STREAM_STEP = 40000000;  // start-offset stride

// The mixed workload combines the three sortedness levels shown in the QuIT
// subset figure: fully sorted (K0_L0), near-sorted (K5_L5), less sorted
// (K25_L25).  (The full BoDS diagonal spans fully-sorted → fully-random —
// K0_L0,K1_L1,K5_L5,K25_L25,K100_L100 — but the mix deliberately excludes the
// K1_L1 and K100_L100 extremes.)
static const char* MIX_SERIES[3] = {"K0_L0", "K5_L5", "K25_L25"};
static constexpr int NMIX = 3;

// Mixed-workload series assignment: an equal 1/NMIX of streams from each of the
// NMIX mix levels.  floor(W/NMIX) streams of each series; the W%NMIX remainder
// go to randomly-chosen distinct series.  The final per-stream labels are then
// shuffled so the levels interleave across thread indices.  Fixed seed ⇒
// reproducible.
static vector<string> assignMixedSeries(size_t W) {
    uint64_t rng = 42;  // deterministic
    auto next = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    vector<int> cnt(NMIX, (int)(W / NMIX));
    int rem = (int)(W % NMIX);
    // Give the W%NMIX remainder to `rem` distinct series, chosen at random.
    int idx[NMIX];
    for (int i = 0; i < NMIX; i++) idx[i] = i;
    for (int r = 0; r < rem; r++) {
        int pick = r + (int)(next() % (unsigned)(NMIX - r));
        std::swap(idx[r], idx[pick]);
        cnt[idx[r]]++;
    }
    vector<string> labels;
    labels.reserve(W);
    for (int s = 0; s < NMIX; s++)
        for (int c = 0; c < cnt[s]; c++) labels.push_back(MIX_SERIES[s]);
    // Fisher–Yates shuffle so sortedness levels interleave across stream
    // indices.
    for (size_t i = labels.size(); i > 1; i--) {
        size_t j = (size_t)(next() % (uint64_t)i);
        std::swap(labels[i - 1], labels[j]);
    }
    return labels;
}

// One BoDS N=20M file per stream, at 40M start-offset steps (start=i*40M+1).
// seriesForStream[i] names the sortedness series stream i is drawn from — all
// identical for a single-series workload, mixed for the mixed workload. Because
// every stream keeps a UNIQUE start offset, distinct-series streams stay in
// disjoint value windows and never collide.
static vector<vector<key_int_t>> buildBods(
    const string& dir, const vector<string>& seriesForStream, size_t W,
    size_t kps) {
    vector<vector<key_int_t>> streams(W);
    for (size_t i = 0; i < W; i++) {
        const string& series = seriesForStream[i];
        const long long start = (long long)i * STREAM_STEP + 1;
        const key_int_t lo = (key_int_t)start;
        const key_int_t hi = (key_int_t)(start + STREAM_N);
        char path[512];
        snprintf(path, sizeof(path), "%s/workload_N%lld_%s_start%lld.bin",
                 dir.c_str(), STREAM_N, series.c_str(), start);
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) {
            perror(path);
            cerr << "Could not open bods stream file: " << path << "\n";
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
        size_t dropped = 0;
        for (size_t j = 0; j < n; j++) {
            key_int_t k = data[j];
            if (k >= lo && k <= hi)
                s.push_back(k);
            else
                dropped++;
        }
        munmap(p, st.st_size);
        ::close(fd);
        cerr << "  stream " << i << ": " << path << "  kept " << s.size()
             << " / " << n << " (dropped " << dropped << " out-of-window)\n";
    }
    return streams;
}

static void pinToCore(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

template <typename TreeT>
static long long runSerial(TreeT& tree,
                           const vector<array<uint8_t, keyBytes>>& enc,
                           const vector<key_int_t>& vals) {
    auto t0 = chrono::high_resolution_clock::now();
    for (size_t j = 0; j < vals.size(); j++)
        tree.insert((uint8_t*)enc[j].data(), vals[j]);
    auto t1 = chrono::high_resolution_clock::now();
    return chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count();
}


// Parallel insert (W threads, one per stream, NO pre-warm), then verify every
// inserted key by lookup after join.  Returns timed insert ns; sets misses.
template <typename TreeT>
static long long runConcurrent(
    size_t W, const vector<vector<key_int_t>>& streams,
    const vector<vector<array<uint8_t, keyBytes>>>& enc, int numCores,
    long long& misses_out, const char* label) {
    cerr << "  ── " << label << " ──\n";
    TreeT tree(static_cast<int>(W));
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    vector<std::thread> threads;
    threads.reserve(W);
    for (size_t i = 0; i < W; i++) {
        threads.emplace_back([&, i]() {
            pinToCore((int)(i % numCores));
            const auto& e = enc[i];
            const auto& v = streams[i];
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
            }
            for (size_t j = 0; j < v.size(); j++)
                tree.insert((int)i, (uint8_t*)e[j].data(), v[j]);
        });
    }
    while (ready.load(std::memory_order_acquire) < (int)W) {
    }
    auto t0 = chrono::high_resolution_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    auto t1 = chrono::high_resolution_clock::now();
    long long conc_ns =
        chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count();

    std::atomic<long long> misses{0};
    std::atomic<long long> queried{0};
    {
        vector<std::thread> vt;
        for (size_t i = 0; i < W; i++)
            vt.emplace_back([&, i]() {
                pinToCore((int)(i % numCores));
                uint8_t key[keyBytes];
                long long lm = 0, q = 0;
                for (key_int_t k : streams[i]) {
                    encodeKey(k, key);
                    ArtNode* leaf = tree.lookup(key);
                    if (!leaf || !isLeaf(leaf) || getLeafValue(leaf) != k) lm++;
                    q++;
                }
                misses.fetch_add(lm, std::memory_order_relaxed);
                queried.fetch_add(q, std::memory_order_relaxed);
            });
        for (auto& th : vt) th.join();
    }
    misses_out = misses.load();
    cerr << "  verify: queried " << queried.load() << " keys, found "
         << (queried.load() - misses_out) << ", missing " << misses_out << "\n";
#ifdef QUART_CONC_STATS
    auto st = tree.stats();
    double restart_pct = st.slow ? 100.0 * st.conflicts / st.slow : 0.0;
    cerr << "  [locks] inserts=" << st.total << "  fast=" << st.fast
         << "  slow=" << st.slow << "\n"
         << "  [locks] descent restarts (reset to root)=" << st.conflicts
         << " (" << restart_pct << "% of slow-path inserts)"
         << "  [read-version invalidated=" << st.restart_read
         << ", lost write-lock upgrade=" << st.restart_upgrade << "]\n"
         << "  [locks] lock-waits (spun on a write-locked node)="
         << st.wait_events << "  (total spin iters=" << st.wait_spins << ")\n"
         << "  [locks] obsolete-tail catches (fast-path)=" << st.obsolete
         << "\n";
#endif
#ifdef QUART_STAIL_FP_STATS
    // stail fast-path effectiveness (only the stail tree carries these counters;
    // the no-fp art tree has no fpStats(), so guard on the concrete type).
    if constexpr (std::is_same_v<TreeT, QuART_conc_olc_stail>) {
        auto fp = tree.fpStats();
        auto pct = [&](uint64_t n) {
            return fp.total ? 100.0 * n / fp.total : 0.0;
        };
        double fast_hit = pct(fp.fast_success);  // of ALL inserts
        double attempt_succ =
            fp.fast_attempt ? 100.0 * fp.fast_success / fp.fast_attempt : 0.0;
        double contend =
            fp.fast_attempt ? 100.0 * fp.fast_contended / fp.fast_attempt : 0.0;
        cerr << fixed << setprecision(1);
        cerr << "  [fp] fast-append HIT RATE=" << fast_hit
             << "% of inserts  (" << fp.fast_success << "/" << fp.total
             << ")\n"
             << "  [fp] tryFastAppend: attempts=" << fp.fast_attempt
             << "  succeeded=" << fp.fast_success << " (" << attempt_succ
             << "%)  fell back to in-region=" << (fp.fast_attempt - fp.fast_success)
             << "\n"
             << "  [fp] fast-append CONTENDED tail write-lock=" << fp.fast_contended
             << " (" << contend << "% of attempts)\n"
             << "  [fp] classify: append(FP@leaf)=" << fp.append << " ("
             << pct(fp.append) << "%)  fp_preserve(FP@sub-leaf)=" << fp.fp_preserve
             << " (" << pct(fp.fp_preserve) << "%)  bridge=" << fp.bridge << " ("
             << pct(fp.bridge) << "%)\n"
             << "  [fp] classify: other_change=" << fp.other_change << " ("
             << pct(fp.other_change) << "%)  other_preserve=" << fp.other_preserve
             << " (" << pct(fp.other_preserve) << "%)  warmup=" << fp.warmup << " ("
             << pct(fp.warmup) << "%)\n";
    }
#endif
    return conc_ns;
}

int main(int argc, char** argv) {
    if (sizeof(key_int_t) != 4) {
        cerr << "ERROR: test_conc_olc requires 32-bit keys.\n";
        return 1;
    }
    if (argc < 2) {
        cerr
            << "usage: " << argv[0]
            << " <threads> [keys_per_stream] [--mode disjoint|deepfork|overlap|"
               "mixed] [--fork-depth F]\n"
               "       [--bods [dir]] [--bods-mixed [dir]] [--series K_L] "
               "[--no-serial]\n"
               "  --bods-mixed : one stream per thread, series drawn evenly "
               "(1/3 each) from\n"
               "                 the 3 mix levels (K0_L0,K5_L5,K25_L25);\n"
               "                 W%3 remainder to random distinct series.\n"
               "  --no-serial  : skip the single-threaded ART baseline "
               "(heavy at\n"
               "                 full 20M/stream × high W).\n";
        return 1;
    }
    size_t W = (size_t)atoll(argv[1]);
    if (W < 1) W = 1;
    size_t per_stream =
        (argc > 2 && argv[2][0] != '-') ? (size_t)atoll(argv[2]) : 40000;
    string mode = "mixed";
    unsigned F = 1;
    bool bods = false;
    bool bods_mixed = false;
    bool no_serial = false;
    // BoDS workload directory: --bods <dir>, else $WORKLOAD_DIR.
    string bodsDir = getenv("WORKLOAD_DIR") ? getenv("WORKLOAD_DIR") : "";
    string series = "K0_L0";
    for (int i = 2; i < argc; i++) {
        string a = argv[i];
        if (a == "--mode" && i + 1 < argc)
            mode = argv[++i];
        else if (a == "--fork-depth" && i + 1 < argc)
            F = (unsigned)atoi(argv[++i]);
        else if (a == "--series" && i + 1 < argc)
            series = argv[++i];
        else if (a == "--no-serial")
            no_serial = true;
        else if (a == "--bods-mixed") {
            bods = true;
            bods_mixed = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') bodsDir = argv[++i];
        } else if (a == "--bods") {
            bods = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') bodsDir = argv[++i];
        }
    }
    if (bods) mode = bods_mixed ? "bods_mixed" : ("bods_" + series);
    // bods files are full N=20M; default to full file (0) unless overridden.
    if (bods && argc <= 2) per_stream = 0;

    int numCores = (int)std::thread::hardware_concurrency();
    if (numCores < 1) numCores = 1;

    cerr << "════════════════════════════════════════════════════════════\n";
    cerr << "conc-olc:  mode=" << mode << (mode == "deepfork" ? "" : "")
         << "  threads=" << W << "  keys/stream=" << per_stream
         << "  cores=" << numCores << "\n";

    vector<vector<key_int_t>> streams;
    if (bods) {
        vector<string> seriesForStream;
        if (bods_mixed) {
            seriesForStream = assignMixedSeries(W);
            cerr << "  mixed series per stream:";
            for (auto& s : seriesForStream) cerr << " " << s;
            cerr << "\n";
        } else {
            seriesForStream.assign(W, series);
        }
        if (bodsDir.empty()) {
            cerr << "error: --bods needs a directory (or set WORKLOAD_DIR)\n";
            return 1;
        }
        streams = buildBods(bodsDir, seriesForStream, W, per_stream);
    } else if (mode == "disjoint")
        streams = buildDisjoint(W, per_stream);
    else if (mode == "deepfork")
        streams = buildDeepFork(W, per_stream, F);
    else if (mode == "overlap")
        streams = buildOverlap(W, per_stream);
    else if (mode == "mixed")
        streams = buildMixed(W, per_stream);
    else {
        cerr << "ERROR: unknown mode '" << mode << "'.\n";
        return 1;
    }
    size_t total = 0;
    for (auto& s : streams) total += s.size();

    vector<vector<array<uint8_t, keyBytes>>> enc(W);
    for (size_t i = 0; i < W; i++) {
        enc[i].resize(streams[i].size());
        for (size_t j = 0; j < streams[i].size(); j++)
            encodeKey(streams[i][j], enc[i][j].data());
    }

    // Interleaved full sequence (round-robin) for the single-threaded
    // baselines. Skipped under --no-serial: at full 20M/stream × high W these
    // baselines cost ~30GB and minutes each, and are not part of the
    // concurrent-throughput sweep.
    long long serial_art_ns = -1;
    if (!no_serial) {
        vector<key_int_t> iv;
        vector<array<uint8_t, keyBytes>> ie;
        iv.reserve(total);
        ie.reserve(total);
        {
            size_t maxlen = 0;
            for (auto& s : streams) maxlen = max(maxlen, s.size());
            for (size_t j = 0; j < maxlen; j++)
                for (size_t i = 0; i < W; i++)
                    if (j < streams[i].size()) {
                        iv.push_back(streams[i][j]);
                        ie.emplace_back();
                        encodeKey(streams[i][j], ie.back().data());
                    }
        }
        {
            ::ART::ART tree;
            serial_art_ns = runSerial(tree, ie, iv);
            cerr << "  serial ART        : " << setw(7)
                 << serial_art_ns / 1'000'000 << " ms\n";
        }
    } else {
        cerr << "  serial baselines skipped (--no-serial)\n";
    }

    // ── ABLATION: two distinct tree types on identical streams ──
    // QuART_conc_olc_art   = plain reference OLC ART, no fast path (every
    // insert
    //                        = full lock-coupled descent from the root)
    // QuART_conc_olc_stail = OLC ART + per-thread stail fast-path tail (the
    //                        contribution: FP_INSERT/BRIDGE/OTHER
    //                        classification
    //                        + reset-counter hysteresis)
    long long ms_nofp = 0;
    long long conc_nofp_ns = runConcurrent<QuART_conc_olc_art>(
        W, streams, enc, numCores, ms_nofp, "OLC (no fp)");
    if (ms_nofp != 0) {
        cerr << "  *** " << ms_nofp << " / " << total
             << " MISSES (no-fp) — insertion FAILED or tree corrupted ***\n";
        return 2;
    }
    long long ms_stail = 0;
    long long conc_stail_ns = runConcurrent<QuART_conc_olc_stail>(
        W, streams, enc, numCores, ms_stail, "OLC+stail-fp");
    if (ms_stail != 0) {
        cerr << "  *** " << ms_stail << " / " << total
             << " MISSES (stail-fp) — insertion FAILED or tree corrupted ***\n";
        return 2;
    }

    double nofp_mips = (double)total / (conc_nofp_ns / 1e9) / 1e6;
    double stail_mips = (double)total / (conc_stail_ns / 1e9) / 1e6;
    double stail_vs_nofp =
        (double)conc_nofp_ns / conc_stail_ns;  // >1 ⇒ fast path helps
    cerr << "  OLC no-fp  (" << W << " thr): " << setw(7)
         << conc_nofp_ns / 1'000'000 << " ms  (" << fixed << setprecision(1)
         << nofp_mips << " M/s)"
         << "  vs ART=" << setprecision(2)
         << (double)serial_art_ns / conc_nofp_ns << "x\n";
    cerr << "  OLC+stail  (" << W << " thr): " << setw(7)
         << conc_stail_ns / 1'000'000 << " ms  (" << setprecision(1)
         << stail_mips << " M/s)"
         << "  vs ART=" << setprecision(2)
         << (double)serial_art_ns / conc_stail_ns << "x\n";
    cerr << "  >>> stail fast-path speedup over plain OLC ART = "
         << setprecision(2) << stail_vs_nofp << "x\n";
    cerr << "  ALL " << total << " keys verified present (both variants).\n";

    // CSV: mode,W,per_stream,total,serial_art_ns,
    //      olc_nofp_ns,olc_stail_ns,nofp_Mips,stail_Mips,nofp_xART,stail_xART,
    //      stail_vs_nofp
    printf("%s,%zu,%zu,%zu,%lld,%lld,%lld,%.2f,%.2f,%.2f,%.2f,%.3f\n",
           mode.c_str(), W, per_stream, total, serial_art_ns,
           conc_nofp_ns, conc_stail_ns, nofp_mips,
           stail_mips, (double)serial_art_ns / conc_nofp_ns,
           (double)serial_art_ns / conc_stail_ns, stail_vs_nofp);
    return 0;
}
