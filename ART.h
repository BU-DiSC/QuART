/*
  Adaptive Radix Tree
  Viktor Leis, 2012
  leis@in.tum.de
 */

#pragma once

#include <assert.h>
#include <emmintrin.h>  // x86 SSE intrinsics
#include <immintrin.h>  // AVX512
#include <stdint.h>     // integer types
#include <stdio.h>
#include <stdlib.h>    // malloc, free
#include <string.h>    // memset, memcpy
#include <sys/time.h>  // gettime

#include <algorithm>  // std::random_shuffle
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <stdexcept>

#include "ArtNode.h"  // ArtNode definitions
#include "Helper.h"   // Helper functions

namespace ART {

// Groups the five FP tracking fields for one workload fingerprint.
struct FpSlot {
    ArtNode*  fp       = nullptr;
    ArtNode*  fp_prev  = nullptr;
    ArtNode*  fp_leaf  = nullptr;
    size_t    fp_depth = 0;
    ArtNode** fp_ref   = nullptr;
    // Cached copy of fp->type to avoid dereferencing fp on every dispatch.
    // Updated wherever fp is assigned (saveToSlot and hook callbacks).
    uint8_t   fp_type  = 0;
    // Cached result of getLeafUpperBytes(getLeafValue(fp_leaf)).
    // Avoids recomputing in the findSlot hot loop.
    // fp_leaf changes only in insert_recursive_change_fp paths, so this
    // is updated once per BRIDGE/NO_MATCH insert (in saveToSlot).
    key_int_t cached_upper = 0;
};

class ART {
   public:
    ArtNode* root;     // pointer to root node of tree
    ArtNode* fp;       // pointer to fast path node
    ArtNode* fp_prev;  // parent node of fp (holds the cell fp_ref points into)
    ArtNode* fp_leaf;  // pointer to leaf node in fast path
    size_t fp_depth;   // depth that will be used during fp insertion
    ArtNode** fp_ref;  // reference to fp node, used for insertion

    // constructor
    ART()
        : root(nullptr),
          fp(nullptr),
          fp_prev(nullptr),
          fp_leaf(nullptr),
          fp_depth(0),
          fp_ref(nullptr) {}

    void insert(uint8_t key[], uintptr_t value) {
        insert(this, root, &root, key, 0, value, maxPrefixLength);
    }

    ArtNode* lookup(uint8_t key[]) {
        return lookup(root, key, maxPrefixLength, 0, maxPrefixLength);
    }

    void printTree() { printTree(this->root, 0); }

    // Method to verify the fast path after each insertion.
    // Returns true if fp has fp_leaf as its maximum leaf.
    bool verifyTailPath() {
        if (this->fp == nullptr) {
            return true;
        }
        if (getLeafValue(maximum(this->fp)) == getLeafValue(this->fp_leaf)) {
            return true;
        }
        std::cerr << "Error: fp_leaf mismatch. Expected "
                  << getLeafValue(maximum(this->fp)) << ", got "
                  << getLeafValue(this->fp_leaf) << "." << std::endl;
        return false;
    }

