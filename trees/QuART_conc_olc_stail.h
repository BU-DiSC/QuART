#pragma once

#include <emmintrin.h>  // _mm_pause

#include <atomic>
#include <cstdint>
#include <vector>

#include "../ART.h"
#include "../ArtNode.h"
#include "QuART.h"
// OLC machinery: OlcLock + version-word helpers, and QuART_conc_olc_base
// (striped stripes_/rootVer_/retire_, stripeVer, reclaimNode, lockTwo/
// unlockTwo, insertChild) — the shared base this tree subclasses.
#include "olc_base.h"

// Fast-path effectiveness counters (compiled only under -DQUART_STAIL_FP_STATS,
// via CMake -DQUART_STAIL_FP_STATS=ON).  STAIL_FP_BUMP is a no-op when the flag
// is off — its argument (a counter member that only exists under the flag) is
// never referenced in that case, so the members can stay ifdef'd out.  Like the
// other stat flags these atomics perturb the insert hot path, so this is a
// counts-only build; take timings from a stats-off build.
#ifdef QUART_STAIL_FP_STATS
#define STAIL_FP_BUMP(x) (x).fetch_add(1, std::memory_order_relaxed)
#else
#define STAIL_FP_BUMP(x) ((void)0)
#endif

namespace ART {

// Per-thread tail cache: only the owning thread writes it.  Cache-line padded
// to stop adjacent threads' caches from false-sharing.
struct alignas(64) ConcOlcCtx {
    ArtNode* fp = nullptr;  // cached tail inner node (branches on last byte)
    ArtNode** fp_ref = nullptr;  // slot in fp's parent that holds fp
    size_t fp_depth = 0;         // depth at which fp branches
    uint8_t fp_type = 0;         // cached fp->type
    key_int_t last_upper = 0;    // upper bytes of the last key this thread saw
    bool initialized = false;
    char pad[64 - (sizeof(ArtNode*) * 2 + sizeof(size_t) * 2 + 2)];
};

// ─────────────────────────────────────────────────────────────────────────────
// QuART_conc_olc_stail — the CONCURRENT version of QuART_stail.
//
// Built on the shared OLC machinery in olc_base.h (QuART_conc_olc_base): the
// lock-free versioned descent, exclusive-lock-only-on-mutated-node protocol,
// and deferred reclamation.  On top of that it adds a per-thread fast-path tail
// plus QuART_stail's richer per-key policy: it CLASSIFIES each key against the
// tail's region via a three-way FP_INSERT / BRIDGE / OTHER test and applies
// reset- counter hysteresis so a short burst of out-of-stream keys doesn't tear
// down the fast path.
//
// This tree is stail's exact decision logic made concurrent and per-stream:
//   • Every thread owns its OWN tail + classifier state (StailState + the
//     per-thread ConcOlcCtx tail), so W near-sorted streams each keep a private
//     fast path — the whole point ("conc_olc_stail = concurrent stail for many
//     near-sorted streams").
//   • The three structural outcomes are mapped onto the SAME verified OLC
//     descent:
//       FP_INSERT @ leaf depth → tryFastAppend (write-lock ONLY the tail node)
//       FP_INSERT @ sub-leaf   → preserve descent (insert, keep the tail)
//       BRIDGE                 → change  descent (insert, move the tail here)
//       OTHER                  → reset-counter hysteresis: change on the Nth
//                                miss, otherwise preserve (== stail exactly)
//
// 1-THREAD EQUIVALENCE TO stail.  With one thread there is no contention, so
// the OLC version words never force a restart, and the sequence of decisions is
// byte-for-byte stail's: identical getKeyType classification (against the same
// cached region upper), identical RESET_COUNTER_INIT=16 hysteresis, identical
// "append vs preserve vs change" routing → identical resulting tree.  The only
// differences are mechanical and invisible to the result: (1) the structural
// insert is done by the OLC descent-from-root rather than stail's
// insert_recursive_{preserve,change}_fp (same key ends in the same slot), and
// (2) every fast append pays one uncontended lock/unlock on the tail's version
// word (the price of being concurrency-safe).  So it makes the same choices as
// stail and is a hair slower per op; with many threads it scales like OLC ART.
//
// Correctness: identical model to QuART_conc_olc_art (verify-every-key after
// join — see olc_base.h).  reclaimNode + deferred reclamation make a
// restored-but-obsoleted tail safe to inspect.
// ─────────────────────────────────────────────────────────────────────────────
class QuART_conc_olc_stail : public QuART_conc_olc_base {
   public:
    explicit QuART_conc_olc_stail(int numThreads)
        : QuART_conc_olc_base(numThreads), ctx(numThreads), st_(numThreads) {}

#ifdef QUART_STAIL_TRACE
    // Per-key decision tallies (same 6 categories as QuART_stail::DecTrace) so
    // a 1-thread run can be diffed against stail to confirm identical
    // decisions.
    struct DecTrace {
        uint64_t warmup = 0, append = 0, fp_preserve = 0, bridge = 0,
                 other_change = 0, other_preserve = 0;
    };
    DecTrace trace;
#endif

#ifdef QUART_STAIL_FP_STATS
    // Fast-path effectiveness snapshot (see STAIL_FP_BUMP above).  The five
    // structural categories + warmup partition every insert (their sum ==
    // total), so this is the concurrent, runtime version of the QUART_STAIL_TRACE
    // histogram plus the tryFastAppend accounting the trace can't see.
    struct FpStats {
        uint64_t total;  // == warmup+append+fp_preserve+bridge+*_change+*_prsv
        // Per-key classification histogram:
        uint64_t warmup;          // no tail yet ⇒ change descent (establishes fp)
        uint64_t append;          // FP_INSERT @ leaf depth  → tryFastAppend
        uint64_t fp_preserve;     // FP_INSERT @ sub-leaf     → in-region insert
        uint64_t bridge;          // BRIDGE (±1 region)       → change descent
        uint64_t other_change;    // OTHER, hysteresis fired  → change descent
        uint64_t other_preserve;  // OTHER, hysteresis held   → out-of-region
        // tryFastAppend accounting:
        uint64_t fast_attempt;    // tryFastAppend calls (== append)
        uint64_t fast_success;    // appended into the tail node in place
        uint64_t fast_contended;  // attempts whose tail write-lock was contended
    };
    FpStats fpStats() const {
        uint64_t w = cls_warmup_.load(std::memory_order_relaxed);
        uint64_t a = cls_append_.load(std::memory_order_relaxed);
        uint64_t fpp = cls_fp_preserve_.load(std::memory_order_relaxed);
        uint64_t br = cls_bridge_.load(std::memory_order_relaxed);
        uint64_t oc = cls_other_change_.load(std::memory_order_relaxed);
        uint64_t op = cls_other_preserve_.load(std::memory_order_relaxed);
        return {w + a + fpp + br + oc + op,
                w,
                a,
                fpp,
                br,
                oc,
                op,
                fp_attempt_.load(std::memory_order_relaxed),
                fp_success_.load(std::memory_order_relaxed),
                fp_contended_.load(std::memory_order_relaxed)};
    }
#endif

