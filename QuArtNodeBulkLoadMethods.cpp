#include "ART.h"
#include "ArtNode.h"

/*
 * Bulk load related bulkLoadinsertNodeX methods
 */

namespace ART {
void Node4::bulkLoadInsertNode4(ART* tree, ArtNode** nodeRef, uint8_t keyByte,
                    ArtNode* child, ArtNode*& cursor) {
    // Insert leaf into inner node
    if (this->count < 4) {
        // Insert element
        unsigned pos;
        for (pos = 0; (pos < this->count) && (this->key[pos] < keyByte); pos++)
            ;
        // Shift keys and children to the right to make space for the new
        // key/child. This preserves the sorted order of keys in the node.
        memmove(this->key + pos + 1, this->key + pos, this->count - pos);
        memmove(this->child + pos + 1, this->child + pos,
                (this->count - pos) * sizeof(uintptr_t));
        this->key[pos] = keyByte;
        this->child[pos] = child;
        this->count++;
    } else {
        // Grow to Node16
        Node16* newNode = new Node16();
        *nodeRef = newNode;
        cursor = newNode; // keep the bulk-load cursor valid after this node grew
        newNode->count = 4;
        copyPrefix(this, newNode);
        for (unsigned i = 0; i < 4; i++)
            newNode->key[i] = flipSign(this->key[i]);
        memcpy(newNode->child, this->child, this->count * sizeof(uintptr_t));
        delete this;
        return newNode->bulkLoadInsertNode16(tree, nodeRef, keyByte, child, cursor);
    }
}

void Node16::bulkLoadInsertNode16(ART* tree, ArtNode** nodeRef, uint8_t keyByte,
                        ArtNode* child, ArtNode*& cursor) {
    // Insert leaf into inner node
    if (this->count < 16) {
        // Insert element

        // Flip the sign bit of the key byte for correct ordering in signed
        // comparisons
        uint8_t keyByteFlipped = flipSign(keyByte);

        // SIMD: Compare keyByteFlipped with all keys in the node in parallel
        // _mm_set1_epi8 sets all 16 bytes of an SSE register to keyByteFlipped
        // _mm_loadu_si128 loads the node's keys into an SSE register
        // _mm_cmplt_epi8 does a signed comparison of each byte
        __m128i cmp = _mm_cmplt_epi8(
            _mm_set1_epi8(keyByteFlipped),
            _mm_loadu_si128(reinterpret_cast<__m128i*>(this->key)));

        // _mm_movemask_epi8 creates a 16-bit mask from the comparison results
        // Only consider the bits for the active keys (this->count)
        uint16_t bitfield =
            _mm_movemask_epi8(cmp) & (0xFFFF >> (16 - this->count));

        // Find the position of the first set bit (i.e., where keyByteFlipped <
        // key[i])
        unsigned pos = bitfield ? ctz(bitfield) : this->count;

        // Shift keys and children to the right to make space for the new
        // key/child. This preserves the sorted order of keys in the node.
        memmove(this->key + pos + 1, this->key + pos, this->count - pos);
        memmove(this->child + pos + 1, this->child + pos,
                (this->count - pos) * sizeof(uintptr_t));
        this->key[pos] = keyByteFlipped;
        this->child[pos] = child;
        this->count++;
    } else {
        // Grow to Node48
        Node48* newNode = new Node48();
        *nodeRef = newNode;
        cursor = newNode; // keep the bulk-load cursor valid after this node grew
        memcpy(newNode->child, this->child, this->count * sizeof(uintptr_t));
        for (unsigned i = 0; i < this->count; i++)
            newNode->childIndex[flipSign(this->key[i])] = i;
        copyPrefix(this, newNode);
        newNode->count = this->count;
        delete this;
        return newNode->bulkLoadInsertNode48(tree, nodeRef, keyByte, child, cursor);
    }
}

void Node48::bulkLoadInsertNode48(ART* tree, ArtNode** nodeRef, uint8_t keyByte,
                          ArtNode* child, ArtNode*& cursor) {
    // Insert leaf into inner node
    if (this->count < 48) {
        // Insert element
        unsigned pos = this->count;
        if (this->child[pos])
            for (pos = 0; this->child[pos] != NULL; pos++)
                ;
        // No memmove needed here because Node48 uses a mapping (childIndex) and
        // a dense array.
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
        cursor = newNode; // keep the bulk-load cursor valid after this node grew
        delete this;
        return newNode->bulkLoadInsertNode256(tree, nodeRef, keyByte, child, cursor);
    }
}

void Node256::bulkLoadInsertNode256(ART* tree [[maybe_unused]],
                            ArtNode** nodeRef [[maybe_unused]], uint8_t keyByte,
                            ArtNode* child, ArtNode*& cursor) {
    // Insert leaf into inner node
    // No memmove needed here because Node256 uses a direct mapping for all
    // possible keys.
    this->count++;
    this->child[keyByte] = child;
}
} 