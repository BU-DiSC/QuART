#pragma once

#include "../ART.h"
#include "../ArtNode.h"
#include "QuART.h"

namespace ART {

// QuART_tail: fp always points to the node on the path to the largest key.
// Inserts a new key using insert_recursive_change_fp when the new key is
// greater than the current maximum, and insert_recursive_preserve_fp otherwise.
class QuART_tail : public QuART {
   public:
    QuART_tail() : QuART() {}

    void insert(uint8_t key[], uintptr_t value) {
        if (this->root == nullptr) {
            insert_recursive_change_fp(
                this->root, &this->root, key, 0, value, maxPrefixLength);
            return;
        }

        int leafValue = getLeafValue(this->fp_leaf);

        // Compare new key with current maximum (fp_leaf) byte by byte.
        for (size_t i = 0; i < maxPrefixLength - 2; i++) {
            uint8_t leafByte =
                (leafValue >> (8 * (maxPrefixLength - 2 - i))) & 0xFF;
            if (key[i] > leafByte) {
                // New key is greater: it becomes the new maximum.
                insert_recursive_change_fp(
                    this->root, &this->root, key, 0, value, maxPrefixLength);
                return;
            } else if (key[i] < leafByte) {
                // New key is smaller: current maximum is preserved.
                insert_recursive_preserve_fp(
                    this->root, &this->root, key, 0, value, maxPrefixLength);
                return;
            }
        }
        if (key[maxPrefixLength - 2] > (leafValue & 0xFF)) {
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
        else {
            // Upper bytes match: fast path starting from fp.
            if (this->fp_depth == maxPrefixLength - 2) {
                // fp is at the last-byte level; insert directly into the fp node.
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
            } else {
                // fp is at an intermediate level; recurse from root to avoid
                // fp_depth / prefix double-counting issues.
                insert_recursive_preserve_fp(
                    this->root, &this->root, key, 0, value, maxPrefixLength);
            }

        }
    }

};

}  // namespace ART