    // Abstract-base requirement: single-threaded entry routes to thread 0.
    void insert(uint8_t key[], uintptr_t value) override {
        insert(0, key, value);
    }

    // Per-thread insert.  Thread `tid` must be the only caller for that tid.
    void insert(int tid, uint8_t key[], uintptr_t value) {
        ConcOlcCtx& c = ctx[tid];
        StailState& s = st_[tid];
        const key_int_t keyUpper = getKeyUpperBytes(key);
#ifdef QUART_CONC_STATS
        cs_total_.fetch_add(1, std::memory_order_relaxed);
#endif

        // Warm-up: no inner tail yet (empty tree, or the thread's first keys
        // before a tail node exists).  Establish the tail via a change descent.
        // Mirrors stail's `root == nullptr ⇒ insert_recursive_change_fp`.
        if (!s.warm || !c.initialized || c.fp == nullptr) {
#ifdef QUART_STAIL_TRACE
            trace.warmup++;
#endif
            STAIL_FP_BUMP(cls_warmup_);
            changeDescent(c, s, key, value, keyUpper);
            return;
        }

        switch (classify(keyUpper, s.cached_upper)) {
            case StailKeyType::FP_INSERT:
                s.reset_counter = RESET_COUNTER_INIT;
                if (c.fp_depth == (size_t)maxPrefixLength - 2) {
#ifdef QUART_STAIL_TRACE
                    trace.append++;
#endif
                    STAIL_FP_BUMP(cls_append_);
                    // Leaf-depth append (stail 2a).  Obsolete / would-grow tail
                    // ⇒ rebuild in-region (advance the tail, keep the anchor).
                    if (!tryFastAppend(c, key, value))
                        inRegionInsert(c, key, value);
                } else {
#ifdef QUART_STAIL_TRACE
                    trace.fp_preserve++;
#endif
                    STAIL_FP_BUMP(cls_fp_preserve_);
                    // Sub-leaf-depth fp ⇒ the key is IN the anchor region, so
                    // advance the tail toward leaf depth (so later same-region
                    // keys fast-append) while keeping the classification anchor
                    // — exactly stail's insert_recursive_preserve_fp(fp, ...).
                    inRegionInsert(c, key, value);
                }
                return;

            case StailKeyType::BRIDGE:
#ifdef QUART_STAIL_TRACE
                trace.bridge++;
#endif
                STAIL_FP_BUMP(cls_bridge_);
                // Adjacent region ⇒ stail's change_fp: rebuild the tail here.
                s.reset_counter = RESET_COUNTER_INIT;
                changeDescent(c, s, key, value, keyUpper);
                return;

            case StailKeyType::OTHER:
                // stail's reset-counter hysteresis: only move the tail after
                // RESET_COUNTER_INIT consecutive out-of-stream keys; until then
                // insert while preserving the current fast path.
                if (s.reset_counter == 0) {
#ifdef QUART_STAIL_TRACE
                    trace.other_change++;
#endif
                    STAIL_FP_BUMP(cls_other_change_);
                    s.reset_counter = RESET_COUNTER_INIT;
                    changeDescent(c, s, key, value, keyUpper);
                } else {
#ifdef QUART_STAIL_TRACE
                    trace.other_preserve++;
#endif
                    STAIL_FP_BUMP(cls_other_preserve_);
                    // Out-of-region key ⇒ stail's insert_recursive_preserve_fp
                    // (from root): insert but leave the fast-path tail and
                    // anchor untouched (the structural change doesn't touch the
                    // tail).
                    s.reset_counter--;
                    outOfRegionInsert(c, key, value);
                }
                return;
        }
    }

