#pragma once

#include "../ART.h"
#include "../ArtNode.h"
#include "QuART.h"

namespace ART {

// QuART_lil: fp always points to the node containing the last inserted
// leaf. Every insert uses insert_recursive_change_fp so fp is updated on each
// insert. When the upper (maxPrefixLength-1) bytes of the new key match those
// of fp_leaf the insertion can start directly from the current fp node;
// otherwise it restarts from the root.
class QuART_lil : public QuART {
   public:
    QuART_lil() : QuART() {}

    void insert(uint8_t key[], uintptr_t value) {
        ArtNode* root = this->root;

        // If the root is null or a single leaf, insert from the root.
        if (root == nullptr || isLeaf(root)) {
            insert_recursive_change_fp(
                this->root, &this->root, key, 0, value, maxPrefixLength);
            return;
        }

        int leafValue = getLeafValue(this->fp_leaf);

        // Check whether all bytes except the last match fp_leaf.
        for (size_t i = 0; i < maxPrefixLength - 2; ++i) {
            uint8_t leafByte =
                (leafValue >> (8 * (maxPrefixLength - 2 - i))) & 0xFF;
            if (leafByte != key[i]) {
                // Upper bytes differ: restart from root.
                insert_recursive_change_fp(
                    this->root, &this->root, key, 0, value, maxPrefixLength);
                return;
            }
        }

        // Upper bytes match: fast path starting from fp.
        if (this->fp_depth == maxPrefixLength - 2) {
            // fp is at the last-byte level; insert directly into the fp node.
            ArtNode* newNode = makeLeaf(value);
            switch (this->fp->type) {
                case NodeType4:
                    static_cast<Node4*>(this->fp)->insertNode4ChangeFp(
                        this, this->fp_ref, key[fp_depth], newNode);
                    break;
                case NodeType16:
                    static_cast<Node16*>(this->fp)->insertNode16ChangeFp(
                        this, this->fp_ref, key[fp_depth], newNode);
                    break;
                case NodeType48:
                    static_cast<Node48*>(this->fp)->insertNode48ChangeFp(
                        this, this->fp_ref, key[fp_depth], newNode);
                    break;
                case NodeType256:
                    static_cast<Node256*>(this->fp)->insertNode256ChangeFp(
                        this, this->fp_ref, key[fp_depth], newNode);
                    break;
            }
        } else {
            // fp is at an intermediate level; recurse from root to avoid
            // fp_depth / prefix double-counting issues.
            insert_recursive_change_fp(
                this->root, &this->root, key, 0, value, maxPrefixLength);
        }
    }
};

}  // namespace ART