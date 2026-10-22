#pragma once

#include <emmintrin.h>  // _mm_pause

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "../ART.h"
#include "../ArtNode.h"
#include "QuART.h"

namespace ART {

// ─────────────────────────────────────────────────────────────────────────────
// olc_base.h — shared Optimistic Lock Coupling (OLC) machinery for the
// concurrent QuART variants.
//
// Two concrete trees are built on this base:
//   • QuART_conc_olc_art   — plain multi-stream OLC ART, NO fast path (every
//                            insert is a full lock-coupled descent from root).
//   • QuART_conc_olc_stail — OLC ART + per-thread fast-path tail with stail's
//                            FP_INSERT/BRIDGE/OTHER classification.
// Both are robust and parameter-free (no fork-depth / regionBytes / pre-warm),
// built to scale even in the HIGH-UNSORTEDNESS regime where almost every insert
// misses the fast path and takes the slow (structural) descent.  The key is how
// that descent synchronizes:
//
//   An EXCLUSIVE latch-coupling descent (every node on the path latched hand-
//   over-hand) makes latching a WRITE (atomic exchange) to the node's stripe,
//   so every thread descending through a shared upper node (the root, above
//   all) bounces that stripe's cache line between cores and serializes — and a
//   try_lock conflict restarts the whole insert from the root.  On random
//   (K=L=25/100) streams almost every insert takes this path, so throughput
//   NEGATIVELY scales past ~8 threads.  OLC avoids this:
//
//   • the descent is LOCK-FREE.  Each node carries a version counter (striped,
//     external — ArtNode is untouched, so nodes stay free of per-node lock
//     memory).  A descender only READS a node's version, reads a child pointer,
//     then re-checks the version (checkOrRestart) before moving down — no write
//     to shared memory on the read path, so the root's version line stays
//     SHARED across all cores and never bounces.  A latch is upgraded to
//     EXCLUSIVE only on the node actually being mutated (and its parent, when a
//     grow/split rewrites the parent's child slot).  Two random inserts
//     conflict only if they mutate the SAME node — rare — so throughput scales
//     on disordered streams.
//
//   This is exactly the technique from Leis et al., "The ART of Practical
//   Synchronization" (DaMoN 2016) — the same authors as ART itself —
//   specialized to this codebase's node methods.  Nothing outside the ART
//   family is used.
//
// Version-word protocol (per node, via a striped external table).
//   A 64-bit word: [ version : 63 bits ][ write-locked : 1 bit (LSB) ].
//     readLock(w)       spin while LSB set, then return the even snapshot v.
//     check(w, v)       (load == v): false => node changed => RESTART.
//     tryUpgrade(w, v)  CAS v -> v|1: got the exclusive lock (fails if the
//                       version moved, i.e. a concurrent writer touched it).
//     writeUnlock(w, v) store v+2: clears the lock, bumps the version so any
//                       optimistic reader that saw v now restarts.
//   Obsolescence is NOT encoded in the word (a striped word is shared by many
//   nodes; a sticky obsolete bit would livelock a co-hashed live node). Instead
//   a grown-out node is stamped NodeTypeObsolete in its own header
//   (reclaimNode), and reclamation is DEFERRED, so an optimistic read of an
//   obsolete node is always memory-safe and is caught by the parent's
//   checkOrRestart (the parent slot no longer points at it).
//
// ── Memory safety under optimistic reads ─────────────────────────────────────
//   A reader may observe a node while a writer mutates it, then restart.  That
//   is safe here because (1) reclamation is deferred (no use-after-free), (2)
//   every node array is fixed-size and single-byte / pointer writes are atomic
//   on x86, so a torn read yields an in-bounds (if stale) child pointer that
//   the ensuing checkOrRestart rejects, and (3) prefixMismatch never recurses
//   into children for these key widths (prefixLength ≤ keyBytes ==
//   maxPrefixLength).  Intended use is parallel-insert → join() → verify;
//   concurrent lookups are not supported.
//
// WRITER–WRITER races are impossible: a node maps to exactly one version word
// and the CAS upgrade gives mutual exclusion, so two writers of the same node
// always serialize.  Functional correctness is checked the hard way — verify
// EVERY key after join — and is ASan-clean.
// ─────────────────────────────────────────────────────────────────────────────

// OLC version+lock word, cache-line padded so hot nodes (the root, upper
// levels) don't false-share their version line with an unrelated stripe.
struct alignas(64) OlcLock {
    std::atomic<uint64_t> w{0};
    char pad[64 - sizeof(std::atomic<uint64_t>)];
};

#ifdef QUART_CONC_STATS
// Process-wide lock-wait counters (only compiled under -DQUART_CONC_STATS). The
// version-word helpers are free functions, so the counters live at namespace
// scope; only the OLC trees use OlcLock, so these accumulate purely during the
// concurrent run (the serial baselines never touch a version word).
//   g_olc_wait_events : times a descender/writer found a node write-locked and
//                       had to spin (i.e. blocked on another thread's writer).
//   g_olc_wait_spins  : total _mm_pause iterations spent in those spins (a
//                       wait-duration proxy).
inline std::atomic<uint64_t> g_olc_wait_events{0};
inline std::atomic<uint64_t> g_olc_wait_spins{0};
#endif

static inline uint64_t olcReadLock(std::atomic<uint64_t>& w) {
#ifdef QUART_CONC_STATS
    bool waited = false;
#endif
    for (;;) {
        uint64_t v = w.load(std::memory_order_acquire);
        if (!(v & 1)) return v;  // even ⇒ unlocked
#ifdef QUART_CONC_STATS
        if (!waited) {
            waited = true;
            g_olc_wait_events.fetch_add(1, std::memory_order_relaxed);
        }
        g_olc_wait_spins.fetch_add(1, std::memory_order_relaxed);
#endif
        _mm_pause();
    }
}
static inline bool olcCheck(std::atomic<uint64_t>& w, uint64_t v) {
    return w.load(std::memory_order_acquire) == v;
}
static inline bool olcTryUpgrade(std::atomic<uint64_t>& w, uint64_t v) {
    return w.compare_exchange_strong(v, v | 1, std::memory_order_acquire,
                                     std::memory_order_relaxed);
}
static inline void olcWriteUnlock(std::atomic<uint64_t>& w, uint64_t v) {
    w.store(v + 2, std::memory_order_release);  // clear lock, bump version
}
static inline uint64_t olcWriteLock(std::atomic<uint64_t>& w) {
    for (;;) {
        uint64_t v = olcReadLock(w);
        if (olcTryUpgrade(w, v)) return v;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// QuART_conc_olc_base — shared OLC state + node-mutation helpers.
//
// Holds the striped external version table (stripes_), the &root guard
// (rootVer_), the deferred-reclamation retire lists (retire_), and the node-
// growing helpers (insertChild/wouldGrow) plus the two-lock acquire/release
// (lockTwo/unlockTwo) that every OLC descent uses.  Abstract: insert() is left
// pure-virtual (from QuART), so only the concrete subclasses (art / stail) are
// instantiable — each supplies its own descendAndInsert() and insert routing.
// ─────────────────────────────────────────────────────────────────────────────
class QuART_conc_olc_base : public QuART {
   public:
    // stripes_ (NSTRIPES × 64B ≈ 8 MB) is heap-allocated via std::vector so the
    // tree object stays small enough to live on the test's stack; a raw
    // std::array member would overflow it.
    explicit QuART_conc_olc_base(int numThreads)
        : QuART(), numThreads_(numThreads), stripes_(NSTRIPES) {}

    ~QuART_conc_olc_base() {
        for (auto& sh : retire_)
            for (auto& pr : sh.v) freeByType(pr.first, pr.second);
    }

    int getNumThreads() const { return numThreads_; }

    // Optimistic-Lock-Coupling concurrent lookup — SAFE to run concurrently with
    // descendAndInsert writers on the same tree (unlike ART::lookup, which has
    // no version checks).  Mirrors the insert descent's version protocol: read a
    // node's version, read its prefix/child optimistically, checkOrRestart the
    // version before trusting anything, and restart from the root on any
    // invalidation.  Never writes shared memory (pure reader), so the root's
    // version line stays SHARED across cores and never bounces.  Returns the
    // matching leaf (tagged pointer) or nullptr if the key is absent.
    ArtNode* olcLookup(uint8_t key[]) {
    restart:
        OlcLock* parentLk = &rootVer_;
        uint64_t pv = olcReadLock(parentLk->w);
        ArtNode* node = root;  // read under the acquired parent read-version
        unsigned depth = 0;

        for (;;) {
            if (node == nullptr) {
                if (!olcCheck(parentLk->w, pv)) goto restart;
                return nullptr;  // empty slot ⇒ absent
            }
            if (isLeaf(node)) {
                // The slot/pointer we followed must still be current; a leaf is
                // an immutable tagged pointer, so its value is stable once the
                // pointer is validated.  Compare the full key.
                if (!olcCheck(parentLk->w, pv)) goto restart;
                uint8_t leafKey[maxPrefixLength];
                loadKey(getLeafValue(node), leafKey);
                for (unsigned i = 0; i < maxPrefixLength; i++)
                    if (leafKey[i] != key[i]) return nullptr;
                return node;
            }

            OlcLock* nodeLk = &stripeVer(node);
            uint64_t v = olcReadLock(nodeLk->w);
            // The pointer we followed to `node` must still be valid.
            if (!olcCheck(parentLk->w, pv)) goto restart;

            if (node->prefixLength) {
                unsigned mm = prefixMismatch(node, key, depth, maxPrefixLength);
                if (!olcCheck(nodeLk->w, v)) goto restart;  // prefix read valid
                if (mm != node->prefixLength) return nullptr;  // absent
                depth += node->prefixLength;
            }

            ArtNode** childRef = findChild(node, key[depth]);
            ArtNode* child = *childRef;
            if (!olcCheck(nodeLk->w, v)) goto restart;  // child ptr consistent

            parentLk = nodeLk;
            pv = v;
            node = child;
            depth += 1;
        }
    }

#ifdef QUART_CONC_STATS
    struct ConcStats {
        uint64_t total, fast, slow, conflicts, obsolete;
        // Restart cause breakdown (restart_read + restart_upgrade == conflicts)
        // and lock-wait counters.
        uint64_t restart_read, restart_upgrade, wait_events, wait_spins;
    };
    ConcStats stats() const {
        uint64_t t = cs_total_.load(std::memory_order_relaxed);
        uint64_t s = cs_slow_.load(std::memory_order_relaxed);
        return {t,
                t - s,
                s,
                cs_conflicts_.load(std::memory_order_relaxed),
                cs_obsolete_.load(std::memory_order_relaxed),
                cs_restart_read_.load(std::memory_order_relaxed),
                cs_restart_upgrade_.load(std::memory_order_relaxed),
                g_olc_wait_events.load(std::memory_order_relaxed),
                g_olc_wait_spins.load(std::memory_order_relaxed)};
    }
#endif

   protected:
#ifdef QUART_CONC_STATS
    std::atomic<uint64_t> cs_total_{0};
    std::atomic<uint64_t> cs_slow_{0};
    std::atomic<uint64_t> cs_conflicts_{0};  // optimistic-descent restarts
    std::atomic<uint64_t> cs_obsolete_{0};   // fast-path obsolete-tail catches
    // Why a descent restarted: an optimistic read-version check failed (a
    // writer touched a node we had already read) vs. we lost the CAS to upgrade
    // a node to the write lock (another writer grabbed it first).  Both force a
    // full restart from the root; their sum equals cs_conflicts_.
    std::atomic<uint64_t> cs_restart_read_{0};
    std::atomic<uint64_t> cs_restart_upgrade_{0};
    // Charge a restart to its cause and return false (the descent's sentinel).
    bool statReadFail() {
        cs_restart_read_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool statUpgradeFail() {
        cs_restart_upgrade_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#else
    static bool statReadFail() { return false; }
    static bool statUpgradeFail() { return false; }
#endif

    static constexpr unsigned NSTRIPES = 1u << 17;  // 131072 version words
    static constexpr unsigned NRETIRE = 512;        // retire-list shards

    struct alignas(64) RetireShard {
        OlcLock lk;  // reuse the version word purely as a mutex for the shard
        std::vector<std::pair<ArtNode*, int8_t>> v;
    };

    int numThreads_;
    std::vector<OlcLock> stripes_;  // per-node version words (striped)
    OlcLock rootVer_;               // guards the &root slot
    std::array<RetireShard, NRETIRE> retire_;

    // Map a node's address to its version word.  Addresses are stable for the
    // run (deferred reclamation); distinct live nodes may share a word (merely
    // conservative — costs an occasional spurious restart, never unsafe).
    OlcLock& stripeVer(const ArtNode* n) {
        uintptr_t p = reinterpret_cast<uintptr_t>(n) >> 4;  // nodes are ≥16B
        p *= 0x9E3779B97F4A7C15ull;
        return stripes_[(p >> 29) & (NSTRIPES - 1)];
    }

    void reclaimNode(ArtNode* n) override {
        int8_t t = n->type;
        n->type = NodeTypeObsolete;
        uintptr_t p = reinterpret_cast<uintptr_t>(n) >> 4;
        p *= 0x9E3779B97F4A7C15ull;
        RetireShard& sh = retire_[(p >> 17) & (NRETIRE - 1)];
        uint64_t lv = olcWriteLock(sh.lk.w);
        sh.v.emplace_back(n, t);
        olcWriteUnlock(sh.lk.w, lv);
    }

    static void freeByType(ArtNode* n, int8_t t) {
        switch (t) {
            case NodeType4:
                delete static_cast<Node4*>(n);
                break;
            case NodeType16:
                delete static_cast<Node16*>(n);
                break;
            case NodeType48:
                delete static_cast<Node48*>(n);
                break;
            case NodeType256:
                delete static_cast<Node256*>(n);
                break;
            default:
                delete n;
                break;
        }
    }

    static bool wouldGrow(ArtNode* n) {
        switch (n->type) {
            case NodeType4:
                return n->count >= 4;
            case NodeType16:
                return n->count >= 16;
            case NodeType48:
                return n->count >= 48;
            default:  // Node256 never grows
                return false;
        }
    }

    // Dispatch a base (fp-agnostic) child insert; may grow `node` (which
    // rewrites *nodeRef in the parent and reclaims the old node).
    void insertChild(ArtNode* node, ArtNode** nodeRef, uint8_t b,
                     ArtNode* leaf) {
        switch (node->type) {
            case NodeType4:
                static_cast<Node4*>(node)->insertNode4(this, nodeRef, b, leaf);
                break;
            case NodeType16:
                static_cast<Node16*>(node)->insertNode16(this, nodeRef, b,
                                                         leaf);
                break;
            case NodeType48:
                static_cast<Node48*>(node)->insertNode48(this, nodeRef, b,
                                                         leaf);
                break;
            case NodeType256:
                static_cast<Node256*>(node)->insertNode256(this, nodeRef, b,
                                                           leaf);
                break;
        }
    }

    // Acquire exclusive locks on parent-slot and node in the fixed order
    // parent-then-node.  Handles stripe ALIASING (parent and node hashing to
    // the same version word): if aliased, a single upgrade covers both, but
    // only when the two snapshots agree (else the word moved between the two
    // reads ⇒ inconsistent ⇒ conflict).  Returns false (holding nothing) on any
    // failure.
    bool lockTwo(OlcLock* parentLk, uint64_t pv, OlcLock* nodeLk, uint64_t v) {
        if (parentLk == nodeLk) {
            if (pv != v) return false;
            return olcTryUpgrade(nodeLk->w, v);
        }
        if (!olcTryUpgrade(parentLk->w, pv)) return false;
        if (!olcTryUpgrade(nodeLk->w, v)) {
            olcWriteUnlock(parentLk->w, pv);
            return false;
        }
        return true;
    }
    void unlockTwo(OlcLock* parentLk, uint64_t pv, OlcLock* nodeLk,
                   uint64_t v) {
        if (parentLk == nodeLk) {
            olcWriteUnlock(nodeLk->w, v);
            return;
        }
        olcWriteUnlock(nodeLk->w, v);
        olcWriteUnlock(parentLk->w, pv);
    }
};

}  // namespace ART