   private:
    static constexpr int RESET_COUNTER_INIT = 16;  // == QuART_stail

    enum class StailKeyType { FP_INSERT, BRIDGE, OTHER };

    // Per-thread classifier state (the tail pointers live in the per-thread
    // ConcOlcCtx).  cache-line padded so adjacent threads don't false-share.
    struct alignas(64) StailState {
        key_int_t cached_upper = 0;  // upper bytes of the current tail region
        int reset_counter = RESET_COUNTER_INIT;
        bool warm = false;
        char pad[64 - sizeof(key_int_t) - sizeof(int) - sizeof(bool)];
    };
    std::vector<ConcOlcCtx> ctx;  // ctx[i] owned by thread i (the tail cache)
    std::vector<StailState> st_;  // st_[i] owned by thread i

#ifdef QUART_STAIL_FP_STATS
    // Fast-path effectiveness counters (see FpStats / STAIL_FP_BUMP).  Shared
    // across all W threads, so relaxed atomics — the only guarantee needed is
    // that the post-join total is exact.
    std::atomic<uint64_t> cls_warmup_{0}, cls_append_{0}, cls_fp_preserve_{0},
        cls_bridge_{0}, cls_other_change_{0}, cls_other_preserve_{0};
    std::atomic<uint64_t> fp_attempt_{0}, fp_success_{0}, fp_contended_{0};
#endif

