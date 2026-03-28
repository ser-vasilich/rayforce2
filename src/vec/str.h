/*
 *   Copyright (c) 2024-2026 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAY_STR_H
#define RAY_STR_H

/*
 * str.h -- String helper functions.
 *
 * String atoms use SSO for <= 7 bytes (stored in sdata[7] with slen).
 * Long strings store data in a CHAR vector pointed to by obj.
 */

#include <rayforce.h>

/* Equality: fast reject on len, then prefix, then full compare.
 * pool_a/pool_b: pool bases for elements a and b respectively (NULL if inline) */
static inline bool ray_str_t_eq(const ray_str_t* a, const char* pool_a,
                               const ray_str_t* b, const char* pool_b) {
    if (a->len != b->len) return false;
    if (a->len == 0) return true;
    if (ray_str_is_inline(a)) {
        return memcmp(a->data, b->data, a->len) == 0;
    }
    /* Both pooled: check prefix first */
    if (memcmp(a->prefix, b->prefix, 4) != 0) return false;
    return memcmp(pool_a + a->pool_off, pool_b + b->pool_off, a->len) == 0;
}

/* Ordering: lexicographic, shorter string is less on prefix tie.
 * pool_a/pool_b: pool bases for elements a and b respectively (NULL if inline) */
static inline int ray_str_t_cmp(const ray_str_t* a, const char* pool_a,
                               const ray_str_t* b, const char* pool_b) {
    const char* pa = ray_str_t_ptr(a, pool_a);
    const char* pb = ray_str_t_ptr(b, pool_b);
    uint32_t min_len = a->len < b->len ? a->len : b->len;
    int r = memcmp(pa, pb, min_len);
    if (r != 0) return r;
    return (a->len > b->len) - (a->len < b->len);
}

/* Hash a ray_str_t element.  Uses FNV-1a which is self-contained and fast for
 * the typical short-to-medium strings stored in ray_str_t.
 * pool_base: pool base pointer for pooled strings (NULL when inline-only). */
static inline uint64_t ray_str_t_hash(const ray_str_t* s, const char* pool_base) {
    if (s->len == 0) return 0x9E3779B97F4A7C15ULL; /* golden ratio constant for empty */
    if (!ray_str_is_inline(s)) {
        assert(pool_base != NULL && "ray_str_t_hash: pooled string requires non-NULL pool_base");
    }
    const char* p = ray_str_is_inline(s) ? s->data : pool_base + s->pool_off;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < s->len; i++) {
        h ^= (uint64_t)(unsigned char)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

#endif /* RAY_STR_H */