    // Grouped bulk loading for a sorted, gap-free workload of 32-bit keys
    // (paper §3.4, Algorithm 2).  Keys are processed one 256-key group at a
    // time: all 256 keys of a group share the upper three bytes (b0,b1,b2) and
    // differ only in the final byte b3 (0..255), so they drop directly into a
    // depth-3 Node256 as its 256 slots, with no per-key root-down traversal.
    // Each group is classified against the previous group's prefix (Table 2):
    //   BL_FP_INSERT : same (b0,b1) prefix, next b2  -> stay under `cursor`
    //   BL_BRIDGE    : b2 wrapped 255->0, a new (b0,b1) parent is materialized
    // `cursor` is the paper's single pointer to the node for the current (b0,b1)
    // prefix, under which the group's Node256 hangs.
    // NOTE: 32-bit-key-specific (fixed 4-level tree structure).  Calling it with
    // QUART_KEY_64 defined produces incorrect results; use regular insert()
    // for 64-bit keys instead.
    void bulkLoad(const std::vector<key_int_t>& keys, const std::vector<key_int_t>& values) {
        ArtNode* cursor = nullptr;          // node for the current (b0,b1) prefix
        ArtNode** cursorRef = &this->root;  // slot in cursor's parent holding it
        std::array<uint8_t, 3> groupPrefix; // current group's (b0,b1,b2)

        // Calculate number of complete groups of 256
        size_t num_complete_groups = (keys.size()-1) / 256;
        size_t remaining_keys = (keys.size()-1) % 256;
        // Special case: handle first group when root is null
        if (root == nullptr) {
            // Materialize root -> b0 -> b1 -> depth-3 Node256 for the first 256 keys
            Node4* rootNode = new Node4();
            rootNode->prefixLength = 0;
            this->root = rootNode;
            
            Node4* b0Node = new Node4();
            b0Node->prefixLength = 0;
            rootNode->insertNode4(this, &this->root, 0, b0Node);
            
            Node4* b1Node = new Node4();
            b1Node->prefixLength = 0;
            b0Node->insertNode4(this, findChild(this->root, 0), 0, b1Node);
            
            Node256* groupNode = new Node256();
            groupNode->prefixLength = 0;
            
            b1Node->insertNode4(this, findChild(*findChild(this->root, 0), 0), 0, groupNode);
            
            cursor = b1Node;
            cursorRef = findChild(*findChild(this->root, 0), 0);
            groupPrefix = {{0, 0, 0}};
            
            // Insert 256 keys into this group
            for (size_t i = 1; i < 256; i++) {
                uint8_t b3 = keys[i] & 0xFF;
                groupNode->child[b3] = makeLeaf(values[i]);
            }
        }
        
        // Process complete groups of 256 keys
        for (size_t group = 1; group < num_complete_groups; group++) {
            size_t start_idx = group * 256;
            
            // Separate the first key of this group into big endian bytes
            uint8_t b0 = (keys[start_idx] >> 24) & 0xFF;
            uint8_t b1 = (keys[start_idx] >> 16) & 0xFF;
            uint8_t b2 = (keys[start_idx] >> 8) & 0xFF;

            // BL_BRIDGE at byte 0: b0 changed with b1=b2=0 (previous b0 was 255) -> new b0 parent
            if (b0 != groupPrefix[0] && b1 == 0 && b2 == 0) {
                Node4* newB0Node = new Node4();
                newB0Node->prefixLength = 0;
                switch (this->root->type) {
                    case NodeType4:
                        static_cast<Node4*>(this->root)->bulkLoadInsertNode4(this, &this->root, b0, newB0Node, cursor);
                        break;
                    case NodeType16:
                        static_cast<Node16*>(this->root)->bulkLoadInsertNode16(this, &this->root, b0, newB0Node, cursor);
                        break;
                    case NodeType48:
                        static_cast<Node48*>(this->root)->bulkLoadInsertNode48(this, &this->root, b0, newB0Node, cursor);
                        break;
                    case NodeType256:
                        static_cast<Node256*>(this->root)->bulkLoadInsertNode256(this, &this->root, b0, newB0Node, cursor);
                        break;
                }
                // hang a new b1 node under the new b0 node
                ArtNode** b0Ref = findChild(this->root, b0);
                ArtNode* b0Node = *b0Ref;
                Node4* newB1Node = new Node4();
                newB1Node->prefixLength = 0;
                static_cast<Node4*>(b0Node)->bulkLoadInsertNode4(this, b0Ref, b1, newB1Node, cursor);
                // advance cursor to the new (b0,b1) node
                cursor = newB1Node;
                cursorRef = findChild(*b0Ref, b1);
                groupPrefix = {b0, 0, 0};

                // materialize the group's Node256 under the new (b0,b1) node
                Node256* groupNode = new Node256();
                groupNode->prefixLength = 0;
                static_cast<Node4*>(newB1Node)->bulkLoadInsertNode4(this, cursorRef, b2, groupNode, cursor);
                // drop all 256 keys straight into the Node256 slots (b3 = slot index)
                for (size_t i = 0; i < 256; i++) {
                    uint8_t b3 = keys[start_idx + i] & 0xFF;
                    groupNode->child[b3] = makeLeaf(values[start_idx + i]);
                }
                continue;
            }
            // BL_BRIDGE at byte 1: b1 changed with b2=0 (previous b1 was 255) -> new b1 node
            else if (b0 == groupPrefix[0] && b1 != groupPrefix[1] && b2 == 0) {
                ArtNode** b0Ref = findChild(this->root, b0);
                ArtNode* b0Node = *b0Ref;
                // create the new (b0,b1) node
                Node4* newB1Node = new Node4();
                newB1Node->prefixLength = 0;
                switch (b0Node->type) {
                    case NodeType4:
                        static_cast<Node4*>(b0Node)->bulkLoadInsertNode4(this, b0Ref, b1, newB1Node, cursor);
                        break;
                    case NodeType16:
                        static_cast<Node16*>(b0Node)->bulkLoadInsertNode16(this, b0Ref, b1, newB1Node, cursor);
                        break;
                    case NodeType48:
                        static_cast<Node48*>(b0Node)->bulkLoadInsertNode48(this, b0Ref, b1, newB1Node, cursor);
                        break;
                    case NodeType256:
                        static_cast<Node256*>(b0Node)->bulkLoadInsertNode256(this, b0Ref, b1, newB1Node, cursor);
                        break;
                }
                // advance cursor to the new (b0,b1) node
                cursor = newB1Node;
                cursorRef = findChild(*b0Ref, b1);
                groupPrefix[1] = b1;
                groupPrefix[2] = 0;
                
                // materialize the group's Node256 under the new (b0,b1) node
                Node256* groupNode = new Node256();
                groupNode->prefixLength = 0;
                static_cast<Node4*>(newB1Node)->bulkLoadInsertNode4(this, cursorRef, b2, groupNode, cursor);
                // drop all 256 keys straight into the Node256 slots (b3 = slot index)
                for (size_t i = 0; i < 256; i++) {
                    uint8_t b3 = keys[start_idx + i] & 0xFF;
                    groupNode->child[b3] = makeLeaf(values[start_idx + i]);
                }              
                continue;
            }

            // BL_FP_INSERT: get/create the group's Node256 at slot b2 under cursor
            ArtNode** groupRef = findChild(cursor, b2);
            Node256* groupNode;
            
            if (*groupRef == nullptr) {
                // create the group's Node256
                groupNode = new Node256();
                groupNode->prefixLength = 0;
                // insert it under cursor (growing cursor if needed)
                switch (cursor->type) {
                    case NodeType4:
                        static_cast<Node4*>(cursor)->bulkLoadInsertNode4(this, cursorRef, b2, groupNode, cursor);
                        break;
                    case NodeType16:
                        static_cast<Node16*>(cursor)->bulkLoadInsertNode16(this, cursorRef, b2, groupNode, cursor);
                        break;
                    case NodeType48:
                        static_cast<Node48*>(cursor)->bulkLoadInsertNode48(this, cursorRef, b2, groupNode, cursor);
                        break;
                    case NodeType256:
                        static_cast<Node256*>(cursor)->bulkLoadInsertNode256(this, cursorRef, b2, groupNode, cursor);
                        break;
                }
                // Re-get the reference after potential expansion
                groupRef = findChild(cursor, b2);
                groupNode = static_cast<Node256*>(*groupRef);
            } else {
                groupNode = static_cast<Node256*>(*groupRef);
            }
            
            // drop all 256 keys straight into the Node256 slots (b3 = slot index)
            for (size_t i = 0; i < 256; i++) {
                uint8_t b3 = keys[start_idx + i] & 0xFF;
                groupNode->child[b3] = makeLeaf(values[start_idx + i]);
            }
            
            // Update groupPrefix to track current position
            groupPrefix[2] = b2;
        }
        
        // Insert remaining keys that don't make a full group of 256
        size_t start_idx2 = num_complete_groups * 256;
        for (size_t i = 0; i <= remaining_keys; i++) {
            //printf("Inserting remaining key %zu: %u\n", i, keys[start_idx2 + i]);
            uint8_t key_bytes[4];
            key_bytes[0] = (keys[start_idx2 + i] >> 24) & 0xFF;
            key_bytes[1] = (keys[start_idx2 + i] >> 16) & 0xFF;
            key_bytes[2] = (keys[start_idx2 + i] >> 8) & 0xFF;
            key_bytes[3] = keys[start_idx2 + i] & 0xFF;
            insert(this, root, &root, key_bytes, 0, values[start_idx2 + i], maxPrefixLength);
        }
        
        return;
    }