    // stail's getKeyType, comparing the key's upper bytes against the cached
    // region upper (== getLeafUpperBytes of the tail leaf, which stail only
    // updates on a change_fp — so a cached upper is exactly equivalent).
    static StailKeyType classify(key_int_t keyUpper, key_int_t leafUpper) {
        if (keyUpper == leafUpper) return StailKeyType::FP_INSERT;
        if (((keyUpper + 1) & upperMask) == leafUpper ||
            ((leafUpper + 1) & upperMask) == keyUpper)
            return StailKeyType::BRIDGE;
        return StailKeyType::OTHER;
    }

    // OLC-locked append of one leaf into this thread's cached tail node c.fp.
    // The caller must have decided (by classification) that `key` belongs in
    // c.fp at leaf depth (c.fp_depth == maxPrefixLength-2).  Write-locks only
    // the tail's version word.  Returns true if the leaf was appended; false if
    // the tail is obsolete or would grow (⇒ caller rebuilds in-region).
    // `leaf` is a tagged pointer (makeLeaf does no allocation), so a false
    // return leaks nothing — the slow path re-tags the same value.
    bool tryFastAppend(ConcOlcCtx& c, uint8_t key[], uintptr_t value) {
        STAIL_FP_BUMP(fp_attempt_);
        std::atomic<uint64_t>& w = stripeVer(c.fp).w;
#ifndef QUART_STAIL_LOCKED_APPEND
        // ── LOCK-FREE fast path: append into a Node256 tail ──
        // OLC's per-append write-lock is unnecessarily pessimistic here.  A
        // Node256 leaf-parent is TERMINAL (never grows/splits/frees), so c.fp is
        // stable; and for a sorted stream successive keys have DISTINCT last
        // bytes, so each append writes a DISTINCT child slot.  Concurrent
        // appends to the same Node256 (even across streams that share this
        // subtree, since distinct keys ⇒ distinct branch bytes in a shared
        // leaf-parent) therefore never collide.  So we store child[b] WITHOUT
        // write-locking the node's version word: the tail's version line stays
        // SHARED across cores instead of bouncing to Modified on every insert —
        // removing the uncontended-atomic "safety tax" that made even 1-thread
        // stail slower than serial stail.  Guard: read the node version, confirm
        // the type snapshot is coherent (no writer held/holds the lock), then
        // store.  count is advisory for a Node256 (lookups index child[]
        // directly; the node never grows) so it is left untouched — matching the
        // pool's tryFastAppendVersioned Node256 path.  Any non-coherent snapshot
        // or non-Node256 tail falls through to the locked path below.
        {
            uint64_t v = olcReadLock(w);
            ArtNode* fp = c.fp;
            if (fp->type == NodeType256 && olcCheck(w, v)) {
                static_cast<Node256*>(fp)->child[key[c.fp_depth]] =
                    makeLeaf(value);
                STAIL_FP_BUMP(fp_success_);
                return true;
            }
        }
#endif
#ifdef QUART_STAIL_FP_STATS
        // Instrumented tail-node write-lock: charge a contention event when the
        // word can't be grabbed on the first try (another thread holds it, or
        // we lose the upgrade CAS), then fall back to the blocking lock.  NB:
        // stripe aliasing means a DIFFERENT node hashing to the same version
        // word also trips this, so it is an upper bound on true tail contention
        // — inherent to the striped design, not a bug.
        uint64_t lv;
        {
            uint64_t v0 = w.load(std::memory_order_acquire);
            if ((v0 & 1) || !olcTryUpgrade(w, v0)) {
                fp_contended_.fetch_add(1, std::memory_order_relaxed);
                lv = olcWriteLock(w);
            } else {
                lv = v0;
            }
        }
#else
        uint64_t lv = olcWriteLock(w);
#endif
        ArtNode* fp = c.fp;
        if (fp->type != NodeTypeObsolete) {
            const uint8_t b = key[c.fp_depth];
            ArtNode* leaf = makeLeaf(value);
            switch (fp->type) {
                case NodeType256:
                    static_cast<Node256*>(fp)->insertNode256(this, c.fp_ref, b,
                                                             leaf);
                    STAIL_FP_BUMP(fp_success_);
                    olcWriteUnlock(w, lv);
                    return true;
                case NodeType4:
                    if (fp->count < 4) {
                        static_cast<Node4*>(fp)->insertNode4(this, c.fp_ref, b,
                                                             leaf);
                        STAIL_FP_BUMP(fp_success_);
                        olcWriteUnlock(w, lv);
                        return true;
                    }
                    break;
                case NodeType16:
                    if (fp->count < 16) {
                        static_cast<Node16*>(fp)->insertNode16(this, c.fp_ref,
                                                               b, leaf);
                        STAIL_FP_BUMP(fp_success_);
                        olcWriteUnlock(w, lv);
                        return true;
                    }
                    break;
                case NodeType48:
                    if (fp->count < 48) {
                        static_cast<Node48*>(fp)->insertNode48(this, c.fp_ref,
                                                               b, leaf);
                        STAIL_FP_BUMP(fp_success_);
                        olcWriteUnlock(w, lv);
                        return true;
                    }
                    break;
            }
        }
#ifdef QUART_CONC_STATS
        else {
            cs_obsolete_.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        olcWriteUnlock(w, lv);  // obsolete or would-grow ⇒ rebuild in-region
        return false;
    }

    // Structural insert that MOVES the tail to the just-built terminal and
    // re-anchors the region (== stail's *_change_fp).  slowInsert's descent
    // sets c.fp/fp_ref/fp_depth/fp_type on success.
    void changeDescent(ConcOlcCtx& c, StailState& s, uint8_t key[],
                       uintptr_t value, key_int_t keyUpper) {
        slowInsert(c, key, value);
        c.last_upper = keyUpper;
        c.initialized = true;
        s.cached_upper = keyUpper;
        s.warm = true;
    }

    // IN-REGION preserve: the key belongs to the tail's region, so advance the
    // tail NODE to the just-built terminal (letting it reach leaf-parent depth
    // so later same-region keys fast-append) while KEEPING the classification
    // anchor.  == stail's insert_recursive_preserve_fp(fp, ...), whose hooks
    // advance fp as they deepen the tail but never move fp_leaf.
    void inRegionInsert(ConcOlcCtx& c, uint8_t key[], uintptr_t value) {
        slowInsert(c, key, value);  // setTail advances c.fp/fp_ref/fp_depth
    }

    // OUT-OF-REGION preserve: the key is outside the tail's region (stail's
    // OTHER + hysteresis).  Insert it structurally but leave the fast-path tail
    // exactly where it was — in stail the fp hooks don't fire for a subtree
    // that doesn't contain fp_leaf, so fp is untouched.  slowInsert's setTail
    // would wrongly repoint the tail into this foreign subtree (and a later
    // same-anchor append would then corrupt it), so snapshot and restore the
    // tail fields. Safe under deferred reclamation: even if the restored tail
    // is later obsoleted, tryFastAppend's obsolete check catches it.
    void outOfRegionInsert(ConcOlcCtx& c, uint8_t key[], uintptr_t value) {
        ArtNode* fp = c.fp;
        ArtNode** fp_ref = c.fp_ref;
        size_t fp_depth = c.fp_depth;
        uint8_t fp_type = c.fp_type;
        slowInsert(c, key, value);
        c.fp = fp;
        c.fp_ref = fp_ref;
        c.fp_depth = fp_depth;
        c.fp_type = fp_type;
    }

    void slowInsert(ConcOlcCtx& c, uint8_t key[], uintptr_t value) {
#ifdef QUART_CONC_STATS
        cs_slow_.fetch_add(1, std::memory_order_relaxed);
#endif
        while (!descendAndInsert(c, key, value)) {
#ifdef QUART_CONC_STATS
            cs_conflicts_.fetch_add(1, std::memory_order_relaxed);
#endif
            for (int b = 0; b < 24; b++) _mm_pause();  // backoff, then restart
        }
    }

    // One optimistic lock-coupled descent+mutation.  Returns true on success,
    // false on a version conflict (caller restarts from the root).  On the read
    // path it takes NO locks — only version snapshots + checkOrRestart — so
    // shared upper nodes are never written by descenders.  It upgrades to an
    // exclusive lock only on the node being mutated (plus its parent when a
    // grow/split rewrites the parent's child slot), in the fixed order
    // parent-then-node, releasing on any failure ⇒ deadlock-free.  On success
    // it updates this thread's tail cache from the terminal it built.
    bool descendAndInsert(ConcOlcCtx& c, uint8_t key[], uintptr_t value) {
        auto setTail = [&](ArtNode* fp, ArtNode** ref, unsigned d) {
            c.fp = fp;
            c.fp_ref = ref;
            c.fp_depth = d;
            c.fp_type = fp->type;
        };

        OlcLock* parentLk = &rootVer_;  // guards *nodeRef (== &root at the top)
        uint64_t pv = olcReadLock(parentLk->w);
        ArtNode** nodeRef = &root;
        ArtNode* node = root;  // read under the acquired parent read-version
        unsigned depth = 0;

        for (;;) {
            if (node == nullptr) {  // empty slot: plant a leaf (slot guarded)
                if (!olcTryUpgrade(parentLk->w, pv)) return statUpgradeFail();
                *nodeRef = makeLeaf(value);
                olcWriteUnlock(parentLk->w, pv);
                c.fp =
                    nullptr;  // no inner tail (first key); fast path stays off
                c.fp_ref = nullptr;
                c.fp_depth = 0;
                return true;
            }

            if (isLeaf(node)) {
                // Root (or any slot) holds a bare leaf: expand it into a Node4.
                // The slot is *nodeRef, guarded by parentLk (== rootVer_ at the
                // top); a leaf has no version word of its own.  Deeper leaves
                // are never reached here — they are caught by the isLeaf(child)
                // branch before we descend into them — so this fires only for a
                // single-key tree's root, but the code is correct for any slot.
                if (!olcTryUpgrade(parentLk->w, pv)) return statUpgradeFail();
                uint8_t existingKey[maxPrefixLength];
                loadKey(getLeafValue(node), existingKey);
                unsigned npl = 0;
                while (existingKey[depth + npl] == key[depth + npl]) npl++;
                Node4* nn = new Node4();
                nn->prefixLength = npl;
                memcpy(nn->prefix, key + depth, min(npl, maxPrefixLength));
                *nodeRef = nn;
                nn->insertNode4(this, nodeRef, existingKey[depth + npl], node);
                nn->insertNode4(this, nodeRef, key[depth + npl],
                                makeLeaf(value));
                setTail(nn, nodeRef, depth + npl);
                olcWriteUnlock(parentLk->w, pv);
                return true;
            }

            OlcLock* nodeLk = &stripeVer(node);
            uint64_t v = olcReadLock(nodeLk->w);
            // The pointer we followed to `node` must still be valid: re-check
            // the parent slot's version before trusting anything read out of
            // `node`.
            if (!olcCheck(parentLk->w, pv)) return statReadFail();

            if (node->prefixLength) {
                unsigned mm = prefixMismatch(node, key, depth, maxPrefixLength);
                if (!olcCheck(nodeLk->w, v))
                    return statReadFail();  // mm read consistent
                if (mm != node->prefixLength) {
                    // ── Prefix split: rewrites *nodeRef (parent) AND node ──
                    if (!lockTwo(parentLk, pv, nodeLk, v))
                        return statUpgradeFail();
                    Node4* nn = new Node4();
                    *nodeRef = nn;
                    nn->prefixLength = mm;
                    memcpy(nn->prefix, node->prefix, min(mm, maxPrefixLength));
                    if (node->prefixLength < maxPrefixLength) {
                        nn->insertNode4(this, nodeRef, node->prefix[mm], node);
                        node->prefixLength -= (mm + 1);
                        memmove(node->prefix, node->prefix + mm + 1,
                                min(node->prefixLength, maxPrefixLength));
                    } else {
                        node->prefixLength -= (mm + 1);
                        uint8_t minKey[maxPrefixLength];
                        loadKey(getLeafValue(minimum(node)), minKey);
                        nn->insertNode4(this, nodeRef, minKey[depth + mm],
                                        node);
                        memmove(node->prefix, minKey + depth + mm + 1,
                                min(node->prefixLength, maxPrefixLength));
                    }
                    nn->insertNode4(this, nodeRef, key[depth + mm],
                                    makeLeaf(value));
                    setTail(nn, nodeRef, depth + mm);
                    unlockTwo(parentLk, pv, nodeLk, v);
                    return true;
                }
                depth += node->prefixLength;
            }

            ArtNode** childRef = findChild(node, key[depth]);
            ArtNode* child = *childRef;
            if (!olcCheck(nodeLk->w, v))
                return statReadFail();  // child ptr consistent

            if (child == nullptr) {
                // ── Add child to `node` (may grow ⇒ rewrites *nodeRef) ──
                if (wouldGrow(node)) {
                    if (!lockTwo(parentLk, pv, nodeLk, v))
                        return statUpgradeFail();
                    insertChild(node, nodeRef, key[depth], makeLeaf(value));
                    setTail(*nodeRef, nodeRef, depth);  // grown node = *nodeRef
                    unlockTwo(parentLk, pv, nodeLk, v);
                } else {
                    if (!olcTryUpgrade(nodeLk->w, v)) return statUpgradeFail();
                    insertChild(node, nodeRef, key[depth], makeLeaf(value));
                    setTail(node, nodeRef, depth);
                    olcWriteUnlock(nodeLk->w, v);
                }
                return true;
            }

            if (isLeaf(child)) {
                // ── Expand leaf → Node4 (rewrites *childRef, inside `node`) ──
                if (!olcTryUpgrade(nodeLk->w, v)) return statUpgradeFail();
                uint8_t existingKey[maxPrefixLength];
                loadKey(getLeafValue(child), existingKey);
                unsigned d2 = depth + 1, npl = 0;
                while (existingKey[d2 + npl] == key[d2 + npl]) npl++;
                Node4* nn = new Node4();
                nn->prefixLength = npl;
                memcpy(nn->prefix, key + d2, min(npl, maxPrefixLength));
                *childRef = nn;
                nn->insertNode4(this, childRef, existingKey[d2 + npl], child);
                nn->insertNode4(this, childRef, key[d2 + npl], makeLeaf(value));
                setTail(nn, childRef, d2 + npl);
                olcWriteUnlock(nodeLk->w, v);
                return true;
            }

            // ── Descend: validate & release parent, hand off to `node` ──
            if (!olcCheck(parentLk->w, pv)) return statReadFail();
            parentLk = nodeLk;
            pv = v;
            nodeRef = childRef;
            node = child;
            depth += 1;
        }
    }
};

}  // namespace ART
