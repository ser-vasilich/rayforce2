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

#include "vec.h"
#include "core/platform.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "vec/embedding.h"
#include "vec/str.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Capacity helpers
 *
 * A vector's capacity is determined by its buddy order:
 *   capacity = (2^order - 32) / elem_size
 * When len reaches capacity, realloc to next power-of-2 data size.
 * -------------------------------------------------------------------------- */

static int64_t vec_capacity(ray_t* vec) {
    size_t block_size = (size_t)1 << vec->order;
    size_t data_space = block_size - 32;  /* 32B ray_t header */
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    if (esz == 0) return 0;
    return (int64_t)(data_space / esz);
}

/* --------------------------------------------------------------------------
 * ray_vec_new
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_new(int8_t type, int64_t capacity) {
    if (type <= 0 || type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (type == RAY_SYM)
        return ray_sym_vec_new(RAY_SYM_W64, capacity);  /* default: global sym IDs */
    if (capacity < 0) return ray_error("range", NULL);

    uint8_t esz = ray_elem_size(type);
    size_t data_size = (size_t)capacity * esz;
    if (esz > 1 && data_size / esz != (size_t)capacity)
        return ray_error("oom", NULL);

    ray_t* v = ray_alloc(data_size);
    if (!v || RAY_IS_ERR(v)) return v;

    v->type = type;
    v->len = 0;
    v->attrs = 0;
    memset(v->nullmap, 0, 16);
    if (type == RAY_STR) v->str_pool = NULL;

    return v;
}

/* --------------------------------------------------------------------------
 * ray_sym_vec_new — create a RAY_SYM vector with adaptive index width
 *
 * sym_width: RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, or RAY_SYM_W64
 * capacity:  number of elements (rows)
 * -------------------------------------------------------------------------- */

ray_t* ray_sym_vec_new(uint8_t sym_width, int64_t capacity) {
    if ((sym_width & ~RAY_SYM_W_MASK) != 0)
        return ray_error("type", NULL);
    if (capacity < 0) return ray_error("range", NULL);

    uint8_t esz = (uint8_t)RAY_SYM_ELEM(sym_width);
    size_t data_size = (size_t)capacity * esz;
    if (esz > 1 && data_size / esz != (size_t)capacity)
        return ray_error("oom", NULL);

    ray_t* v = ray_alloc(data_size);
    if (!v || RAY_IS_ERR(v)) return v;

    v->type = RAY_SYM;
    v->len = 0;
    v->attrs = sym_width;  /* lower 2 bits encode width */
    memset(v->nullmap, 0, 16);

    return v;
}

/* --------------------------------------------------------------------------
 * ray_vec_append
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_append(ray_t* vec, const void* elem) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type <= 0 || vec->type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (vec->type == RAY_STR) return ray_error("type", NULL);

    /* COW: if shared, copy first */
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;

    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    int64_t cap = vec_capacity(vec);

    /* Grow if needed */
    if (vec->len >= cap) {
        size_t new_data_size = (size_t)(vec->len + 1) * esz;
        /* Round up to next power of 2 block */
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s = 32;
            while (s < new_data_size) {
                if (s > SIZE_MAX / 2) return ray_error("oom", NULL);
                s *= 2;
            }
            new_data_size = s;
        }
        ray_t* new_vec = ray_scratch_realloc(vec, new_data_size);
        if (!new_vec || RAY_IS_ERR(new_vec)) return new_vec;
        vec = new_vec;
    }

    /* Append element */
    char* dst = (char*)ray_data(vec) + vec->len * esz;
    memcpy(dst, elem, esz);
    vec->len++;

    return vec;
}

/* --------------------------------------------------------------------------
 * ray_vec_set
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_set(ray_t* vec, int64_t idx, const void* elem) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type == RAY_STR) return ray_error("type", NULL);
    if (idx < 0 || idx >= vec->len)
        return ray_error("range", NULL);

    /* COW: if shared, copy first */
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;

    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    char* dst = (char*)ray_data(vec) + idx * esz;
    memcpy(dst, elem, esz);

    return vec;
}