    // Insert one 32-bit key via the normal insert path (used for ragged ends of a range).
    inline void bulkInsertOne(key_int_t k, key_int_t v) {
        uint8_t kb[4];
        kb[0] = (k >> 24) & 0xFF; kb[1] = (k >> 16) & 0xFF;
        kb[2] = (k >> 8) & 0xFF;  kb[3] = k & 0xFF;
        insert(this, root, &root, kb, 0, v, maxPrefixLength);
    }

    // Algorithm 2's getOrCreateChild: get the child slot for byte `b` under
    // *parentRef, creating it (and growing the parent as needed) if absent.
    // leafLevel => materialize a depth-3 Node256 (the group node), else a Node4
    // path node. Returns a ref into the (possibly grown) parent's child array.
    ArtNode** getOrCreateChild(ArtNode** parentRef, uint8_t b, bool leafLevel) {
        ArtNode** cref = findChild(*parentRef, b);
        if (*cref != nullptr) return cref;
        ArtNode* child = leafLevel ? static_cast<ArtNode*>(new Node256())
                                   : static_cast<ArtNode*>(new Node4());
        child->prefixLength = 0;
        ArtNode* dummy = nullptr;
        ArtNode* parent = *parentRef;
        switch (parent->type) {
            case NodeType4:   static_cast<Node4*>(parent)->bulkLoadInsertNode4(this, parentRef, b, child, dummy); break;
            case NodeType16:  static_cast<Node16*>(parent)->bulkLoadInsertNode16(this, parentRef, b, child, dummy); break;
            case NodeType48:  static_cast<Node48*>(parent)->bulkLoadInsertNode48(this, parentRef, b, child, dummy); break;
            case NodeType256: static_cast<Node256*>(parent)->bulkLoadInsertNode256(this, parentRef, b, child, dummy); break;
        }
        return findChild(*parentRef, b);  // parent may have grown
    }

