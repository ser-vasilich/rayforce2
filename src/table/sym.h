/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_SYM_H
#define RAY_SYM_H

/*
 * sym.h -- Global symbol intern table.
 *
 * Sequential mode: simple hash map + array. wyhash (truncated to 32-bit),
 * open addressing with linear probing. Stores (hash32 << 32) | (id + 1)
 * so that 0 means empty bucket.
 */

#include <rayforce.h>
#include "core/types.h"

/* Symbol width encoding (lower 2 bits of attrs when type == RAY_SYM) */
#define RAY_SYM_W_MASK   0x03
#define RAY_SYM_W8       0x00   /* uint8_t  indices -- dict <= 255 entries */
#define RAY_SYM_W16      0x01   /* uint16_t indices -- dict <= 65,535 */
#define RAY_SYM_W32      0x02   /* uint32_t indices -- dict <= 4,294,967,295 */
#define RAY_SYM_W64      0x03   /* uint64_t indices -- dict > 4B entries */

/* Helper macros */
#define RAY_IS_SYM(t)         ((t) == RAY_SYM)
#define RAY_SYM_ELEM(attrs)   (1u << ((attrs) & RAY_SYM_W_MASK))  /* 1,2,4,8 */

/* Determine optimal SYM width for a given dictionary size */
static inline uint8_t ray_sym_dict_width(int64_t dict_size) {
    if (dict_size <= 255)        return RAY_SYM_W8;
    if (dict_size <= 65535)      return RAY_SYM_W16;
    if (dict_size <= 4294967295) return RAY_SYM_W32;
    return RAY_SYM_W64;
}

/* SYM-aware element size: returns adaptive width for RAY_SYM columns */
static inline uint8_t ray_sym_elem_size(int8_t type, uint8_t attrs) {
    if (type == RAY_SYM) return (uint8_t)RAY_SYM_ELEM(attrs);
    return ray_elem_size(type);
}

/* Read a dictionary index from a RAY_SYM column (adaptive width) */
static inline int64_t ray_read_sym(const void* data, int64_t row, int8_t type, uint8_t attrs) {
    (void)type; /* only RAY_SYM now */
    switch (attrs & RAY_SYM_W_MASK) {
        case RAY_SYM_W8:  return ((const uint8_t*)data)[row];
        case RAY_SYM_W16: return ((const uint16_t*)data)[row];
        case RAY_SYM_W32: return ((const uint32_t*)data)[row];
        case RAY_SYM_W64: return ((const int64_t*)data)[row];
    }
    return 0;
}

/* Write a dictionary index into a RAY_SYM column (adaptive width) */
static inline void ray_write_sym(void* data, int64_t row, uint64_t val, int8_t type, uint8_t attrs) {
    (void)type; /* only RAY_SYM now */
    switch (attrs & RAY_SYM_W_MASK) {
        case RAY_SYM_W8:  ((uint8_t*)data)[row]  = (uint8_t)val;  break;
        case RAY_SYM_W16: ((uint16_t*)data)[row] = (uint16_t)val; break;
        case RAY_SYM_W32: ((uint32_t*)data)[row] = (uint32_t)val; break;
        case RAY_SYM_W64: ((int64_t*)data)[row]  = (int64_t)val;  break;
    }
}

/* Intern with pre-computed wyhash, no lock.
 * Caller must guarantee single-threaded access. */
int64_t ray_sym_intern_prehashed(uint32_t hash, const char* str, size_t len);

#endif /* RAY_SYM_H */