/* --------------------------------------------------------------------------
 * ray_vec_get
 * -------------------------------------------------------------------------- */

void* ray_vec_get(ray_t* vec, int64_t idx) {
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    if (vec->type == RAY_STR) return NULL;

    /* Slice path: redirect to parent */
    if (vec->attrs & RAY_ATTR_SLICE) {
        ray_t* parent = vec->slice_parent;
        int64_t offset = vec->slice_offset;
        if (idx < 0 || idx >= vec->len) return NULL;
        uint8_t esz = ray_sym_elem_size(parent->type, parent->attrs);
        return (char*)ray_data(parent) + (offset + idx) * esz;
    }

    if (idx < 0 || idx >= vec->len) return NULL;
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    return (char*)ray_data(vec) + idx * esz;
}

/* --------------------------------------------------------------------------
 * ray_vec_slice  (zero-copy view)
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_slice(ray_t* vec, int64_t offset, int64_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (offset < 0 || len < 0 || offset > vec->len || len > vec->len - offset)
        return ray_error("range", NULL);

    /* If input is already a slice, resolve to ultimate parent */
    ray_t* parent = vec;
    int64_t parent_offset = offset;
    if (vec->attrs & RAY_ATTR_SLICE) {
        parent = vec->slice_parent;
        parent_offset = vec->slice_offset + offset;
    }

    /* Allocate a header-only block for the slice view */
    ray_t* s = ray_alloc(0);
    if (!s || RAY_IS_ERR(s)) return s;

    s->type = parent->type;
    s->attrs = RAY_ATTR_SLICE | (parent->attrs & RAY_SYM_W_MASK);
    s->len = len;
    s->slice_parent = parent;
    s->slice_offset = parent_offset;

    /* Retain the parent so it stays alive */
    ray_retain(parent);

    return s;
}

