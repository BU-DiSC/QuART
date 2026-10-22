/*
 * Helper.h
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
#include <cstdio>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <vector>

namespace ART {

// ── Key-width configuration ───────────────────────────────────────────────────
// Define QUART_KEY_64 (e.g. -DQUART_KEY_64 at compile time, or via the
// CMake option of the same name) to switch to 64-bit integer keys.
// Without it the default is 32-bit integer keys.
#ifdef QUART_KEY_64
    using key_int_t = uint64_t;
    static constexpr unsigned keyBytes = 9;   // 8 key bytes + 1 null terminator
#else
    using key_int_t = uint32_t;
    static constexpr unsigned keyBytes = 5;   // 4 key bytes + 1 null terminator
#endif

// Mask for all key bytes except the lowest one (used for FP slot classification).
static constexpr key_int_t upperMask = (key_int_t)-1 >> 8;

uint8_t flipSign(uint8_t keyByte) {
    // Flip the sign bit, enables signed SSE comparison of unsigned values, used
    // by Node16
    return keyByte ^ 128;
}

inline void loadKey(key_int_t tid, uint8_t key[]) {
    // Store the key of the tuple into the key vector (big-endian byte order).
#ifdef QUART_KEY_64
    reinterpret_cast<uint64_t*>(key)[0] = __builtin_bswap64(tid);
    key[8] = 0;  // null terminator byte
#else
    reinterpret_cast<uint32_t*>(key)[0] = __builtin_bswap32(tid);
    key[4] = 0;  // null terminator byte
#endif
}

// Extract the upper (sizeof(key_int_t)-1) bytes from a key byte array.
// These are bytes key[0..sizeof(key_int_t)-2], i.e. all but the last byte.
//
// The key is stored big-endian (see loadKey), so the whole key value is a single
// byte-swapped load and the upper bytes are that value >> 8.  We do width-
// specific straight-line loads here (mirroring loadKey) rather than a generic
// byte-by-byte shift/or loop: under -march=native each branch is a single MOVBE
// + shift, vs. ~7 instructions (32-bit) / ~19 (64-bit) for the loop.  Reading
// the full key_int_t width is in-bounds because the key array always has a
// trailing null byte (keyBytes = sizeof(key_int_t) + 1); the low byte is
// discarded by the shift.
inline key_int_t getKeyUpperBytes(const uint8_t key[]) {
#ifdef QUART_KEY_64
    uint64_t v;
    memcpy(&v, key, sizeof(v));
    return __builtin_bswap64(v) >> 8;
#else
    uint32_t v;
    memcpy(&v, key, sizeof(v));
    return __builtin_bswap32(v) >> 8;
#endif
}

// Extract the upper bytes from a stored leaf value (equivalent to value >> 8,
// masked to sizeof(key_int_t)-1 bytes).
inline key_int_t getLeafUpperBytes(uintptr_t leafValue) {
    return static_cast<key_int_t>(leafValue >> 8) & upperMask;
}

static inline unsigned ctz(uint16_t x) {
    // Count trailing zeros, only defined for x>0
#ifdef __GNUC__
    return __builtin_ctz(x);
#else
    // Adapted from Hacker's Delight
    unsigned n = 1;
    if ((x & 0xFF) == 0) {
        n += 8;
        x = x >> 8;
    }
    if ((x & 0x0F) == 0) {
        n += 4;
        x = x >> 4;
    }
    if ((x & 0x03) == 0) {
        n += 2;
        x = x >> 2;
    }
    return n - (x & 1);
#endif
}

unsigned min(unsigned a, unsigned b) {
    // Helper function
    return (a < b) ? a : b;
}

// Function for representing key/prefix arrays as integers.
// Used for debugging and printing.
int arrToInt(const std::array<uint8_t, 4>& key) {
    return (int32_t(key[0]) << 24) | (int32_t(key[1]) << 16) |
           (int32_t(key[2]) << 8) | (int32_t(key[3]));
}
std::array<uint8_t, 4> intToArr(int value) {
    return {
        static_cast<uint8_t>((value / 16777216) % 256),  // 2^24
        static_cast<uint8_t>((value / 65536) % 256),     // 2^16
        static_cast<uint8_t>((value / 256) % 256),       // 2^8
        static_cast<uint8_t>(value % 256)                // 2^0
    };
}

}  // namespace ART