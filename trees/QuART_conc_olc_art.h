#pragma once

#include <emmintrin.h>  // _mm_pause

#include <cstdint>

#include "../ART.h"
#include "../ArtNode.h"
#include "QuART.h"
// OLC machinery: OlcLock + version-word helpers, and QuART_conc_olc_base
// (striped stripes_/rootVer_/retire_, stripeVer, reclaimNode, lockTwo/
// unlockTwo, insertChild) — the shared base this tree subclasses.
#include "olc_base.h"

namespace ART {

// ─────────────────────────────────────────────────────────────────────────────
// QuART_conc_olc_art — plain, multi-stream, concurrent OLC ART.  NO fast path.
//
// The no-fast-path sibling of QuART_conc_olc_stail: same robust,
// parameter-free, Optimistic-Lock-Coupling slow-path descent (lock-free
// versioned reads; exclusive lock only on the node being mutated, plus its
// parent on a grow/ split), but every insert is a full lock-coupled descent
// from the root.  It is the reference OLC ART baseline for MANY sorted streams
// inserted by MANY threads — the honest "what does the per-thread fast-path
// tail actually buy?" control for QuART_conc_olc_stail.
//
// This class keeps NO per-thread tail context at all: no cached tail, and the
// descent does not maintain one.  So it is a strictly cheaper plain OLC ART (no
// tail bookkeeping stores per insert), making it the fair no-fp control.  It
// shares the striped version table + node-mutation helpers with the fast-path
// tree through the common QuART_conc_olc_base.
//
// Correctness model is identical to QuART_conc_olc_stail: ASan-clean + verify-
// every-key.  See the long comment in olc_base.h.  Concurrent lookups are not
// supported — the intended use is parallel-insert → join() → verify.
// ─────────────────────────────────────────────────────────────────────────────
class QuART_conc_olc_art : public QuART_conc_olc_base {
   public:
    explicit QuART_conc_olc_art(int numThreads)
        : QuART_conc_olc_base(numThreads) {}

    // Abstract-base requirement: single-threaded entry routes through insert().
    void insert(uint8_t key[], uintptr_t value) override {
        insert(0, key, value);
    }

    // Per-thread insert.  `tid` is unused (no per-thread state) but kept for a
    // signature identical to QuART_conc_olc_stail so both drop into the same
    // harness.
    void insert(int /*tid*/, uint8_t key[], uintptr_t value) {
#ifdef QUART_CONC_STATS
        cs_total_.fetch_add(1, std::memory_order_relaxed);
        cs_slow_.fetch_add(1, std::memory_order_relaxed);
#endif
        while (!descendAndInsert(key, value)) {
#ifdef QUART_CONC_STATS
            cs_conflicts_.fetch_add(1, std::memory_order_relaxed);
#endif
            for (int b = 0; b < 24; b++) _mm_pause();  // backoff, then restart
        }
    }

   private:
    // One optimistic lock-coupled descent+mutation.  Returns true on success,
    // false on a version conflict (caller restarts from the root).  Identical
    // to QuART_conc_olc_stail::descendAndInsert but with NO tail-cache
    // maintenance — this tree keeps no per-thread fast-path context.
    bool descendAndInsert(uint8_t key[], uintptr_t value) {
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
                return true;
            }

            if (isLeaf(node)) {
                // Root (or any slot) holds a bare leaf: expand it into a Node4.
                // The slot is *nodeRef, guarded by parentLk (== rootVer_ at the
                // top); a leaf has no version word of its own.
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
                    unlockTwo(parentLk, pv, nodeLk, v);
                } else {
                    if (!olcTryUpgrade(nodeLk->w, v)) return statUpgradeFail();
                    insertChild(node, nodeRef, key[depth], makeLeaf(value));
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