/* --------------------------------------------------------------------------
 * ray_vec_concat
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_concat(ray_t* a, ray_t* b) {
    if (!a || RAY_IS_ERR(a)) return a;
    if (!b || RAY_IS_ERR(b)) return b;
    if (a->type != b->type)
        return ray_error("type", NULL);

    if (a->type == RAY_STR) {
        int64_t total_len = a->len + b->len;
        if (total_len < a->len) return ray_error("oom", NULL);

        ray_t* result = ray_vec_new(RAY_STR, total_len);
        if (!result || RAY_IS_ERR(result)) return result;
        result->len = total_len;

        ray_str_t* dst = (ray_str_t*)ray_data(result);

        /* Resolve a's data (may be a slice) */
        const ray_str_t* a_elems = (a->attrs & RAY_ATTR_SLICE)
            ? &((const ray_str_t*)ray_data(a->slice_parent))[a->slice_offset]
            : (const ray_str_t*)ray_data(a);
        ray_t* a_pool_owner = (a->attrs & RAY_ATTR_SLICE) ? a->slice_parent : a;

        /* Resolve b's data (may be a slice) */
        const ray_str_t* b_elems = (b->attrs & RAY_ATTR_SLICE)
            ? &((const ray_str_t*)ray_data(b->slice_parent))[b->slice_offset]
            : (const ray_str_t*)ray_data(b);
        ray_t* b_pool_owner = (b->attrs & RAY_ATTR_SLICE) ? b->slice_parent : b;

        /* Copy a's elements as-is */
        memcpy(dst, a_elems, (size_t)a->len * sizeof(ray_str_t));

        /* Merge pools: a's pool + b's pool */
        int64_t a_pool_size = (a_pool_owner->str_pool) ? a_pool_owner->str_pool->len : 0;
        int64_t b_pool_size = (b_pool_owner->str_pool) ? b_pool_owner->str_pool->len : 0;
        int64_t total_pool = a_pool_size + b_pool_size;

        /* Guard: total pool must fit in uint32_t for pool_off rebasing */
        if (total_pool > (int64_t)UINT32_MAX) {
            ray_release(result);
            return ray_error("range", NULL);
        }

        if (total_pool > 0) {
            result->str_pool = ray_alloc((size_t)total_pool);
            if (!result->str_pool || RAY_IS_ERR(result->str_pool)) {
                result->str_pool = NULL;
                ray_release(result);
                return ray_error("oom", NULL);
            }
            result->str_pool->type = RAY_U8;
            result->str_pool->len = total_pool;
            char* pool_dst = (char*)ray_data(result->str_pool);
            if (a_pool_size > 0)
                memcpy(pool_dst, ray_data(a_pool_owner->str_pool), (size_t)a_pool_size);
            if (b_pool_size > 0)
                memcpy(pool_dst + a_pool_size, ray_data(b_pool_owner->str_pool), (size_t)b_pool_size);
        }

        /* Copy b's elements, rebasing pool offsets */
        for (int64_t i = 0; i < b->len; i++) {
            dst[a->len + i] = b_elems[i];
            if (!ray_str_is_inline(&b_elems[i]) && b_elems[i].len > 0) {
                dst[a->len + i].pool_off += (uint32_t)a_pool_size;
            }
        }

        /* Propagate null bitmaps from a and b.
         * Slices don't carry RAY_ATTR_HAS_NULLS — check RAY_ATTR_SLICE too. */
        if ((a->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) ||
            (b->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE))) {
            for (int64_t i = 0; i < a->len; i++) {
                if (ray_vec_is_null((ray_t*)a, i)) {
                    ray_err_t err = ray_vec_set_null_checked(result, i, true);
                    if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
                }
            }
            for (int64_t i = 0; i < b->len; i++) {
                if (ray_vec_is_null((ray_t*)b, i)) {
                    ray_err_t err = ray_vec_set_null_checked(result, a->len + i, true);
                    if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
                }
            }
        }

        return result;
    }

    uint8_t a_esz = ray_sym_elem_size(a->type, a->attrs);
    uint8_t b_esz = ray_sym_elem_size(b->type, b->attrs);
    /* Use the wider of the two widths for SYM columns — carry only width bits,
     * not flags like RAY_ATTR_SLICE or RAY_ATTR_HAS_NULLS from inputs. */
    uint8_t out_attrs = (a_esz >= b_esz) ? (a->attrs & RAY_SYM_W_MASK) : (b->attrs & RAY_SYM_W_MASK);
    uint8_t esz = (a_esz >= b_esz) ? a_esz : b_esz;

    int64_t total_len = a->len + b->len;
    if (total_len < a->len) return ray_error("oom", NULL); /* overflow */
    size_t data_size = (size_t)total_len * esz;
    if (esz > 1 && data_size / esz != (size_t)total_len)
        return ray_error("oom", NULL); /* multiplication overflow */

    ray_t* result = ray_alloc(data_size);
    if (!result || RAY_IS_ERR(result)) return result;

    result->type = a->type;
    result->len = total_len;
    result->attrs = out_attrs;
    memset(result->nullmap, 0, 16);

    /* For SYM with mismatched widths, widen element-by-element */
    if (a->type == RAY_SYM && a_esz != b_esz) {
        void* dst = ray_data(result);
        for (int64_t i = 0; i < a->len; i++) {
            int64_t val = ray_read_sym(ray_data(a), i, a->type, a->attrs);
            ray_write_sym(dst, i, (uint64_t)val, result->type, result->attrs);
        }
        for (int64_t i = 0; i < b->len; i++) {
            int64_t val = ray_read_sym(ray_data(b), i, b->type, b->attrs);
            ray_write_sym(dst, a->len + i, (uint64_t)val, result->type, result->attrs);
        }
    } else {
        /* Same width: fast memcpy path */
        void* a_data = (a->attrs & RAY_ATTR_SLICE) ?
            ((char*)ray_data(a->slice_parent) + a->slice_offset * esz) :
            ray_data(a);
        memcpy(ray_data(result), a_data, (size_t)a->len * esz);

        void* b_data = (b->attrs & RAY_ATTR_SLICE) ?
            ((char*)ray_data(b->slice_parent) + b->slice_offset * esz) :
            ray_data(b);
        memcpy((char*)ray_data(result) + (size_t)a->len * esz, b_data,
               (size_t)b->len * esz);
    }

    /* Propagate null bitmaps from a and b.
     * Slices don't carry RAY_ATTR_HAS_NULLS — check RAY_ATTR_SLICE too. */
    if ((a->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) ||
        (b->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE))) {
        for (int64_t i = 0; i < a->len; i++) {
            if (ray_vec_is_null((ray_t*)a, i)) {
                ray_err_t err = ray_vec_set_null_checked(result, i, true);
                if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
            }
        }
        for (int64_t i = 0; i < b->len; i++) {
            if (ray_vec_is_null((ray_t*)b, i)) {
                ray_err_t err = ray_vec_set_null_checked(result, a->len + i, true);
                if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
            }
        }
    }

    /* LIST/TABLE columns hold child pointers — retain them */
    if (a->type == RAY_LIST || a->type == RAY_TABLE) {
        ray_t** ptrs = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < total_len; i++) {
            if (ptrs[i]) ray_retain(ptrs[i]);
        }
    }

    return result;
}

