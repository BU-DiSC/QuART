#include "ART.h"
#include "ArtNode.h"

/*
 * QuART related insertNodeX methods
 */

namespace ART {
// fp insert method for Node4
// fp insert method for Node4 that changes fp_leaf
void Node4::insertNode4ChangeFp(ART* tree, ArtNode** nodeRef,
                                     uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    if (this->count < 4) {
        // Insert element
        unsigned pos;
        for (pos = 0; (pos < this->count) && (this->key[pos] < keyByte); pos++)
            ;
        memmove(this->key + pos + 1, this->key + pos, this->count - pos);
        memmove(this->child + pos + 1, this->child + pos,
                (this->count - pos) * sizeof(uintptr_t));
        this->key[pos] = keyByte;
        this->child[pos] = child;

        // Update fp parameters
        tree->fp_leaf = child;
        tree->fp = this;
        tree->fp_ref = nodeRef;

        this->count++;
        // Notify any slot whose fp_prev == this; the memmove above may have
        // shifted the cell their fp_ref pointed at.
        tree->onParentShifted(this);
    } else {
        // Grow to Node16
        Node16* newNode = new Node16();
        *nodeRef = newNode;
        newNode->count = 4;
        copyPrefix(this, newNode);
        for (unsigned i = 0; i < 4; i++)
            newNode->key[i] = flipSign(this->key[i]);
        memcpy(newNode->child, this->child, this->count * sizeof(uintptr_t));

        // Update any slot whose fp/fp_prev pointed at the old node.
        tree->onNodeReplaced(this, newNode, nodeRef);
        delete this;
        return newNode->insertNode16ChangeFp(tree, nodeRef, keyByte,
                                                  child);
    }
}

// fp insert method for Node16 that changes fp_leaf
void Node16::insertNode16ChangeFp(ART* tree, ArtNode** nodeRef,
                                       uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    if (this->count < 16) {
        // Insert element
        uint8_t keyByteFlipped = flipSign(keyByte);
        __m128i cmp = _mm_cmplt_epi8(
            _mm_set1_epi8(keyByteFlipped),
            _mm_loadu_si128(reinterpret_cast<__m128i*>(this->key)));
        uint16_t bitfield =
            _mm_movemask_epi8(cmp) & (0xFFFF >> (16 - this->count));
        unsigned pos = bitfield ? ctz(bitfield) : this->count;
        memmove(this->key + pos + 1, this->key + pos, this->count - pos);
        memmove(this->child + pos + 1, this->child + pos,
                (this->count - pos) * sizeof(uintptr_t));

        this->key[pos] = keyByteFlipped;
        this->child[pos] = child;

        // Update fp parameters
        tree->fp_leaf = child;
        tree->fp = this;
        tree->fp_ref = nodeRef;

        this->count++;
        // Notify any slot whose fp_prev == this; the memmove above may have
        // shifted the cell their fp_ref pointed at.
        tree->onParentShifted(this);
    } else {
        // Grow to Node48
        Node48* newNode = new Node48();
        *nodeRef = newNode;
        memcpy(newNode->child, this->child, this->count * sizeof(uintptr_t));
        for (unsigned i = 0; i < this->count; i++)
            newNode->childIndex[flipSign(this->key[i])] = i;
        copyPrefix(this, newNode);
        newNode->count = this->count;

        // Update any slot whose fp/fp_prev pointed at the old node.
        tree->onNodeReplaced(this, newNode, nodeRef);
        delete this;
        return newNode->insertNode48ChangeFp(tree, nodeRef, keyByte,
                                                  child);
    }
}

// fp insert method for Node48 that changes fp_leaf
void Node48::insertNode48ChangeFp(ART* tree, ArtNode** nodeRef,
                                       uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    if (this->count < 48) {
        // Insert element
        unsigned pos = this->count;
        if (this->child[pos])
            for (pos = 0; this->child[pos] != NULL; pos++)
                ;
        this->child[pos] = child;
        this->childIndex[keyByte] = pos;
        this->count++;

        // Update fp parameters
        tree->fp_leaf = child;
        tree->fp = this;
        tree->fp_ref = nodeRef;

    } else {
        // Grow to Node256
        Node256* newNode = new Node256();
        for (unsigned i = 0; i < 256; i++)
            if (this->childIndex[i] != 48)
                newNode->child[i] = this->child[this->childIndex[i]];
        newNode->count = this->count;
        copyPrefix(this, newNode);
        *nodeRef = newNode;

        // Update any slot whose fp/fp_prev pointed at the old node.
        tree->onNodeReplaced(this, newNode, nodeRef);
        delete this;
        return newNode->insertNode256ChangeFp(tree, nodeRef, keyByte,
                                                   child);
    }
}

// fp insert method for Node256 that changes fp_leaf
void Node256::insertNode256ChangeFp(ART* tree, ArtNode** nodeRef,
                                         uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    this->count++;
    this->child[keyByte] = child;

    // Update fp parameters
    tree->fp_leaf = child;
    tree->fp = this;
    tree->fp_ref = nodeRef;
}

// fp insert method for Node4 that correctly tracks fp_ref in a
// very special case of prefix expansion
void Node4::insertNode4PreserveFpPrefixExpansion(ART* tree,
                                                      ArtNode** nodeRef,
                                                      uint8_t keyByte,
                                                      ArtNode* child) {
    // Insert element
    unsigned pos;
    for (pos = 0; (pos < this->count) && (this->key[pos] < keyByte); pos++)
        ;
    memmove(this->key + pos + 1, this->key + pos, this->count - pos);
    memmove(this->child + pos + 1, this->child + pos,
            (this->count - pos) * sizeof(uintptr_t));
    this->key[pos] = keyByte;
    this->child[pos] = child;
    this->count++;

    // Notify all fp slots that may track child; the cell holding fp has moved.
    tree->onFpRefUpdate(child, &this->child[pos]);
    // Also notify any slot whose fp_prev == this; the memmove above may have
    // shifted the cell their fp_ref pointed at.
    tree->onParentShifted(this);
}

// fp insert method for Node4 that does not change fp_leaf
void Node4::insertNode4PreserveFp(ART* tree, ArtNode** nodeRef,
                                       uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    if (this->count < 4) {
        // Insert element
        unsigned pos;
        for (pos = 0; (pos < this->count) && (this->key[pos] < keyByte); pos++)
            ;
        memmove(this->key + pos + 1, this->key + pos, this->count - pos);
        memmove(this->child + pos + 1, this->child + pos,
                (this->count - pos) * sizeof(uintptr_t));
        this->key[pos] = keyByte;
        this->child[pos] = child;
        this->count++;
        // Notify any slot whose fp_prev == this; the memmove above may have
        // shifted the cell their fp_ref pointed at.
        tree->onParentShifted(this);
    } else {
        // Grow to Node16
        Node16* newNode = new Node16();
        *nodeRef = newNode;
        newNode->count = 4;
        copyPrefix(this, newNode);
        for (unsigned i = 0; i < 4; i++)
            newNode->key[i] = flipSign(this->key[i]);
        memcpy(newNode->child, this->child, this->count * sizeof(uintptr_t));

        // Update all fp slots whose fp or fp_prev pointed at this node.
        tree->onNodeReplaced(this, newNode, nodeRef);
        delete this;

        return newNode->insertNode16PreserveFp(tree, nodeRef, keyByte,
                                                    child);
    }
}

// fp insert method for Node16 that does not change fp_leaf
void Node16::insertNode16PreserveFp(ART* tree, ArtNode** nodeRef,
                                         uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    if (this->count < 16) {
        // Insert element
        uint8_t keyByteFlipped = flipSign(keyByte);
        __m128i cmp = _mm_cmplt_epi8(
            _mm_set1_epi8(keyByteFlipped),
            _mm_loadu_si128(reinterpret_cast<__m128i*>(this->key)));
        uint16_t bitfield =
            _mm_movemask_epi8(cmp) & (0xFFFF >> (16 - this->count));
        unsigned pos = bitfield ? ctz(bitfield) : this->count;
        memmove(this->key + pos + 1, this->key + pos, this->count - pos);
        memmove(this->child + pos + 1, this->child + pos,
                (this->count - pos) * sizeof(uintptr_t));
        this->key[pos] = keyByteFlipped;
        this->child[pos] = child;
        this->count++;
        // Notify any slot whose fp_prev == this; the memmove above may have
        // shifted the cell their fp_ref pointed at.
        tree->onParentShifted(this);
    } else {
        // Grow to Node48
        Node48* newNode = new Node48();
        *nodeRef = newNode;

        memcpy(newNode->child, this->child, this->count * sizeof(uintptr_t));
        for (unsigned i = 0; i < this->count; i++)
            newNode->childIndex[flipSign(this->key[i])] = i;
        copyPrefix(this, newNode);
        newNode->count = this->count;

        // Update all fp slots whose fp or fp_prev pointed at this node.
        tree->onNodeReplaced(this, newNode, nodeRef);
        delete this;

        return newNode->insertNode48PreserveFp(tree, nodeRef, keyByte,
                                                    child);
    }
}

void Node48::insertNode48PreserveFp(ART* tree, ArtNode** nodeRef,
                                         uint8_t keyByte, ArtNode* child) {
    // Insert leaf into inner node
    if (this->count < 48) {
        // Insert element
        unsigned pos = this->count;
        if (this->child[pos])
            for (pos = 0; this->child[pos] != NULL; pos++)
                ;
        this->child[pos] = child;
        this->childIndex[keyByte] = pos;
        this->count++;
    } else {
        // Grow to Node256
        Node256* newNode = new Node256();
        for (unsigned i = 0; i < 256; i++)
            if (this->childIndex[i] != 48)
                newNode->child[i] = this->child[this->childIndex[i]];
        newNode->count = this->count;
        copyPrefix(this, newNode);
        *nodeRef = newNode;

        // Update all fp slots whose fp or fp_prev pointed at this node.
        tree->onNodeReplaced(this, newNode, nodeRef);
        delete this;

        // There is no need for a insertNode256PreserveFp method
        // because Node256 can't expand further
        return newNode->insertNode256(tree, nodeRef, keyByte, child);
    }
}
}  // namespace ART