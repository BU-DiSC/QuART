#pragma once

#include "../ART.h"
#include "../ArtNode.h"
#include "QuART.h"

enum class KeyType { FP_INSERT, BRIDGE, OTHER };

namespace ART {

class QuART_stail : public QuART {
   private:
    static constexpr int RESET_COUNTER_INIT = 16;
    int reset_counter;

   public:
    QuART_stail() : QuART(), reset_counter(RESET_COUNTER_INIT) {}

#ifdef QUART_STAIL_TRACE
    // Decision-type tallies (only compiled under -DQUART_STAIL_TRACE), used to
    // confirm QuART_conc_olc_stail makes the identical per-key decisions at 1
    // thread.  Categories: warmup (root==null), append (FP_INSERT @ leaf
    // depth), fp_preserve (FP_INSERT @ sub-leaf), bridge, other_change,
    // other_preserve.
    struct DecTrace {
        uint64_t warmup = 0, append = 0, fp_preserve = 0, bridge = 0,
                 other_change = 0, other_preserve = 0;
    };
    DecTrace trace;
#endif

    void insert(uint8_t key[], uintptr_t value) {
        /* stail insert */

        ArtNode* root = this->root;

        // If the root is null, insert from scratch and change fp
        if (root == nullptr) {
#ifdef QUART_STAIL_TRACE
            trace.warmup++;
#endif
            insert_recursive_change_fp(this->root, &this->root, key, 0, value,
                                       maxPrefixLength);
            return;
        }

        KeyType type = getKeyType(key);

        if (type == KeyType::FP_INSERT) {
            if (this->reset_counter != RESET_COUNTER_INIT)
                this->reset_counter = RESET_COUNTER_INIT;
            if (this->fp_depth == maxPrefixLength - 2) {
#ifdef QUART_STAIL_TRACE
                trace.append++;
#endif
                // Insert leaf into fp
                ArtNode* newNode = makeLeaf(value);
                switch (this->fp->type) {
                    case NodeType4:
                        static_cast<Node4*>(this->fp)->insertNode4PreserveFp(
                            this, this->fp_ref, key[fp_depth], newNode);
                        break;
                    case NodeType16:
                        static_cast<Node16*>(this->fp)->insertNode16PreserveFp(
                            this, this->fp_ref, key[fp_depth], newNode);
                        break;
                    case NodeType48:
                        static_cast<Node48*>(this->fp)->insertNode48PreserveFp(
                            this, this->fp_ref, key[fp_depth], newNode);
                        break;
                    case NodeType256:
                        static_cast<Node256*>(this->fp)->insertNode256(
                            this, this->fp_ref, key[fp_depth], newNode);
                        break;
                }
                return;
            }
            // Else, we call the recursive function and let it handle leaf
            // expansion or prefix mismatch if there is one. If not, it will
            // directly insert into the fp
            else {
#ifdef QUART_STAIL_TRACE
                trace.fp_preserve++;
#endif
                insert_recursive_preserve_fp(this->fp, this->fp_ref, key,
                                             fp_depth, value, maxPrefixLength,
                                             this->fp_prev);
                return;
            }
        } else if (type == KeyType::BRIDGE) {
#ifdef QUART_STAIL_TRACE
            trace.bridge++;
#endif
            if (this->reset_counter != RESET_COUNTER_INIT)
                this->reset_counter = RESET_COUNTER_INIT;
            insert_recursive_change_fp(this->root, &this->root, key, 0, value,
                                       maxPrefixLength);
            return;
        } else {  // OTHER (both greater and less)
            if (this->reset_counter == 0) {
#ifdef QUART_STAIL_TRACE
                trace.other_change++;
#endif
                this->reset_counter = RESET_COUNTER_INIT;
                insert_recursive_change_fp(this->root, &this->root, key, 0,
                                           value, maxPrefixLength);
            } else {
#ifdef QUART_STAIL_TRACE
                trace.other_preserve++;
#endif
                this->reset_counter--;
                insert_recursive_preserve_fp(this->root, &this->root, key, 0,
                                             value, maxPrefixLength);
            }
            return;
        }
    }

    KeyType getKeyType(uint8_t key[]) {
        key_int_t leafUpper = getLeafUpperBytes(getLeafValue(this->fp_leaf));
        key_int_t keyUpper = getKeyUpperBytes(key);

        if (keyUpper == leafUpper) return KeyType::FP_INSERT;
        if (((keyUpper + 1) & upperMask) == leafUpper ||
            ((leafUpper + 1) & upperMask) == keyUpper)
            return KeyType::BRIDGE;
        return KeyType::OTHER;
    }
};

}  // namespace ART