/* --------------------------------------------------------------------------
 * ray_vec_from_raw
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_from_raw(int8_t type, const void* data, int64_t count) {
    if (type <= 0 || type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (type == RAY_STR) return ray_error("type", NULL);
    if (count < 0) return ray_error("range", NULL);

    /* RAY_SYM defaults to W64 (global sym IDs) */
    uint8_t sym_w = (type == RAY_SYM) ? RAY_SYM_W64 : 0;
    uint8_t esz = ray_sym_elem_size(type, sym_w);
    size_t data_size = (size_t)count * esz;

    ray_t* v = ray_alloc(data_size);
    if (!v || RAY_IS_ERR(v)) return v;

    v->type = type;
    v->len = count;
    v->attrs = sym_w;
    memset(v->nullmap, 0, 16);

    memcpy(ray_data(v), data, data_size);

    /* LIST/TABLE elements are child pointers — retain them */
    if (type == RAY_LIST || type == RAY_TABLE) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < count; i++) {
            if (ptrs[i]) ray_retain(ptrs[i]);
        }
    }

    return v;
}

/* --------------------------------------------------------------------------
 * Null bitmap operations
 *
 * Inline: for vectors with <=128 elements, bits stored in nullmap[16] (128 bits).
 * External: for >128 elements, allocate a U8 vector bitmap via ext_nullmap.
 * -------------------------------------------------------------------------- */