    // Bulk-load a CONTIGUOUS key range (keys[i] == keys[0]+i) at ANY offset.
    // Generalizes bulkLoad (which requires keys[0]==0): the 256-value-aligned
    // interior is filled group-by-group into Node256 leaves; the (<256) leading
    // and trailing partials are inserted one-by-one. Caller should compressTree().
    void bulkLoadRange(const std::vector<key_int_t>& keys, const std::vector<key_int_t>& values) {
        size_t n = keys.size();
        if (n == 0) return;
        // first index whose value is 256-aligned (b3 == 0)
        size_t firstAligned = 0;
        while (firstAligned < n && (keys[firstAligned] & 0xFF) != 0) firstAligned++;
        if (root == nullptr) { Node4* r = new Node4(); r->prefixLength = 0; root = r; }
        // fill complete 256-aligned groups
        size_t g = firstAligned;
        while (g + 256 <= n) {
            key_int_t V = keys[g];  // V & 0xFF == 0
            uint8_t b0 = (V >> 24) & 0xFF, b1 = (V >> 16) & 0xFF, b2 = (V >> 8) & 0xFF;
            // getOrCreatePath(root, b0, b1) then the group's Node256 at b2
            ArtNode** b0Ref = getOrCreateChild(&root, b0, false);
            ArtNode** b1Ref = getOrCreateChild(b0Ref, b1, false);
            ArtNode** groupRef = getOrCreateChild(b1Ref, b2, true);
            Node256* groupNode = static_cast<Node256*>(*groupRef);
            for (size_t j = 0; j < 256; j++) groupNode->child[j] = makeLeaf(values[g + j]);
            g += 256;
        }
        size_t endAligned = g;
        // ragged ends via normal insert (disjoint b2 from the aligned groups)
        for (size_t k = 0; k < firstAligned; k++) bulkInsertOne(keys[k], values[k]);
        for (size_t k = endAligned; k < n; k++) bulkInsertOne(keys[k], values[k]);
    }