ray_err_t ray_vec_set_null_checked(ray_t* vec, int64_t idx, bool is_null) {
    if (!vec || RAY_IS_ERR(vec)) return RAY_ERR_TYPE;
    if (vec->attrs & RAY_ATTR_SLICE) return RAY_ERR_TYPE; /* cannot set null on slice — COW first */
    if (idx < 0 || idx >= vec->len) return RAY_ERR_RANGE;

    /* Mark HAS_NULLS if setting a null (defer for RAY_STR until ext alloc succeeds) */
    if (is_null && vec->type != RAY_STR) vec->attrs |= RAY_ATTR_HAS_NULLS;

    if (!(vec->attrs & RAY_ATTR_NULLMAP_EXT)) {
        /* RAY_STR uses bytes 8-15 for str_pool — must skip inline nullmap
         * and promote to external immediately to avoid aliasing corruption */
        if (vec->type != RAY_STR && idx < 128) {
            /* Inline nullmap path (<=128 elements, non-STR types) */
            int byte_idx = (int)(idx / 8);
            int bit_idx = (int)(idx % 8);
            if (is_null)
                vec->nullmap[byte_idx] |= (uint8_t)(1u << bit_idx);
            else
                vec->nullmap[byte_idx] &= (uint8_t)~(1u << bit_idx);
            return RAY_OK;
        }
        /* Need to promote to external nullmap */
        int64_t bitmap_len = (vec->len + 7) / 8;
        ray_t* ext = ray_vec_new(RAY_U8, bitmap_len);
        if (!ext || RAY_IS_ERR(ext)) return RAY_ERR_OOM;
        ext->len = bitmap_len;
        if (vec->type == RAY_STR) {
            /* RAY_STR: nullmap bytes contain str_ext_null/str_pool, not bits */
            memset(ray_data(ext), 0, (size_t)bitmap_len);
        } else {
            /* Copy existing inline bits */
            memcpy(ray_data(ext), vec->nullmap, 16);
            /* Zero remaining bytes */
            if (bitmap_len > 16)
                memset((char*)ray_data(ext) + 16, 0, (size_t)(bitmap_len - 16));
        }
        vec->attrs |= RAY_ATTR_NULLMAP_EXT;
        if (is_null) vec->attrs |= RAY_ATTR_HAS_NULLS;
        vec->ext_nullmap = ext;
    }

    /* External nullmap path */
    ray_t* ext = vec->ext_nullmap;
    /* Grow external bitmap if needed */
    int64_t needed_bytes = (idx / 8) + 1;
    if (needed_bytes > ext->len) {
        int64_t new_len = (vec->len + 7) / 8;
        if (new_len < needed_bytes) new_len = needed_bytes;
        size_t new_data_size = (size_t)new_len;
        int64_t old_len = ext->len;
        ray_t* new_ext = ray_scratch_realloc(ext, new_data_size);
        if (!new_ext || RAY_IS_ERR(new_ext)) return RAY_ERR_OOM;
        /* Zero new bytes */
        if (new_len > old_len)
            memset((char*)ray_data(new_ext) + old_len, 0,
                   (size_t)(new_len - old_len));
        new_ext->len = new_len;
        vec->ext_nullmap = new_ext;
        ext = new_ext;
    }

    uint8_t* bits = (uint8_t*)ray_data(ext);
    int byte_idx = (int)(idx / 8);
    int bit_idx = (int)(idx % 8);
    if (is_null)
        bits[byte_idx] |= (uint8_t)(1u << bit_idx);
    else
        bits[byte_idx] &= (uint8_t)~(1u << bit_idx);
    return RAY_OK;
}

void ray_vec_set_null(ray_t* vec, int64_t idx, bool is_null) {
    (void)ray_vec_set_null_checked(vec, idx, is_null);
}

/* --------------------------------------------------------------------------
 * str_pool_cow — ensure pool is privately owned after ray_cow()
 *
 * After ray_cow(), the copy shares the same str_pool as the original.
 * ray_retain_owned_refs bumps pool rc, so direct mutation would corrupt
 * the original's pool data (or ray_scratch_realloc would ray_free a
 * shared block).  Deep-copy the pool when rc > 1.
 * -------------------------------------------------------------------------- */

static ray_t* str_pool_cow(ray_t* vec) {
    if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) return vec;
    uint32_t pool_rc = ray_atomic_load(&vec->str_pool->rc);
    if (pool_rc <= 1) return vec;

    size_t pool_data_size = ((size_t)1 << vec->str_pool->order) - 32;
    ray_t* new_pool = ray_alloc(pool_data_size);
    if (!new_pool || RAY_IS_ERR(new_pool)) return NULL;

    size_t copy_bytes = (size_t)vec->str_pool->len;
    if (copy_bytes > pool_data_size) copy_bytes = pool_data_size;

    uint8_t saved_order = new_pool->order;
    uint8_t saved_mmod  = new_pool->mmod;
    memcpy(new_pool, vec->str_pool, 32 + copy_bytes);
    new_pool->order = saved_order;
    new_pool->mmod  = saved_mmod;
    ray_atomic_store(&new_pool->rc, 1);

    ray_release(vec->str_pool);
    vec->str_pool = new_pool;
    return vec;
}