    // Compress the tree by merging nodes with a single child
    int compressTree() {
        int compressed_nodes = 0;
        compressTreeHelper(root, &root, compressed_nodes, 1);
        return compressed_nodes;
    }

   protected:
    // Scans node's child array for target and returns a pointer to that child
    // slot, or nullptr if not found.  Used to recompute fp_ref after a node
    // expansion replaces fp_prev with a larger node type.
    ArtNode** findChildPtr(ArtNode* node, ArtNode* target) {
        switch (node->type) {
            case NodeType4: {
                auto* n = static_cast<Node4*>(node);
                for (int i = 0; i < n->count; i++)
                    if (n->child[i] == target) return &n->child[i];
                break;
            }
            case NodeType16: {
                auto* n = static_cast<Node16*>(node);
                for (int i = 0; i < n->count; i++)
                    if (n->child[i] == target) return &n->child[i];
                break;
            }
            case NodeType48: {
                auto* n = static_cast<Node48*>(node);
                for (int i = 0; i < 48; i++)
                    if (n->child[i] == target) return &n->child[i];
                break;
            }
            case NodeType256: {
                auto* n = static_cast<Node256*>(node);
                for (int i = 0; i < 256; i++)
                    if (n->child[i] == target) return &n->child[i];
                break;
            }
        }
        return nullptr;
    }

   public:
    // Hook: an inner node was replaced by a larger node type during expansion.
    // Updates fp and fp_prev tracking for any slot pointing at oldNode.
    // Default implementation handles the single-fp case.
    virtual void onNodeReplaced(ArtNode* oldNode, ArtNode* newNode,
                                ArtNode** newNodeRef) {
        if (fp == oldNode) {
            fp     = newNode;
            fp_ref = newNodeRef;
        } else if (fp_prev == oldNode) {
            fp_prev = newNode;
            ArtNode** ref = findChildPtr(newNode, fp);
            if (ref) fp_ref = ref;
        }
    }

    // Hook: a new parent node was spliced above fp (prefix expansion);
    // the child cell that holds fp has moved to a new address.
    virtual void onFpRefUpdate(ArtNode* targetFp, ArtNode** newRef) {
        if (fp == targetFp) fp_ref = newRef;
    }

    // Hook: the leaf tracked by fp_leaf was expanded into a Node4.
    // Adjusts fp_depth and redirects fp/fp_ref/fp_prev to the new Node4.
    virtual void onLeafExpanded(ArtNode* oldLeaf, Node4* newNode,
                                ArtNode** nodeRef, ArtNode* prevNode) {
        if (fp_leaf == oldLeaf) {
            if (!isLeaf(fp)) {
                fp_depth += fp->prefixLength;
                fp_depth++;
            }
            fp      = newNode;
            fp_ref  = nodeRef;
            fp_prev = prevNode;
        }
    }

    // Hook: a prefix mismatch spliced a new Node4 above fpNode.
    // Updates fp_prev and fp_depth for the slot that was tracking fpNode.
    virtual void onPrefixMismatch(ArtNode* fpNode, Node4* newParent,
                                  unsigned mismatchPrefixLen) {
        if (fp == fpNode) {
            fp_prev  = newParent;
            fp_depth += mismatchPrefixLen + 1;
        }
    }