/* --------------------------------------------------------------------------
 * String pool dead-byte tracking
 *
 * Dead bytes are stored as a uint32_t in the pool block's nullmap[0..3],
 * which is otherwise unused (the pool is a raw CHAR vector).
 * -------------------------------------------------------------------------- */

static inline uint32_t str_pool_dead(ray_t* vec) {
    if (!vec->str_pool) return 0;
    uint32_t d;
    memcpy(&d, vec->str_pool->nullmap, 4);
    return d;
}

static inline void str_pool_add_dead(ray_t* vec, uint32_t bytes) {
    uint32_t d = str_pool_dead(vec);
    d = (d > UINT32_MAX - bytes) ? UINT32_MAX : d + bytes;
    memcpy(vec->str_pool->nullmap, &d, 4);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_append — append a string to a RAY_STR vector
 *
 * Strings <= 12 bytes are inlined in the ray_str_t element.
 * Strings > 12 bytes store a 4-byte prefix + offset into a growable pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_append(ray_t* vec, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (len > UINT32_MAX) return ray_error("range", NULL);

    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!str_pool_cow(vec)) goto fail_oom;

    int64_t pool_off = 0;
    if (len > RAY_STR_INLINE_MAX) {
        if (!vec->str_pool) {
            size_t init_pool = len < 256 ? 256 : len * 2;
            vec->str_pool = ray_alloc(init_pool);
            if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) {
                vec->str_pool = NULL;
                goto fail_oom;
            }
            vec->str_pool->type = RAY_U8;
            vec->str_pool->len = 0;
        }

        int64_t pool_used = vec->str_pool->len;
        size_t pool_cap = ((size_t)1 << vec->str_pool->order) - 32;
        if ((size_t)pool_used + len > pool_cap) {
            size_t need = (size_t)pool_used + len;
            size_t new_cap = pool_cap;
            if (new_cap == 0) new_cap = 256;
            while (new_cap < need) {
                if (new_cap > SIZE_MAX / 2) goto fail_oom;
                new_cap *= 2;
            }
            ray_t* np = ray_scratch_realloc(vec->str_pool, new_cap);
            if (!np || RAY_IS_ERR(np)) goto fail_oom;
            vec->str_pool = np;
        }

        if ((uint64_t)pool_used > UINT32_MAX) goto fail_range;
        pool_off = pool_used;
    }

    /* Grow element array if needed — pool is already ready */
    int64_t cap = vec_capacity(vec);
    if (vec->len >= cap) {
        size_t new_data_size = (size_t)(vec->len + 1) * sizeof(ray_str_t);
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s2 = 32;
            while (s2 < new_data_size) {
                if (s2 > SIZE_MAX / 2) goto fail_oom;
                s2 *= 2;
            }
            new_data_size = s2;
        }
        ray_t* nv = ray_scratch_realloc(vec, new_data_size);
        if (!nv || RAY_IS_ERR(nv)) goto fail_oom;
        vec = nv;
    }

    ray_str_t* elem = &((ray_str_t*)ray_data(vec))[vec->len];
    memset(elem, 0, sizeof(ray_str_t));
    elem->len = (uint32_t)len;

    if (len <= RAY_STR_INLINE_MAX) {
        if (len > 0) memcpy(elem->data, s, len);
    } else {
        /* Copy string into pool (already allocated above) */
        char* pool_base = (char*)ray_data(vec->str_pool);
        memcpy(pool_base + pool_off, s, len);

        memcpy(elem->prefix, s, 4);
        elem->pool_off = (uint32_t)pool_off;
        vec->str_pool->len = pool_off + (int64_t)len;
    }

    vec->len++;
    return vec;

fail_oom:
    if (vec != original) ray_release(vec);
    return ray_error("oom", NULL);
fail_range:
    if (vec != original) ray_release(vec);
    return ray_error("range", NULL);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_get — read a string from a RAY_STR vector by index
 *
 * Returns a pointer to the string data (inline or pool) and sets *out_len.
 * Returns NULL for invalid input or out-of-bounds index.
 * -------------------------------------------------------------------------- */

const char* ray_str_vec_get(ray_t* vec, int64_t idx, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!vec || RAY_IS_ERR(vec) || vec->type != RAY_STR) return NULL;
    if (idx < 0 || idx >= vec->len) return NULL;

    /* Slice: redirect to parent */
    ray_t* data_owner = vec;
    int64_t data_idx = idx;
    if (vec->attrs & RAY_ATTR_SLICE) {
        data_owner = vec->slice_parent;
        data_idx = vec->slice_offset + idx;
    }

    const ray_str_t* elem = &((const ray_str_t*)ray_data(data_owner))[data_idx];
    if (out_len) *out_len = elem->len;

    if (elem->len == 0) return "";
    if (ray_str_is_inline(elem)) return elem->data;

    /* Pooled: resolve via pool */
    if (!data_owner->str_pool) return NULL;
    return (const char*)ray_data(data_owner->str_pool) + elem->pool_off;
}

/* --------------------------------------------------------------------------
 * ray_str_vec_set — update string at index in a RAY_STR vector
 *
 * Overwrites element at idx. Old pooled bytes become dead space (reclaimed
 * by ray_str_vec_compact). New pooled strings are appended to the pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_set(ray_t* vec, int64_t idx, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (idx < 0 || idx >= vec->len) return ray_error("range", NULL);
    if (len > UINT32_MAX) return ray_error("range", NULL);

    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!str_pool_cow(vec)) goto fail_oom;

    ray_str_t* elem = &((ray_str_t*)ray_data(vec))[idx];

    if (len <= RAY_STR_INLINE_MAX) {
        /* Track dead bytes if old string was pooled */
        if (!ray_str_is_inline(elem) && elem->len > 0 && vec->str_pool) {
            str_pool_add_dead(vec, elem->len);
        }
        memset(elem, 0, sizeof(ray_str_t));
        elem->len = (uint32_t)len;
        if (len > 0) memcpy(elem->data, s, len);
    } else {
        if (!vec->str_pool) {
            size_t init_pool = len < 256 ? 256 : len * 2;
            vec->str_pool = ray_alloc(init_pool);
            if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) {
                vec->str_pool = NULL;
                goto fail_oom;
            }
            vec->str_pool->type = RAY_U8;
            vec->str_pool->len = 0;
        }

        /* Grow pool if needed */
        int64_t pool_used = vec->str_pool->len;
        size_t pool_cap = ((size_t)1 << vec->str_pool->order) - 32;
        if ((size_t)pool_used + len > pool_cap) {
            size_t need = (size_t)pool_used + len;
            size_t new_cap = pool_cap;
            if (new_cap == 0) new_cap = 256;
            while (new_cap < need) {
                if (new_cap > SIZE_MAX / 2) goto fail_oom;
                new_cap *= 2;
            }
            ray_t* np = ray_scratch_realloc(vec->str_pool, new_cap);
            if (!np || RAY_IS_ERR(np)) goto fail_oom;
            vec->str_pool = np;
        }

        if ((uint64_t)pool_used > UINT32_MAX) goto fail_range;

        /* Pool alloc succeeded — now safe to modify the element */
        if (!ray_str_is_inline(elem) && elem->len > 0 && vec->str_pool) {
            str_pool_add_dead(vec, elem->len);
        }

        char* pool_base = (char*)ray_data(vec->str_pool);
        memcpy(pool_base + pool_used, s, len);
        memset(elem, 0, sizeof(ray_str_t));
        elem->len = (uint32_t)len;
        memcpy(elem->prefix, s, 4);
        elem->pool_off = (uint32_t)pool_used;
        vec->str_pool->len = pool_used + (int64_t)len;
    }

    return vec;