    // Hook: reclaim a node that was grown out of and unlinked from the tree.
    // Called by the base Node*::insert* methods in place of `delete this`.
    // Default = plain delete (dispatched by type so the correct-size operator
    // delete / trivial dtor runs); identical in effect to the original code.
    // The concurrent variant overrides this to defer the free.
    virtual void reclaimNode(ArtNode* n) {
        switch (n->type) {
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

    // Hook: a sorted insertion into `node` used memmove to shift existing
    // children, potentially invalidating any fp_ref that pointed into the
    // shifted range.  Implementors should re-derive fp_ref for every slot
    // where fp_prev == node using findChildPtr(node, fp).
    virtual void onParentShifted(ArtNode* node) {
        if (fp_prev == node) {
            ArtNode** ref = findChildPtr(node, fp);
            if (ref) fp_ref = ref;
        }
    }

   private:
    // Void insert function
    void insert(ART* tree, ArtNode* node, ArtNode** nodeRef, uint8_t key[],
                unsigned depth, uintptr_t value, unsigned maxKeyLength) {
        // Insert the leaf value into the tree

        if (node == NULL) {
            *nodeRef = makeLeaf(value);
            return;
        }

        if (isLeaf(node)) {
            // Replace leaf with Node4 and store both leaves in it
            uint8_t existingKey[maxKeyLength];
            loadKey(getLeafValue(node), existingKey);
            unsigned newPrefixLength = 0;
            while (existingKey[depth + newPrefixLength] ==
                   key[depth + newPrefixLength])
                newPrefixLength++;

            Node4* newNode = new Node4();
            newNode->prefixLength = newPrefixLength;
            memcpy(newNode->prefix, key + depth,
                   min(newPrefixLength, maxPrefixLength));
            *nodeRef = newNode;

            newNode->insertNode4(this, nodeRef,
                                 existingKey[depth + newPrefixLength], node);
            newNode->insertNode4(this, nodeRef, key[depth + newPrefixLength],
                                 makeLeaf(value));
            return;
        }

        // Handle prefix of inner node
        if (node->prefixLength) {
            unsigned mismatchPos =
                prefixMismatch(node, key, depth, maxKeyLength);
            if (mismatchPos != node->prefixLength) {
                // Prefix differs, create new node
                Node4* newNode = new Node4();
                *nodeRef = newNode;
                newNode->prefixLength = mismatchPos;
                memcpy(newNode->prefix, node->prefix,
                       min(mismatchPos, maxPrefixLength));
                // Break up prefix
                if (node->prefixLength < maxPrefixLength) {
                    newNode->insertNode4(this, nodeRef,
                                         node->prefix[mismatchPos], node);
                    node->prefixLength -= (mismatchPos + 1);
                    memmove(node->prefix, node->prefix + mismatchPos + 1,
                            min(node->prefixLength, maxPrefixLength));
                } else {
                    node->prefixLength -= (mismatchPos + 1);
                    uint8_t minKey[maxKeyLength];
                    loadKey(getLeafValue(minimum(node)), minKey);
                    newNode->insertNode4(this, nodeRef,
                                         minKey[depth + mismatchPos], node);
                    memmove(node->prefix, minKey + depth + mismatchPos + 1,
                            min(node->prefixLength, maxPrefixLength));
                }
                newNode->insertNode4(this, nodeRef, key[depth + mismatchPos],
                                     makeLeaf(value));
                return;
            }
            depth += node->prefixLength;
        }

        // Recurse
        ArtNode** child = findChild(node, key[depth]);
        if (*child) {
            insert(tree, *child, child, key, depth + 1, value, maxKeyLength);
            return;
        }

        // Insert leaf into inner node
        ArtNode* newNode = makeLeaf(value);
        switch (node->type) {
            case NodeType4:
                static_cast<Node4*>(node)->insertNode4(this, nodeRef,
                                                       key[depth], newNode);
                break;
            case NodeType16:
                static_cast<Node16*>(node)->insertNode16(this, nodeRef,
                                                         key[depth], newNode);
                break;
            case NodeType48:
                static_cast<Node48*>(node)->insertNode48(this, nodeRef,
                                                         key[depth], newNode);
                break;
            case NodeType256:
                static_cast<Node256*>(node)->insertNode256(this, nodeRef,
                                                           key[depth], newNode);
                break;
        }
    }

    // Lookup function, returns ArtNode
    ArtNode* lookup(ArtNode* node, uint8_t key[], unsigned keyLength,
                    unsigned depth, unsigned maxKeyLength) {
        // Find the node with a matching key, optimistic version

        bool skippedPrefix = false;  // Did we optimistically skip some prefix
                                     // without checking it?

        while (node != NULL) {
            if (isLeaf(node)) {
                if (!skippedPrefix && depth == keyLength)  // No check required
                    return node;

                if (depth != keyLength) {
                    // Check leaf
                    uint8_t leafKey[maxKeyLength];
                    loadKey(getLeafValue(node), leafKey);
                    for (unsigned i = (skippedPrefix ? 0 : depth);
                         i < keyLength; i++)
                        if (leafKey[i] != key[i]) return NULL;
                }
                return node;
            }

            if (node->prefixLength) {
                if (node->prefixLength < maxPrefixLength) {
                    for (unsigned pos = 0; pos < node->prefixLength; pos++)
                        if (key[depth + pos] != node->prefix[pos]) return NULL;
                } else
                    skippedPrefix = true;
                depth += node->prefixLength;
            }

            node = *findChild(node, key[depth]);
            depth++;
        }

        return NULL;
    }

    // Erase function, deletes a leaf from the tree
    void erase(ArtNode* node, ArtNode** nodeRef, uint8_t key[],
               unsigned keyLength, unsigned depth, unsigned maxKeyLength) {
        // Delete a leaf from a tree

        if (!node) return;

        if (isLeaf(node)) {
            // Make sure we have the right leaf
            if (leafMatches(node, key, keyLength, depth, maxKeyLength))
                *nodeRef = NULL;
            return;
        }

        // Handle prefix
        if (node->prefixLength) {
            if (prefixMismatch(node, key, depth, maxKeyLength) !=
                node->prefixLength)
                return;
            depth += node->prefixLength;
        }

        ArtNode** child = findChild(node, key[depth]);
        if (isLeaf(*child) &&
            leafMatches(*child, key, keyLength, depth, maxKeyLength)) {
            // Leaf found, delete it in inner node
            switch (node->type) {
                case NodeType4:
                    static_cast<Node4*>(node)->eraseNode4(this, nodeRef, child);
                    break;
                case NodeType16:
                    static_cast<Node16*>(node)->eraseNode16(this, nodeRef,
                                                            child);
                    break;
                case NodeType48:
                    static_cast<Node48*>(node)->eraseNode48(this, nodeRef,
                                                            key[depth]);
                    break;
                case NodeType256:
                    static_cast<Node256*>(node)->eraseNode256(this, nodeRef,
                                                              key[depth]);
                    break;
            }
        } else {
            // Recurse
            erase(*child, child, key, keyLength, depth + 1, maxKeyLength);
        }
    }

    void printTree(ArtNode* node, int depth) {
        if (!node) return;

        // Indent based on depth
        for (int i = 0; i < depth; i++) {
            printf("  ");
        }

        if (isLeaf(node)) {
            printf("Leaf(%lu)\n", getLeafValue(node));
            return;
        }

        switch (node->type) {
            case NodeType4: {
                Node4* n = static_cast<Node4*>(node);
                printf("Node4 [%p]\n", static_cast<void*>(n));
                for (unsigned i = 0; i < n->count; i++) {
                    printTree(n->child[i], depth + 1);
                }
                break;
            }
            case NodeType16: {
                Node16* n = static_cast<Node16*>(node);
                printf("Node16 [%p]\n", static_cast<void*>(n));
                for (unsigned i = 0; i < n->count; i++) {
                    printTree(n->child[i], depth + 1);
                }
                break;
            }
            case NodeType48: {
                Node48* n = static_cast<Node48*>(node);
                printf("Node48 [%p]\n", static_cast<void*>(n));
                for (unsigned i = 0; i < 256; i++) {
                    if (n->childIndex[i] != emptyMarker) {
                        printTree(n->child[n->childIndex[i]], depth + 1);
                    }
                }
                break;
            }
            case NodeType256: {
                Node256* n = static_cast<Node256*>(node);
                printf("Node256 [%p]\n", static_cast<void*>(n));
                for (unsigned i = 0; i < 256; i++) {
                    if (n->child[i]) {
                        printTree(n->child[i], depth + 1);
                    }
                }
                break;
            }
        }
    }

    // Helper function to compress the tree, called recursively using DFS
    void compressTreeHelper(ArtNode* node, ArtNode** nodeRef, int& compressed_nodes, int depth) {
        if (!node || isLeaf(node) || depth == 4) return;
        
        // First, recursively compress children
        switch (node->type) {
            case NodeType4: {
                Node4* n = static_cast<Node4*>(node);
                for (unsigned i = 0; i < n->count; i++) {
                    compressTreeHelper(n->child[i], &n->child[i], compressed_nodes, depth + 1);
                }
                break;
            }
            case NodeType16: {
                Node16* n = static_cast<Node16*>(node);
                for (unsigned i = 0; i < n->count; i++) {
                    compressTreeHelper(n->child[i], &n->child[i], compressed_nodes, depth + 1);
                }
                break;
            }
            case NodeType48: {
                Node48* n = static_cast<Node48*>(node);
                for (unsigned i = 0; i < 256; i++) {
                    if (n->childIndex[i] != emptyMarker) {
                        compressTreeHelper(n->child[n->childIndex[i]], 
                                        &n->child[n->childIndex[i]], 
                                        compressed_nodes, depth + 1);
                    }
                }
                break;
            }
            case NodeType256: {
                Node256* n = static_cast<Node256*>(node);
                for (unsigned i = 0; i < 256; i++) {
                    if (n->child[i]) {
                        compressTreeHelper(n->child[i], &n->child[i], compressed_nodes, depth + 1);
                    }
                }
                break;
            }
        }
        
        // After compressing children, check if this node has count == 1
        if (node->count == 1) {
            // Find the single child
            ArtNode* single_child = nullptr;
            uint8_t child_key = 0;
            
            switch (node->type) {
                case NodeType4: {
                    Node4* n = static_cast<Node4*>(node);
                    single_child = n->child[0];
                    child_key = n->key[0];
                    break;
                }
                case NodeType16: {
                    Node16* n = static_cast<Node16*>(node);
                    single_child = n->child[0];
                    child_key = n->key[0];
                    break;
                }
                case NodeType48: {
                    Node48* n = static_cast<Node48*>(node);
                    for (unsigned i = 0; i < 256; i++) {
                        if (n->childIndex[i] != emptyMarker) {
                            single_child = n->child[n->childIndex[i]];
                            child_key = i;
                            break;
                        }
                    }
                    break;
                }
                case NodeType256: {
                    Node256* n = static_cast<Node256*>(node);
                    for (unsigned i = 0; i < 256; i++) {
                        if (n->child[i]) {
                            single_child = n->child[i];
                            child_key = i;
                            break;
                        }
                    }
                    break;
                }
            }
            
            // Only compress if the child is not a leaf (to preserve tree structure)
            if (single_child && !isLeaf(single_child)) {
                // Combine prefixes: current node's prefix + child_key + child's prefix
                uint8_t combined_prefix[maxPrefixLength];
                unsigned combined_length = 0;
                
                // Copy current node's prefix
                unsigned copy_len = min(node->prefixLength, maxPrefixLength);
                memcpy(combined_prefix, node->prefix, copy_len);
                combined_length += copy_len;
                
                // Add the child key byte
                if (combined_length < maxPrefixLength) {
                    combined_prefix[combined_length++] = child_key;
                }
                
                // Add child's prefix
                if (combined_length < maxPrefixLength) {
                    unsigned child_prefix_len = min(single_child->prefixLength, maxPrefixLength - combined_length);
                    memcpy(combined_prefix + combined_length, single_child->prefix, child_prefix_len);
                    combined_length += child_prefix_len;
                }
                
                // Update child's prefix
                single_child->prefixLength = node->prefixLength + 1 + single_child->prefixLength;
                memcpy(single_child->prefix, combined_prefix, min(combined_length, maxPrefixLength));
                
                // Replace current node with the child
                *nodeRef = single_child;
                delete node;
                compressed_nodes++;
            }
        }
    }

};

// Definition of the reclamation hook declared in ArtNode.h.  ART is complete
// here, so we can dispatch to the (possibly overridden) virtual reclaimNode.
inline void reclaimArtNode(ART* tree, ArtNode* n) { tree->reclaimNode(n); }

}  // namespace ART