fail_oom:
    if (vec != original) ray_release(vec);
    return ray_error("oom", NULL);
fail_range:
    if (vec != original) ray_release(vec);
    return ray_error("range", NULL);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_compact — reclaim dead pool space
 *
 * Allocates a fresh pool containing only live pooled strings, updates
 * element offsets, and releases the old pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_compact(ray_t* vec) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (!vec->str_pool || str_pool_dead(vec) == 0) return vec;

    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!str_pool_cow(vec)) {
        if (vec != original) ray_release(vec);
        return ray_error("oom", NULL);
    }

    /* Compute true live size by scanning elements — avoids overflow when
     * the dead-byte counter (uint32_t) has saturated at UINT32_MAX. */
    ray_str_t* elems = (ray_str_t*)ray_data(vec);
    size_t live_size = 0;
    for (int64_t i = 0; i < vec->len; i++) {
        if (ray_vec_is_null(vec, i) || ray_str_is_inline(&elems[i]) || elems[i].len == 0) continue;
        live_size += elems[i].len;
    }

    if (live_size == 0) {
        ray_release(vec->str_pool);
        vec->str_pool = NULL;
        return vec;
    }

    ray_t* new_pool = ray_alloc(live_size);
    if (!new_pool || RAY_IS_ERR(new_pool)) return vec;
    new_pool->type = RAY_U8;
    new_pool->len = 0;
    memset(new_pool->nullmap, 0, 16);

    char* old_base = (char*)ray_data(vec->str_pool);
    char* new_base = (char*)ray_data(new_pool);
    uint32_t write_off = 0;

    for (int64_t i = 0; i < vec->len; i++) {
        if (ray_vec_is_null(vec, i) || ray_str_is_inline(&elems[i]) || elems[i].len == 0) continue;

        uint32_t slen = elems[i].len;
        memcpy(new_base + write_off, old_base + elems[i].pool_off, slen);
        elems[i].pool_off = write_off;
        write_off += slen;
    }

    new_pool->len = (int64_t)write_off;
    ray_release(vec->str_pool);
    vec->str_pool = new_pool;

    return vec;
}

/* --------------------------------------------------------------------------
 * ray_embedding_new — create a flat F32 vector for N*D embedding storage
 * -------------------------------------------------------------------------- */

ray_t* ray_embedding_new(int64_t nrows, int32_t dim) {
    int64_t total = nrows * (int64_t)dim;
    ray_t* v = ray_vec_new(RAY_F32, total);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = total;
    return v;
}

bool ray_vec_is_null(ray_t* vec, int64_t idx) {
    if (!vec || RAY_IS_ERR(vec)) return false;
    if (idx < 0 || idx >= vec->len) return false;

    /* Slice: delegate to parent with adjusted index */
    if (vec->attrs & RAY_ATTR_SLICE) {
        ray_t* parent = vec->slice_parent;
        int64_t pidx = vec->slice_offset + idx;
        return ray_vec_is_null(parent, pidx);
    }

    if (!(vec->attrs & RAY_ATTR_HAS_NULLS)) return false;

    if (vec->attrs & RAY_ATTR_NULLMAP_EXT) {
        ray_t* ext = vec->ext_nullmap;
        int64_t byte_idx = idx / 8;
        if (byte_idx >= ext->len) return false;
        uint8_t* bits = (uint8_t*)ray_data(ext);
        return (bits[byte_idx] >> (idx % 8)) & 1;
    }

    /* Inline nullmap — not available for RAY_STR (bytes 0-15 hold str_pool) */
    if (vec->type == RAY_STR) return false;
    if (idx >= 128) return false;
    int byte_idx = (int)(idx / 8);
    int bit_idx = (int)(idx % 8);
    return (vec->nullmap[byte_idx] >> bit_idx) & 1;
}
