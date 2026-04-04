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

#include "ops/exec_internal.h"

/* Global profiler instance (zero-initialized = inactive) */
ray_profile_t g_ray_profile;

/* --------------------------------------------------------------------------
 * Materialize a MAPCOMMON column into a flat RAY_SYM vector.
 * Expands key_values × row_counts into one SYM ID per row.
 * -------------------------------------------------------------------------- */
ray_t* materialize_mapcommon(ray_t* mc) {
    ray_t** mc_ptrs = (ray_t**)ray_data(mc);
    ray_t* kv = mc_ptrs[0];   /* key_values: typed vec (DATE/I64/SYM) */
    ray_t* rc = mc_ptrs[1];   /* row_counts: RAY_I64 vec of n_parts */
    int64_t n_parts = kv->len;
    int8_t kv_type = kv->type;
    size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
    const char* kdata = (const char*)ray_data(kv);
    const int64_t* counts = (const int64_t*)ray_data(rc);

    int64_t total = 0;
    for (int64_t p = 0; p < n_parts; p++) total += counts[p];

    ray_t* flat = ray_vec_new(kv_type, total);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = total;

    /* Pattern-fill: broadcast each partition's key value across its row range.
     * Typed fill avoids per-element memcpy overhead. */
    char* out = (char*)ray_data(flat);
    int64_t off = 0;
    for (int64_t p = 0; p < n_parts; p++) {
        int64_t cnt = counts[p];
        if (esz == 8) {
            uint64_t v;
            memcpy(&v, kdata + (size_t)p * 8, 8);
            uint64_t* dst = (uint64_t*)(out + off * 8);
            for (int64_t r = 0; r < cnt; r++) dst[r] = v;
        } else if (esz == 4) {
            uint32_t v;
            memcpy(&v, kdata + (size_t)p * 4, 4);
            uint32_t* dst = (uint32_t*)(out + off * 4);
            for (int64_t r = 0; r < cnt; r++) dst[r] = v;
        } else {
            for (int64_t r = 0; r < cnt; r++)
                memcpy(out + (off + r) * esz, kdata + (size_t)p * esz, esz);
        }
        off += cnt;
    }
    return flat;
}

/* Materialize first N rows of a MAPCOMMON column into a flat typed vector. */
ray_t* materialize_mapcommon_head(ray_t* mc, int64_t n) {
    ray_t** mc_ptrs = (ray_t**)ray_data(mc);
    ray_t* kv = mc_ptrs[0];
    ray_t* rc = mc_ptrs[1];
    int64_t n_parts = kv->len;
    int8_t kv_type = kv->type;
    size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
    const char* kdata = (const char*)ray_data(kv);
    const int64_t* counts = (const int64_t*)ray_data(rc);

    ray_t* flat = ray_vec_new(kv_type, n);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = n;

    char* out = (char*)ray_data(flat);
    int64_t off = 0;
    for (int64_t p = 0; p < n_parts && off < n; p++) {
        int64_t take = counts[p];
        if (take > n - off) take = n - off;
        if (esz == 8) {
            uint64_t v;
            memcpy(&v, kdata + (size_t)p * 8, 8);
            uint64_t* dst = (uint64_t*)(out + off * 8);
            for (int64_t r = 0; r < take; r++) dst[r] = v;
        } else if (esz == 4) {
            uint32_t v;
            memcpy(&v, kdata + (size_t)p * 4, 4);
            uint32_t* dst = (uint32_t*)(out + off * 4);
            for (int64_t r = 0; r < take; r++) dst[r] = v;
        } else {
            for (int64_t r = 0; r < take; r++)
                memcpy(out + (off + r) * esz, kdata + (size_t)p * esz, esz);
        }
        off += take;
    }
    return flat;
}

/* Materialize MAPCOMMON through a boolean filter predicate. */
ray_t* materialize_mapcommon_filter(ray_t* mc, ray_t* pred, int64_t pass_count) {
    ray_t** mc_ptrs = (ray_t**)ray_data(mc);
    ray_t* kv = mc_ptrs[0];
    ray_t* rc = mc_ptrs[1];
    int64_t n_parts = kv->len;
    int8_t kv_type = kv->type;
    size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
    const char* kdata = (const char*)ray_data(kv);
    const int64_t* counts = (const int64_t*)ray_data(rc);

    ray_t* flat = ray_vec_new(kv_type, pass_count);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = pass_count;

    char* out = (char*)ray_data(flat);
    int64_t out_idx = 0;
    int64_t row = 0;
    int64_t part_idx = 0;
    int64_t part_end = counts[0];

    ray_morsel_t mp;
    ray_morsel_init(&mp, pred);
    while (ray_morsel_next(&mp)) {
        const uint8_t* bits = (const uint8_t*)mp.morsel_ptr;
        for (int64_t i = 0; i < mp.morsel_len; i++, row++) {
            while (part_idx < n_parts - 1 && row >= part_end) {
                part_idx++;
                part_end += counts[part_idx];
            }
            if (bits[i])
                memcpy(out + (size_t)out_idx++ * esz,
                       kdata + (size_t)part_idx * esz, esz);
        }
    }
    return flat;
}


/* ============================================================================
 * Parallel index gather — used by filter, sort, and join
 * ============================================================================ */

void multi_gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    multi_gather_ctx_t* c = (multi_gather_ctx_t*)raw;
    const int64_t* restrict idx = c->idx;
    int64_t nc = c->ncols;

    /* Process one column at a time per batch of rows.
     * This focuses random reads on a single source array, giving the
     * hardware prefetcher only 1 stream to track (instead of ncols
     * concurrent streams, which overflows the L2 miss queue). */
#define MG_BATCH 512
#define MG_PF    32
    for (int64_t base = start; base < end; base += MG_BATCH) {
        int64_t bstart = base;
        int64_t bend = base + MG_BATCH;
        if (bend > end) bend = end;
        for (int64_t col = 0; col < nc; col++) {
            uint8_t e = c->esz[col];
            char* src = c->srcs[col];
            char* dst = c->dsts[col];
            if (e == 8) {
                const uint64_t* restrict s8 = (const uint64_t*)src;
                uint64_t* restrict d8 = (uint64_t*)dst;
                for (int64_t i = bstart; i < bend; i++) {
                    if (i + MG_PF < bend)
                        __builtin_prefetch(&s8[idx[i + MG_PF]], 0, 0);
                    d8[i] = s8[idx[i]];
                }
            } else if (e == 4) {
                const uint32_t* restrict s4 = (const uint32_t*)src;
                uint32_t* restrict d4 = (uint32_t*)dst;
                for (int64_t i = bstart; i < bend; i++) {
                    if (i + MG_PF < bend)
                        __builtin_prefetch(&s4[idx[i + MG_PF]], 0, 0);
                    d4[i] = s4[idx[i]];
                }
            } else {
                for (int64_t i = bstart; i < bend; i++) {
                    if (i + MG_PF < bend)
                        __builtin_prefetch(src + idx[i + MG_PF] * e, 0, 0);
                    memcpy(dst + i * e, src + idx[i] * e, e);
                }
            }
        }
    }
#undef MG_PF
#undef MG_BATCH
}

/* Parallel index gather — single column with prefetching */
void gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gather_ctx_t* c = (gather_ctx_t*)raw;
    char* restrict src = (char*)ray_data(c->src_col);
    char* restrict dst = (char*)ray_data(c->dst_col);
    uint8_t esz = c->esz;
    const int64_t* restrict idx = c->idx;
#define GATHER_PF 16

    if (c->nullable) {
        for (int64_t i = start; i < end; i++) {
            if (i + GATHER_PF < end) {
                int64_t pf = idx[i + GATHER_PF];
                if (pf >= 0) __builtin_prefetch(src + pf * esz, 0, 0);
            }
            int64_t r = idx[i];
            if (r >= 0)
                memcpy(dst + i * esz, src + r * esz, esz);
            else
                memset(dst + i * esz, 0, esz);
        }
    } else {
        for (int64_t i = start; i < end; i++) {
            if (i + GATHER_PF < end)
                __builtin_prefetch(src + idx[i + GATHER_PF] * esz, 0, 0);
            memcpy(dst + i * esz, src + idx[i] * esz, esz);
        }
    }
#undef GATHER_PF
}

/* (filter execution moved to filter.c) */


/* ============================================================================
 * Sort execution (simple insertion sort)
 * ============================================================================ */

/* Forward declarations — exec_node wraps exec_node_inner with profiling */
/* exec_node declared extern in exec_internal.h */
static ray_t* exec_node_inner(ray_graph_t* g, ray_op_t* op);



/* ============================================================================
 * OP_IF: ternary select  result[i] = cond[i] ? then[i] : else[i]
 * ============================================================================ */

static ray_t* exec_if(ray_graph_t* g, ray_op_t* op) {
    /* cond = inputs[0], then = inputs[1], else_id stored in ext->literal */
    ray_t* cond_v = exec_node(g, op->inputs[0]);
    ray_t* then_v = exec_node(g, op->inputs[1]);

    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t else_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* else_v = exec_node(g, &g->nodes[else_id]);

    if (!cond_v || RAY_IS_ERR(cond_v)) {
        if (then_v && !RAY_IS_ERR(then_v)) ray_release(then_v);
        if (else_v && !RAY_IS_ERR(else_v)) ray_release(else_v);
        return cond_v;
    }
    if (!then_v || RAY_IS_ERR(then_v)) {
        ray_release(cond_v);
        if (else_v && !RAY_IS_ERR(else_v)) ray_release(else_v);
        return then_v;
    }
    if (!else_v || RAY_IS_ERR(else_v)) {
        ray_release(cond_v); ray_release(then_v);
        return else_v;
    }

    int64_t len = cond_v->len;
    bool then_scalar = ray_is_atom(then_v) || (then_v->type > 0 && then_v->len == 1);
    bool else_scalar = ray_is_atom(else_v) || (else_v->type > 0 && else_v->len == 1);
    if (then_scalar && !else_scalar) len = else_v->len;
    if (!then_scalar) len = then_v->len;

    int8_t out_type = op->out_type;
    ray_t* result = ray_vec_new(out_type, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(cond_v); ray_release(then_v); ray_release(else_v);
        return result;
    }
    result->len = len;

    uint8_t* cond_p = (uint8_t*)ray_data(cond_v);

    if (out_type == RAY_F64) {
        double t_scalar = then_scalar ? (ray_is_atom(then_v) ? then_v->f64 : ((double*)ray_data(then_v))[0]) : 0;
        double e_scalar = else_scalar ? (ray_is_atom(else_v) ? else_v->f64 : ((double*)ray_data(else_v))[0]) : 0;
        double* t_arr = then_scalar ? NULL : (double*)ray_data(then_v);
        double* e_arr = else_scalar ? NULL : (double*)ray_data(else_v);
        double* dst = (double*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_I64) {
        int64_t t_scalar = then_scalar ? (ray_is_atom(then_v) ? then_v->i64 : ((int64_t*)ray_data(then_v))[0]) : 0;
        int64_t e_scalar = else_scalar ? (ray_is_atom(else_v) ? else_v->i64 : ((int64_t*)ray_data(else_v))[0]) : 0;
        int64_t* t_arr = then_scalar ? NULL : (int64_t*)ray_data(then_v);
        int64_t* e_arr = else_scalar ? NULL : (int64_t*)ray_data(else_v);
        int64_t* dst = (int64_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_I32) {
        int32_t t_scalar = then_scalar ? (ray_is_atom(then_v) ? then_v->i32 : ((int32_t*)ray_data(then_v))[0]) : 0;
        int32_t e_scalar = else_scalar ? (ray_is_atom(else_v) ? else_v->i32 : ((int32_t*)ray_data(else_v))[0]) : 0;
        int32_t* t_arr = then_scalar ? NULL : (int32_t*)ray_data(then_v);
        int32_t* e_arr = else_scalar ? NULL : (int32_t*)ray_data(else_v);
        int32_t* dst = (int32_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_STR) {
        /* RAY_STR: resolve each side to string data and ray_str_vec_append.
         * Scalars may be -RAY_STR or RAY_SYM atoms. */
        result->len = 0; /* ray_str_vec_append manages len */
        for (int64_t i = 0; i < len; i++) {
            const char* sp;
            size_t sl;
            if (cond_p[i]) {
                if (then_scalar) {
                    if (then_v->type == -RAY_STR) {
                        sp = ray_str_ptr(then_v);
                        sl = ray_str_len(then_v);
                    } else if (then_v->type == RAY_STR) {
                        sp = ray_str_vec_get(then_v, 0, &sl);
                        if (!sp) { sp = ""; sl = 0; }
                    } else if (RAY_IS_SYM(then_v->type)) {
                        ray_t* s = ray_sym_str(then_v->i64);
                        sp = s ? ray_str_ptr(s) : "";
                        sl = s ? ray_str_len(s) : 0;
                    } else { sp = ""; sl = 0; }
                } else if (then_v->type == RAY_STR) {
                    sp = ray_str_vec_get(then_v, i, &sl);
                    if (!sp) { sp = ""; sl = 0; }
                } else {
                    /* RAY_SYM column */
                    int64_t sid = ray_read_sym(ray_data(then_v), i, then_v->type, then_v->attrs);
                    ray_t* sa = ray_sym_str(sid);
                    sp = sa ? ray_str_ptr(sa) : "";
                    sl = sa ? ray_str_len(sa) : 0;
                }
            } else {
                if (else_scalar) {
                    if (else_v->type == -RAY_STR) {
                        sp = ray_str_ptr(else_v);
                        sl = ray_str_len(else_v);
                    } else if (else_v->type == RAY_STR) {
                        sp = ray_str_vec_get(else_v, 0, &sl);
                        if (!sp) { sp = ""; sl = 0; }
                    } else if (RAY_IS_SYM(else_v->type)) {
                        ray_t* s = ray_sym_str(else_v->i64);
                        sp = s ? ray_str_ptr(s) : "";
                        sl = s ? ray_str_len(s) : 0;
                    } else { sp = ""; sl = 0; }
                } else if (else_v->type == RAY_STR) {
                    sp = ray_str_vec_get(else_v, i, &sl);
                    if (!sp) { sp = ""; sl = 0; }
                } else {
                    /* RAY_SYM column */
                    int64_t sid = ray_read_sym(ray_data(else_v), i, else_v->type, else_v->attrs);
                    ray_t* sa = ray_sym_str(sid);
                    sp = sa ? ray_str_ptr(sa) : "";
                    sl = sa ? ray_str_len(sa) : 0;
                }
            }
            result = ray_str_vec_append(result, sp, sl);
            if (RAY_IS_ERR(result)) break;
        }
    } else if (out_type == RAY_SYM) {
        /* SYM columns may have narrow widths (W8/W16/W32) — use ray_read_sym.
         * Scalars may be string atoms that need interning. Output is always W64. */
        int64_t t_scalar = 0, e_scalar = 0;
        if (then_scalar) {
            if (then_v->type == -RAY_STR) {
                t_scalar = ray_sym_intern(ray_str_ptr(then_v), ray_str_len(then_v));
            } else {
                t_scalar = then_v->i64;
            }
        }
        if (else_scalar) {
            if (else_v->type == -RAY_STR) {
                e_scalar = ray_sym_intern(ray_str_ptr(else_v), ray_str_len(else_v));
            } else {
                e_scalar = else_v->i64;
            }
        }
        int64_t* dst = (int64_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            int64_t tv = then_scalar ? t_scalar
                : ray_read_sym(ray_data(then_v), i, then_v->type, then_v->attrs);
            int64_t ev = else_scalar ? e_scalar
                : ray_read_sym(ray_data(else_v), i, else_v->type, else_v->attrs);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_BOOL || out_type == RAY_U8) {
        uint8_t t_scalar = then_scalar ? then_v->b8 : 0;
        uint8_t e_scalar = else_scalar ? else_v->b8 : 0;
        uint8_t* t_arr = then_scalar ? NULL : (uint8_t*)ray_data(then_v);
        uint8_t* e_arr = else_scalar ? NULL : (uint8_t*)ray_data(else_v);
        uint8_t* dst = (uint8_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_TIMESTAMP || out_type == RAY_TIME || out_type == RAY_DATE) {
        /* TIMESTAMP is 8B like I64; DATE and TIME are 4B like I32 */
        if (out_type == RAY_TIMESTAMP) {
            int64_t t_scalar2 = then_scalar ? then_v->i64 : 0;
            int64_t e_scalar2 = else_scalar ? else_v->i64 : 0;
            int64_t* t_arr = then_scalar ? NULL : (int64_t*)ray_data(then_v);
            int64_t* e_arr = else_scalar ? NULL : (int64_t*)ray_data(else_v);
            int64_t* dst = (int64_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++)
                dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar2)
                                   : (e_arr ? e_arr[i] : e_scalar2);
        } else {
            int32_t t_scalar2 = then_scalar ? then_v->i32 : 0;
            int32_t e_scalar2 = else_scalar ? else_v->i32 : 0;
            int32_t* t_arr = then_scalar ? NULL : (int32_t*)ray_data(then_v);
            int32_t* e_arr = else_scalar ? NULL : (int32_t*)ray_data(else_v);
            int32_t* dst = (int32_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++)
                dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar2)
                                   : (e_arr ? e_arr[i] : e_scalar2);
        }
    } else if (out_type == RAY_I16) {
        int16_t t_scalar = then_scalar ? (int16_t)then_v->i32 : 0;
        int16_t e_scalar = else_scalar ? (int16_t)else_v->i32 : 0;
        int16_t* t_arr = then_scalar ? NULL : (int16_t*)ray_data(then_v);
        int16_t* e_arr = else_scalar ? NULL : (int16_t*)ray_data(else_v);
        int16_t* dst = (int16_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    }

    ray_release(cond_v); ray_release(then_v); ray_release(else_v);
    return result;
}

/* ============================================================================
 * OP_LIKE: SQL LIKE pattern matching on SYM columns
 * ============================================================================ */

/* Simple SQL LIKE matcher: % = any (including empty), _ = single char.
 * Pattern is re-interpreted per row; could be optimized with precompilation
 * (e.g., compile once to NFA/DFA) for large datasets. */
static bool like_match(const char* str, size_t slen, const char* pat, size_t plen) {
    size_t si = 0, pi = 0;
    size_t star_p = (size_t)-1, star_s = 0;
    while (si < slen) {
        if (pi < plen && (pat[pi] == str[si] || pat[pi] == '_')) {
            si++; pi++;
        } else if (pi < plen && pat[pi] == '%') {
            star_p = pi; star_s = si;
            pi++;
        } else if (star_p != (size_t)-1) {
            pi = star_p + 1;
            star_s++;
            si = star_s;
        } else {
            return false;
        }
    }
    while (pi < plen && pat[pi] == '%') pi++;
    return pi == plen;
}

static ray_t* exec_like(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* pat_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    /* Get pattern string */
    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    int8_t in_type = input->type;
    if (in_type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            const char* sp = ray_str_t_ptr(&elems[i], pool);
            size_t sl = elems[i].len;
            dst[i] = like_match(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else if (RAY_IS_SYM(in_type)) {
        const void* base = ray_data(input);
        for (int64_t i = 0; i < len; i++) {
            int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
            ray_t* s = ray_sym_str(sym_id);
            if (!s) { dst[i] = 0; continue; }
            const char* sp = ray_str_ptr(s);
            size_t sl = ray_str_len(s);
            dst[i] = like_match(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else {
        memset(dst, 0, (size_t)len);
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* Case-insensitive LIKE: compare characters via tolower(). */
static bool ilike_match(const char* str, size_t slen, const char* pat, size_t plen) {
    size_t si = 0, pi = 0;
    size_t star_p = (size_t)-1, star_s = 0;
    while (si < slen) {
        if (pi < plen && pat[pi] != '%') {
            unsigned char sc = (unsigned char)str[si];
            unsigned char pc = (unsigned char)pat[pi];
            if (pc == '_' || (sc >= 'A' && sc <= 'Z' ? sc + 32 : sc) ==
                             (pc >= 'A' && pc <= 'Z' ? pc + 32 : pc)) {
                si++; pi++;
            } else if (star_p != (size_t)-1) {
                pi = star_p + 1; star_s++; si = star_s;
            } else {
                return false;
            }
        } else if (pi < plen && pat[pi] == '%') {
            star_p = pi; star_s = si; pi++;
        } else if (star_p != (size_t)-1) {
            pi = star_p + 1; star_s++; si = star_s;
        } else {
            return false;
        }
    }
    while (pi < plen && pat[pi] == '%') pi++;
    return pi == plen;
}

static ray_t* exec_ilike(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* pat_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    int8_t in_type = input->type;
    if (in_type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            const char* sp = ray_str_t_ptr(&elems[i], pool);
            size_t sl = elems[i].len;
            dst[i] = ilike_match(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else if (RAY_IS_SYM(in_type)) {
        const void* base = ray_data(input);
        for (int64_t i = 0; i < len; i++) {
            int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
            ray_t* s = ray_sym_str(sym_id);
            if (!s) { dst[i] = 0; continue; }
            dst[i] = ilike_match(ray_str_ptr(s), ray_str_len(s), pat_str, pat_len) ? 1 : 0;
        }
    } else {
        memset(dst, 0, (size_t)len);
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* ============================================================================
 * String functions: UPPER, LOWER, TRIM, STRLEN, SUBSTR, REPLACE, CONCAT
 *
 * These functions call ray_sym_intern() per output row, which is
 * O(n * sym_table_lookup) per string op.  Acceptable for current workloads;
 * could be optimized with batch interning if profiling shows a bottleneck.
 * ============================================================================ */

/* UPPER / LOWER / TRIM — unary SYM/STR → SYM/STR */
static ray_t* exec_string_unary(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    int64_t len = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, len);
    } else {
        result = ray_vec_new(RAY_SYM, len);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    if (!is_str) result->len = len;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    uint16_t opc = op->opcode;
    for (int64_t i = 0; i < len; i++) {
        /* Propagate null */
        if (ray_vec_is_null((ray_t*)input, i)) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }

        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        if (sl >= sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, sl + 1);
            if (!buf) {
                ray_release(result);
                ray_release(input);
                return ray_error("oom", NULL);
            }
        }
        size_t out_len = sl;
        if (opc == OP_UPPER) {
            for (size_t j = 0; j < out_len; j++) buf[j] = (char)toupper((unsigned char)sp[j]);
        } else if (opc == OP_LOWER) {
            for (size_t j = 0; j < out_len; j++) buf[j] = (char)tolower((unsigned char)sp[j]);
        } else { /* OP_TRIM */
            size_t start = 0, end = sl;
            while (start < sl && isspace((unsigned char)sp[start])) start++;
            while (end > start && isspace((unsigned char)sp[end - 1])) end--;
            out_len = end - start;
            memcpy(buf, sp + start, out_len);
        }

        if (is_str) {
            result = ray_str_vec_append(result, buf, out_len);
            if (RAY_IS_ERR(result)) { scratch_free(dyn_hdr); break; }
        } else {
            buf[out_len] = '\0';
            sym_dst[i] = ray_sym_intern(buf, out_len);
        }
        scratch_free(dyn_hdr);
    }
    ray_release(input);
    return result;
}

/* LENGTH — SYM → I64 */
static ray_t* exec_strlen(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;
    int64_t* dst = (int64_t*)ray_data(result);

    if (input->type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null((ray_t*)input, i)) {
                dst[i] = 0;
                ray_vec_set_null(result, i, true);
                continue;
            }
            dst[i] = (int64_t)elems[i].len;
        }
    } else {
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null((ray_t*)input, i)) {
                dst[i] = 0;
                ray_vec_set_null(result, i, true);
                continue;
            }
            const char* sp; size_t sl;
            sym_elem(input, i, &sp, &sl);
            dst[i] = (int64_t)sl;
        }
    }
    ray_release(input);
    return result;
}

/* SUBSTR(str, start, len) — 1-based start */
static ray_t* exec_substr(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* start_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (start_v && !RAY_IS_ERR(start_v)) ray_release(start_v); return input; }
    if (!start_v || RAY_IS_ERR(start_v)) { ray_release(input); return start_v; }

    /* Get len arg from ext node's literal field */
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t len_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* len_v = exec_node(g, &g->nodes[len_id]);
    if (!len_v || RAY_IS_ERR(len_v)) { ray_release(input); ray_release(start_v); return len_v; }

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, nrows);
    } else {
        result = ray_vec_new(RAY_SYM, nrows);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); ray_release(start_v); ray_release(len_v); return result; }
    if (!is_str) result->len = nrows;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    /* start_v and len_v may be atom scalars or vectors.
     * Handle RAY_I32 vectors correctly (read as int32_t, not int64_t). */
    int64_t s_scalar = 0, l_scalar = 0;
    const int64_t* s_data = NULL;
    const int64_t* l_data = NULL;
    const int32_t* s_data_i32 = NULL;
    const int32_t* l_data_i32 = NULL;
    if (start_v->type == -RAY_I64) s_scalar = start_v->i64;
    else if (start_v->type == -RAY_F64) s_scalar = (int64_t)start_v->f64;
    else if (start_v->len == 1) {
        if (start_v->type == RAY_F64)
            s_scalar = (int64_t)((double*)ray_data(start_v))[0];
        else if (start_v->type == RAY_I32)
            s_scalar = (int64_t)((int32_t*)ray_data(start_v))[0];
        else
            s_scalar = ((int64_t*)ray_data(start_v))[0];
    }
    else if (start_v->type == RAY_I32) s_data_i32 = (const int32_t*)ray_data(start_v);
    else s_data = (const int64_t*)ray_data(start_v);
    if (len_v->type == -RAY_I64) l_scalar = len_v->i64;
    else if (len_v->type == -RAY_F64) l_scalar = (int64_t)len_v->f64;
    else if (len_v->len == 1) {
        if (len_v->type == RAY_F64)
            l_scalar = (int64_t)((double*)ray_data(len_v))[0];
        else if (len_v->type == RAY_I32)
            l_scalar = (int64_t)((int32_t*)ray_data(len_v))[0];
        else
            l_scalar = ((int64_t*)ray_data(len_v))[0];
    }
    else if (len_v->type == RAY_I32) l_data_i32 = (const int32_t*)ray_data(len_v);
    else l_data = (const int64_t*)ray_data(len_v);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null — from input, start, or length */
        if (ray_vec_is_null((ray_t*)input, i) ||
            ((s_data || s_data_i32) && ray_vec_is_null((ray_t*)start_v, i)) ||
            ((l_data || l_data_i32) && ray_vec_is_null((ray_t*)len_v, i))) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }
        int64_t st = (s_data ? s_data[i] : s_data_i32 ? (int64_t)s_data_i32[i] : s_scalar) - 1; /* 1-based → 0-based */
        int64_t ln = l_data ? l_data[i] : l_data_i32 ? (int64_t)l_data_i32[i] : l_scalar;
        if (st < 0) st = 0;
        if ((size_t)st >= sl) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
            }
            else { sym_dst[i] = ray_sym_intern("", 0); }
            continue;
        }
        if (ln < 0 || ln > (int64_t)(sl - (size_t)st)) ln = (int64_t)sl - st;
        if (is_str) {
            result = ray_str_vec_append(result, sp + st, (size_t)ln);
            if (RAY_IS_ERR(result)) break;
        } else {
            sym_dst[i] = ray_sym_intern(sp + st, (size_t)ln);
        }
    }
    ray_release(input); ray_release(start_v); ray_release(len_v);
    return result;
}

/* REPLACE(str, from, to) */
static ray_t* exec_replace(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* from_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (from_v && !RAY_IS_ERR(from_v)) ray_release(from_v); return input; }
    if (!from_v || RAY_IS_ERR(from_v)) { ray_release(input); return from_v; }

    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t to_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* to_v = exec_node(g, &g->nodes[to_id]);
    if (!to_v || RAY_IS_ERR(to_v)) { ray_release(input); ray_release(from_v); return to_v; }

    /* from_v and to_v should be string constants (SYM atoms) */
    const char* from_str = ray_str_ptr(from_v);
    size_t from_len = ray_str_len(from_v);
    const char* to_str = ray_str_ptr(to_v);
    size_t to_len = ray_str_len(to_v);

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, nrows);
    } else {
        result = ray_vec_new(RAY_SYM, nrows);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); ray_release(from_v); ray_release(to_v); return result; }
    if (!is_str) result->len = nrows;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null */
        if (ray_vec_is_null((ray_t*)input, i)) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }
        /* Simple find-and-replace-all */
        /* Worst case: every char is a match, each replaced by to_len bytes.
         * Guard against size_t overflow when to_len >> from_len. */
        size_t n_matches = (from_len > 0) ? sl / from_len : 0;
        size_t worst;
        if (from_len > 0 && to_len > from_len && n_matches > SIZE_MAX / to_len) {
            worst = SIZE_MAX; /* overflow → cap at max; scratch_alloc will OOM */
        } else if (from_len > 0 && to_len >= from_len) {
            /* Expanding or same-size: max output when every chunk matches */
            worst = n_matches * to_len + (sl % from_len) + 1;
        } else {
            /* Shrinking or from_len==0: max output when nothing matches → sl */
            worst = sl + 1;
        }
        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        if (worst > sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, worst);
            if (!buf) {
                ray_release(result);
                ray_release(input); ray_release(from_v); ray_release(to_v);
                return ray_error("oom", NULL);
            }
        }
        size_t buf_cap = dyn_hdr ? worst : sizeof(sbuf);
        size_t bi = 0;
        for (size_t j = 0; j < sl; ) {
            if (from_len > 0 && j + from_len <= sl && memcmp(sp + j, from_str, from_len) == 0) {
                if (bi + to_len < buf_cap) { memcpy(buf + bi, to_str, to_len); bi += to_len; }
                j += from_len;
            } else {
                if (bi < buf_cap - 1) buf[bi++] = sp[j];
                j++;
            }
        }
        if (is_str) {
            result = ray_str_vec_append(result, buf, bi);
            if (RAY_IS_ERR(result)) { scratch_free(dyn_hdr); break; }
        } else {
            buf[bi] = '\0';
            sym_dst[i] = ray_sym_intern(buf, bi);
        }
        scratch_free(dyn_hdr);
    }
    ray_release(input); ray_release(from_v); ray_release(to_v);
    return result;
}

/* CONCAT(a, b, ...) */
static ray_t* exec_concat(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    int64_t raw_nargs = ext->sym;
    if (raw_nargs < 2 || raw_nargs > 255) return ray_error("domain", NULL);
    int n_args = (int)raw_nargs;

    /* Evaluate all inputs */
    ray_t* args_stack[16];
    ray_t** args = args_stack;
    ray_t* args_hdr = NULL;
    if (n_args > 16) {
        args = (ray_t**)scratch_calloc(&args_hdr, (size_t)n_args * sizeof(ray_t*));
        if (!args) return ray_error("oom", NULL);
    }

    args[0] = exec_node(g, op->inputs[0]);
    args[1] = exec_node(g, op->inputs[1]);
    uint32_t* trail = (uint32_t*)((char*)(ext + 1));
    for (int i = 2; i < n_args; i++) {
        args[i] = exec_node(g, &g->nodes[trail[i - 2]]);
    }
    /* Error check */
    for (int i = 0; i < n_args; i++) {
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i];
            for (int j = 0; j < n_args; j++) {
                if (j != i && args[j] && !RAY_IS_ERR(args[j])) ray_release(args[j]);
            }
            scratch_free(args_hdr);
            return err;
        }
    }

    /* Derive nrows from first vector arg (scalar args have byte-length in len) */
    int64_t nrows = 1;
    bool out_str = false;
    for (int a = 0; a < n_args; a++) {
        int8_t at = args[a]->type;
        if (at == RAY_STR) { out_str = true; if (nrows == 1) nrows = args[a]->len; }
        if (RAY_IS_SYM(at)) { if (nrows == 1) nrows = args[a]->len; }
        if (!ray_is_atom(args[a]) && nrows == 1) { nrows = args[a]->len; }
    }
    ray_t* result = ray_vec_new(out_str ? RAY_STR : RAY_SYM, nrows);
    if (!result || RAY_IS_ERR(result)) {
        for (int i = 0; i < n_args; i++) ray_release(args[i]);
        scratch_free(args_hdr);
        return result;
    }
    if (!out_str) result->len = nrows;
    int64_t* dst = out_str ? NULL : (int64_t*)ray_data(result);

    for (int64_t r = 0; r < nrows; r++) {
        /* Check if any arg is null at this row */
        bool any_null = false;
        for (int a = 0; a < n_args; a++) {
            if (!ray_is_atom(args[a]) && ray_vec_is_null((ray_t*)args[a], r < args[a]->len ? r : 0)) {
                any_null = true;
                break;
            }
        }
        if (any_null) {
            if (out_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                dst[r] = 0;
                ray_vec_set_null(result, r, true);
            }
            continue;
        }
        /* Pre-scan to compute total concat length for this row */
        size_t total = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* p;
                str_resolve(args[a], &elems, &p);
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                total += elems[ar].len;
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                sym_elem(args[a], ar, &sp, &sl);
                total += sl;
            } else if (t == -RAY_STR) {
                total += ray_str_len(args[a]);
            }
        }
        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        size_t buf_cap = sizeof(sbuf);
        if (total >= sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, total + 1);
            if (!buf) {
                ray_release(result);
                for (int i = 0; i < n_args; i++) ray_release(args[i]);
                scratch_free(args_hdr);
                return ray_error("oom", NULL);
            }
            buf_cap = total + 1;
        }
        size_t bi = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* pool;
                str_resolve(args[a], &elems, &pool);
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                const char* sp = ray_str_t_ptr(&elems[ar], pool);
                size_t sl = elems[ar].len;
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                sym_elem(args[a], ar, &sp, &sl);
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (t == -RAY_STR) {
                const char* sp = ray_str_ptr(args[a]);
                size_t sl = ray_str_len(args[a]);
                if (sp && bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            }
        }
        if (out_str) {
            result = ray_str_vec_append(result, buf, bi);
            if (RAY_IS_ERR(result)) { scratch_free(dyn_hdr); break; }
        } else {
            buf[bi] = '\0';
            dst[r] = ray_sym_intern(buf, bi);
        }
        scratch_free(dyn_hdr);
    }
    for (int i = 0; i < n_args; i++) ray_release(args[i]);
    scratch_free(args_hdr);
    return result;
}

/* ============================================================================
 * EXTRACT — date/time component extraction from temporal columns
 *
 * Input:  RAY_TIMESTAMP (i64 µs since 2000-01-01), RAY_DATE (i32 days since
 *         2000-01-01), or RAY_TIME (i32 ms since midnight).
 * Output: i64 vector of extracted field values.
 *
 * Uses Howard Hinnant's civil_from_days algorithm (public domain) for
 * Gregorian calendar decomposition.
 * ============================================================================ */

static ray_t* exec_extract(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) { ray_release(input); return ray_error("nyi", NULL); }

    int64_t field = ext->sym;
    int64_t len = input->len;
    int8_t in_type = input->type;

    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;

    int64_t* out = (int64_t*)ray_data(result);

    #undef  USEC_PER_SEC
    #define USEC_PER_SEC  1000000LL
    #define USEC_PER_MIN  (60LL  * USEC_PER_SEC)
    #define USEC_PER_HOUR (3600LL * USEC_PER_SEC)
    #define USEC_PER_DAY  (86400LL * USEC_PER_SEC)

    ray_morsel_t m;
    ray_morsel_init(&m, input);
    int64_t off = 0;

    while (ray_morsel_next(&m)) {
        int64_t n = m.morsel_len;

        for (int64_t i = 0; i < n; i++) {
            int64_t us;
            if (in_type == RAY_DATE) {
                /* int32 days since 2000-01-01 → microseconds */
                int32_t d = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)d * USEC_PER_DAY;
            } else if (in_type == RAY_TIME) {
                /* int32 milliseconds since midnight → microseconds */
                int32_t ms = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)ms * 1000LL;
            } else {
                /* RAY_TIMESTAMP / RAY_I64: already microseconds */
                us = ((const int64_t*)m.morsel_ptr)[i];
            }

            if (field == RAY_EXTRACT_EPOCH) {
                out[off + i] = us;
            } else if (field == RAY_EXTRACT_HOUR) {
                int64_t day_us = us % USEC_PER_DAY;
                if (day_us < 0) day_us += USEC_PER_DAY;
                out[off + i] = day_us / USEC_PER_HOUR;
            } else if (field == RAY_EXTRACT_MINUTE) {
                int64_t day_us = us % USEC_PER_DAY;
                if (day_us < 0) day_us += USEC_PER_DAY;
                out[off + i] = (day_us % USEC_PER_HOUR) / USEC_PER_MIN;
            } else if (field == RAY_EXTRACT_SECOND) {
                int64_t day_us = us % USEC_PER_DAY;
                if (day_us < 0) day_us += USEC_PER_DAY;
                out[off + i] = (day_us % USEC_PER_MIN) / USEC_PER_SEC;
            } else {
                /* Calendar fields: YEAR, MONTH, DAY, DOW, DOY */
                /* Floor-divide microseconds to get day count */
                int64_t days_since_2000 = us / USEC_PER_DAY;
                if (us < 0 && us % USEC_PER_DAY != 0) days_since_2000--;

                /* Hinnant civil_from_days: shift to 0000-03-01 era-based epoch */
                int64_t z = days_since_2000 + 10957 + 719468;
                int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                uint64_t doe = (uint64_t)(z - era * 146097);
                uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                int64_t y = (int64_t)yoe + era * 400;
                uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100);
                uint64_t mp = (5*doy_mar + 2) / 153;
                uint64_t d = doy_mar - (153*mp + 2) / 5 + 1;
                uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
                y += (mo <= 2);

                if (field == RAY_EXTRACT_YEAR) {
                    out[off + i] = y;
                } else if (field == RAY_EXTRACT_MONTH) {
                    out[off + i] = (int64_t)mo;
                } else if (field == RAY_EXTRACT_DAY) {
                    out[off + i] = (int64_t)d;
                } else if (field == RAY_EXTRACT_DOW) {
                    /* ISO day of week: Mon=1 .. Sun=7
                     * 2000-01-01 was Saturday (ISO 6).
                     * Formula: ((days%7)+7+5)%7 + 1 */
                    out[off + i] = ((days_since_2000 % 7) + 7 + 5) % 7 + 1;
                } else if (field == RAY_EXTRACT_DOY) {
                    /* Day of year [1..366], January-based */
                    static const int dbm[13] = {
                        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
                    };
                    if (mo < 1 || mo > 12) { out[off + i] = 0; continue; }
                    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                    int64_t doy_jan = dbm[mo] + (int64_t)d;
                    if (mo > 2 && leap) doy_jan++;
                    out[off + i] = doy_jan;
                } else {
                    out[off + i] = 0;
                }
            }
        }
        off += n;
    }

    #undef USEC_PER_SEC
    #undef USEC_PER_MIN
    #undef USEC_PER_HOUR
    #undef USEC_PER_DAY

    ray_release(input);
    return result;
}

/* ============================================================================
 * DATE_TRUNC — truncate temporal value to specified precision
 *
 * Input:  RAY_TIMESTAMP (i64 µs since 2000-01-01), RAY_DATE (i32 days since
 *         2000-01-01), or RAY_TIME (i32 ms since midnight).
 * Output: RAY_TIMESTAMP (i64 µs) — always returns microseconds since 2000-01-01.
 * Sub-day: modular arithmetic. Month/year: calendar decompose + recompose.
 * ============================================================================ */

/* Convert (year, month, day) to days since 2000-01-01 using the inverse of
 * Hinnant's civil_from_days. */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m > 2 ? (uint64_t)m - 3 : (uint64_t)m + 9) + 2) / 5 + (uint64_t)d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468 - 10957;
}

static ray_t* exec_date_trunc(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) { ray_release(input); return ray_error("nyi", NULL); }

    int64_t field = ext->sym;
    int64_t len = input->len;
    int8_t in_type = input->type;

    ray_t* result = ray_vec_new(RAY_TIMESTAMP, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;

    int64_t* out = (int64_t*)ray_data(result);

    #define DT_USEC_PER_SEC  1000000LL
    #define DT_USEC_PER_MIN  (60LL  * DT_USEC_PER_SEC)
    #define DT_USEC_PER_HOUR (3600LL * DT_USEC_PER_SEC)
    #define DT_USEC_PER_DAY  (86400LL * DT_USEC_PER_SEC)

    ray_morsel_t m;
    ray_morsel_init(&m, input);
    int64_t off = 0;

    while (ray_morsel_next(&m)) {
        int64_t n = m.morsel_len;

        for (int64_t i = 0; i < n; i++) {
            int64_t us;
            if (in_type == RAY_DATE) {
                int32_t d = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)d * DT_USEC_PER_DAY;
            } else if (in_type == RAY_TIME) {
                int32_t ms = ((const int32_t*)m.morsel_ptr)[i];
                us = (int64_t)ms * 1000LL;
            } else {
                us = ((const int64_t*)m.morsel_ptr)[i];
            }

            switch (field) {
                case RAY_EXTRACT_SECOND: {
                    /* Truncate to second boundary */
                    int64_t r = us % DT_USEC_PER_SEC;
                    out[off + i] = us - r - (r < 0 ? DT_USEC_PER_SEC : 0);
                    break;
                }
                case RAY_EXTRACT_MINUTE: {
                    int64_t r = us % DT_USEC_PER_MIN;
                    out[off + i] = us - r - (r < 0 ? DT_USEC_PER_MIN : 0);
                    break;
                }
                case RAY_EXTRACT_HOUR: {
                    int64_t r = us % DT_USEC_PER_HOUR;
                    out[off + i] = us - r - (r < 0 ? DT_USEC_PER_HOUR : 0);
                    break;
                }
                case RAY_EXTRACT_DAY: {
                    int64_t r = us % DT_USEC_PER_DAY;
                    out[off + i] = us - r - (r < 0 ? DT_USEC_PER_DAY : 0);
                    break;
                }
                case RAY_EXTRACT_MONTH: {
                    /* Decompose to y/m/d, set d=1, recompose */
                    int64_t days2k = us / DT_USEC_PER_DAY;
                    if (us < 0 && us % DT_USEC_PER_DAY != 0) days2k--;
                    int64_t z = days2k + 10957 + 719468;
                    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                    uint64_t doe = (uint64_t)(z - era * 146097);
                    uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                    int64_t y = (int64_t)yoe + era * 400;
                    uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100);
                    uint64_t mp = (5*doy_mar + 2) / 153;
                    uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
                    y += (mo <= 2);
                    out[off + i] = days_from_civil(y, (int64_t)mo, 1) * DT_USEC_PER_DAY;
                    break;
                }
                case RAY_EXTRACT_YEAR: {
                    /* Decompose to y/m/d, set m=1 d=1, recompose */
                    int64_t days2k = us / DT_USEC_PER_DAY;
                    if (us < 0 && us % DT_USEC_PER_DAY != 0) days2k--;
                    int64_t z = days2k + 10957 + 719468;
                    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                    uint64_t doe = (uint64_t)(z - era * 146097);
                    uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                    int64_t y = (int64_t)yoe + era * 400;
                    uint64_t doy_mar = doe - (365*yoe + yoe/4 - yoe/100);
                    uint64_t mp = (5*doy_mar + 2) / 153;
                    uint64_t mo = mp < 10 ? mp + 3 : mp - 9;
                    y += (mo <= 2);
                    out[off + i] = days_from_civil(y, 1, 1) * DT_USEC_PER_DAY;
                    break;
                }
                default:
                    out[off + i] = us;
                    break;
            }
        }
        off += n;
    }

    #undef DT_USEC_PER_SEC
    #undef DT_USEC_PER_MIN
    #undef DT_USEC_PER_HOUR
    #undef DT_USEC_PER_DAY

    ray_release(input);
    return result;
}

/* ============================================================================
 * Window function execution
 * ============================================================================ */

/* Compare rows ra and rb on the given key columns. Returns true if any differ. */
static inline bool win_keys_differ(ray_t* const* vecs, uint8_t n_keys,
                                    int64_t ra, int64_t rb) {
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* col = vecs[k];
        if (!col) continue;
        switch (col->type) {
        case RAY_I64: case RAY_TIMESTAMP:
            if (((const int64_t*)ray_data(col))[ra] !=
                ((const int64_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_F64: {
            double a = ((const double*)ray_data(col))[ra];
            double b = ((const double*)ray_data(col))[rb];
            if (a != b) return true;
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            if (((const int32_t*)ray_data(col))[ra] !=
                ((const int32_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_SYM:
            if (ray_read_sym(ray_data(col), ra, col->type, col->attrs) !=
                ray_read_sym(ray_data(col), rb, col->type, col->attrs)) return true;
            break;
        case RAY_I16:
            if (((const int16_t*)ray_data(col))[ra] !=
                ((const int16_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_BOOL: case RAY_U8:
            if (((const uint8_t*)ray_data(col))[ra] !=
                ((const uint8_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_STR: {
            const ray_str_t* elems;
            const char* pool;
            str_resolve(col, &elems, &pool);
            if (!ray_str_t_eq(&elems[ra], pool, &elems[rb], pool)) return true;
            break;
        }
        default: break;
        }
    }
    return false;
}

static inline double win_read_f64(ray_t* col, int64_t row) {
    switch (col->type) {
    case RAY_F64: return ((const double*)ray_data(col))[row];
    case RAY_I64: case RAY_TIMESTAMP:
        return (double)((const int64_t*)ray_data(col))[row];
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        return (double)((const int32_t*)ray_data(col))[row];
    case RAY_SYM:
        return (double)ray_read_sym(ray_data(col), row, col->type, col->attrs);
    case RAY_I16: return (double)((const int16_t*)ray_data(col))[row];
    case RAY_BOOL: case RAY_U8: return (double)((const uint8_t*)ray_data(col))[row];
    default: return 0.0;
    }
}

static inline int64_t win_read_i64(ray_t* col, int64_t row) {
    switch (col->type) {
    case RAY_I64: case RAY_TIMESTAMP:
        return ((const int64_t*)ray_data(col))[row];
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        return (int64_t)((const int32_t*)ray_data(col))[row];
    case RAY_SYM:
        return ray_read_sym(ray_data(col), row, col->type, col->attrs);
    case RAY_F64: return (int64_t)((const double*)ray_data(col))[row];
    case RAY_I16: return (int64_t)((const int16_t*)ray_data(col))[row];
    case RAY_BOOL: case RAY_U8: return (int64_t)((const uint8_t*)ray_data(col))[row];
    default: return 0;
    }
}

/* Resolve a graph op node to a column vector from tbl */
static ray_t* win_resolve_vec(ray_graph_t* g, ray_op_t* key_op, ray_t* tbl,
                              uint8_t* owned) {
    ray_op_ext_t* key_ext = find_ext(g, key_op->id);
    if (key_ext && key_ext->base.opcode == OP_SCAN) {
        *owned = 0;
        return ray_table_get_col(tbl, key_ext->sym);
    }
    *owned = 1;
    ray_t* saved = g->table;
    g->table = tbl;
    ray_t* v = exec_node(g, key_op);
    g->table = saved;
    return v;
}

/* Compute window functions for one partition [ps, pe) in sorted_idx */
static void win_compute_partition(
    ray_t* const* order_vecs, uint8_t n_order,
    ray_t* const* func_vecs, const uint8_t* func_kinds, const int64_t* func_params,
    uint8_t n_funcs,
    uint8_t frame_start, uint8_t frame_end,
    const int64_t* sorted_idx, int64_t ps, int64_t pe,
    ray_t* const* result_vecs, const bool* is_f64)
{
    if (ps >= pe) return; /* empty partition — nothing to compute */
    int64_t part_len = pe - ps;

    for (uint8_t f = 0; f < n_funcs; f++) {
        uint8_t kind = func_kinds[f];
        ray_t* fvec = func_vecs[f];
        ray_t* rvec = result_vecs[f];
        bool whole = (frame_start == RAY_BOUND_UNBOUNDED_PRECEDING &&
                      frame_end == RAY_BOUND_UNBOUNDED_FOLLOWING);

        switch (kind) {
        case RAY_WIN_ROW_NUMBER: {
            int64_t* out = (int64_t*)ray_data(rvec);
            for (int64_t i = ps; i < pe; i++)
                out[sorted_idx[i]] = i - ps + 1;
            break;
        }
        case RAY_WIN_RANK: {
            int64_t* out = (int64_t*)ray_data(rvec);
            int64_t rank = 1;
            out[sorted_idx[ps]] = 1;
            for (int64_t i = ps + 1; i < pe; i++) {
                if (n_order > 0 && win_keys_differ(order_vecs, n_order,
                        sorted_idx[i-1], sorted_idx[i]))
                    rank = i - ps + 1;
                out[sorted_idx[i]] = rank;
            }
            break;
        }
        case RAY_WIN_DENSE_RANK: {
            int64_t* out = (int64_t*)ray_data(rvec);
            int64_t rank = 1;
            out[sorted_idx[ps]] = 1;
            for (int64_t i = ps + 1; i < pe; i++) {
                if (n_order > 0 && win_keys_differ(order_vecs, n_order,
                        sorted_idx[i-1], sorted_idx[i]))
                    rank++;
                out[sorted_idx[i]] = rank;
            }
            break;
        }
        case RAY_WIN_NTILE: {
            int64_t n = func_params[f];
            if (n <= 0) n = 1;
            int64_t* out = (int64_t*)ray_data(rvec);
            for (int64_t i = ps; i < pe; i++)
                out[sorted_idx[i]] = ((i - ps) * n) / part_len + 1;
            break;
        }
        case RAY_WIN_COUNT: {
            int64_t* out = (int64_t*)ray_data(rvec);
            if (whole) {
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = part_len;
            } else {
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = i - ps + 1;
            }
            break;
        }
        case RAY_WIN_SUM: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    double t = 0.0;
                    for (int64_t i = ps; i < pe; i++)
                        t += win_read_f64(fvec, sorted_idx[i]);
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = t;
                } else {
                    double acc = 0.0;
                    for (int64_t i = ps; i < pe; i++) {
                        acc += win_read_f64(fvec, sorted_idx[i]);
                        out[sorted_idx[i]] = acc;
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    int64_t t = 0;
                    for (int64_t i = ps; i < pe; i++)
                        t += win_read_i64(fvec, sorted_idx[i]);
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = t;
                } else {
                    int64_t acc = 0;
                    for (int64_t i = ps; i < pe; i++) {
                        acc += win_read_i64(fvec, sorted_idx[i]);
                        out[sorted_idx[i]] = acc;
                    }
                }
            }
            break;
        }
        case RAY_WIN_AVG: {
            if (!fvec) break;
            double* out = (double*)ray_data(rvec);
            if (whole) {
                double t = 0.0;
                for (int64_t i = ps; i < pe; i++)
                    t += win_read_f64(fvec, sorted_idx[i]);
                double avg = t / (double)part_len;
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = avg;
            } else {
                double acc = 0.0;
                for (int64_t i = ps; i < pe; i++) {
                    acc += win_read_f64(fvec, sorted_idx[i]);
                    out[sorted_idx[i]] = acc / (double)(i - ps + 1);
                }
            }
            break;
        }
        case RAY_WIN_MIN: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    double mn = DBL_MAX;
                    for (int64_t i = ps; i < pe; i++) {
                        double v = win_read_f64(fvec, sorted_idx[i]);
                        if (v < mn) mn = v;
                    }
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = mn;
                } else {
                    double mn = DBL_MAX;
                    for (int64_t i = ps; i < pe; i++) {
                        double v = win_read_f64(fvec, sorted_idx[i]);
                        if (v < mn) mn = v;
                        out[sorted_idx[i]] = mn;
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    int64_t mn = INT64_MAX;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t v = win_read_i64(fvec, sorted_idx[i]);
                        if (v < mn) mn = v;
                    }
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = mn;
                } else {
                    int64_t mn = INT64_MAX;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t v = win_read_i64(fvec, sorted_idx[i]);
                        if (v < mn) mn = v;
                        out[sorted_idx[i]] = mn;
                    }
                }
            }
            break;
        }
        case RAY_WIN_MAX: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    double mx = -DBL_MAX;
                    for (int64_t i = ps; i < pe; i++) {
                        double v = win_read_f64(fvec, sorted_idx[i]);
                        if (v > mx) mx = v;
                    }
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = mx;
                } else {
                    double mx = -DBL_MAX;
                    for (int64_t i = ps; i < pe; i++) {
                        double v = win_read_f64(fvec, sorted_idx[i]);
                        if (v > mx) mx = v;
                        out[sorted_idx[i]] = mx;
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    int64_t mx = INT64_MIN;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t v = win_read_i64(fvec, sorted_idx[i]);
                        if (v > mx) mx = v;
                    }
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = mx;
                } else {
                    int64_t mx = INT64_MIN;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t v = win_read_i64(fvec, sorted_idx[i]);
                        if (v > mx) mx = v;
                        out[sorted_idx[i]] = mx;
                    }
                }
            }
            break;
        }
        case RAY_WIN_LAG: {
            if (!fvec) break;
            int64_t offset = func_params[f];
            if (offset <= 0) offset = 1;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i - offset;
                    out[sorted_idx[i]] = (src >= ps)
                        ? win_read_f64(fvec, sorted_idx[src]) : NAN;
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i - offset;
                    out[sorted_idx[i]] = (src >= ps)
                        ? win_read_i64(fvec, sorted_idx[src]) : 0;
                }
            }
            break;
        }
        case RAY_WIN_LEAD: {
            if (!fvec) break;
            int64_t offset = func_params[f];
            if (offset <= 0) offset = 1;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i + offset;
                    out[sorted_idx[i]] = (src < pe)
                        ? win_read_f64(fvec, sorted_idx[src]) : NAN;
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i + offset;
                    out[sorted_idx[i]] = (src < pe)
                        ? win_read_i64(fvec, sorted_idx[src]) : 0;
                }
            }
            break;
        }
        case RAY_WIN_FIRST_VALUE: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                double first = win_read_f64(fvec, sorted_idx[ps]);
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = first;
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                int64_t first = win_read_i64(fvec, sorted_idx[ps]);
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = first;
            }
            break;
        }
        case RAY_WIN_LAST_VALUE: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    double last = win_read_f64(fvec, sorted_idx[pe - 1]);
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = last;
                } else {
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = win_read_f64(fvec, sorted_idx[i]);
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    int64_t last = win_read_i64(fvec, sorted_idx[pe - 1]);
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = last;
                } else {
                    for (int64_t i = ps; i < pe; i++)
                        out[sorted_idx[i]] = win_read_i64(fvec, sorted_idx[i]);
                }
            }
            break;
        }
        case RAY_WIN_NTH_VALUE: {
            if (!fvec) break;
            int64_t nth = func_params[f];
            if (nth < 1) nth = 1;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                double val = (nth <= part_len)
                    ? win_read_f64(fvec, sorted_idx[ps + nth - 1]) : NAN;
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = val;
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                int64_t val = (nth <= part_len)
                    ? win_read_i64(fvec, sorted_idx[ps + nth - 1]) : 0;
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = val;
            }
            break;
        }
        } /* switch */
    } /* for each func */
}

/* Parallel per-partition window compute context */
typedef struct {
    ray_t** order_vecs;
    uint8_t n_order;
    ray_t** func_vecs;
    uint8_t* func_kinds;
    int64_t* func_params;
    uint8_t n_funcs;
    uint8_t frame_start;
    uint8_t frame_end;
    int64_t* sorted_idx;
    int64_t* part_offsets;
    ray_t** result_vecs;
    bool* is_f64;
} win_par_ctx_t;

static void win_par_fn(void* arg, uint32_t worker_id,
                       int64_t start, int64_t end) {
    (void)worker_id;
    win_par_ctx_t* ctx = (win_par_ctx_t*)arg;
    for (int64_t p = start; p < end; p++) {
        win_compute_partition(
            ctx->order_vecs, ctx->n_order,
            ctx->func_vecs, ctx->func_kinds, ctx->func_params,
            ctx->n_funcs, ctx->frame_start, ctx->frame_end,
            ctx->sorted_idx, ctx->part_offsets[p], ctx->part_offsets[p + 1],
            ctx->result_vecs, ctx->is_f64);
    }
}

/* Parallel gather of partition key values into contiguous array.
 * Eliminates random-access reads during Phase 2 boundary detection. */
typedef struct {
    const int64_t* sorted_idx;
    uint64_t*      pkey_sorted;
    ray_t**         sort_vecs;
    uint8_t        n_part;
} pkey_gather_ctx_t;

static void pkey_gather_fn(void* arg, uint32_t wid,
                            int64_t start, int64_t end) {
    (void)wid;
    pkey_gather_ctx_t* ctx = (pkey_gather_ctx_t*)arg;
    const int64_t* sidx = ctx->sorted_idx;
    uint64_t* out = ctx->pkey_sorted;

    if (ctx->n_part == 1) {
        ray_t* pk = ctx->sort_vecs[0];
        const void* pkd = ray_data(pk);
        if (RAY_IS_SYM(pk->type)) {
            for (int64_t i = start; i < end; i++)
                out[i] = (uint64_t)ray_read_sym(pkd, sidx[i], pk->type, pk->attrs);
        } else if (pk->type == RAY_I32 || pk->type == RAY_DATE || pk->type == RAY_TIME) {
            const int32_t* src = (const int32_t*)pkd;
            for (int64_t i = start; i < end; i++)
                out[i] = (uint64_t)((uint32_t)(src[sidx[i]] - INT32_MIN));
        } else {
            const uint64_t* src = (const uint64_t*)pkd;
            for (int64_t i = start; i < end; i++)
                out[i] = src[sidx[i]];
        }
    } else {
        for (int64_t i = start; i < end; i++) {
            int64_t r = sidx[i];
            uint64_t key = 0;
            for (uint8_t k = 0; k < ctx->n_part; k++) {
                ray_t* col = ctx->sort_vecs[k];
                const void* d = ray_data(col);
                if (RAY_IS_SYM(col->type))
                    key = (key << 32) | (uint32_t)ray_read_sym(d, r, col->type, col->attrs);
                else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME)
                    key = (key << 32) | (uint32_t)(((const int32_t*)d)[r] - INT32_MIN);
                else {
                    key = (key << 32) | (uint32_t)((const uint64_t*)d)[r];
                }
            }
            out[i] = key;
        }
    }
}

static ray_t* exec_window(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);
    uint8_t n_part  = ext->window.n_part_keys;
    uint8_t n_order = ext->window.n_order_keys;
    uint8_t n_funcs = ext->window.n_funcs;
    /* Guard against uint8_t overflow on n_part + n_order */
    if ((uint16_t)n_part + n_order > 255)
        return ray_error("nyi", NULL);
    uint8_t n_sort  = n_part + n_order;

    if (nrows == 0 || n_funcs == 0) {
        ray_retain(tbl);
        return tbl;
    }

    /* --- Phase 0: Resolve key and func_input vectors --- */
    /* VLAs below are bounded by uint8_t limits (max 255 each),
     * so max ~10KB on stack; bounded by uint8_t limits. */
    ray_t* sort_vecs[n_sort > 0 ? n_sort : 1];
    uint8_t sort_owned[n_sort > 0 ? n_sort : 1];
    uint8_t sort_descs[n_sort > 0 ? n_sort : 1];
    memset(sort_owned, 0, sizeof(sort_owned));
    memset(sort_descs, 0, sizeof(sort_descs));

    for (uint8_t k = 0; k < n_part; k++) {
        sort_vecs[k] = win_resolve_vec(g, ext->window.part_keys[k], tbl,
                                        &sort_owned[k]);
        sort_descs[k] = 0;  /* partition keys always ASC */
        if (!sort_vecs[k] || RAY_IS_ERR(sort_vecs[k])) {
            ray_t* err = sort_vecs[k] ? sort_vecs[k] : ray_error("nyi", NULL);
            for (uint8_t j = 0; j < k; j++)
                if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                    ray_release(sort_vecs[j]);
            return err;
        }
    }
    for (uint8_t k = 0; k < n_order; k++) {
        sort_vecs[n_part + k] = win_resolve_vec(g, ext->window.order_keys[k],
                                                 tbl, &sort_owned[n_part + k]);
        sort_descs[n_part + k] = ext->window.order_descs[k];
        if (!sort_vecs[n_part + k] || RAY_IS_ERR(sort_vecs[n_part + k])) {
            ray_t* err = sort_vecs[n_part + k] ? sort_vecs[n_part + k]
                                               : ray_error("nyi", NULL);
            for (uint8_t j = 0; j < n_part + k; j++)
                if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                    ray_release(sort_vecs[j]);
            return err;
        }
    }

    ray_t* func_vecs[n_funcs];
    uint8_t func_owned[n_funcs];
    ray_t* result_vecs[n_funcs];
    bool is_f64[n_funcs];
    memset(func_owned, 0, sizeof(func_owned));
    memset(result_vecs, 0, sizeof(result_vecs));
    for (uint8_t f = 0; f < n_funcs; f++) {
        ray_op_t* fi = ext->window.func_inputs[f];
        if (fi) {
            func_vecs[f] = win_resolve_vec(g, fi, tbl, &func_owned[f]);
            if (!func_vecs[f] || RAY_IS_ERR(func_vecs[f])) {
                ray_t* err = func_vecs[f] ? func_vecs[f] : ray_error("nyi", NULL);
                for (uint8_t j = 0; j < f; j++)
                    if (func_owned[j] && func_vecs[j] && !RAY_IS_ERR(func_vecs[j]))
                        ray_release(func_vecs[j]);
                for (uint8_t j = 0; j < n_sort; j++)
                    if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                        ray_release(sort_vecs[j]);
                return err;
            }
        } else {
            func_vecs[f] = NULL;
        }
    }

    /* --- Phase 1: Sort by (partition_keys ++ order_keys) --- */
    ray_t* radix_itmp_hdr = NULL;
    ray_t* win_enum_rank_hdrs[n_sort > 0 ? n_sort : 1];
    memset(win_enum_rank_hdrs, 0, sizeof(win_enum_rank_hdrs));

    ray_t* indices_hdr = NULL;
    int64_t* indices = (int64_t*)scratch_alloc(&indices_hdr,
                                (size_t)nrows * sizeof(int64_t));
    if (!indices) goto oom;
    for (int64_t i = 0; i < nrows; i++) indices[i] = i;

    int64_t* sorted_idx = indices;

    if (n_sort > 0 && nrows <= 64) {
        sort_cmp_ctx_t cmp_ctx = {
            .vecs = sort_vecs, .desc = sort_descs,
            .nulls_first = NULL, .n_sort = n_sort,
        };
        sort_insertion(&cmp_ctx, indices, nrows);
    } else if (n_sort > 0) {
        /* --- Radix sort fast path --- */
        bool can_radix = true;
        for (uint8_t k = 0; k < n_sort; k++) {
            if (!sort_vecs[k]) { can_radix = false; break; }
            int8_t t = sort_vecs[k]->type;
            if (t != RAY_I64 && t != RAY_F64 && t != RAY_I32 && t != RAY_I16 &&
                t != RAY_BOOL && t != RAY_U8 && t != RAY_SYM &&
                t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP) {
                can_radix = false; break;
            }
        }
        bool radix_done = false;

        if (can_radix) {
            ray_pool_t* pool = ray_pool_get();

            /* Build SYM rank mappings */
            uint32_t* enum_ranks[n_sort];
            memset(enum_ranks, 0, n_sort * sizeof(uint32_t*));
            for (uint8_t k = 0; k < n_sort; k++) {
                if (RAY_IS_SYM(sort_vecs[k]->type)) {
                    enum_ranks[k] = build_enum_rank(sort_vecs[k], nrows,
                                                     &win_enum_rank_hdrs[k]);
                    if (!enum_ranks[k]) { can_radix = false; break; }
                }
            }

            if (can_radix && n_sort == 1) {
                /* Single-key sort */
                uint8_t key_nbytes = radix_key_bytes(sort_vecs[0]->type);
                ray_pool_t* sk_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool : NULL;
                ray_t *keys_hdr;
                uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
                                    (size_t)nrows * sizeof(uint64_t));
                if (keys) {
                    radix_encode_ctx_t enc = {
                        .keys = keys, .data = ray_data(sort_vecs[0]),
                        .type = sort_vecs[0]->type,
                        .col_attrs = sort_vecs[0]->attrs,
                        .desc = sort_descs[0],
                        .nulls_first = sort_descs[0], /* default: NULLS FIRST for DESC */
                        .enum_rank = enum_ranks[0], .n_keys = 1,
                    };
                    if (sk_pool)
                        ray_pool_dispatch(sk_pool, radix_encode_fn, &enc, nrows);
                    else
                        radix_encode_fn(&enc, 0, 0, nrows);

                    if (nrows <= RADIX_SORT_THRESHOLD) {
                        key_introsort(keys, indices, nrows);
                        sorted_idx = indices;
                        radix_done = true;
                    } else {
                        ray_t *ktmp_hdr, *itmp_hdr;
                        uint64_t* ktmp = (uint64_t*)scratch_alloc(&ktmp_hdr,
                                            (size_t)nrows * sizeof(uint64_t));
                        int64_t*  itmp = (int64_t*)scratch_alloc(&itmp_hdr,
                                            (size_t)nrows * sizeof(int64_t));
                        if (ktmp && itmp) {
                            sorted_idx = radix_sort_run(sk_pool, keys, indices,
                                                         ktmp, itmp, nrows,
                                                         key_nbytes, NULL);
                            radix_done = (sorted_idx != NULL);
                        }
                        scratch_free(ktmp_hdr);
                        if (sorted_idx != itmp) scratch_free(itmp_hdr);
                        else radix_itmp_hdr = itmp_hdr;
                    }
                }
                scratch_free(keys_hdr);
            } else if (can_radix && n_sort > 1) {
                /* Multi-key composite radix sort */
                ray_pool_t* pool2 = pool;
                int64_t mins[n_sort], maxs[n_sort];
                uint8_t total_bits = 0;
                bool fits = true;

                ray_pool_t* mk_prescan_pool2 = (nrows >= SMALL_POOL_THRESHOLD) ? pool2 : NULL;
                if (n_sort <= MK_PRESCAN_MAX_KEYS && mk_prescan_pool2) {
                    uint32_t nw = ray_pool_total_workers(mk_prescan_pool2);
                    size_t pw_count = (size_t)nw * n_sort;
                    int64_t pw_mins_stack[512], pw_maxs_stack[512];
                    ray_t *pw_mins_hdr = NULL, *pw_maxs_hdr = NULL;
                    int64_t* pw_mins = (pw_count <= 512)
                        ? pw_mins_stack
                        : (int64_t*)scratch_alloc(&pw_mins_hdr, pw_count * sizeof(int64_t));
                    int64_t* pw_maxs = (pw_count <= 512)
                        ? pw_maxs_stack
                        : (int64_t*)scratch_alloc(&pw_maxs_hdr, pw_count * sizeof(int64_t));
                    for (size_t i = 0; i < pw_count; i++) {
                        pw_mins[i] = INT64_MAX;
                        pw_maxs[i] = INT64_MIN;
                    }
                    mk_prescan_ctx_t pctx = {
                        .vecs = sort_vecs, .enum_ranks = enum_ranks,
                        .n_keys = n_sort, .nrows = nrows, .n_workers = nw,
                        .pw_mins = pw_mins, .pw_maxs = pw_maxs,
                    };
                    ray_pool_dispatch(mk_prescan_pool2, mk_prescan_fn, &pctx, nrows);

                    for (uint8_t k = 0; k < n_sort; k++) {
                        int64_t kmin = INT64_MAX, kmax = INT64_MIN;
                        for (uint32_t w = 0; w < nw; w++) {
                            int64_t wmin = pw_mins[w * n_sort + k];
                            int64_t wmax = pw_maxs[w * n_sort + k];
                            if (wmin < kmin) kmin = wmin;
                            if (wmax > kmax) kmax = wmax;
                        }
                        mins[k] = kmin;
                        maxs[k] = kmax;
                        uint64_t range = (uint64_t)(kmax - kmin);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        total_bits += bits;
                    }
                    if (pw_mins_hdr) scratch_free(pw_mins_hdr);
                    if (pw_maxs_hdr) scratch_free(pw_maxs_hdr);
                } else {
                    for (uint8_t k = 0; k < n_sort; k++) {
                        ray_t* col = sort_vecs[k];
                        int64_t kmin = INT64_MAX, kmax = INT64_MIN;
                        if (enum_ranks[k]) {
                            const void* cdata = ray_data(col);
                            int8_t ctype = col->type;
                            uint8_t cattrs = col->attrs;
                            for (int64_t i = 0; i < nrows; i++) {
                                uint32_t raw = (uint32_t)ray_read_sym(cdata, i, ctype, cattrs);
                                int64_t v = (int64_t)enum_ranks[k][raw];
                                if (v < kmin) kmin = v;
                                if (v > kmax) kmax = v;
                            }
                        } else if (col->type == RAY_I64 || col->type == RAY_TIMESTAMP) {
                            const int64_t* d = (const int64_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = d[i];
                                if (d[i] > kmax) kmax = d[i];
                            }
                        } else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME) {
                            const int32_t* d = (const int32_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        } else if (col->type == RAY_I16) {
                            const int16_t* d = (const int16_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        } else if (col->type == RAY_BOOL || col->type == RAY_U8) {
                            const uint8_t* d = (const uint8_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        }
                        mins[k] = kmin;
                        maxs[k] = kmax;
                        uint64_t range = (uint64_t)(kmax - kmin);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        total_bits += bits;
                    }
                }

                if (total_bits > 64) fits = false;

                if (fits) {
                    uint8_t bit_shifts[n_sort];
                    uint8_t accum = 0;
                    for (int k = n_sort - 1; k >= 0; k--) {
                        bit_shifts[k] = accum;
                        uint64_t range = (uint64_t)(maxs[k] - mins[k]);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        accum += bits;
                    }

                    uint8_t comp_nbytes = (total_bits + 7) / 8;
                    if (comp_nbytes < 1) comp_nbytes = 1;
                    ray_pool_t* mk_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool2 : NULL;

                    ray_t *keys_hdr;
                    uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
                                        (size_t)nrows * sizeof(uint64_t));
                    if (keys) {
                        radix_encode_ctx_t enc = {
                            .keys = keys, .n_keys = n_sort, .vecs = sort_vecs,
                        };
                        for (uint8_t k = 0; k < n_sort; k++) {
                            enc.mins[k] = mins[k];
                            enc.ranges[k] = maxs[k] - mins[k];
                            enc.bit_shifts[k] = bit_shifts[k];
                            enc.descs[k] = sort_descs[k];
                            enc.enum_ranks[k] = enum_ranks[k];
                        }
                        if (mk_pool)
                            ray_pool_dispatch(mk_pool, radix_encode_fn, &enc, nrows);
                        else
                            radix_encode_fn(&enc, 0, 0, nrows);

                        if (nrows <= RADIX_SORT_THRESHOLD) {
                            key_introsort(keys, indices, nrows);
                            sorted_idx = indices;
                            radix_done = true;
                        } else {
                            ray_t *ktmp_hdr, *itmp_hdr;
                            uint64_t* ktmp = (uint64_t*)scratch_alloc(&ktmp_hdr,
                                                (size_t)nrows * sizeof(uint64_t));
                            int64_t*  itmp = (int64_t*)scratch_alloc(&itmp_hdr,
                                                (size_t)nrows * sizeof(int64_t));
                            if (ktmp && itmp) {
                                sorted_idx = radix_sort_run(mk_pool, keys, indices,
                                                             ktmp, itmp, nrows,
                                                             comp_nbytes, NULL);
                                radix_done = (sorted_idx != NULL);
                            }
                            scratch_free(ktmp_hdr);
                            if (sorted_idx != itmp) scratch_free(itmp_hdr);
                            else radix_itmp_hdr = itmp_hdr;
                        }
                    }
                    scratch_free(keys_hdr);
                }
            }
        }

        /* --- Merge sort fallback --- */
        if (!radix_done) {
            sort_cmp_ctx_t cmp_ctx = {
                .vecs = sort_vecs, .desc = sort_descs,
                .nulls_first = NULL, .n_sort = n_sort,
            };
            ray_t* tmp_hdr;
            int64_t* tmp = (int64_t*)scratch_alloc(&tmp_hdr,
                                (size_t)nrows * sizeof(int64_t));
            if (!tmp) { scratch_free(indices_hdr); indices_hdr = NULL; goto oom; }

            ray_pool_t* pool = ray_pool_get();
            uint32_t nw = pool ? ray_pool_total_workers(pool) : 1;
            if (pool && nw > 1 && nrows > 1024) {
                sort_phase1_ctx_t p1ctx = {
                    .cmp_ctx = &cmp_ctx, .indices = indices, .tmp = tmp,
                    .nrows = nrows, .n_chunks = nw,
                };
                ray_pool_dispatch_n(pool, sort_phase1_fn, &p1ctx, nw);

                int64_t chunk_size = (nrows + nw - 1) / nw;
                int64_t run_size = chunk_size;
                int64_t* src = indices;
                int64_t* dst = tmp;
                while (run_size < nrows) {
                    int64_t n_pairs = (nrows + 2 * run_size - 1) / (2 * run_size);
                    sort_merge_ctx_t mctx = {
                        .cmp_ctx = &cmp_ctx, .src = src, .dst = dst,
                        .nrows = nrows, .run_size = run_size,
                    };
                    if (n_pairs > 1)
                        ray_pool_dispatch_n(pool, sort_merge_fn, &mctx,
                                            (uint32_t)n_pairs);
                    else
                        sort_merge_fn(&mctx, 0, 0, n_pairs);
                    int64_t* t = src; src = dst; dst = t;
                    run_size *= 2;
                }
                if (src != indices)
                    memcpy(indices, src, (size_t)nrows * sizeof(int64_t));
            } else {
                sort_merge_recursive(&cmp_ctx, indices, tmp, nrows);
            }
            scratch_free(tmp_hdr);
            sorted_idx = indices;
        }
    }

    /* --- Phase 2: Find partition boundaries --- */
    /* Overallocate part_offsets to worst case (single-pass, no counting pass) */
    ray_t* poff_hdr = NULL;
    int64_t* part_offsets = (int64_t*)scratch_alloc(&poff_hdr,
                                (size_t)(nrows + 1) * sizeof(int64_t));
    if (!part_offsets) { scratch_free(indices_hdr); goto oom; }

    part_offsets[0] = 0;
    int64_t n_parts = 0;

    if (n_part > 0) {
        /* Check if we can pack partition keys into uint64 for fast gather.
         * Multi-key packing shifts each key by 32 bits, so any key requiring
         * >32 bits in a multi-key scenario would be truncated.  Force fallback
         * when any 64-bit key appears alongside other keys. */
        uint8_t pk_bits = 0;
        bool can_pack = true;
        bool has_64bit_key = false;
        for (uint8_t k = 0; k < n_part; k++) {
            int8_t t = sort_vecs[k]->type;
            if (RAY_IS_SYM(t) || t == RAY_I32 || t == RAY_DATE || t == RAY_TIME) pk_bits += 32;
            else if (t == RAY_I64 || t == RAY_SYM || t == RAY_TIMESTAMP ||
                     t == RAY_F64) { pk_bits += 64; has_64bit_key = true; }
            else { can_pack = false; break; }
            if (pk_bits > 64) { can_pack = false; break; }
        }
        /* If multi-key with any 64-bit type, the <<32 packing truncates.
         * Force sequential fallback for correctness. */
        if (can_pack && n_part > 1 && has_64bit_key) can_pack = false;

        ray_t* pkey_hdr = NULL;
        uint64_t* pkey_sorted = can_pack ?
            (uint64_t*)scratch_alloc(&pkey_hdr, (size_t)nrows * sizeof(uint64_t))
            : NULL;

        if (pkey_sorted) {
            /* Parallel gather partition keys into contiguous array */
            pkey_gather_ctx_t gctx = {
                .sorted_idx = sorted_idx, .pkey_sorted = pkey_sorted,
                .sort_vecs = sort_vecs, .n_part = n_part,
            };
            ray_pool_t* gpool = ray_pool_get();
            if (gpool)
                ray_pool_dispatch(gpool, pkey_gather_fn, &gctx, nrows);
            else
                pkey_gather_fn(&gctx, 0, 0, nrows);

            /* Sequential scan on contiguous data (no random access) */
            for (int64_t i = 1; i < nrows; i++)
                if (pkey_sorted[i] != pkey_sorted[i - 1])
                    part_offsets[++n_parts] = i;

            scratch_free(pkey_hdr);
        } else {
            /* Fallback: single-pass random-access comparison */
            for (int64_t i = 1; i < nrows; i++)
                if (win_keys_differ(sort_vecs, n_part,
                                    sorted_idx[i - 1], sorted_idx[i]))
                    part_offsets[++n_parts] = i;
        }
        part_offsets[++n_parts] = nrows;
    } else {
        /* No partition keys: entire table is one partition.
         * Minor memory waste (part_offsets sized for nrows+1) but no
         * correctness issue — only indices 0 and 1 are used. */
        part_offsets[1] = nrows;
        n_parts = 1;
    }

    /* Check cancellation before expensive per-partition compute */
    {
        ray_pool_t* cpool = ray_pool_get();
        if (pool_cancelled(cpool)) {
            scratch_free(poff_hdr);
            scratch_free(indices_hdr);
            if (radix_itmp_hdr) scratch_free(radix_itmp_hdr);
            for (uint8_t k = 0; k < n_sort; k++)
                if (win_enum_rank_hdrs[k]) scratch_free(win_enum_rank_hdrs[k]);
            for (uint8_t k = 0; k < n_sort; k++)
                if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
                    ray_release(sort_vecs[k]);
            for (uint8_t f = 0; f < n_funcs; f++)
                if (func_owned[f] && func_vecs[f] && !RAY_IS_ERR(func_vecs[f]))
                    ray_release(func_vecs[f]);
            return ray_error("cancel", NULL);
        }
    }

    /* --- Phase 3: Allocate result vectors and compute per-partition --- */
    for (uint8_t f = 0; f < n_funcs; f++) {
        uint8_t kind = ext->window.func_kinds[f];
        ray_t* fvec = func_vecs[f];

        bool out_f64 = false;
        if (kind == RAY_WIN_AVG) {
            out_f64 = true;
        } else if (kind == RAY_WIN_SUM || kind == RAY_WIN_MIN ||
                   kind == RAY_WIN_MAX || kind == RAY_WIN_LAG ||
                   kind == RAY_WIN_LEAD || kind == RAY_WIN_FIRST_VALUE ||
                   kind == RAY_WIN_LAST_VALUE || kind == RAY_WIN_NTH_VALUE) {
            out_f64 = fvec && fvec->type == RAY_F64;
        }

        is_f64[f] = out_f64;
        result_vecs[f] = ray_vec_new(out_f64 ? RAY_F64 : RAY_I64, nrows);
        if (!result_vecs[f] || RAY_IS_ERR(result_vecs[f])) {
            for (uint8_t j = 0; j < f; j++) ray_release(result_vecs[j]);
            scratch_free(poff_hdr);
            scratch_free(indices_hdr);
            goto oom;
        }
        result_vecs[f]->len = nrows;
        memset(ray_data(result_vecs[f]), 0, (size_t)nrows * 8);
    }

    /* Order key vectors start at sort_vecs[n_part] */
    ray_t** order_vecs = n_order > 0 ? &sort_vecs[n_part] : NULL;

    {
        ray_pool_t* p3pool = ray_pool_get();
        if (p3pool && n_parts > 1) {
            win_par_ctx_t pctx = {
                .order_vecs = order_vecs, .n_order = n_order,
                .func_vecs = func_vecs, .func_kinds = ext->window.func_kinds,
                .func_params = ext->window.func_params, .n_funcs = n_funcs,
                .frame_start = ext->window.frame_start,
                .frame_end = ext->window.frame_end,
                .sorted_idx = sorted_idx, .part_offsets = part_offsets,
                .result_vecs = result_vecs, .is_f64 = is_f64,
            };
            ray_pool_dispatch_n(p3pool, win_par_fn, &pctx, (uint32_t)n_parts);
        } else {
            for (int64_t p = 0; p < n_parts; p++) {
                win_compute_partition(
                    order_vecs, n_order,
                    func_vecs, ext->window.func_kinds, ext->window.func_params,
                    n_funcs, ext->window.frame_start, ext->window.frame_end,
                    sorted_idx, part_offsets[p], part_offsets[p + 1],
                    result_vecs, is_f64);
            }
        }
    }

    /* --- Phase 4: Build result table --- */
    ray_t* result = ray_table_new(ncols + n_funcs);
    if (!result || RAY_IS_ERR(result)) {
        for (uint8_t f = 0; f < n_funcs; f++) ray_release(result_vecs[f]);
        scratch_free(poff_hdr);
        scratch_free(indices_hdr);
        goto oom;
    }

    /* Pass-through original columns */
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        int64_t name_id = ray_table_col_name(tbl, c);
        ray_retain(col);
        result = ray_table_add_col(result, name_id, col);
        ray_release(col);
    }

    /* Add window result columns with auto-generated names */
    for (uint8_t f = 0; f < n_funcs; f++) {
        char buf[16] = "_w";
        int pos = 2;
        if (f >= 100) buf[pos++] = '0' + (f / 100);
        if (f >= 10)  buf[pos++] = '0' + ((f / 10) % 10);
        buf[pos++] = '0' + (f % 10);
        buf[pos] = '\0';
        int64_t name_id = ray_sym_intern(buf, (size_t)pos);
        result = ray_table_add_col(result, name_id, result_vecs[f]);
        ray_release(result_vecs[f]);
    }

    scratch_free(poff_hdr);
    if (radix_itmp_hdr) scratch_free(radix_itmp_hdr);
    scratch_free(indices_hdr);
    for (uint8_t k = 0; k < n_sort; k++)
        if (win_enum_rank_hdrs[k]) scratch_free(win_enum_rank_hdrs[k]);

    /* Free owned key/func vectors */
    for (uint8_t k = 0; k < n_sort; k++)
        if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
            ray_release(sort_vecs[k]);
    for (uint8_t f = 0; f < n_funcs; f++)
        if (func_owned[f] && func_vecs[f] && !RAY_IS_ERR(func_vecs[f]))
            ray_release(func_vecs[f]);

    return result;

oom:
    if (radix_itmp_hdr) scratch_free(radix_itmp_hdr);
    for (uint8_t k = 0; k < n_sort; k++)
        if (win_enum_rank_hdrs[k]) scratch_free(win_enum_rank_hdrs[k]);
    for (uint8_t k = 0; k < n_sort; k++)
        if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
            ray_release(sort_vecs[k]);
    for (uint8_t f = 0; f < n_funcs; f++) {
        if (func_owned[f] && func_vecs[f] && !RAY_IS_ERR(func_vecs[f]))
            ray_release(func_vecs[f]);
        if (result_vecs[f] && !RAY_IS_ERR(result_vecs[f]))
            ray_release(result_vecs[f]);
    }
    return ray_error("oom", NULL);
}

/* ============================================================================
 * Graph execution functions
 * ============================================================================ */

/* exec_expand_factorized: emit factorized output for expand+group fusion.
 * Returns a table with _src (unique sources) and _count (degree per source).
 * This avoids materializing the full (src, dst) cross-product. */
static ray_t* exec_expand_factorized(ray_rel_t* rel, uint8_t direction, ray_t* src_vec) {
    int64_t n_src = src_vec->len;
    int64_t* src_data = (int64_t*)ray_data(src_vec);

    /* Compute degrees for each source node */
    ray_t* out_src = ray_vec_new(RAY_I64, n_src > 0 ? n_src : 1);
    ray_t* out_cnt = ray_vec_new(RAY_I64, n_src > 0 ? n_src : 1);
    if (!out_src || RAY_IS_ERR(out_src) || !out_cnt || RAY_IS_ERR(out_cnt)) {
        if (out_src && !RAY_IS_ERR(out_src)) ray_release(out_src);
        if (out_cnt && !RAY_IS_ERR(out_cnt)) ray_release(out_cnt);
        return ray_error("oom", NULL);
    }

    int64_t* sd = (int64_t*)ray_data(out_src);
    int64_t* cd = (int64_t*)ray_data(out_cnt);
    int64_t out_len = 0;

    for (int64_t i = 0; i < n_src; i++) {
        int64_t node = src_data[i];
        int64_t deg = 0;
        if (direction == 0 || direction == 2) {
            if (node >= 0 && node < rel->fwd.n_nodes)
                deg += ray_csr_degree(&rel->fwd, node);
        }
        if (direction == 1 || direction == 2) {
            if (node >= 0 && node < rel->rev.n_nodes)
                deg += ray_csr_degree(&rel->rev, node);
        }
        if (deg > 0) {
            sd[out_len] = node;
            cd[out_len] = deg;
            out_len++;
        }
    }
    out_src->len = out_len;
    out_cnt->len = out_len;

    int64_t src_sym = ray_sym_intern("_src", 4);
    int64_t cnt_sym = ray_sym_intern("_count", 6);
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(out_src); ray_release(out_cnt);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, src_sym, out_src);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(out_src); ray_release(out_cnt); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, cnt_sym, out_cnt);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(out_src); ray_release(out_cnt); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    ray_release(out_src); ray_release(out_cnt);
    return result;
}

/* exec_expand: 1-hop CSR neighbor expansion.
 * Count-then-fill pattern (same as exec_join). */
static ray_t* exec_expand(ray_graph_t* g, ray_op_t* op, ray_t* src_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    /* Factorized mode: emit pre-aggregated degree counts */
    if (ext->graph.factorized)
        return exec_expand_factorized(rel, ext->graph.direction, src_vec);

    uint8_t direction = ext->graph.direction;
    int64_t n_src = src_vec->len;
    int64_t* src_data = (int64_t*)ray_data(src_vec);

    /* SIP runtime: check for source-side selection bitmap stored on the
     * expand ext node (set by optimizer sip_pass or manually for testing).
     *
     * If sip_sel is not pre-built but the optimizer left a filter hint in
     * pad[2..3], build a source-side bitmap by marking all source nodes
     * that have degree > 0 in the active CSR direction. */
    uint64_t* src_sel_bits = NULL;
    int64_t src_sel_len = 0;
    ray_t* sip_sel = (ray_t*)ext->graph.sip_sel;
    if (!sip_sel) {
        uint8_t filter_hint = ext->base.pad[2];
        if (filter_hint > 0 && n_src > 64) {
            /* Build SIP bitmap: mark source nodes with degree > 0.
             * For direction==2 (both), check both fwd and rev CSRs. */
            int64_t nn = rel->fwd.n_nodes;
            if (rel->rev.n_nodes > nn) nn = rel->rev.n_nodes;
            ray_t* built_sel = ray_sel_new(nn);
            if (built_sel && !RAY_IS_ERR(built_sel)) {
                uint64_t* bits = ray_sel_bits(built_sel);
                if (direction == 0 || direction == 2) {
                    for (int64_t nd = 0; nd < rel->fwd.n_nodes; nd++)
                        if (ray_csr_degree(&rel->fwd, nd) > 0)
                            RAY_SEL_BIT_SET(bits, nd);
                }
                if (direction == 1 || direction == 2) {
                    for (int64_t nd = 0; nd < rel->rev.n_nodes; nd++)
                        if (ray_csr_degree(&rel->rev, nd) > 0)
                            RAY_SEL_BIT_SET(bits, nd);
                }
                ext->graph.sip_sel = built_sel;
                sip_sel = built_sel;
            }
        }
    }
    if (sip_sel && !RAY_IS_ERR(sip_sel) && sip_sel->type == RAY_SEL) {
        src_sel_bits = ray_sel_bits(sip_sel);
        src_sel_len = sip_sel->len;
    }

    /* Helper to expand one CSR direction */
    #define EXPAND_DIR(csr_ptr) do { \
        ray_csr_t* csr = (csr_ptr); \
        /* Phase 1: count total output pairs */ \
        int64_t total = 0; \
        for (int64_t i = 0; i < n_src; i++) { \
            int64_t node = src_data[i]; \
            /* SIP skip: if source node not in selection, skip */ \
            if (src_sel_bits && node >= 0 && node < src_sel_len \
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue; \
            if (node >= 0 && node < csr->n_nodes) \
                total += ray_csr_degree(csr, node); \
        } \
        /* Phase 2: fill */ \
        ray_t* d_src = ray_vec_new(RAY_I64, total > 0 ? total : 1); \
        ray_t* d_dst = ray_vec_new(RAY_I64, total > 0 ? total : 1); \
        if (!d_src || RAY_IS_ERR(d_src) || !d_dst || RAY_IS_ERR(d_dst)) { \
            if (d_src && !RAY_IS_ERR(d_src)) ray_release(d_src); \
            if (d_dst && !RAY_IS_ERR(d_dst)) ray_release(d_dst); \
            return ray_error("oom", NULL); \
        } \
        d_src->len = total; d_dst->len = total; \
        int64_t* sd = (int64_t*)ray_data(d_src); \
        int64_t* dd = (int64_t*)ray_data(d_dst); \
        int64_t pos = 0; \
        for (int64_t i = 0; i < n_src; i++) { \
            int64_t node = src_data[i]; \
            if (node < 0 || node >= csr->n_nodes) continue; \
            /* SIP skip: must match count phase */ \
            if (src_sel_bits && node < src_sel_len \
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue; \
            int64_t cnt; \
            int64_t* nbrs = ray_csr_neighbors(csr, node, &cnt); \
            for (int64_t j = 0; j < cnt; j++) { \
                sd[pos] = node; \
                dd[pos] = nbrs[j]; \
                pos++; \
            } \
        } \
        /* Build result table */ \
        int64_t src_sym = ray_sym_intern("_src", 4); \
        int64_t dst_sym = ray_sym_intern("_dst", 4); \
        ray_t* result = ray_table_new(2); \
        if (!result || RAY_IS_ERR(result)) { \
            ray_release(d_src); ray_release(d_dst); \
            return ray_error("oom", NULL); \
        } \
        ray_t* _tmp = ray_table_add_col(result, src_sym, d_src); \
        if (!_tmp || RAY_IS_ERR(_tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); } \
        result = _tmp; \
        _tmp = ray_table_add_col(result, dst_sym, d_dst); \
        if (!_tmp || RAY_IS_ERR(_tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); } \
        result = _tmp; \
        ray_release(d_src); ray_release(d_dst); \
        return result; \
    } while (0)

    if (direction == 0) {
        EXPAND_DIR(&rel->fwd);
    } else if (direction == 1) {
        EXPAND_DIR(&rel->rev);
    } else {
        /* direction == 2: both — expand fwd, then rev, concat */
        ray_csr_t* fwd = &rel->fwd;
        ray_csr_t* rev = &rel->rev;

        /* Count forward */
        int64_t fwd_total = 0;
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (src_sel_bits && node >= 0 && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            if (node >= 0 && node < fwd->n_nodes)
                fwd_total += ray_csr_degree(fwd, node);
        }
        /* Count reverse */
        int64_t rev_total = 0;
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (src_sel_bits && node >= 0 && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            if (node >= 0 && node < rev->n_nodes)
                rev_total += ray_csr_degree(rev, node);
        }

        int64_t total = fwd_total + rev_total;
        ray_t* d_src = ray_vec_new(RAY_I64, total > 0 ? total : 1);
        ray_t* d_dst = ray_vec_new(RAY_I64, total > 0 ? total : 1);
        if (!d_src || RAY_IS_ERR(d_src) || !d_dst || RAY_IS_ERR(d_dst)) {
            if (d_src && !RAY_IS_ERR(d_src)) ray_release(d_src);
            if (d_dst && !RAY_IS_ERR(d_dst)) ray_release(d_dst);
            return ray_error("oom", NULL);
        }
        d_src->len = total; d_dst->len = total;
        int64_t* sd = (int64_t*)ray_data(d_src);
        int64_t* dd = (int64_t*)ray_data(d_dst);
        int64_t pos = 0;

        /* Fill forward */
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (node < 0 || node >= fwd->n_nodes) continue;
            if (src_sel_bits && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            int64_t cnt;
            int64_t* nbrs = ray_csr_neighbors(fwd, node, &cnt);
            for (int64_t j = 0; j < cnt; j++) {
                sd[pos] = node; dd[pos] = nbrs[j]; pos++;
            }
        }
        /* Fill reverse */
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (node < 0 || node >= rev->n_nodes) continue;
            if (src_sel_bits && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            int64_t cnt;
            int64_t* nbrs = ray_csr_neighbors(rev, node, &cnt);
            for (int64_t j = 0; j < cnt; j++) {
                sd[pos] = node; dd[pos] = nbrs[j]; pos++;
            }
        }

        int64_t src_sym = ray_sym_intern("_src", 4);
        int64_t dst_sym = ray_sym_intern("_dst", 4);
        ray_t* result = ray_table_new(2);
        if (!result || RAY_IS_ERR(result)) {
            ray_release(d_src); ray_release(d_dst);
            return ray_error("oom", NULL);
        }
        ray_t* tmp = ray_table_add_col(result, src_sym, d_src);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        tmp = ray_table_add_col(result, dst_sym, d_dst);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        ray_release(d_src); ray_release(d_dst);
        return result;
    }
    #undef EXPAND_DIR
}

/* exec_var_expand: iterative BFS with depth limit and cycle detection */
static ray_t* exec_var_expand(ray_graph_t* g, ray_op_t* op, ray_t* start_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    uint8_t direction = ext->graph.direction;
    uint8_t min_depth = ext->graph.min_depth;
    uint8_t max_depth = ext->graph.max_depth;
    ray_csr_t* csr_fwd = &rel->fwd;
    ray_csr_t* csr_rev = &rel->rev;
    /* For direction==2 (both), use fwd for n_nodes bound but expand both */
    ray_csr_t* csr = (direction == 1) ? csr_rev : csr_fwd;

    int64_t n_start = start_vec->len;
    int64_t* start_data = (int64_t*)ray_data(start_vec);

    /* Pre-allocate output buffers (grow as needed) */
    int64_t out_cap = 1024;
    ray_t *start_hdr, *end_hdr, *depth_hdr;
    int64_t* out_start = (int64_t*)scratch_alloc(&start_hdr, (size_t)out_cap * sizeof(int64_t));
    int64_t* out_end   = (int64_t*)scratch_alloc(&end_hdr,   (size_t)out_cap * sizeof(int64_t));
    int64_t* out_depth = (int64_t*)scratch_alloc(&depth_hdr, (size_t)out_cap * sizeof(int64_t));
    if (!out_start || !out_end || !out_depth) {
        scratch_free(start_hdr); scratch_free(end_hdr); scratch_free(depth_hdr);
        return ray_error("oom", NULL);
    }
    int64_t out_count = 0;

    /* For direction==2, use the larger n_nodes bound */
    int64_t bfs_n_nodes = csr->n_nodes;
    if (direction == 2 && csr_rev->n_nodes > bfs_n_nodes)
        bfs_n_nodes = csr_rev->n_nodes;

    /* BFS per start node */
    for (int64_t s = 0; s < n_start; s++) {
        int64_t start_node = start_data[s];
        if (start_node < 0 || start_node >= bfs_n_nodes) continue;

        /* Visited bitmap via RAY_SEL */
        ray_t* visited_sel = ray_sel_new(bfs_n_nodes);
        if (!visited_sel || RAY_IS_ERR(visited_sel)) continue;
        uint64_t* visited = ray_sel_bits(visited_sel);
        RAY_SEL_BIT_SET(visited, start_node);

        /* Frontier */
        ray_t* front_hdr;
        int64_t front_cap = 256;
        int64_t* frontier = (int64_t*)scratch_alloc(&front_hdr, (size_t)front_cap * sizeof(int64_t));
        if (!frontier) { ray_release(visited_sel); continue; }
        frontier[0] = start_node;
        int64_t front_len = 1;

        for (uint8_t depth = 1; depth <= max_depth && front_len > 0; depth++) {
            ray_t* next_hdr;
            int64_t next_cap = (front_len > INT64_MAX / 4) ? INT64_MAX : front_len * 4;
            if (next_cap < 64) next_cap = 64;
            int64_t* next_front = (int64_t*)scratch_alloc(&next_hdr, (size_t)next_cap * sizeof(int64_t));
            if (!next_front) { scratch_free(front_hdr); ray_release(visited_sel); goto cleanup; }
            int64_t next_len = 0;

            for (int64_t f = 0; f < front_len; f++) {
                int64_t node = frontier[f];
                /* Expand neighbors from active CSR(s).
                 * For direction==2 (both), expand fwd then rev. */
                int n_csrs = (direction == 2) ? 2 : 1;
                ray_csr_t* csrs[2] = { csr, csr_rev };
                for (int ci = 0; ci < n_csrs; ci++) {
                    ray_csr_t* cur_csr = csrs[ci];
                    if (node < 0 || node >= cur_csr->n_nodes) continue;
                int64_t cnt;
                int64_t* nbrs = ray_csr_neighbors(cur_csr, node, &cnt);
                for (int64_t j = 0; j < cnt; j++) {
                    int64_t nbr = nbrs[j];
                    if (nbr < 0 || nbr >= bfs_n_nodes) continue;
                    if (RAY_SEL_BIT_TEST(visited, nbr)) continue;
                    RAY_SEL_BIT_SET(visited, nbr);

                    /* Grow next_front if needed */
                    if (next_len >= next_cap) {
                        if (next_cap > INT64_MAX / 2) break;
                        int64_t new_cap = next_cap * 2;
                        int64_t* new_nf = (int64_t*)scratch_realloc(&next_hdr,
                            (size_t)next_cap * sizeof(int64_t),
                            (size_t)new_cap * sizeof(int64_t));
                        if (!new_nf) break;
                        next_front = new_nf;
                        next_cap = new_cap;
                    }
                    next_front[next_len++] = nbr;

                    /* Emit if within depth range */
                    if (depth >= min_depth) {
                        if (out_count >= out_cap) {
                            if (out_cap > INT64_MAX / 2) break;
                            int64_t new_oc = out_cap * 2;
                            /* Grow all three buffers atomically — alloc new
                             * copies first, commit only if all succeed. */
                            ray_t *ns_h = NULL, *ne_h = NULL, *nd_h = NULL;
                            size_t old_sz = (size_t)out_cap * sizeof(int64_t);
                            size_t new_sz = (size_t)new_oc * sizeof(int64_t);
                            int64_t* ns = (int64_t*)scratch_alloc(&ns_h, new_sz);
                            int64_t* ne = (int64_t*)scratch_alloc(&ne_h, new_sz);
                            int64_t* nd_buf = (int64_t*)scratch_alloc(&nd_h, new_sz);
                            if (!ns || !ne || !nd_buf) {
                                scratch_free(ns_h); scratch_free(ne_h); scratch_free(nd_h);
                                break;
                            }
                            memcpy(ns, out_start, old_sz);
                            memcpy(ne, out_end, old_sz);
                            memcpy(nd_buf, out_depth, old_sz);
                            scratch_free(start_hdr); scratch_free(end_hdr); scratch_free(depth_hdr);
                            start_hdr = ns_h; end_hdr = ne_h; depth_hdr = nd_h;
                            out_start = ns; out_end = ne; out_depth = nd_buf;
                            out_cap = new_oc;
                        }
                        out_start[out_count] = start_node;
                        out_end[out_count] = nbr;
                        out_depth[out_count] = depth;
                        out_count++;
                    }
                }
                } /* end for ci (CSR directions) */
            }

            scratch_free(front_hdr);
            front_hdr = next_hdr;
            frontier = next_front;
            front_len = next_len;
        }

        scratch_free(front_hdr);
        ray_release(visited_sel);
    }

cleanup:;
    /* Build output table */
    ray_t* v_start = ray_vec_from_raw(RAY_I64, out_start, out_count);
    ray_t* v_end   = ray_vec_from_raw(RAY_I64, out_end,   out_count);
    ray_t* v_depth = ray_vec_from_raw(RAY_I64, out_depth, out_count);
    scratch_free(start_hdr); scratch_free(end_hdr); scratch_free(depth_hdr);

    if (!v_start || RAY_IS_ERR(v_start) || !v_end || RAY_IS_ERR(v_end) ||
        !v_depth || RAY_IS_ERR(v_depth)) {
        if (v_start && !RAY_IS_ERR(v_start)) ray_release(v_start);
        if (v_end && !RAY_IS_ERR(v_end)) ray_release(v_end);
        if (v_depth && !RAY_IS_ERR(v_depth)) ray_release(v_depth);
        return ray_error("oom", NULL);
    }

    int64_t start_sym = ray_sym_intern("_start", 6);
    int64_t end_sym   = ray_sym_intern("_end", 4);
    int64_t depth_sym = ray_sym_intern("_depth", 6);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(v_start); ray_release(v_end); ray_release(v_depth);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, start_sym, v_start);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_start); ray_release(v_end); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, end_sym, v_end);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_start); ray_release(v_end); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, depth_sym, v_depth);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_start); ray_release(v_end); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    ray_release(v_start); ray_release(v_end); ray_release(v_depth);
    return result;
}

/* exec_shortest_path: BFS from src to dst with parent tracking */
static ray_t* exec_shortest_path(ray_graph_t* g, ray_op_t* op,
                                 ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);
    uint8_t direction = ext->graph.direction;
    ray_csr_t* csr = (direction == 1) ? &rel->rev : &rel->fwd;
    ray_csr_t* csr_rev = &rel->rev;
    int n_csrs = (direction == 2) ? 2 : 1;
    ray_csr_t* csrs[2] = { csr, csr_rev };
    int64_t bfs_n_nodes = csr->n_nodes;
    if (direction == 2 && csr_rev->n_nodes > bfs_n_nodes)
        bfs_n_nodes = csr_rev->n_nodes;
    uint8_t max_depth = ext->graph.max_depth;

    /* Extract single I64 values */
    int64_t src_node, dst_node;
    if (ray_is_atom(src_val)) {
        src_node = src_val->i64;
    } else {
        if (src_val->len == 0) return ray_error("range", NULL);
        src_node = ((int64_t*)ray_data(src_val))[0];
    }
    if (ray_is_atom(dst_val)) {
        dst_node = dst_val->i64;
    } else {
        if (dst_val->len == 0) return ray_error("range", NULL);
        dst_node = ((int64_t*)ray_data(dst_val))[0];
    }

    if (src_node < 0 || src_node >= bfs_n_nodes ||
        dst_node < 0 || dst_node >= bfs_n_nodes)
        return ray_error("range", NULL);

    /* Special case: src == dst */
    if (src_node == dst_node) {
        ray_t* v_node = ray_vec_from_raw(RAY_I64, &src_node, 1);
        int64_t zero = 0;
        ray_t* v_depth = ray_vec_from_raw(RAY_I64, &zero, 1);
        if (!v_node || RAY_IS_ERR(v_node) || !v_depth || RAY_IS_ERR(v_depth)) {
            if (v_node && !RAY_IS_ERR(v_node)) ray_release(v_node);
            if (v_depth && !RAY_IS_ERR(v_depth)) ray_release(v_depth);
            return ray_error("oom", NULL);
        }
        ray_t* result = ray_table_new(2);
        if (!result || RAY_IS_ERR(result)) { ray_release(v_node); ray_release(v_depth); return ray_error("oom", NULL); }
        ray_t* tmp = ray_table_add_col(result, sym_intern_safe("_node", 5), v_node);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        tmp = ray_table_add_col(result, sym_intern_safe("_depth", 6), v_depth);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        ray_release(v_node); ray_release(v_depth);
        return result;
    }

    /* Allocate parent array (-1 = unvisited) */
    ray_t* parent_hdr;
    int64_t* parent = (int64_t*)scratch_alloc(&parent_hdr,
                                               (size_t)bfs_n_nodes * sizeof(int64_t));
    if (!parent) return ray_error("oom", NULL);
    memset(parent, 0xFF, (size_t)bfs_n_nodes * sizeof(int64_t)); /* -1 */
    parent[src_node] = src_node;

    /* BFS queue */
    ray_t* queue_hdr;
    int64_t q_cap = 1024;
    int64_t* queue = (int64_t*)scratch_alloc(&queue_hdr, (size_t)q_cap * sizeof(int64_t));
    if (!queue) { scratch_free(parent_hdr); return ray_error("oom", NULL); }
    queue[0] = src_node;
    int64_t q_start = 0, q_end = 1;
    bool found = false;

    for (uint8_t depth = 1; depth <= max_depth && !found; depth++) {
        int64_t level_end = q_end;
        for (int64_t qi = q_start; qi < level_end && !found; qi++) {
            int64_t node = queue[qi];
            for (int ci = 0; ci < n_csrs && !found; ci++) {
                ray_csr_t* cur_csr = csrs[ci];
                if (node < 0 || node >= cur_csr->n_nodes) continue;
                int64_t cnt;
                int64_t* nbrs = ray_csr_neighbors(cur_csr, node, &cnt);
                for (int64_t j = 0; j < cnt; j++) {
                    int64_t nbr = nbrs[j];
                    if (nbr < 0 || nbr >= bfs_n_nodes) continue;
                    if (parent[nbr] != -1) continue;
                    parent[nbr] = node;

                    if (nbr == dst_node) { found = true; break; }

                    /* Grow queue if needed */
                    if (q_end >= q_cap) {
                        if (q_cap > INT64_MAX / 2) { found = false; goto bfs_done; }
                        int64_t new_cap = q_cap * 2;
                        int64_t* new_q = (int64_t*)scratch_realloc(&queue_hdr,
                            (size_t)q_cap * sizeof(int64_t),
                            (size_t)new_cap * sizeof(int64_t));
                        if (!new_q) { found = false; goto bfs_done; }
                        queue = new_q;
                        q_cap = new_cap;
                    }
                    queue[q_end++] = nbr;
                }
            } /* end for ci (CSR directions) */
        }
        q_start = level_end;
    }

bfs_done:
    scratch_free(queue_hdr);

    if (!found) {
        scratch_free(parent_hdr);
        return ray_error("range", NULL);
    }

    /* Reconstruct path */
    int64_t path_buf[256];
    int64_t path_len = 0;
    int64_t cur = dst_node;
    while (cur != src_node && path_len < 255) {
        path_buf[path_len++] = cur;
        cur = parent[cur];
    }
    if (path_len < 256)
        path_buf[path_len++] = src_node;
    scratch_free(parent_hdr);

    /* Reverse path */
    for (int64_t i = 0; i < path_len / 2; i++) {
        int64_t tmp = path_buf[i];
        path_buf[i] = path_buf[path_len - 1 - i];
        path_buf[path_len - 1 - i] = tmp;
    }

    /* Build output table */
    ray_t* v_node = ray_vec_from_raw(RAY_I64, path_buf, path_len);
    ray_t* v_depth = ray_vec_new(RAY_I64, path_len);
    if (!v_node || RAY_IS_ERR(v_node) || !v_depth || RAY_IS_ERR(v_depth)) {
        if (v_node && !RAY_IS_ERR(v_node)) ray_release(v_node);
        if (v_depth && !RAY_IS_ERR(v_depth)) ray_release(v_depth);
        return ray_error("oom", NULL);
    }
    v_depth->len = path_len;
    int64_t* dep_data = (int64_t*)ray_data(v_depth);
    for (int64_t i = 0; i < path_len; i++) dep_data[i] = i;

    int64_t node_sym  = ray_sym_intern("_node", 5);
    int64_t depth_sym = ray_sym_intern("_depth", 6);
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) { ray_release(v_node); ray_release(v_depth); return ray_error("oom", NULL); }
    ray_t* tmp = ray_table_add_col(result, node_sym, v_node);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, depth_sym, v_depth);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    ray_release(v_node); ray_release(v_depth);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_pagerank: iterative PageRank over CSR adjacency.
 *
 * rank[v] = (1 - d)/N + d * SUM(rank[u] / out_degree[u]) for u in in_neighbors(v)
 *
 * Uses reverse CSR for in-neighbors, forward CSR for out-degree.
 * -------------------------------------------------------------------------- */
static ray_t* exec_pagerank(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n       = rel->fwd.n_nodes;
    uint16_t iters  = ext->graph.max_iter;
    double damping  = ext->graph.damping;

    if (n <= 0) return ray_error("length", NULL);

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double* rank     = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    double* rank_new = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    if (!rank || !rank_new) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    double init = 1.0 / (double)n;
    for (int64_t i = 0; i < n; i++) rank[i] = init;

    /* Get raw CSR arrays for direct access */
    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    double base = (1.0 - damping) / (double)n;

    for (uint16_t iter = 0; iter < iters; iter++) {
        /* Dangling node correction: redistribute rank of zero-out-degree nodes */
        double dangling_sum = 0.0;
        for (int64_t u = 0; u < n; u++) {
            if (fwd_off[u + 1] == fwd_off[u]) dangling_sum += rank[u];
        }
        double adjusted_base = base + damping * dangling_sum / (double)n;

        for (int64_t v = 0; v < n; v++) {
            double sum = 0.0;
            /* Iterate over in-neighbors of v using reverse CSR */
            int64_t rev_start = rev_off[v];
            int64_t rev_end   = rev_off[v + 1];
            for (int64_t j = rev_start; j < rev_end; j++) {
                int64_t u = rev_tgt[j];
                /* out_degree of u from forward CSR */
                int64_t out_deg = fwd_off[u + 1] - fwd_off[u];
                if (out_deg > 0) {
                    sum += rank[u] / (double)out_deg;
                }
            }
            rank_new[v] = adjusted_base + damping * sum;
        }
        /* Swap */
        double* tmp = rank;
        rank = rank_new;
        rank_new = tmp;
    }

    /* Build output table: _node (I64), _rank (F64) */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* rank_vec = ray_vec_new(RAY_F64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !rank_vec || RAY_IS_ERR(rank_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (rank_vec && !RAY_IS_ERR(rank_vec)) ray_release(rank_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  rdata = (double*)ray_data(rank_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        rdata[i] = rank[i];
    }
    node_vec->len = n;
    rank_vec->len = n;

    ray_scratch_arena_reset(&arena);

    /* Package as table with named columns */
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(rank_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_rank", 5), rank_vec);
    ray_release(rank_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_connected_comp: connected components via label propagation.
 * Treats graph as undirected (uses both forward and reverse CSR).
 * O(diameter * |E|) time.
 * -------------------------------------------------------------------------- */
static ray_t* exec_connected_comp(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    int64_t* label = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!label) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    /* Initialize: each node is its own component */
    for (int64_t i = 0; i < n; i++) label[i] = i;

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    /* Iterate until convergence */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int64_t v = 0; v < n; v++) {
            int64_t min_label = label[v];
            /* Forward neighbors */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t u = fwd_tgt[j];
                if (label[u] < min_label) min_label = label[u];
            }
            /* Reverse neighbors */
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t u = rev_tgt[j];
                if (label[u] < min_label) min_label = label[u];
            }
            if (min_label < label[v]) {
                label[v] = min_label;
                changed = true;
            }
        }
    }

    /* Build output table */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* comp_vec = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !comp_vec || RAY_IS_ERR(comp_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (comp_vec && !RAY_IS_ERR(comp_vec)) ray_release(comp_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    int64_t* cdata = (int64_t*)ray_data(comp_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        cdata[i] = label[i];
    }
    node_vec->len = n;
    comp_vec->len = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(comp_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_component", 10), comp_vec);
    ray_release(comp_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_dijkstra: weighted shortest path via Dijkstra's algorithm.
 * Uses a binary min-heap. Reads edge weights from CSR property table.
 * Returns table with _node (I64), _dist (F64), _depth (I64).
 * -------------------------------------------------------------------------- */

/* Min-heap entry for Dijkstra */
typedef struct {
    double   dist;
    int64_t  node;
} dijk_entry_t;

static void dijk_heap_push(dijk_entry_t* heap, int64_t* size,
                            double dist, int64_t node) {
    int64_t i = (*size)++;
    heap[i].dist = dist;
    heap[i].node = node;
    /* Sift up */
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap[parent].dist <= heap[i].dist) break;
        dijk_entry_t tmp = heap[parent];
        heap[parent] = heap[i];
        heap[i] = tmp;
        i = parent;
    }
}

static dijk_entry_t dijk_heap_pop(dijk_entry_t* heap, int64_t* size) {
    dijk_entry_t top = heap[0];
    (*size)--;
    if (*size > 0) {
        heap[0] = heap[*size];
        /* Sift down */
        int64_t i = 0;
        while (1) {
            int64_t left  = 2 * i + 1;
            int64_t right = 2 * i + 2;
            int64_t smallest = i;
            if (left  < *size && heap[left].dist  < heap[smallest].dist) smallest = left;
            if (right < *size && heap[right].dist < heap[smallest].dist) smallest = right;
            if (smallest == i) break;
            dijk_entry_t tmp = heap[i];
            heap[i] = heap[smallest];
            heap[smallest] = tmp;
            i = smallest;
        }
    }
    return top;
}

/* Reusable Dijkstra with optional node/edge masks (for Yen's k-shortest) */
static double dijkstra_masked(
    int64_t* fwd_off, int64_t* fwd_tgt, int64_t* fwd_row,
    double* weights, int64_t n,
    int64_t src_id, int64_t dst_id,
    bool* node_mask,    /* NULL or bool[n]: true = blocked */
    bool* edge_mask,    /* NULL or bool[m]: true = blocked */
    double* dist,       /* pre-allocated double[n] */
    int64_t* parent,    /* pre-allocated int64_t[n] */
    dijk_entry_t* heap, /* pre-allocated */
    bool* visited)      /* pre-allocated bool[n] */
{
    for (int64_t i = 0; i < n; i++) {
        dist[i] = 1e308;
        parent[i] = -1;
        visited[i] = false;
    }

    dist[src_id] = 0.0;
    int64_t heap_size = 0;
    dijk_heap_push(heap, &heap_size, 0.0, src_id);

    while (heap_size > 0) {
        dijk_entry_t top = dijk_heap_pop(heap, &heap_size);
        int64_t u = top.node;
        if (visited[u]) continue;
        visited[u] = true;

        if (u == dst_id) break;

        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            if (edge_mask && edge_mask[j]) continue;
            int64_t v = fwd_tgt[j];
            if (node_mask && node_mask[v]) continue;
            int64_t edge_row = fwd_row[j];
            double w = weights[edge_row];
            double new_dist = dist[u] + w;
            if (new_dist < dist[v]) {
                dist[v] = new_dist;
                parent[v] = u;
                dijk_heap_push(heap, &heap_size, new_dist, v);
            }
        }
    }

    return dist[dst_id];
}

static ray_t* exec_dijkstra(ray_graph_t* g, ray_op_t* op,
                             ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);
    if (!rel->fwd.props) return ray_error("schema", NULL); /* need edge properties */

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    int64_t src_id = ray_is_atom(src_val) ? src_val->i64 : ((int64_t*)ray_data(src_val))[0];
    int64_t dst_id = !dst_val ? -1 : ray_is_atom(dst_val) ? dst_val->i64 : ((int64_t*)ray_data(dst_val))[0];

    if (src_id < 0 || src_id >= n) return ray_error("range", NULL);
    if (dst_id != -1 && (dst_id < 0 || dst_id >= n)) return ray_error("range", NULL);

    /* Find weight column in edge properties */
    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* props = rel->fwd.props;
    ray_t* weight_vec = ray_table_get_col(props, weight_sym);
    if (!weight_vec || RAY_IS_ERR(weight_vec)) return ray_error("schema", NULL);
    if (weight_vec->type != RAY_F64) return ray_error("schema", NULL);
    double* weights = (double*)ray_data(weight_vec);

    /* Allocate working arrays.
     * Heap capacity = max(n, m) + 1: each edge relaxation can push one entry,
     * and with lazy deletion (visited check on pop) the heap can grow up to m. */
    int64_t heap_cap = (m > n ? m : n) + 1;

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  dist    = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    bool*    visited = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    int64_t* depth   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    dijk_entry_t* heap = (dijk_entry_t*)ray_scratch_arena_push(&arena,
                              (size_t)heap_cap * sizeof(dijk_entry_t));
    if (!dist || !visited || !depth || !heap) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(visited, 0, (size_t)n * sizeof(bool));
    memset(depth, 0, (size_t)n * sizeof(int64_t));

    for (int64_t i = 0; i < n; i++) {
        dist[i] = 1e308;  /* infinity */
    }
    dist[src_id] = 0.0;

    int64_t heap_size = 0;
    dijk_heap_push(heap, &heap_size, 0.0, src_id);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    while (heap_size > 0) {
        dijk_entry_t top = dijk_heap_pop(heap, &heap_size);
        int64_t u = top.node;
        if (visited[u]) continue;
        visited[u] = true;

        if (u == dst_id) break;  /* early exit if destination reached */

        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            int64_t v = fwd_tgt[j];
            int64_t edge_row = fwd_row[j];
            double w = weights[edge_row];
            double new_dist = dist[u] + w;
            if (new_dist < dist[v]) {
                dist[v] = new_dist;
                depth[v] = depth[u] + 1;
                dijk_heap_push(heap, &heap_size, new_dist, v);
            }
        }
    }

    /* Collect reachable nodes */
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist[i] < 1e308) count++;
    }

    ray_t* node_vec  = ray_vec_new(RAY_I64, count);
    ray_t* dist_vec  = ray_vec_new(RAY_F64, count);
    ray_t* depth_vec = ray_vec_new(RAY_I64, count);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !dist_vec || RAY_IS_ERR(dist_vec) ||
        !depth_vec || RAY_IS_ERR(depth_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec)) ray_release(dist_vec);
        if (depth_vec && !RAY_IS_ERR(depth_vec)) ray_release(depth_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  ddata = (double*)ray_data(dist_vec);
    int64_t* hdata = (int64_t*)ray_data(depth_vec);
    int64_t idx = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist[i] < 1e308) {
            ndata[idx] = i;
            ddata[idx] = dist[i];
            hdata[idx] = depth[i];
            idx++;
        }
    }
    node_vec->len = count;
    dist_vec->len = count;
    depth_vec->len = count;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(dist_vec);
        ray_release(depth_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    result = ray_table_add_col(result, sym_intern_safe("_depth", 6), depth_vec);
    ray_release(depth_vec);

    return result;
}

/* exec_wco_join: Worst-Case Optimal Join via general Leapfrog Triejoin */
static ray_t* exec_wco_join(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t** rels = (ray_rel_t**)ext->wco.rels;
    uint8_t n_rels = ext->wco.n_rels;
    uint8_t n_vars = ext->wco.n_vars;

    if (!rels || n_rels == 0) return ray_error("schema", NULL);
    if (n_vars > LFTJ_MAX_VARS) return ray_error("nyi", NULL);

    /* Validate sorted CSR (both fwd and rev, since LFTJ may use either) */
    for (uint8_t r = 0; r < n_rels; r++) {
        if (!rels[r] || !rels[r]->fwd.sorted || !rels[r]->rev.sorted)
            return ray_error("domain", NULL);
    }

    /* Build binding plan */
    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!lftj_build_default_plan(&ctx, rels, n_rels, n_vars))
        return ray_error("nyi", NULL);

    /* Allocate output buffers */
    int64_t out_cap = 4096;
    ray_t* col_data_block;
    int64_t** col_data = (int64_t**)scratch_alloc(&col_data_block,
                              (size_t)n_vars * sizeof(int64_t*));
    if (!col_data) {
        scratch_free(col_data_block);
        return ray_error("oom", NULL);
    }

    for (uint8_t v = 0; v < n_vars; v++) {
        ray_t* h = ray_alloc((size_t)out_cap * sizeof(int64_t));
        if (!h) {
            for (uint8_t j = 0; j < v; j++) ray_free(ctx.buf_hdrs[j]);
            scratch_free(col_data_block);
            return ray_error("oom", NULL);
        }
        ctx.buf_hdrs[v] = h;
        col_data[v] = (int64_t*)ray_data(h);
    }

    ctx.col_data = col_data;
    ctx.out_count = 0;
    ctx.out_cap = out_cap;
    ctx.oom = false;

    /* Run general LFTJ enumeration */
    lftj_enumerate(&ctx, 0);

    if (ctx.oom) {
        for (uint8_t v = 0; v < n_vars; v++) ray_free(ctx.buf_hdrs[v]);
        scratch_free(col_data_block);
        return ray_error("oom", NULL);
    }

    /* Build output table */
    ray_t* result = ray_table_new(n_vars);
    if (!result || RAY_IS_ERR(result)) {
        for (uint8_t v = 0; v < n_vars; v++) ray_free(ctx.buf_hdrs[v]);
        scratch_free(col_data_block);
        return ray_error("oom", NULL);
    }

    for (uint8_t v = 0; v < n_vars; v++) {
        ray_t* vec = ray_vec_from_raw(RAY_I64, ctx.col_data[v], ctx.out_count);
        ray_free(ctx.buf_hdrs[v]);
        if (!vec || RAY_IS_ERR(vec)) {
            for (uint8_t j = v + 1; j < n_vars; j++) ray_free(ctx.buf_hdrs[j]);
            scratch_free(col_data_block);
            ray_release(result);
            return ray_error("oom", NULL);
        }
        char name_buf[12];
        int n = snprintf(name_buf, sizeof(name_buf), "_v%d", v);
        int64_t name_id = ray_sym_intern(name_buf, (size_t)n);
        ray_t* new_result = ray_table_add_col(result, name_id, vec);
        ray_release(vec);
        if (!new_result || RAY_IS_ERR(new_result)) {
            for (uint8_t j = v + 1; j < n_vars; j++) ray_free(ctx.buf_hdrs[j]);
            scratch_free(col_data_block);
            ray_release(result);
            return ray_error("oom", NULL);
        }
        result = new_result;
    }

    scratch_free(col_data_block);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_louvain: community detection via Louvain modularity optimization.
 * Phase 1 only (no graph contraction).
 * Maximizes modularity Q = (1/2m) * SUM[(A_ij - k_i*k_j/2m) * delta(c_i, c_j)]
 * Treats graph as undirected. Uses forward+reverse CSR.
 * -------------------------------------------------------------------------- */
static ray_t* exec_louvain(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    uint16_t max_iter = ext->graph.max_iter;

    if (n <= 0) return ray_error("length", NULL);

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    int64_t* community = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* degree    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* comm_tot  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!community || !degree || !comm_tot) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    /* Initialize: each node in its own community */
    for (int64_t i = 0; i < n; i++) {
        community[i] = i;
        degree[i] = (fwd_off[i+1] - fwd_off[i]) + (rev_off[i+1] - rev_off[i]);
        comm_tot[i] = degree[i];
    }

    double two_m = (double)(2 * m);
    if (two_m == 0) two_m = 1;

    /* Scratch space for per-community edge counts (reused across iterations).
     * k_i_in[c] = number of edges from node v to community c. */
    int64_t* k_i_in = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    /* Track which communities were touched so we can reset k_i_in efficiently */
    int64_t* touched = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!k_i_in || !touched) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(k_i_in, 0, (size_t)n * sizeof(int64_t));

    for (uint16_t iter = 0; iter < max_iter; iter++) {
        bool moved = false;
        for (int64_t v = 0; v < n; v++) {
            int64_t old_comm = community[v];
            int64_t n_touched = 0;

            /* Aggregate edges per neighbor community (forward + reverse) */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t c = community[fwd_tgt[j]];
                if (c == old_comm) continue;
                if (k_i_in[c] == 0) touched[n_touched++] = c;
                k_i_in[c]++;
            }
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t c = community[rev_tgt[j]];
                if (c == old_comm) continue;
                if (k_i_in[c] == 0) touched[n_touched++] = c;
                k_i_in[c]++;
            }

            /* Evaluate modularity gain for each candidate community.
             * delta_Q = k_i_in[c] / two_m - (sigma_tot[c] * k_v) / (two_m * two_m) */
            int64_t best_comm = old_comm;
            double best_gain = 0.0;
            double k_v = (double)degree[v];

            for (int64_t t = 0; t < n_touched; t++) {
                int64_t c = touched[t];
                double sigma_tot = (double)comm_tot[c];
                double gain = (double)k_i_in[c] / two_m
                            - (sigma_tot * k_v) / (two_m * two_m);
                if (gain > best_gain) {
                    best_gain = gain;
                    best_comm = c;
                }
            }

            /* Reset k_i_in for touched communities */
            for (int64_t t = 0; t < n_touched; t++) {
                k_i_in[touched[t]] = 0;
            }

            if (best_comm != old_comm) {
                comm_tot[old_comm] -= degree[v];
                comm_tot[best_comm] += degree[v];
                community[v] = best_comm;
                moved = true;
            }
        }
        if (!moved) break;
    }

    /* Normalize community IDs to 0..k-1 */
    int64_t* remap = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!remap) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < n; i++) remap[i] = -1;
    int64_t next_id = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = community[i];
        if (remap[c] < 0) remap[c] = next_id++;
        community[i] = remap[c];
    }

    /* Build output table */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* comm_vec = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !comm_vec || RAY_IS_ERR(comm_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (comm_vec && !RAY_IS_ERR(comm_vec)) ray_release(comm_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    int64_t* cdata = (int64_t*)ray_data(comm_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        cdata[i] = community[i];
    }
    node_vec->len = n;
    comm_vec->len = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(comm_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_community", 10), comm_vec);
    ray_release(comm_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_degree_cent: in/out/total degree from CSR offsets. O(n).
 * -------------------------------------------------------------------------- */
static ray_t* exec_degree_cent(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);

    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* in_vec   = ray_vec_new(RAY_I64, n);
    ray_t* out_vec  = ray_vec_new(RAY_I64, n);
    ray_t* deg_vec  = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !in_vec   || RAY_IS_ERR(in_vec)   ||
        !out_vec  || RAY_IS_ERR(out_vec)  ||
        !deg_vec  || RAY_IS_ERR(deg_vec)) {
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (in_vec   && !RAY_IS_ERR(in_vec))   ray_release(in_vec);
        if (out_vec  && !RAY_IS_ERR(out_vec))  ray_release(out_vec);
        if (deg_vec  && !RAY_IS_ERR(deg_vec))  ray_release(deg_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata   = (int64_t*)ray_data(node_vec);
    int64_t* in_data = (int64_t*)ray_data(in_vec);
    int64_t* out_data= (int64_t*)ray_data(out_vec);
    int64_t* deg_data= (int64_t*)ray_data(deg_vec);

    for (int64_t i = 0; i < n; i++) {
        ndata[i]    = i;
        out_data[i] = fwd_off[i + 1] - fwd_off[i];
        in_data[i]  = rev_off[i + 1] - rev_off[i];
        deg_data[i] = out_data[i] + in_data[i];
    }
    node_vec->len = n;
    in_vec->len   = n;
    out_vec->len  = n;
    deg_vec->len  = n;

    ray_t* result = ray_table_new(4);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(in_vec);
        ray_release(out_vec);  ray_release(deg_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_in_degree", 10), in_vec);
    ray_release(in_vec);
    result = ray_table_add_col(result, sym_intern_safe("_out_degree", 11), out_vec);
    ray_release(out_vec);
    result = ray_table_add_col(result, sym_intern_safe("_degree", 7), deg_vec);
    ray_release(deg_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_topsort: topological sort via Kahn's algorithm. O(n+m).
 * Returns error if graph contains a cycle.
 * -------------------------------------------------------------------------- */
static ray_t* exec_topsort(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    int64_t* in_deg = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* queue  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* order  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!in_deg || !queue || !order) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    /* Compute in-degrees from reverse CSR */
    for (int64_t i = 0; i < n; i++)
        in_deg[i] = rev_off[i + 1] - rev_off[i];

    /* Enqueue zero-degree nodes */
    int64_t head = 0, tail = 0;
    for (int64_t i = 0; i < n; i++) {
        if (in_deg[i] == 0) queue[tail++] = i;
    }

    /* BFS — decrement in-degrees, enqueue new zeros */
    int64_t count = 0;
    while (head < tail) {
        int64_t v = queue[head++];
        order[v] = count++;

        int64_t start = fwd_off[v];
        int64_t end   = fwd_off[v + 1];
        for (int64_t j = start; j < end; j++) {
            int64_t u = fwd_tgt[j];
            if (--in_deg[u] == 0) queue[tail++] = u;
        }
    }

    /* Cycle detection: not all nodes processed */
    if (count < n) {
        ray_scratch_arena_reset(&arena);
        return ray_error("domain", NULL);  /* cycle detected */
    }

    /* Build result */
    ray_t* node_vec  = ray_vec_new(RAY_I64, n);
    ray_t* order_vec = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !order_vec || RAY_IS_ERR(order_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (order_vec && !RAY_IS_ERR(order_vec)) ray_release(order_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    int64_t* odata = (int64_t*)ray_data(order_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        odata[i] = order[i];
    }
    node_vec->len  = n;
    order_vec->len = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(order_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_order", 6), order_vec);
    ray_release(order_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_cluster_coeff: clustering coefficient via triangle counting. O(n*d^2).
 * For each node v, count triangles among undirected neighbors using bitset.
 * -------------------------------------------------------------------------- */
static ray_t* exec_cluster_coeff(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Scratch: merged neighbor list per node (max possible size = n) */
    int64_t* nbrs = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    /* Scratch: quick-lookup set for neighbor checking */
    uint8_t* in_nbr = (uint8_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(uint8_t));
    if (!nbrs || !in_nbr) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(in_nbr, 0, (size_t)n * sizeof(uint8_t));

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    /* Allocate result vectors */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* lcc_vec  = ray_vec_new(RAY_F64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !lcc_vec || RAY_IS_ERR(lcc_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (lcc_vec  && !RAY_IS_ERR(lcc_vec))  ray_release(lcc_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  ldata = (double*)ray_data(lcc_vec);

    for (int64_t v = 0; v < n; v++) {
        ndata[v] = v;

        /* Merge forward and reverse neighbors into deduplicated list */
        int64_t deg = 0;
        for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
            int64_t u = fwd_tgt[j];
            if (u >= 0 && u < n && !in_nbr[u]) {
                in_nbr[u] = 1;
                nbrs[deg++] = u;
            }
        }
        for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
            int64_t u = rev_tgt[j];
            if (u >= 0 && u < n && !in_nbr[u]) {
                in_nbr[u] = 1;
                nbrs[deg++] = u;
            }
        }

        if (deg < 2) {
            ldata[v] = 0.0;
        } else {
            /* Count directed fwd edges between neighbors of v */
            int64_t triangles = 0;
            for (int64_t i = 0; i < deg; i++) {
                int64_t u = nbrs[i];
                /* Check fwd edges of u against neighbor set */
                for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
                    if (in_nbr[fwd_tgt[j]]) triangles++;
                }
            }
            ldata[v] = (double)triangles / ((double)deg * (double)(deg - 1));
        }

        /* Reset in_nbr for next node */
        for (int64_t i = 0; i < deg; i++) in_nbr[nbrs[i]] = 0;
    }

    node_vec->len = n;
    lcc_vec->len  = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(lcc_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_coefficient", 12), lcc_vec);
    ray_release(lcc_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_betweenness: Brandes betweenness centrality. O(n*m) exact,
 * O(sample*m) approximate when sample_size > 0.
 * -------------------------------------------------------------------------- */
static ray_t* exec_betweenness(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);
    uint16_t sample = ext->graph.max_iter;
    int64_t n_sources = (sample > 0 && (int64_t)sample < n) ? (int64_t)sample : n;

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  cb      = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    double*  sigma   = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    double*  delta   = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    int64_t* dist    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* queue   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* stack   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    /* Predecessor storage: flat CSR-style array with per-node offsets.
     * Two-pass approach: BFS counts predecessors per node, prefix-sum builds
     * offsets, then a second pass over the stack fills pred_data in grouped order. */
    int64_t m_total = rel->fwd.n_edges + rel->rev.n_edges;
    if (m_total == 0) m_total = 1;
    int64_t* pred_data   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)m_total * sizeof(int64_t));
    int64_t* pred_off    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)(n + 1) * sizeof(int64_t));
    int64_t* pred_cursor = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    /* Per-v dedup marker: tracks which neighbors were already counted via fwd edges
     * to avoid double-counting sigma/predecessors for bidirectional edges. */
    int64_t* seen_epoch  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    if (!cb || !sigma || !delta || !dist || !queue || !stack ||
        !pred_data || !pred_off || !pred_cursor || !seen_epoch) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    memset(cb, 0, (size_t)n * sizeof(double));

    int64_t stride = (sample > 0 && (int64_t)sample < n) ? (n / n_sources) : 1;

    for (int64_t si = 0; si < n_sources; si++) {
        int64_t s = (si * stride) % n;

        /* Initialize */
        for (int64_t i = 0; i < n; i++) {
            sigma[i] = 0.0;
            delta[i] = 0.0;
            dist[i]  = -1;
        }
        sigma[s] = 1.0;
        dist[s]  = 0;
        memset(pred_off, 0, (size_t)(n + 1) * sizeof(int64_t));
        memset(seen_epoch, 0, (size_t)n * sizeof(int64_t));

        /* BFS pass 1: discover nodes, compute sigma, count predecessors */
        int64_t q_head = 0, q_tail = 0;
        int64_t stack_top = 0;
        queue[q_tail++] = s;

        /* Use epoch counter to deduplicate: for each v popped from queue,
         * mark forward neighbors with epoch, then skip reverse neighbors
         * already marked (bidirectional edges). Epoch increments per v. */
        int64_t epoch = 0;
        while (q_head < q_tail) {
            int64_t v = queue[q_head++];
            stack[stack_top++] = v;
            epoch++;

            /* Forward neighbors */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t w = fwd_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
                if (dist[w] == dist[v] + 1) {
                    sigma[w] += sigma[v];
                    pred_off[w + 1]++;
                    seen_epoch[w] = epoch;  /* mark w as counted for this v */
                }
            }
            /* Reverse neighbors (undirected), skip if already counted via fwd */
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t w = rev_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
                if (dist[w] == dist[v] + 1 && seen_epoch[w] != epoch) {
                    sigma[w] += sigma[v];
                    pred_off[w + 1]++;
                }
            }
        }

        /* Convert pred_off counts to cumulative offsets */
        for (int64_t i = 1; i <= n; i++)
            pred_off[i] += pred_off[i - 1];

        /* BFS pass 2: fill pred_data grouped by target node using write cursors.
         * Same dedup logic as pass 1 to avoid duplicate predecessor entries. */
        for (int64_t i = 0; i < n; i++) pred_cursor[i] = pred_off[i];
        epoch = 0;
        for (int64_t si2 = 0; si2 < stack_top; si2++) {
            int64_t v = stack[si2];
            epoch++;
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t w = fwd_tgt[j];
                if (dist[w] == dist[v] + 1) {
                    pred_data[pred_cursor[w]++] = v;
                    seen_epoch[w] = epoch;
                }
            }
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t w = rev_tgt[j];
                if (dist[w] == dist[v] + 1 && seen_epoch[w] != epoch)
                    pred_data[pred_cursor[w]++] = v;
            }
        }

        /* Back-propagation of dependencies */
        while (stack_top > 0) {
            int64_t w = stack[--stack_top];
            for (int64_t pi = pred_off[w]; pi < pred_off[w + 1]; pi++) {
                int64_t v = pred_data[pi];
                delta[v] += (sigma[v] / sigma[w]) * (1.0 + delta[w]);
            }
            if (w != s) cb[w] += delta[w];
        }
    }

    /* Undirected normalization: BFS from each source counts every unordered
     * pair {s,t} twice (once as source=s, once as source=t), so halve. */
    for (int64_t i = 0; i < n; i++) cb[i] /= 2.0;

    /* Normalize if sampled */
    if (sample > 0 && (int64_t)sample < n) {
        double scale = (double)n / (double)sample;
        for (int64_t i = 0; i < n; i++) cb[i] *= scale;
    }

    /* Build result table */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* cent_vec = ray_vec_new(RAY_F64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !cent_vec || RAY_IS_ERR(cent_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (cent_vec && !RAY_IS_ERR(cent_vec)) ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  cdata = (double*)ray_data(cent_vec);
    for (int64_t i = 0; i < n; i++) { ndata[i] = i; cdata[i] = cb[i]; }
    node_vec->len = n;
    cent_vec->len = n;
    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); ray_release(cent_vec); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, sym_intern_safe("_centrality", 11), cent_vec);
    ray_release(cent_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    return result;
}

/* --------------------------------------------------------------------------
 * exec_closeness: closeness centrality via BFS distance sums.
 * closeness[v] = reachable / sum_dist[v]. O(n*m) exact,
 * O(sample*m) approximate when sample_size > 0.
 * -------------------------------------------------------------------------- */
static ray_t* exec_closeness(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);
    uint16_t sample = ext->graph.max_iter;
    int64_t n_sources = (sample > 0 && (int64_t)sample < n) ? (int64_t)sample : n;

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  closeness = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    int64_t* dist      = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* queue     = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    if (!closeness || !dist || !queue) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    memset(closeness, 0, (size_t)n * sizeof(double));

    int64_t stride = (sample > 0 && (int64_t)sample < n) ? (n / n_sources) : 1;

    for (int64_t si = 0; si < n_sources; si++) {
        int64_t s = (si * stride) % n;

        /* Initialize distances */
        for (int64_t i = 0; i < n; i++) dist[i] = -1;
        dist[s] = 0;

        /* BFS from s */
        int64_t q_head = 0, q_tail = 0;
        queue[q_tail++] = s;

        while (q_head < q_tail) {
            int64_t v = queue[q_head++];

            /* Forward neighbors */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t w = fwd_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
            }
            /* Reverse neighbors (undirected) */
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t w = rev_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
            }
        }

        /* Sum distances and count reachable nodes */
        int64_t sum_dist = 0;
        int64_t reachable = 0;
        for (int64_t i = 0; i < n; i++) {
            if (dist[i] > 0) {
                sum_dist += dist[i];
                reachable++;
            }
        }

        if (reachable > 0 && sum_dist > 0) {
            closeness[s] = (double)reachable / (double)sum_dist;
        }
    }

    /* Build result table: when sampling, only emit computed nodes */
    int64_t n_out = n_sources;
    ray_t* node_vec = ray_vec_new(RAY_I64, n_out);
    ray_t* cent_vec = ray_vec_new(RAY_F64, n_out);
    if (!node_vec || RAY_IS_ERR(node_vec) || !cent_vec || RAY_IS_ERR(cent_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (cent_vec && !RAY_IS_ERR(cent_vec)) ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  cdata = (double*)ray_data(cent_vec);
    if (n_sources == n) {
        for (int64_t i = 0; i < n; i++) { ndata[i] = i; cdata[i] = closeness[i]; }
    } else {
        for (int64_t si = 0; si < n_sources; si++) {
            int64_t s = (si * stride) % n;
            ndata[si] = s;
            cdata[si] = closeness[s];
        }
    }
    node_vec->len = n_out;
    cent_vec->len = n_out;
    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); ray_release(cent_vec); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, sym_intern_safe("_centrality", 11), cent_vec);
    ray_release(cent_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    return result;
}

/* --------------------------------------------------------------------------
 * exec_mst: Minimum Spanning Tree / Forest via Kruskal's algorithm.
 * Collects weighted edges from forward CSR, sorts by weight, builds MST
 * using union-find with path compression and union by rank.
 * -------------------------------------------------------------------------- */
typedef struct { double w; int64_t src; int64_t dst; } mst_edge_t;

static int mst_edge_cmp(const void* a, const void* b) {
    double da = ((const mst_edge_t*)a)->w;
    double db = ((const mst_edge_t*)b)->w;
    return (da > db) - (da < db);
}

static int64_t uf_find(int64_t* parent, int64_t x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}

static bool uf_union(int64_t* parent, int64_t* rank_arr, int64_t a, int64_t b) {
    a = uf_find(parent, a); b = uf_find(parent, b);
    if (a == b) return false;
    if (rank_arr[a] < rank_arr[b]) { int64_t tmp = a; a = b; b = tmp; }
    parent[b] = a;
    if (rank_arr[a] == rank_arr[b]) rank_arr[a]++;
    return true;
}

static ray_t* exec_mst(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel || !rel->fwd.props) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    if (n <= 0) return ray_error("length", NULL);

    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* weight_vec = ray_table_get_col(rel->fwd.props, weight_sym);
    if (!weight_vec || weight_vec->type != RAY_F64) return ray_error("schema", NULL);
    double* weights = (double*)ray_data(weight_vec);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    mst_edge_t* edges_arr = (mst_edge_t*)ray_scratch_arena_push(&arena,
                                (size_t)m * sizeof(mst_edge_t));
    int64_t* uf_parent = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* uf_rank   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!edges_arr || !uf_parent || !uf_rank) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    /* Fill edge array from forward CSR */
    int64_t ei = 0;
    for (int64_t u = 0; u < n; u++) {
        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            edges_arr[ei].src = u;
            edges_arr[ei].dst = fwd_tgt[j];
            edges_arr[ei].w   = weights[fwd_row[j]];
            ei++;
        }
    }

    /* Sort edges by weight */
    qsort(edges_arr, (size_t)ei, sizeof(mst_edge_t), mst_edge_cmp);

    /* Initialize union-find */
    for (int64_t i = 0; i < n; i++) { uf_parent[i] = i; uf_rank[i] = 0; }

    /* Build MST */
    int64_t max_mst = n - 1;
    int64_t mst_count = 0;
    ray_t* src_vec = ray_vec_new(RAY_I64, max_mst);
    ray_t* dst_vec = ray_vec_new(RAY_I64, max_mst);
    ray_t* wt_vec  = ray_vec_new(RAY_F64, max_mst);
    if (!src_vec || RAY_IS_ERR(src_vec) ||
        !dst_vec || RAY_IS_ERR(dst_vec) ||
        !wt_vec  || RAY_IS_ERR(wt_vec)) {
        ray_scratch_arena_reset(&arena);
        if (src_vec && !RAY_IS_ERR(src_vec)) ray_release(src_vec);
        if (dst_vec && !RAY_IS_ERR(dst_vec)) ray_release(dst_vec);
        if (wt_vec  && !RAY_IS_ERR(wt_vec))  ray_release(wt_vec);
        return ray_error("oom", NULL);
    }

    int64_t* sdata = (int64_t*)ray_data(src_vec);
    int64_t* ddata = (int64_t*)ray_data(dst_vec);
    double*  wdata = (double*)ray_data(wt_vec);

    for (int64_t i = 0; i < ei && mst_count < max_mst; i++) {
        if (uf_union(uf_parent, uf_rank, edges_arr[i].src, edges_arr[i].dst)) {
            sdata[mst_count] = edges_arr[i].src;
            ddata[mst_count] = edges_arr[i].dst;
            wdata[mst_count] = edges_arr[i].w;
            mst_count++;
        }
    }

    src_vec->len = mst_count;
    dst_vec->len = mst_count;
    wt_vec->len  = mst_count;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(src_vec); ray_release(dst_vec); ray_release(wt_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_src", 4), src_vec);
    ray_release(src_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dst", 4), dst_vec);
    ray_release(dst_vec);
    result = ray_table_add_col(result, sym_intern_safe("_weight", 7), wt_vec);
    ray_release(wt_vec);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_random_walk: random walk from source node using xorshift64 PRNG.
 * -------------------------------------------------------------------------- */
static ray_t* exec_random_walk(ray_graph_t* g, ray_op_t* op, ray_t* src_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    uint16_t walk_len = ext->graph.max_iter;
    if (n <= 0) return ray_error("length", NULL);

    int64_t start_node;
    if (ray_is_atom(src_val)) {
        start_node = src_val->i64;
    } else {
        start_node = ((int64_t*)ray_data(src_val))[0];
    }
    if (start_node < 0 || start_node >= n) return ray_error("range", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);

    int64_t total = (int64_t)walk_len + 1;
    ray_t* step_vec = ray_vec_new(RAY_I64, total);
    ray_t* node_vec = ray_vec_new(RAY_I64, total);
    if (!step_vec || RAY_IS_ERR(step_vec) || !node_vec || RAY_IS_ERR(node_vec)) {
        if (step_vec && !RAY_IS_ERR(step_vec)) ray_release(step_vec);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        return ray_error("oom", NULL);
    }

    int64_t* sdata = (int64_t*)ray_data(step_vec);
    int64_t* ndata = (int64_t*)ray_data(node_vec);

    /* xorshift64 PRNG seeded from source node */
    uint64_t rng = (uint64_t)start_node * 6364136223846793005ULL + 1442695040888963407ULL;
    if (rng == 0) rng = 1;

    int64_t current = start_node;
    int64_t count = 0;
    for (int64_t i = 0; i < total; i++) {
        sdata[i] = i;
        ndata[i] = current;
        count++;
        if (i < walk_len) {
            int64_t deg = fwd_off[current + 1] - fwd_off[current];
            if (deg == 0) break;  /* dead end */
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            int64_t pick = (int64_t)(rng % (uint64_t)deg);
            current = fwd_tgt[fwd_off[current] + pick];
        }
    }

    step_vec->len = count;
    node_vec->len = count;

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(step_vec); ray_release(node_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_step", 5), step_vec);
    ray_release(step_vec);
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_dfs: depth-first search from source node. O(n+m).
 * -------------------------------------------------------------------------- */
static ray_t* exec_dfs(ray_graph_t* g, ray_op_t* op, ray_t* src_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    uint8_t max_depth = ext->graph.max_depth;
    if (n <= 0) return ray_error("length", NULL);

    /* Get source node ID */
    int64_t start_node;
    if (ray_is_atom(src_val)) {
        start_node = src_val->i64;
    } else {
        start_node = ((int64_t*)ray_data(src_val))[0];
    }
    if (start_node < 0 || start_node >= n) return ray_error("range", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Stack can hold up to m entries (one per edge traversal) */
    int64_t m = rel->fwd.n_edges;
    int64_t stack_cap = m > n ? m + 1 : n + 1;

    int64_t* stack_node   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)stack_cap * sizeof(int64_t));
    int64_t* stack_depth  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)stack_cap * sizeof(int64_t));
    int64_t* stack_parent = (int64_t*)ray_scratch_arena_push(&arena, (size_t)stack_cap * sizeof(int64_t));
    uint8_t* visited      = (uint8_t*)ray_scratch_arena_push(&arena, (size_t)n);
    int64_t* res_node     = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* res_depth    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* res_parent   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!stack_node || !stack_depth || !stack_parent || !visited ||
        !res_node || !res_depth || !res_parent) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    memset(visited, 0, (size_t)n);

    /* Push source */
    int64_t sp = 0;
    stack_node[sp]   = start_node;
    stack_depth[sp]  = 0;
    stack_parent[sp] = -1;
    sp++;

    int64_t count = 0;

    while (sp > 0) {
        sp--;
        int64_t v = stack_node[sp];
        int64_t d = stack_depth[sp];
        int64_t p = stack_parent[sp];

        if (visited[v]) continue;
        visited[v] = 1;

        res_node[count]   = v;
        res_depth[count]  = d;
        res_parent[count] = p;
        count++;

        if (d < max_depth) {
            /* Push neighbors in reverse order so first neighbor is visited first */
            int64_t start = fwd_off[v];
            int64_t end   = fwd_off[v + 1];
            for (int64_t j = end - 1; j >= start; j--) {
                int64_t u = fwd_tgt[j];
                if (!visited[u]) {
                    stack_node[sp]   = u;
                    stack_depth[sp]  = d + 1;
                    stack_parent[sp] = v;
                    sp++;
                }
            }
        }
    }

    /* Build result vectors */
    ray_t* node_vec   = ray_vec_new(RAY_I64, count);
    ray_t* depth_vec  = ray_vec_new(RAY_I64, count);
    ray_t* parent_vec = ray_vec_new(RAY_I64, count);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !depth_vec || RAY_IS_ERR(depth_vec) ||
        !parent_vec || RAY_IS_ERR(parent_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (depth_vec && !RAY_IS_ERR(depth_vec)) ray_release(depth_vec);
        if (parent_vec && !RAY_IS_ERR(parent_vec)) ray_release(parent_vec);
        return ray_error("oom", NULL);
    }

    memcpy(ray_data(node_vec),   res_node,   (size_t)count * sizeof(int64_t));
    memcpy(ray_data(depth_vec),  res_depth,  (size_t)count * sizeof(int64_t));
    memcpy(ray_data(parent_vec), res_parent, (size_t)count * sizeof(int64_t));
    node_vec->len   = count;
    depth_vec->len  = count;
    parent_vec->len = count;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(depth_vec); ray_release(parent_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_depth", 6), depth_vec);
    ray_release(depth_vec);
    result = ray_table_add_col(result, sym_intern_safe("_parent", 7), parent_vec);
    ray_release(parent_vec);

    return result;
}

/* exec_astar: A* shortest path with Euclidean coordinate heuristic */
static ray_t* exec_astar(ray_graph_t* g, ray_op_t* op,
                         ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);
    if (!rel->fwd.props) return ray_error("schema", NULL);

    ray_t* np = (ray_t*)ext->graph.node_props;
    if (!np) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    int64_t src_id = src_val->i64;
    int64_t dst_id = dst_val->i64;

    if (src_id < 0 || src_id >= n) return ray_error("range", NULL);
    if (dst_id < 0 || dst_id >= n) return ray_error("range", NULL);

    /* Resolve weight column from edge properties */
    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* weight_vec = ray_table_get_col(rel->fwd.props, weight_sym);
    if (!weight_vec || RAY_IS_ERR(weight_vec)) return ray_error("schema", NULL);
    double* weights_arr = (double*)ray_data(weight_vec);

    /* Resolve coordinate columns from node properties */
    ray_t* lat_vec = ray_table_get_col(np, ext->graph.coord_col_syms[0]);
    ray_t* lon_vec = ray_table_get_col(np, ext->graph.coord_col_syms[1]);
    if (!lat_vec || !lon_vec) return ray_error("schema", NULL);
    double* lat = (double*)ray_data(lat_vec);
    double* lon = (double*)ray_data(lon_vec);

    int64_t heap_cap = (m > n ? m : n) + 1;

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  dist_a    = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    bool*    visited = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    int64_t* depth_a   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    dijk_entry_t* heap = (dijk_entry_t*)ray_scratch_arena_push(&arena,
                              (size_t)heap_cap * sizeof(dijk_entry_t));
    if (!dist_a || !visited || !depth_a || !heap) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(visited, 0, (size_t)n * sizeof(bool));
    memset(depth_a, 0, (size_t)n * sizeof(int64_t));

    for (int64_t i = 0; i < n; i++) dist_a[i] = 1e308;
    dist_a[src_id] = 0.0;

    /* A* uses f = g + h; heap stores f-cost for priority ordering */
    double dx = lat[src_id] - lat[dst_id];
    double dy = lon[src_id] - lon[dst_id];
    double h0 = sqrt(dx * dx + dy * dy);
    int64_t heap_size = 0;
    dijk_heap_push(heap, &heap_size, h0, src_id);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    while (heap_size > 0) {
        dijk_entry_t top = dijk_heap_pop(heap, &heap_size);
        int64_t u = top.node;
        if (visited[u]) continue;
        visited[u] = true;

        if (u == dst_id) break;

        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            int64_t v = fwd_tgt[j];
            int64_t edge_row = fwd_row[j];
            double w = weights_arr[edge_row];
            double new_dist = dist_a[u] + w;
            if (new_dist < dist_a[v]) {
                dist_a[v] = new_dist;
                depth_a[v] = depth_a[u] + 1;
                /* f = g + h (Euclidean heuristic) */
                double hdx = lat[v] - lat[dst_id];
                double hdy = lon[v] - lon[dst_id];
                double hv = sqrt(hdx * hdx + hdy * hdy);
                dijk_heap_push(heap, &heap_size, new_dist + hv, v);
            }
        }
    }

    /* Collect reachable nodes */
    int64_t acount = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist_a[i] < 1e308) acount++;
    }

    ray_t* node_vec  = ray_vec_new(RAY_I64, acount);
    ray_t* dist_vec  = ray_vec_new(RAY_F64, acount);
    ray_t* depth_vec = ray_vec_new(RAY_I64, acount);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !dist_vec || RAY_IS_ERR(dist_vec) ||
        !depth_vec || RAY_IS_ERR(depth_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec)) ray_release(dist_vec);
        if (depth_vec && !RAY_IS_ERR(depth_vec)) ray_release(depth_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata_a = (int64_t*)ray_data(node_vec);
    double*  ddata_a = (double*)ray_data(dist_vec);
    int64_t* hdata_a = (int64_t*)ray_data(depth_vec);
    int64_t idx = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist_a[i] < 1e308) {
            ndata_a[idx] = i;
            ddata_a[idx] = dist_a[i];
            hdata_a[idx] = depth_a[i];
            idx++;
        }
    }
    node_vec->len = acount;
    dist_vec->len = acount;
    depth_vec->len = acount;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(dist_vec);
        ray_release(depth_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    result = ray_table_add_col(result, sym_intern_safe("_depth", 6), depth_vec);
    ray_release(depth_vec);

    return result;
}

/* exec_k_shortest: Yen's k-shortest paths via iterative masked Dijkstra */
static ray_t* exec_k_shortest(ray_graph_t* g, ray_op_t* op,
                               ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel || !rel->fwd.props) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    int64_t src_id = src_val->i64;
    int64_t dst_id = dst_val->i64;
    uint16_t K = ext->graph.max_iter;

    if (src_id < 0 || src_id >= n || dst_id < 0 || dst_id >= n)
        return ray_error("range", NULL);

    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* weight_vec = ray_table_get_col(rel->fwd.props, weight_sym);
    if (!weight_vec || RAY_IS_ERR(weight_vec)) return ray_error("schema", NULL);
    double* weights_k = (double*)ray_data(weight_vec);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    int64_t heap_cap = (m > n ? m : n) + 1;

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Dijkstra working arrays */
    double*       dist_arr  = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    int64_t*      parent    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    bool*         vis       = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    dijk_entry_t* heap      = (dijk_entry_t*)ray_scratch_arena_push(&arena,
                                    (size_t)heap_cap * sizeof(dijk_entry_t));
    bool*         node_mask = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    bool*         edge_mask = (bool*)ray_scratch_arena_push(&arena, (size_t)m * sizeof(bool));

    /* Path storage: K paths, each up to n nodes */
    int64_t* paths_data = (int64_t*)ray_scratch_arena_push(&arena, (size_t)K * (size_t)n * sizeof(int64_t));
    int64_t* path_lens  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)K * sizeof(int64_t));
    double*  path_costs = (double*)ray_scratch_arena_push(&arena, (size_t)K * sizeof(double));

    /* Candidate storage */
    int64_t max_cand = (int64_t)K * n;
    if (max_cand > 4096) max_cand = 4096;
    int64_t* cand_data  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)max_cand * (size_t)n * sizeof(int64_t));
    int64_t* cand_lens  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)max_cand * sizeof(int64_t));
    double*  cand_costs = (double*)ray_scratch_arena_push(&arena, (size_t)max_cand * sizeof(double));

    /* Temp buffer for path reconstruction */
    int64_t* tmp_path = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    if (!dist_arr || !parent || !vis || !heap || !node_mask || !edge_mask ||
        !paths_data || !path_lens || !path_costs ||
        !cand_data || !cand_lens || !cand_costs || !tmp_path) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    int64_t num_found = 0;
    int64_t num_cand  = 0;

    /* Step 1: Find shortest path P[0] */
    double d = dijkstra_masked(fwd_off, fwd_tgt, fwd_row, weights_k, n,
                                src_id, dst_id, NULL, NULL,
                                dist_arr, parent, heap, vis);

    if (d >= 1e308) {
        ray_scratch_arena_reset(&arena);
        ray_t* nv = ray_vec_new(RAY_I64, 0); nv->len = 0;
        ray_t* dv = ray_vec_new(RAY_F64, 0); dv->len = 0;
        ray_t* pv = ray_vec_new(RAY_I64, 0); pv->len = 0;
        ray_t* result = ray_table_new(3);
        result = ray_table_add_col(result, sym_intern_safe("_path_id", 8), pv); ray_release(pv);
        result = ray_table_add_col(result, sym_intern_safe("_node", 5), nv); ray_release(nv);
        result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dv); ray_release(dv);
        return result;
    }

    /* Reconstruct P[0] from parent array (reverse then flip) */
    int64_t plen = 0;
    for (int64_t v = dst_id; v != -1; v = parent[v]) {
        tmp_path[plen++] = v;
        if (plen > n) break;  /* safety: avoid infinite loop on corrupt parent */
    }
    for (int64_t i = 0; i < plen / 2; i++) {
        int64_t tmp = tmp_path[i];
        tmp_path[i] = tmp_path[plen - 1 - i];
        tmp_path[plen - 1 - i] = tmp;
    }

    memcpy(&paths_data[0], tmp_path, (size_t)plen * sizeof(int64_t));
    path_lens[0] = plen;
    path_costs[0] = d;
    num_found = 1;

    /* Step 2: Iteratively find paths P[1]..P[K-1] */
    for (uint16_t k = 1; k < K; k++) {
        int64_t* prev_path = &paths_data[(int64_t)(k - 1) * n];
        int64_t prev_len = path_lens[k - 1];

        for (int64_t i = 0; i < prev_len - 1; i++) {
            int64_t spur_node = prev_path[i];

            /* Compute root path cost */
            double root_cost = 0.0;
            for (int64_t r = 0; r < i; r++) {
                int64_t from = prev_path[r];
                int64_t to   = prev_path[r + 1];
                for (int64_t e = fwd_off[from]; e < fwd_off[from + 1]; e++) {
                    if (fwd_tgt[e] == to) {
                        root_cost += weights_k[fwd_row[e]];
                        break;
                    }
                }
            }

            /* Mask edges used by found paths sharing the root prefix */
            memset(edge_mask, 0, (size_t)m * sizeof(bool));
            memset(node_mask, 0, (size_t)n * sizeof(bool));

            for (int64_t j = 0; j < num_found; j++) {
                int64_t* pj = &paths_data[j * n];
                int64_t pj_len = path_lens[j];
                if (pj_len <= i) continue;

                bool same_prefix = true;
                for (int64_t r = 0; r <= i; r++) {
                    if (pj[r] != prev_path[r]) { same_prefix = false; break; }
                }
                if (!same_prefix) continue;

                int64_t from = pj[i];
                int64_t to   = pj[i + 1];
                for (int64_t e = fwd_off[from]; e < fwd_off[from + 1]; e++) {
                    if (fwd_tgt[e] == to) { edge_mask[e] = true; break; }
                }
            }

            /* Mask root path nodes except spur node */
            for (int64_t r = 0; r < i; r++) {
                node_mask[prev_path[r]] = true;
            }

            /* Dijkstra from spur to dst with masks */
            double spur_dist = dijkstra_masked(fwd_off, fwd_tgt, fwd_row, weights_k, n,
                                                spur_node, dst_id, node_mask, edge_mask,
                                                dist_arr, parent, heap, vis);
            if (spur_dist >= 1e308) continue;

            /* Reconstruct spur path */
            int64_t spur_len = 0;
            for (int64_t v = dst_id; v != -1; v = parent[v]) {
                tmp_path[spur_len++] = v;
                if (spur_len > n) break;
            }
            for (int64_t a = 0; a < spur_len / 2; a++) {
                int64_t tmp = tmp_path[a];
                tmp_path[a] = tmp_path[spur_len - 1 - a];
                tmp_path[spur_len - 1 - a] = tmp;
            }

            double total_cost = root_cost + spur_dist;
            int64_t total_len = i + spur_len;
            if (total_len > n || num_cand >= max_cand) continue;

            /* Check for duplicate candidates */
            bool dup = false;
            for (int64_t c = 0; c < num_cand && !dup; c++) {
                if (cand_lens[c] != total_len) continue;
                bool same = true;
                int64_t* cp = &cand_data[c * n];
                for (int64_t r = 0; r < i && same; r++) {
                    if (cp[r] != prev_path[r]) same = false;
                }
                for (int64_t r = 0; r < spur_len && same; r++) {
                    if (cp[i + r] != tmp_path[r]) same = false;
                }
                if (same) dup = true;
            }
            /* Check against already-found paths */
            for (int64_t f = 0; f < num_found && !dup; f++) {
                if (path_lens[f] != total_len) continue;
                bool same = true;
                int64_t* fp = &paths_data[f * n];
                for (int64_t r = 0; r < i && same; r++) {
                    if (fp[r] != prev_path[r]) same = false;
                }
                for (int64_t r = 0; r < spur_len && same; r++) {
                    if (fp[i + r] != tmp_path[r]) same = false;
                }
                if (same) dup = true;
            }
            if (dup) continue;

            /* Store candidate: root_path[0..i-1] + spur_path */
            int64_t* cp = &cand_data[num_cand * n];
            memcpy(cp, prev_path, (size_t)i * sizeof(int64_t));
            memcpy(cp + i, tmp_path, (size_t)spur_len * sizeof(int64_t));
            cand_lens[num_cand] = total_len;
            cand_costs[num_cand] = total_cost;
            num_cand++;
        }

        if (num_cand == 0) break;

        /* Pick cheapest candidate */
        int64_t best = 0;
        for (int64_t c = 1; c < num_cand; c++) {
            if (cand_costs[c] < cand_costs[best]) best = c;
        }

        memcpy(&paths_data[(int64_t)k * n], &cand_data[best * n],
               (size_t)cand_lens[best] * sizeof(int64_t));
        path_lens[k] = cand_lens[best];
        path_costs[k] = cand_costs[best];
        num_found++;

        /* Remove used candidate (swap with last) */
        if (best < num_cand - 1) {
            memcpy(&cand_data[best * n], &cand_data[(num_cand - 1) * n],
                   (size_t)cand_lens[num_cand - 1] * sizeof(int64_t));
            cand_lens[best] = cand_lens[num_cand - 1];
            cand_costs[best] = cand_costs[num_cand - 1];
        }
        num_cand--;
    }

    /* Build output: _path_id, _node, _dist (running dist along each path) */
    int64_t total_rows = 0;
    for (int64_t k = 0; k < num_found; k++) total_rows += path_lens[k];

    ray_t* pid_vec  = ray_vec_new(RAY_I64, total_rows);
    ray_t* node_vec = ray_vec_new(RAY_I64, total_rows);
    ray_t* dist_vec = ray_vec_new(RAY_F64, total_rows);
    if (!pid_vec  || RAY_IS_ERR(pid_vec) ||
        !node_vec || RAY_IS_ERR(node_vec) ||
        !dist_vec || RAY_IS_ERR(dist_vec)) {
        ray_scratch_arena_reset(&arena);
        if (pid_vec  && !RAY_IS_ERR(pid_vec))  ray_release(pid_vec);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec)) ray_release(dist_vec);
        return ray_error("oom", NULL);
    }

    int64_t* pids  = (int64_t*)ray_data(pid_vec);
    int64_t* nodes_k = (int64_t*)ray_data(node_vec);
    double*  dists = (double*)ray_data(dist_vec);

    int64_t row = 0;
    for (int64_t k = 0; k < num_found; k++) {
        int64_t* path = &paths_data[k * n];
        int64_t pk_len = path_lens[k];
        double running = 0.0;
        for (int64_t j = 0; j < pk_len; j++) {
            pids[row]  = k;
            nodes_k[row] = path[j];
            if (j > 0) {
                int64_t from = path[j - 1];
                int64_t to   = path[j];
                for (int64_t e = fwd_off[from]; e < fwd_off[from + 1]; e++) {
                    if (fwd_tgt[e] == to) {
                        running += weights_k[fwd_row[e]];
                        break;
                    }
                }
            }
            dists[row] = running;
            row++;
        }
    }

    pid_vec->len  = total_rows;
    node_vec->len = total_rows;
    dist_vec->len = total_rows;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(pid_vec); ray_release(node_vec); ray_release(dist_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_path_id", 8), pid_vec);
    ray_release(pid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_cosine_sim: cosine similarity between embedding column and query vector.
 * dot(a,b) / (||a|| * ||b||) per row.
 * Input: RAY_F32 embedding column (flat N*D floats)
 * Output: RAY_F64 vector of similarities (one per row)
 * -------------------------------------------------------------------------- */
static ray_t* exec_cosine_sim(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;

    if (!query || dim <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);

    const float* data = (const float*)ray_data(emb_vec);

    /* Precompute query norm */
    double q_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        q_norm_sq += (double)query[j] * (double)query[j];
    }
    double q_norm = sqrt(q_norm_sq);

    /* Compute per-row similarity */
    ray_t* result = ray_vec_new(RAY_F64, nrows);
    if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
    result->len = nrows;
    double* out = (double*)ray_data(result);

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double dot = 0.0;
        double r_norm_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            dot += (double)row[j] * (double)query[j];
            r_norm_sq += (double)row[j] * (double)row[j];
        }
        double r_norm = sqrt(r_norm_sq);
        double denom = q_norm * r_norm;
        out[i] = (denom > 0.0) ? dot / denom : 0.0;
    }

    return result;
}

/* --------------------------------------------------------------------------
 * exec_euclidean_dist: euclidean distance between embedding column and query.
 * sqrt(sum((a_i - b_i)^2)) per row.
 * -------------------------------------------------------------------------- */
static ray_t* exec_euclidean_dist(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;

    if (!query || dim <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);

    const float* data = (const float*)ray_data(emb_vec);

    ray_t* result = ray_vec_new(RAY_F64, nrows);
    if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
    result->len = nrows;
    double* out = (double*)ray_data(result);

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double sum_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            double d = (double)row[j] - (double)query[j];
            sum_sq += d * d;
        }
        out[i] = sqrt(sum_sq);
    }

    return result;
}

/* --------------------------------------------------------------------------
 * exec_knn: brute-force K nearest neighbors via cosine similarity.
 * Returns RAY_TABLE with _rowid (I64) and _similarity (F64), sorted desc.
 * -------------------------------------------------------------------------- */

/* Min-heap entry for KNN (track worst of top-K) */
typedef struct {
    double  sim;
    int64_t rowid;
} knn_entry_t;

static void knn_heap_insert(knn_entry_t* heap, int64_t k, int64_t* size,
                             double sim, int64_t rowid) {
    if (*size < k) {
        /* Heap not full: insert and sift up */
        int64_t i = (*size)++;
        heap[i].sim = sim;
        heap[i].rowid = rowid;
        /* Sift up (min-heap: root = lowest similarity = worst of top-K) */
        while (i > 0) {
            int64_t parent = (i - 1) / 2;
            if (heap[parent].sim <= heap[i].sim) break;
            knn_entry_t tmp = heap[parent]; heap[parent] = heap[i]; heap[i] = tmp;
            i = parent;
        }
    } else if (sim > heap[0].sim) {
        /* Better than worst in heap: replace root and sift down */
        heap[0].sim = sim;
        heap[0].rowid = rowid;
        int64_t i = 0;
        while (1) {
            int64_t left = 2*i+1, right = 2*i+2, smallest = i;
            if (left < *size && heap[left].sim < heap[smallest].sim) smallest = left;
            if (right < *size && heap[right].sim < heap[smallest].sim) smallest = right;
            if (smallest == i) break;
            knn_entry_t tmp = heap[i]; heap[i] = heap[smallest]; heap[smallest] = tmp;
            i = smallest;
        }
    }
}

static ray_t* exec_knn(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;
    int64_t k = ext->vector.k;

    if (!query || dim <= 0 || k <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);
    if (k > nrows) k = nrows;

    const float* data = (const float*)ray_data(emb_vec);

    /* Precompute query norm */
    double q_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) q_norm_sq += (double)query[j] * query[j];
    double q_norm = sqrt(q_norm_sq);

    /* Min-heap for top-K */
    ray_t* heap_hdr = NULL;
    knn_entry_t* heap = (knn_entry_t*)scratch_alloc(&heap_hdr, (size_t)k * sizeof(knn_entry_t));
    if (!heap) return ray_error("oom", NULL);
    int64_t heap_size = 0;

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double dot = 0.0, r_norm_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            dot += (double)row[j] * query[j];
            r_norm_sq += (double)row[j] * row[j];
        }
        double r_norm = sqrt(r_norm_sq);
        double denom = q_norm * r_norm;
        double sim = (denom > 0.0) ? dot / denom : 0.0;
        knn_heap_insert(heap, k, &heap_size, sim, i);
    }

    /* Simple insertion sort (k is small) — descending by similarity */
    for (int64_t i = 1; i < heap_size; i++) {
        knn_entry_t key = heap[i];
        int64_t j = i - 1;
        while (j >= 0 && heap[j].sim < key.sim) {
            heap[j + 1] = heap[j];
            j--;
        }
        heap[j + 1] = key;
    }

    /* Build output table: _rowid (I64), _similarity (F64) */
    ray_t* rowid_vec = ray_vec_new(RAY_I64, heap_size);
    ray_t* sim_vec   = ray_vec_new(RAY_F64, heap_size);
    if (!rowid_vec || RAY_IS_ERR(rowid_vec) || !sim_vec || RAY_IS_ERR(sim_vec)) {
        scratch_free(heap_hdr);
        if (rowid_vec && !RAY_IS_ERR(rowid_vec)) ray_release(rowid_vec);
        if (sim_vec && !RAY_IS_ERR(sim_vec)) ray_release(sim_vec);
        return ray_error("oom", NULL);
    }

    int64_t* rdata = (int64_t*)ray_data(rowid_vec);
    double*  sdata = (double*)ray_data(sim_vec);
    for (int64_t i = 0; i < heap_size; i++) {
        rdata[i] = heap[i].rowid;
        sdata[i] = heap[i].sim;
    }
    rowid_vec->len = heap_size;
    sim_vec->len   = heap_size;

    scratch_free(heap_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(rowid_vec);
        ray_release(sim_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_rowid", 6), rowid_vec);
    ray_release(rowid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_similarity", 11), sim_vec);
    ray_release(sim_vec);

    return result;
}

static ray_t* exec_hnsw_knn(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_hnsw_t* idx = (ray_hnsw_t*)ext->hnsw.hnsw_idx;
    const float* query = ext->hnsw.query_vec;
    int32_t dim = ext->hnsw.dim;
    int64_t k = ext->hnsw.k;
    int32_t ef = ext->hnsw.ef_search;

    if (!idx || !query || dim <= 0 || k <= 0) return ray_error("schema", NULL);

    /* Pre-allocate output arrays */
    ray_t* ids_hdr = NULL;
    int64_t* out_ids = (int64_t*)scratch_alloc(&ids_hdr, (size_t)k * sizeof(int64_t));
    if (!out_ids) return ray_error("oom", NULL);

    ray_t* dists_hdr = NULL;
    double* out_dists = (double*)scratch_alloc(&dists_hdr, (size_t)k * sizeof(double));
    if (!out_dists) { scratch_free(ids_hdr); return ray_error("oom", NULL); }

    int64_t n_found = ray_hnsw_search(idx, query, dim, k, ef, out_ids, out_dists);

    /* Build output table: _rowid (I64), _similarity (F64) */
    ray_t* rowid_vec = ray_vec_new(RAY_I64, n_found);
    ray_t* sim_vec   = ray_vec_new(RAY_F64, n_found);
    if (!rowid_vec || RAY_IS_ERR(rowid_vec) || !sim_vec || RAY_IS_ERR(sim_vec)) {
        scratch_free(ids_hdr);
        scratch_free(dists_hdr);
        if (rowid_vec && !RAY_IS_ERR(rowid_vec)) ray_release(rowid_vec);
        if (sim_vec && !RAY_IS_ERR(sim_vec)) ray_release(sim_vec);
        return ray_error("oom", NULL);
    }

    int64_t* rdata = (int64_t*)ray_data(rowid_vec);
    double*  sdata = (double*)ray_data(sim_vec);
    for (int64_t i = 0; i < n_found; i++) {
        rdata[i] = out_ids[i];
        sdata[i] = 1.0 - out_dists[i];  /* convert distance back to similarity */
    }
    rowid_vec->len = n_found;
    sim_vec->len   = n_found;

    scratch_free(ids_hdr);
    scratch_free(dists_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(rowid_vec);
        ray_release(sim_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_rowid", 6), rowid_vec);
    ray_release(rowid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_similarity", 11), sim_vec);
    ray_release(sim_vec);

    return result;
}

/* Broadcast a scalar atom to a column vector of nrows elements.
 * Returns a new vector (caller owns).  On failure returns ray_error(). */
ray_t* broadcast_scalar(ray_t* atom, int64_t nrows) {
    if (!atom) return ray_error("domain", NULL);
    if (nrows <= 0) {
        /* Empty table: return an empty vector of the matching type */
        int8_t at = atom->type;
        int8_t vt;
        if      (at == -RAY_STR)  vt = RAY_STR;
        else if (at == -RAY_I64)  vt = RAY_I64;
        else if (at == -RAY_F64)  vt = RAY_F64;
        else if (at == -RAY_BOOL) vt = RAY_BOOL;
        else if (at == -RAY_SYM)  vt = RAY_SYM;
        else return ray_error("type", NULL);
        return ray_vec_new(vt, 0);
    }
    int8_t at = atom->type;

    /* -RAY_STR → RAY_STR column */
    if (at == -RAY_STR) {
        const char* sp = ray_str_ptr(atom);
        size_t sl = ray_str_len(atom);
        ray_t* vec = ray_vec_new(RAY_STR, nrows);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        for (int64_t r = 0; r < nrows; r++) {
            vec = ray_str_vec_append(vec, sp, sl);
            if (RAY_IS_ERR(vec)) return vec;
        }
        return vec;
    }

    /* Numeric / bool / sym scalars */
    int8_t vt;
    if      (at == -RAY_I64)  vt = RAY_I64;
    else if (at == -RAY_F64)  vt = RAY_F64;
    else if (at == -RAY_BOOL) vt = RAY_BOOL;
    else if (at == -RAY_SYM)  vt = RAY_SYM;
    else return ray_error("type", NULL);

    size_t esz = (vt == RAY_BOOL) ? 1 : 8;
    ray_t* vec = ray_vec_new(vt, nrows);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    uint8_t elem[8] = {0};
    memcpy(elem, &atom->i64, esz);
    for (int64_t r = 0; r < nrows; r++) {
        vec = ray_vec_append(vec, elem);
        if (RAY_IS_ERR(vec)) return vec;
    }
    return vec;
}

/* ============================================================================
 * exec_pivot — single-pass hash-aggregated pivot table
 *
 * Groups by (index_cols, pivot_col), aggregates value_col, then unstacks
 * pivot values into separate output columns.
 * ============================================================================ */

static ray_t* exec_pivot(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_idx   = ext->pivot.n_index;
    uint16_t agg_op = ext->pivot.agg_op;
    int64_t nrows   = ray_table_nrows(tbl);

    /* Resolve input columns */
    ray_t* idx_vecs[16];
    for (uint8_t i = 0; i < n_idx; i++) {
        ray_op_ext_t* ie = find_ext(g, ext->pivot.index_cols[i]->id);
        idx_vecs[i] = (ie && ie->base.opcode == OP_SCAN)
                     ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!idx_vecs[i]) return ray_error("domain", "pivot: index column not found");
    }

    ray_op_ext_t* pe = find_ext(g, ext->pivot.pivot_col->id);
    ray_t* pcol = (pe && pe->base.opcode == OP_SCAN)
                ? ray_table_get_col(tbl, pe->sym) : NULL;
    if (!pcol) return ray_error("domain", "pivot: pivot column not found");

    ray_op_ext_t* ve = find_ext(g, ext->pivot.value_col->id);
    ray_t* vcol = (ve && ve->base.opcode == OP_SCAN)
                ? ray_table_get_col(tbl, ve->sym) : NULL;
    if (!vcol) return ray_error("domain", "pivot: value column not found");

    if (nrows == 0) return ray_table_new(0);

    /* Combined keys: index_cols + pivot_col */
    uint8_t n_keys = n_idx + 1;
    if (n_keys > 8) return ray_error("limit", "pivot: too many index columns");

    void*   key_data[8];
    int8_t  key_types[8];
    uint8_t key_attrs[8];
    for (uint8_t k = 0; k < n_idx; k++) {
        key_data[k]  = ray_data(idx_vecs[k]);
        key_types[k] = idx_vecs[k]->type;
        key_attrs[k] = idx_vecs[k]->attrs;
    }
    key_data[n_idx]  = ray_data(pcol);
    key_types[n_idx] = pcol->type;
    key_attrs[n_idx] = pcol->attrs;

    /* Single agg input: value column */
    ray_t* agg_vecs[1] = { vcol };
    uint16_t agg_ops[1] = { agg_op };

    /* Compute need_flags for the agg op */
    uint8_t need_flags = GHT_NEED_SUM; /* always need sum (used for FIRST/LAST too) */
    if (agg_op == OP_MIN) need_flags |= GHT_NEED_MIN;
    if (agg_op == OP_MAX) need_flags |= GHT_NEED_MAX;

    ght_layout_t ly = ght_compute_layout(n_keys, 1, agg_vecs, need_flags, agg_ops);

    /* Hash-aggregate all rows */
    uint32_t ht_cap = 1024;
    while (ht_cap < (uint32_t)nrows / 4 && ht_cap < (1u << 24)) ht_cap <<= 1;

    group_ht_t ht;
    if (!group_ht_init(&ht, ht_cap, &ly)) return ray_error("oom", NULL);
    group_rows_range(&ht, key_data, key_types, key_attrs, agg_vecs, 0, nrows);

    uint32_t grp_count = ht.grp_count;
    if (grp_count == 0) { group_ht_free(&ht); return ray_table_new(0); }

    /* Phase 2: Collect distinct pivot values and distinct index keys.
     * Each group row layout: [hash:8][key0:8]...[keyN-1:8][pivot_val:8][accum...] */

    /* Collect distinct pivot values */
    uint32_t pv_cap = 64, pv_count = 0;
    ray_t* pv_hdr = NULL;
    int64_t* pv_vals = (int64_t*)scratch_alloc(&pv_hdr, pv_cap * sizeof(int64_t));
    if (!pv_vals) { group_ht_free(&ht); return ray_error("oom", NULL); }

    for (uint32_t gi = 0; gi < grp_count; gi++) {
        const char* row = ht.rows + (size_t)gi * ly.row_stride;
        int64_t pval = ((const int64_t*)(row + 8))[n_idx]; /* pivot key is last */
        /* Linear scan for distinct (pivot values are typically few) */
        bool found = false;
        for (uint32_t p = 0; p < pv_count; p++) {
            if (pv_vals[p] == pval) { found = true; break; }
        }
        if (!found) {
            if (pv_count >= pv_cap) {
                uint32_t new_cap = pv_cap * 2;
                int64_t* new_pv = (int64_t*)scratch_realloc(&pv_hdr,
                    pv_cap * sizeof(int64_t), new_cap * sizeof(int64_t));
                if (!new_pv) { group_ht_free(&ht); return ray_error("oom", NULL); }
                pv_vals = new_pv;
                pv_cap = new_cap;
            }
            pv_vals[pv_count++] = pval;
        }
    }

    /* Collect distinct index keys.
     * Build a small HT mapping index-key-combo → row index in output. */
    uint32_t ix_cap = 256, ix_count = 0;
    ray_t* ix_hdr = NULL;
    /* Each entry: [hash:8][idx_keys:8*n_idx] */
    size_t ix_entry = 8 + (size_t)n_idx * 8;
    char* ix_rows = (char*)scratch_alloc(&ix_hdr, ix_cap * ix_entry);
    if (!ix_rows) { scratch_free(pv_hdr); group_ht_free(&ht); return ray_error("oom", NULL); }

    /* Map: group_id → (ix_row, pv_idx) for result cell placement */
    ray_t* map_hdr = NULL;
    uint32_t* grp_ix  = (uint32_t*)scratch_alloc(&map_hdr, grp_count * 2 * sizeof(uint32_t));
    if (!grp_ix) { scratch_free(ix_hdr); scratch_free(pv_hdr); group_ht_free(&ht); return ray_error("oom", NULL); }
    uint32_t* grp_pv = grp_ix + grp_count;

    for (uint32_t gi = 0; gi < grp_count; gi++) {
        const char* row = ht.rows + (size_t)gi * ly.row_stride;
        const int64_t* keys = (const int64_t*)(row + 8);

        /* Hash index keys only (exclude pivot key) */
        uint64_t ih = 0;
        for (uint8_t k = 0; k < n_idx; k++) {
            uint64_t kh = (key_types[k] == RAY_F64)
                ? ray_hash_f64(*(const double*)&keys[k])
                : ray_hash_i64(keys[k]);
            ih = (k == 0) ? kh : ray_hash_combine(ih, kh);
        }

        /* Find or insert index key */
        uint32_t ix_row = UINT32_MAX;
        for (uint32_t j = 0; j < ix_count; j++) {
            const char* ix_entry_p = ix_rows + j * ix_entry;
            if (*(const uint64_t*)ix_entry_p != ih) continue;
            if (memcmp(ix_entry_p + 8, keys, (size_t)n_idx * 8) == 0) {
                ix_row = j;
                break;
            }
        }
        if (ix_row == UINT32_MAX) {
            if (ix_count >= ix_cap) {
                uint32_t new_cap = ix_cap * 2;
                char* new_rows = (char*)scratch_realloc(&ix_hdr,
                    ix_cap * ix_entry, new_cap * ix_entry);
                if (!new_rows) {
                    scratch_free(map_hdr); scratch_free(pv_hdr);
                    group_ht_free(&ht); return ray_error("oom", NULL);
                }
                ix_rows = new_rows;
                ix_cap = new_cap;
            }
            ix_row = ix_count++;
            char* dst = ix_rows + ix_row * ix_entry;
            *(uint64_t*)dst = ih;
            memcpy(dst + 8, keys, (size_t)n_idx * 8);
        }

        /* Find pivot column index */
        int64_t pval = keys[n_idx];
        uint32_t pv_idx = 0;
        for (uint32_t p = 0; p < pv_count; p++) {
            if (pv_vals[p] == pval) { pv_idx = p; break; }
        }

        grp_ix[gi] = ix_row;
        grp_pv[gi] = pv_idx;
    }

    /* Phase 3: Build output table */
    bool val_is_f64 = vcol->type == RAY_F64;
    int8_t out_agg_type;
    switch (agg_op) {
        case OP_AVG:   out_agg_type = RAY_F64; break;
        case OP_COUNT: out_agg_type = RAY_I64; break;
        case OP_SUM:   out_agg_type = val_is_f64 ? RAY_F64 : RAY_I64; break;
        default:       out_agg_type = vcol->type; break;
    }

    int64_t out_ncols = (int64_t)n_idx + (int64_t)pv_count;
    ray_t* result = ray_table_new(out_ncols);
    if (!result || RAY_IS_ERR(result)) goto pivot_cleanup;

    /* Index columns */
    for (uint8_t k = 0; k < n_idx; k++) {
        ray_t* new_col = col_vec_new(idx_vecs[k], (int64_t)ix_count);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); result = ray_error("oom", NULL); goto pivot_cleanup; }
        new_col->len = (int64_t)ix_count;
        uint8_t esz = col_esz(idx_vecs[k]);
        int8_t kt = idx_vecs[k]->type;
        for (uint32_t r = 0; r < ix_count; r++) {
            const char* ix_entry_p = ix_rows + r * ix_entry;
            int64_t kv = ((const int64_t*)(ix_entry_p + 8))[k];
            if (kt == RAY_F64) {
                memcpy((char*)ray_data(new_col) + (size_t)r * esz, &kv, 8);
            } else {
                write_col_i64(ray_data(new_col), (int64_t)r, kv, kt, new_col->attrs);
            }
        }
        if (idx_vecs[k]->type == RAY_STR)
            col_propagate_str_pool(new_col, idx_vecs[k]);

        ray_op_ext_t* ie = find_ext(g, ext->pivot.index_cols[k]->id);
        result = ray_table_add_col(result, ie->sym, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) goto pivot_cleanup;
    }

    /* Value columns — one per distinct pivot value */
    {
    int8_t s = ly.agg_val_slot[0]; /* single agg input → slot 0 */
    for (uint32_t p = 0; p < pv_count; p++) {
        ray_t* new_col = (out_agg_type == vcol->type)
                        ? col_vec_new(vcol, (int64_t)ix_count)
                        : ray_vec_new(out_agg_type, (int64_t)ix_count);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); result = ray_error("oom", NULL); goto pivot_cleanup; }
        new_col->len = (int64_t)ix_count;

        /* Initialize with zero (missing cells get 0) */
        memset(ray_data(new_col), 0, (size_t)ix_count * (out_agg_type == RAY_F64 ? 8 : (size_t)col_esz(new_col)));

        for (uint32_t gi = 0; gi < grp_count; gi++) {
            if (grp_pv[gi] != p) continue;
            uint32_t r = grp_ix[gi];
            const char* row = ht.rows + (size_t)gi * ly.row_stride;
            int64_t cnt = *(const int64_t*)(const void*)row;

            if (out_agg_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s)
                                       : (double)ROW_RD_I64(row, ly.off_sum, s);
                        break;
                    case OP_AVG:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s) / cnt
                                       : (double)ROW_RD_I64(row, ly.off_sum, s) / cnt;
                        break;
                    case OP_MIN:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_min, s)
                                       : (double)ROW_RD_I64(row, ly.off_min, s);
                        break;
                    case OP_MAX:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_max, s)
                                       : (double)ROW_RD_I64(row, ly.off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s)
                                       : (double)ROW_RD_I64(row, ly.off_sum, s);
                        break;
                    default: v = 0.0; break;
                }
                ((double*)ray_data(new_col))[r] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:   v = ROW_RD_I64(row, ly.off_sum, s); break;
                    case OP_COUNT: v = cnt; break;
                    case OP_MIN:   v = ROW_RD_I64(row, ly.off_min, s); break;
                    case OP_MAX:   v = ROW_RD_I64(row, ly.off_max, s); break;
                    case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly.off_sum, s); break;
                    default:       v = 0; break;
                }
                write_col_i64(ray_data(new_col), (int64_t)r, v, out_agg_type, new_col->attrs);
            }
        }

        /* Column name from pivot value — match pivot_val_to_sym semantics */
        int64_t pval = pv_vals[p];
        int64_t col_sym;
        if (pcol->type == RAY_SYM) {
            col_sym = pval;
        } else {
            char buf[128];
            int len = 0;
            int8_t pt = key_types[n_idx];
            if (pt == RAY_F64) {
                double fv;
                memcpy(&fv, &pval, 8);
                len = snprintf(buf, sizeof(buf), "%g", fv);
            } else if (pt == RAY_BOOL) {
                len = snprintf(buf, sizeof(buf), "%s", pval ? "true" : "false");
            } else if (pt == RAY_I64 || pt == RAY_I32 || pt == RAY_I16 ||
                       pt == RAY_DATE || pt == RAY_TIME || pt == RAY_TIMESTAMP) {
                len = snprintf(buf, sizeof(buf), "%ld", (long)pval);
            } else {
                len = snprintf(buf, sizeof(buf), "col%ld", (long)pval);
            }
            col_sym = ray_sym_intern(buf, (size_t)len);
        }

        result = ray_table_add_col(result, col_sym, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) goto pivot_cleanup;
    }
    }

pivot_cleanup:
    scratch_free(map_hdr);
    scratch_free(ix_hdr);
    scratch_free(pv_hdr);
    group_ht_free(&ht);
    return result;
}

/* ============================================================================
 * Recursive executor
 * ============================================================================ */

/* Is this opcode a "heavy" pipeline breaker worth profiling? */
static inline bool op_is_heavy(uint16_t opc) {
    return opc == OP_FILTER || opc == OP_SORT || opc == OP_GROUP ||
           opc == OP_JOIN   || opc == OP_WINDOW_JOIN || opc == OP_SELECT ||
           opc == OP_HEAD   || opc == OP_TAIL || opc == OP_WINDOW ||
           opc == OP_PIVOT  ||
           (opc >= OP_EXPAND && opc <= OP_HNSW_KNN);
}

ray_t* exec_node(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    bool profiling = g_ray_profile.active && op_is_heavy(op->opcode);
    const char* oname = NULL;
    if (profiling) {
        oname = ray_opcode_name(op->opcode);
        ray_profile_span_start(oname);
    }

    ray_t* _prof_result = exec_node_inner(g, op);

    if (profiling)
        ray_profile_span_end(oname);

    return _prof_result;
}

static ray_t* exec_node_inner(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    switch (op->opcode) {
        case OP_SCAN: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext) return ray_error("nyi", NULL);

            /* Resolve table: pad[0..1] stores table_id+1 (0 = default g->table) */
            uint16_t stored_table_id = 0;
            memcpy(&stored_table_id, ext->base.pad, sizeof(uint16_t));
            ray_t* scan_tbl;
            if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables) {
                scan_tbl = g->tables[stored_table_id - 1];
            } else {
                scan_tbl = g->table;
            }
            if (!scan_tbl) return ray_error("schema", NULL);
            ray_t* col = ray_table_get_col(scan_tbl, ext->sym);
            if (!col) return ray_error("schema", NULL);
            if (col->type == RAY_MAPCOMMON)
                return materialize_mapcommon(col);
            if (RAY_IS_PARTED(col->type)) {
                /* Concat parted segments into flat vector (cold path) */
                int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                ray_t** sps = (ray_t**)ray_data(col);
                uint8_t sba = (base == RAY_SYM && col->len > 0 && sps[0])
                            ? sps[0]->attrs : 0;
                int64_t total = ray_parted_nrows(col);
                ray_t* flat = typed_vec_new(base, sba, total);
                if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
                flat->len = total;
                ray_t** segs = sps;
                size_t esz = (size_t)ray_sym_elem_size(base, sba);
                int64_t off = 0;
                for (int64_t s = 0; s < col->len; s++) {
                    if (segs[s] && segs[s]->len > 0) {
                        memcpy((char*)ray_data(flat) + off * esz,
                               ray_data(segs[s]), (size_t)segs[s]->len * esz);
                        off += segs[s]->len;
                    }
                }
                return flat;
            }
            ray_retain(col);
            return col;
        }

        case OP_CONST: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext || !ext->literal) return ray_error("nyi", NULL);
            ray_retain(ext->literal);
            return ext->literal;
        }

        /* Unary element-wise */
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_SQRT:
        case OP_LOG: case OP_EXP: case OP_CEIL: case OP_FLOOR:
        case OP_ISNULL: case OP_CAST:
        /* Binary element-wise */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR:
        case OP_MIN2: case OP_MAX2: {
            /* Try compiled expression first (fuses entire subtree) */
            if (g->table) {
                int64_t nr = ray_table_nrows(g->table);
                if (nr > 0) {
                    ray_expr_t ex;
                    if (expr_compile(g, g->table, op, &ex)) {
                        ray_t* vec = expr_eval_full(&ex, nr);
                        if (vec && !RAY_IS_ERR(vec)) return vec;
                    }
                }
            }
            /* Fallback: recursive per-node evaluation */
            if (op->arity == 1) {
                ray_t* input = exec_node(g, op->inputs[0]);
                if (!input || RAY_IS_ERR(input)) return input;
                ray_t* result = exec_elementwise_unary(g, op, input);
                ray_release(input);
                return result;
            } else {
                ray_t* lhs = exec_node(g, op->inputs[0]);
                ray_t* rhs = exec_node(g, op->inputs[1]);
                if (!lhs || RAY_IS_ERR(lhs)) { if (rhs && !RAY_IS_ERR(rhs)) ray_release(rhs); return lhs; }
                if (!rhs || RAY_IS_ERR(rhs)) { ray_release(lhs); return rhs; }
                ray_t* result = exec_elementwise_binary(g, op, lhs, rhs);
                ray_release(lhs);
                ray_release(rhs);
                return result;
            }
        }

        /* Reductions */
        case OP_SUM: case OP_PROD: case OP_MIN: case OP_MAX:
        case OP_COUNT: case OP_AVG: case OP_FIRST: case OP_LAST:
        case OP_STDDEV: case OP_STDDEV_POP: case OP_VAR: case OP_VAR_POP: {
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            /* Compact lazy selection before reducing — filters may have
             * set g->selection without materializing a compacted table. */
            bool own_input = (input != g->table);
            if (g->selection && input->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, input, g->selection);
                if (own_input) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                own_input = true;
            }
            ray_t* result = exec_reduction(g, op, input);
            if (own_input) ray_release(input);
            return result;
        }

        case OP_COUNT_DISTINCT: {
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* result = exec_count_distinct(g, op, input);
            ray_release(input);
            return result;
        }

        case OP_FILTER: {
            /* HAVING fusion: FILTER(GROUP) — evaluate the predicate against
             * the GROUP result rather than the original input table.
             * SCAN nodes in the predicate tree resolve column names via
             * g->table, so we temporarily swap it to the GROUP output. */
            ray_op_t* filter_child = op->inputs[0];
            if (filter_child && filter_child->opcode == OP_GROUP) {
                ray_t* group_result = exec_node(g, filter_child);
                if (!group_result || RAY_IS_ERR(group_result))
                    return group_result;

                ray_t* saved_table = g->table;
                ray_t* saved_sel   = g->selection;
                g->table     = group_result;
                g->selection = NULL;

                ray_t* pred = exec_node(g, op->inputs[1]);

                g->table     = saved_table;
                g->selection = saved_sel;

                if (!pred || RAY_IS_ERR(pred)) {
                    ray_release(group_result);
                    return pred;
                }

                ray_t* result = exec_filter(g, op, group_result, pred);
                ray_release(pred);
                ray_release(group_result);
                return result;
            }

            ray_t* input = exec_node(g, op->inputs[0]);
            ray_t* pred  = exec_node(g, op->inputs[1]);
            if (!input || RAY_IS_ERR(input)) { if (pred && !RAY_IS_ERR(pred)) ray_release(pred); return input; }
            if (!pred || RAY_IS_ERR(pred)) { ray_release(input); return pred; }

            /* Lazy filter: convert predicate to RAY_SEL bitmap instead of
             * materializing a compacted table.  Only for TABLE inputs —
             * downstream ops (group-by) consume the bitmap directly;
             * boundary ops (sort/join/window) compact on demand.
             * Vector inputs must still materialize immediately since
             * downstream ops like COUNT rely on compacted length. */
            if (pred->type == RAY_BOOL && input->type == RAY_TABLE) {
                ray_t* new_sel = ray_sel_from_pred(pred);
                ray_release(pred);
                if (!new_sel || RAY_IS_ERR(new_sel)) { ray_release(input); return new_sel; }

                if (g->selection) {
                    /* Chained filter: AND with existing selection */
                    ray_t* merged = ray_sel_and(g->selection, new_sel);
                    ray_release(new_sel);
                    ray_release(g->selection);
                    g->selection = merged;
                } else {
                    g->selection = new_sel;
                }
                return input;  /* original table, not compacted */
            }

            /* Eager filter for vector inputs and non-BOOL predicates */
            ray_t* result = exec_filter(g, op, input, pred);
            ray_release(input);
            ray_release(pred);
            return result;
        }

        case OP_SORT: {
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* tbl = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before sort (needs dense data) */
            if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, tbl, g->selection);
                if (input != g->table) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                tbl = compacted;
            }
            ray_t* result = exec_sort(g, op, tbl, 0);
            if (input != g->table) ray_release(input);
            return result;
        }

        case OP_GROUP: {
            ray_t* tbl = g->table;
            ray_t* owned_tbl = NULL;

            /* Factorized pipeline: detect OP_EXPAND (factorized) → OP_GROUP.
             * When the group key is _src and there's a factorized expand node
             * in the graph, execute the expand first and pipe its output as
             * the group input table.  This connects the expand→group pipeline
             * that would otherwise disconnect since GROUP reads g->table. */
            {
                ray_op_ext_t* gext = find_ext(g, op->id);
                if (gext && gext->n_keys == 1) {
                    ray_op_ext_t* kx = find_ext(g, gext->keys[0]->id);
                    int64_t src_sym = ray_sym_intern("_src", 4);
                    if (kx && kx->base.opcode == OP_SCAN && kx->sym == src_sym) {
                        /* Find the factorized OP_EXPAND connected to this GROUP.
                         * The expand must be the one whose output the GROUP
                         * is scanning (connected via OP_SCAN inputs). */
                        for (uint32_t ei = 0; ei < g->ext_count; ei++) {
                            ray_op_ext_t* ex = g->ext_nodes[ei];
                            if (ex && ex->base.id < g->node_count
                                && g->nodes[ex->base.id].opcode == OP_EXPAND
                                && ex->graph.factorized) {
                                ray_op_t* expand_op = &g->nodes[ex->base.id];
                                ray_t* expand_result = exec_node(g, expand_op);
                                if (!expand_result || RAY_IS_ERR(expand_result))
                                    return expand_result;
                                if (expand_result->type == RAY_TABLE) {
                                    ray_t* saved = g->table;
                                    g->table = expand_result;
                                    ray_t* result = exec_group(g, op, expand_result, 0);
                                    g->table = saved;
                                    ray_release(expand_result);
                                    return result;
                                }
                                ray_release(expand_result);
                                break;
                            }
                        }
                    }
                }
            }

            /* Always compact lazy selection before GROUP BY.
             * The sequential fallback path (group_rows_range) does not
             * honour the selection bitmap, so we must materialize a
             * compacted table upfront.  The DA and radix-parallel paths
             * *do* check the bitmap, but we cannot predict which path
             * exec_group will choose, and compaction is cheap relative
             * to the aggregation work that follows.  This also prevents
             * a stale g->selection from leaking into downstream ops
             * (e.g. SORT), which would otherwise try to sel_compact the
             * already-aggregated result with a mismatched-length bitmap
             * and produce empty or corrupt output. */
            if (g->selection && g->selection->type == RAY_SEL) {
                ray_t* compacted = sel_compact(g, tbl, g->selection);
                if (!compacted || RAY_IS_ERR(compacted)) return compacted;
                ray_release(g->selection);
                g->selection = NULL;
                owned_tbl = compacted;
                tbl = compacted;
            }
            ray_t* result = exec_group(g, op, tbl, 0);
            if (owned_tbl) ray_release(owned_tbl);
            return result;
        }

        case OP_PIVOT: {
            ray_t* tbl = g->table;
            ray_t* owned_tbl = NULL;
            if (g->selection && g->selection->type == RAY_SEL) {
                ray_t* compacted = sel_compact(g, tbl, g->selection);
                if (!compacted || RAY_IS_ERR(compacted)) return compacted;
                ray_release(g->selection);
                g->selection = NULL;
                owned_tbl = compacted;
                tbl = compacted;
            }
            ray_t* result = exec_pivot(g, op, tbl);
            if (owned_tbl) ray_release(owned_tbl);
            return result;
        }

        case OP_JOIN: {
            ray_t* left = exec_node(g, op->inputs[0]);
            ray_t* right = exec_node(g, op->inputs[1]);
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            /* Compact lazy selection before join (needs dense data) */
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection);
                ray_release(left);
                ray_release(g->selection);
                g->selection = NULL;
                left = compacted;
            }
            ray_t* result = exec_join(g, op, left, right);
            ray_release(left);
            ray_release(right);
            return result;
        }

        case OP_ANTIJOIN: {
            ray_t* left = exec_node(g, op->inputs[0]);
            ray_t* right = exec_node(g, op->inputs[1]);
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection);
                ray_release(left);
                ray_release(g->selection);
                g->selection = NULL;
                left = compacted;
            }
            ray_t* result = exec_antijoin(g, op, left, right);
            ray_release(left);
            ray_release(right);
            return result;
        }

        case OP_WINDOW_JOIN: {
            ray_t* left = exec_node(g, op->inputs[0]);
            ray_t* right = exec_node(g, op->inputs[1]);
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection);
                ray_release(left);
                ray_release(g->selection);
                g->selection = NULL;
                left = compacted;
            }
            ray_t* result = exec_window_join(g, op, left, right);
            ray_release(left);
            ray_release(right);
            return result;
        }

        case OP_WINDOW: {
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* wdf = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before window (needs dense data) */
            if (g->selection && wdf && !RAY_IS_ERR(wdf) && wdf->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, wdf, g->selection);
                if (input != g->table) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                wdf = compacted;
            }
            ray_t* result = exec_window(g, op, wdf);
            if (input != g->table) ray_release(input);
            return result;
        }

        case OP_HEAD: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            int64_t n = ext ? ext->sym : 10;

            /* Fused sort+limit: detect SORT child → only gather N rows */
            ray_op_t* child_op = op->inputs[0];
            if (child_op && child_op->opcode == OP_SORT) {
                ray_t* sort_input = exec_node(g, child_op->inputs[0]);
                if (!sort_input || RAY_IS_ERR(sort_input)) return sort_input;
                ray_t* tbl = (sort_input->type == RAY_TABLE) ? sort_input : g->table;
                /* Compact lazy selection before sort */
                if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, tbl, g->selection);
                    if (sort_input != g->table) ray_release(sort_input);
                    ray_release(g->selection);
                    g->selection = NULL;
                    sort_input = compacted;
                    tbl = compacted;
                }
                ray_t* result = exec_sort(g, child_op, tbl, n);
                if (sort_input != g->table) ray_release(sort_input);
                return result;
            }

            /* HEAD(GROUP) optimization: pass limit hint to exec_group
             * so it can short-circuit the per-partition loop when all
             * GROUP BY keys are MAPCOMMON.  The normal HEAD logic below
             * still trims the result to N rows regardless. */
            ray_t* input;
            if (child_op && child_op->opcode == OP_GROUP) {
                ray_t* tbl = g->table;
                if (!tbl || RAY_IS_ERR(tbl)) return tbl;
                ray_t* owned_tbl = NULL;
                if (g->selection && tbl->type == RAY_TABLE) {
                    int needs = 0;
                    int64_t nc = ray_table_ncols(tbl);
                    for (int64_t c = 0; c < nc; c++) {
                        ray_t* col = ray_table_get_col_idx(tbl, c);
                        if (col && !RAY_IS_PARTED(col->type)
                            && col->type != RAY_MAPCOMMON) {
                            needs = 1; break;
                        }
                    }
                    if (needs) {
                        ray_t* compacted = sel_compact(g, tbl, g->selection);
                        if (!compacted || RAY_IS_ERR(compacted)) return compacted;
                        ray_release(g->selection);
                        g->selection = NULL;
                        owned_tbl = compacted;
                        tbl = compacted;
                    }
                }
                input = exec_group(g, child_op, tbl, n);
                if (owned_tbl) ray_release(owned_tbl);
            } else if (child_op && child_op->opcode == OP_FILTER) {
                /* HEAD(FILTER): early-termination filter — gather only
                 * the first N matching rows instead of all matches. */
                ray_t* filter_input = exec_node(g, child_op->inputs[0]);
                if (!filter_input || RAY_IS_ERR(filter_input))
                    return filter_input;

                /* Compact lazy selection before filter evaluation */
                ray_t* ftbl = (filter_input->type == RAY_TABLE)
                           ? filter_input : g->table;
                if (g->selection && ftbl && ftbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, ftbl, g->selection);
                    if (filter_input != g->table) ray_release(filter_input);
                    ray_release(g->selection);
                    g->selection = NULL;
                    filter_input = compacted;
                    ftbl = compacted;
                }

                /* Swap table for predicate evaluation */
                ray_t* saved_table = g->table;
                g->table = ftbl;
                ray_t* pred = exec_node(g, child_op->inputs[1]);
                g->table = saved_table;

                if (!pred || RAY_IS_ERR(pred)) {
                    if (filter_input != saved_table)
                        ray_release(filter_input);
                    return pred;
                }

                ray_t* result = exec_filter_head(ftbl, pred, n);
                ray_release(pred);
                if (filter_input != saved_table)
                    ray_release(filter_input);
                return result;
            } else {
                input = exec_node(g, op->inputs[0]);
            }
            if (!input || RAY_IS_ERR(input)) return input;
            if (input->type == RAY_TABLE) {
                int64_t ncols = ray_table_ncols(input);
                int64_t nrows = ray_table_nrows(input);
                if (n > nrows) n = nrows;
                ray_t* result = ray_table_new(ncols);
                for (int64_t c = 0; c < ncols; c++) {
                    ray_t* col = ray_table_get_col_idx(input, c);
                    int64_t name_id = ray_table_col_name(input, c);
                    if (!col) continue;
                    if (col->type == RAY_MAPCOMMON) {
                        ray_t* mc_head = materialize_mapcommon_head(col, n);
                        if (mc_head && !RAY_IS_ERR(mc_head)) {
                            result = ray_table_add_col(result, name_id, mc_head);
                            ray_release(mc_head);
                        }
                        continue;
                    }
                    if (RAY_IS_PARTED(col->type)) {
                        /* Copy first n rows from parted segments */
                        int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                        ray_t** sp = (ray_t**)ray_data(col);
                        uint8_t ba = (base == RAY_SYM && col->len > 0 && sp[0])
                                   ? sp[0]->attrs : 0;
                        uint8_t esz = ray_sym_elem_size(base, ba);
                        ray_t* head_vec = typed_vec_new(base, ba, n);
                        if (head_vec && !RAY_IS_ERR(head_vec)) {
                            head_vec->len = n;
                            ray_t** segs = (ray_t**)ray_data(col);
                            int64_t remaining = n;
                            int64_t dst_off = 0;
                            for (int64_t s = 0; s < col->len && remaining > 0; s++) {
                                int64_t take = segs[s]->len;
                                if (take > remaining) take = remaining;
                                memcpy((char*)ray_data(head_vec) + dst_off * esz,
                                       ray_data(segs[s]), (size_t)take * esz);
                                dst_off += take;
                                remaining -= take;
                            }
                        }
                        result = ray_table_add_col(result, name_id, head_vec);
                        ray_release(head_vec);
                    } else {
                        /* Flat column: direct copy */
                        uint8_t esz = col_esz(col);
                        ray_t* head_vec = col_vec_new(col, n);
                        if (head_vec && !RAY_IS_ERR(head_vec)) {
                            head_vec->len = n;
                            memcpy(ray_data(head_vec), ray_data(col),
                                   (size_t)n * esz);
                        }
                        result = ray_table_add_col(result, name_id, head_vec);
                        ray_release(head_vec);
                    }
                }
                ray_release(input);
                return result;
            }
            if (n > input->len) n = input->len;
            /* Materialized copy for vector head */
            uint8_t esz = col_esz(input);
            ray_t* result = col_vec_new(input, n);
            if (result && !RAY_IS_ERR(result)) {
                result->len = n;
                memcpy(ray_data(result), ray_data(input), (size_t)n * esz);
            }
            ray_release(input);
            return result;
        }

        case OP_TAIL: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            int64_t n = ext ? ext->sym : 10;
            if (input->type == RAY_TABLE) {
                int64_t ncols = ray_table_ncols(input);
                int64_t nrows = ray_table_nrows(input);
                if (n > nrows) n = nrows;
                int64_t skip = nrows - n;
                ray_t* result = ray_table_new(ncols);
                for (int64_t c = 0; c < ncols; c++) {
                    ray_t* col = ray_table_get_col_idx(input, c);
                    int64_t name_id = ray_table_col_name(input, c);
                    if (!col) continue;
                    if (col->type == RAY_MAPCOMMON) {
                        /* Materialize last N rows from MAPCOMMON partitions */
                        ray_t** mc_ptrs = (ray_t**)ray_data(col);
                        ray_t* kv = mc_ptrs[0];
                        ray_t* rc = mc_ptrs[1];
                        int64_t n_parts = kv->len;
                        size_t esz = (size_t)col_esz(kv);
                        const char* kdata = (const char*)ray_data(kv);
                        const int64_t* counts = (const int64_t*)ray_data(rc);
                        ray_t* flat = col_vec_new(kv, n);
                        if (flat && !RAY_IS_ERR(flat)) {
                            flat->len = n;
                            char* out = (char*)ray_data(flat);
                            /* Walk partitions from end, fill output from end */
                            int64_t remaining = n;
                            int64_t dst = n;
                            for (int64_t p = n_parts - 1; p >= 0 && remaining > 0; p--) {
                                int64_t take = counts[p];
                                if (take > remaining) take = remaining;
                                dst -= take;
                                for (int64_t r = 0; r < take; r++)
                                    memcpy(out + (dst + r) * esz, kdata + (size_t)p * esz, esz);
                                remaining -= take;
                            }
                        }
                        result = ray_table_add_col(result, name_id, flat);
                        ray_release(flat);
                        continue;
                    }
                    if (RAY_IS_PARTED(col->type)) {
                        /* Copy last N rows from parted segments */
                        int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                        ray_t** tsp = (ray_t**)ray_data(col);
                        uint8_t tba = (base == RAY_SYM && col->len > 0 && tsp[0])
                                    ? tsp[0]->attrs : 0;
                        uint8_t esz = ray_sym_elem_size(base, tba);
                        ray_t* tail_vec = typed_vec_new(base, tba, n);
                        if (tail_vec && !RAY_IS_ERR(tail_vec)) {
                            tail_vec->len = n;
                            ray_t** segs = (ray_t**)ray_data(col);
                            int64_t remaining = n;
                            int64_t dst = n;
                            for (int64_t s = col->len - 1; s >= 0 && remaining > 0; s--) {
                                int64_t take = segs[s]->len;
                                if (take > remaining) take = remaining;
                                dst -= take;
                                memcpy((char*)ray_data(tail_vec) + (size_t)dst * esz,
                                       (char*)ray_data(segs[s]) + (size_t)(segs[s]->len - take) * esz,
                                       (size_t)take * esz);
                                remaining -= take;
                            }
                        }
                        result = ray_table_add_col(result, name_id, tail_vec);
                        ray_release(tail_vec);
                    } else {
                        /* Flat column: direct copy */
                        uint8_t esz = col_esz(col);
                        ray_t* tail_vec = col_vec_new(col, n);
                        if (tail_vec && !RAY_IS_ERR(tail_vec)) {
                            tail_vec->len = n;
                            memcpy(ray_data(tail_vec),
                                   (char*)ray_data(col) + (size_t)skip * esz,
                                   (size_t)n * esz);
                        }
                        result = ray_table_add_col(result, name_id, tail_vec);
                        ray_release(tail_vec);
                    }
                }
                ray_release(input);
                return result;
            }
            if (n > input->len) n = input->len;
            int64_t skip = input->len - n;
            uint8_t esz = col_esz(input);
            ray_t* result = col_vec_new(input, n);
            if (result && !RAY_IS_ERR(result)) {
                result->len = n;
                memcpy(ray_data(result),
                       (char*)ray_data(input) + (size_t)skip * esz,
                       (size_t)n * esz);
            }
            ray_release(input);
            return result;
        }

        case OP_IF: {
            return exec_if(g, op);
        }

        case OP_LIKE: {
            return exec_like(g, op);
        }

        case OP_ILIKE: {
            return exec_ilike(g, op);
        }

        case OP_UPPER: case OP_LOWER: case OP_TRIM: {
            return exec_string_unary(g, op);
        }
        case OP_STRLEN: {
            return exec_strlen(g, op);
        }
        case OP_SUBSTR: {
            return exec_substr(g, op);
        }
        case OP_REPLACE: {
            return exec_replace(g, op);
        }
        case OP_CONCAT: {
            return exec_concat(g, op);
        }

        case OP_EXTRACT: {
            return exec_extract(g, op);
        }

        case OP_DATE_TRUNC: {
            return exec_date_trunc(g, op);
        }

        case OP_ALIAS: {
            return exec_node(g, op->inputs[0]);
        }

        case OP_MATERIALIZE: {
            return exec_node(g, op->inputs[0]);
        }

        case OP_SELECT: {
            /* Column projection: select/compute columns from input table */
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            if (input->type != RAY_TABLE) {
                ray_release(input);
                return ray_error("nyi", NULL);
            }
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext) { ray_release(input); return ray_error("nyi", NULL); }
            uint8_t n_cols = ext->sort.n_cols;
            ray_op_t** columns = ext->sort.columns;
            ray_t* result = ray_table_new(n_cols);

            /* Set g->table so SCAN nodes inside expressions resolve correctly */
            ray_t* saved_table = g->table;
            g->table = input;

            for (uint8_t c = 0; c < n_cols; c++) {
                if (columns[c]->opcode == OP_SCAN) {
                    /* Direct column reference — copy from input table */
                    ray_op_ext_t* col_ext = find_ext(g, columns[c]->id);
                    if (!col_ext) continue;
                    int64_t name_id = col_ext->sym;
                    ray_t* src_col = ray_table_get_col(input, name_id);
                    if (src_col) {
                        ray_retain(src_col);
                        result = ray_table_add_col(result, name_id, src_col);
                        ray_release(src_col);
                    }
                } else {
                    /* Expression column — evaluate against input table */
                    ray_t* vec = exec_node(g, columns[c]);
                    if (!vec || RAY_IS_ERR(vec)) {
                        ray_release(result);
                        g->table = saved_table;
                        ray_release(input);
                        return vec ? vec : ray_error("nyi", NULL);
                    }
                    /* Broadcast scalar atoms to full column vectors */
                    if (vec->type < 0) {
                        int64_t nr = ray_table_nrows(input);
                        ray_t* col = broadcast_scalar(vec, nr);
                        ray_release(vec);
                        vec = col;
                        if (!vec || RAY_IS_ERR(vec)) {
                            ray_release(result);
                            g->table = saved_table;
                            ray_release(input);
                            return vec ? vec : ray_error("nyi", NULL);
                        }
                    }
                    /* Synthetic name: _expr_0, _expr_1, ... */
                    char name_buf[16];
                    int n = 0;
                    name_buf[n++] = '_'; name_buf[n++] = 'e';
                    if (c >= 100) name_buf[n++] = '0' + (c / 100);
                    if (c >= 10)  name_buf[n++] = '0' + ((c / 10) % 10);
                    name_buf[n++] = '0' + (c % 10);
                    int64_t name_id = ray_sym_intern(name_buf, (size_t)n);
                    result = ray_table_add_col(result, name_id, vec);
                    ray_release(vec);
                }
            }

            g->table = saved_table;
            ray_release(input);
            return result;
        }

        case OP_EXPAND: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_expand(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_VAR_EXPAND: {
            ray_t* start = exec_node(g, op->inputs[0]);
            if (!start || RAY_IS_ERR(start)) return start;
            ray_t* result = exec_var_expand(g, op, start);
            ray_release(start);
            return result;
        }

        case OP_SHORTEST_PATH: {
            ray_t* src = exec_node(g, op->inputs[0]);
            ray_t* dst = exec_node(g, op->inputs[1]);
            if (!src || RAY_IS_ERR(src)) {
                if (dst && !RAY_IS_ERR(dst)) ray_release(dst);
                return src;
            }
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_shortest_path(g, op, src, dst);
            ray_release(src);
            ray_release(dst);
            return result;
        }

        case OP_WCO_JOIN: {
            return exec_wco_join(g, op);
        }

        case OP_PAGERANK: {
            return exec_pagerank(g, op);
        }

        case OP_CONNECTED_COMP: {
            return exec_connected_comp(g, op);
        }

        case OP_DIJKSTRA: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = op->inputs[1] ? exec_node(g, op->inputs[1]) : NULL;
            if (dst && RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_dijkstra(g, op, src, dst);
            ray_release(src);
            if (dst) ray_release(dst);
            return result;
        }

        case OP_LOUVAIN: {
            return exec_louvain(g, op);
        }

        case OP_DEGREE_CENT: {
            return exec_degree_cent(g, op);
        }

        case OP_TOPSORT: {
            return exec_topsort(g, op);
        }

        case OP_DFS: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_dfs(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_CLUSTER_COEFF: {
            return exec_cluster_coeff(g, op);
        }

        case OP_BETWEENNESS: {
            return exec_betweenness(g, op);
        }

        case OP_CLOSENESS: {
            return exec_closeness(g, op);
        }

        case OP_MST: {
            return exec_mst(g, op);
        }

        case OP_RANDOM_WALK: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_random_walk(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_ASTAR: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = exec_node(g, op->inputs[1]);
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_astar(g, op, src, dst);
            ray_release(src); ray_release(dst);
            return result;
        }

        case OP_K_SHORTEST: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = exec_node(g, op->inputs[1]);
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_k_shortest(g, op, src, dst);
            ray_release(src); ray_release(dst);
            return result;
        }

        case OP_COSINE_SIM: {
            ray_t* emb = exec_node(g, op->inputs[0]);
            if (!emb || RAY_IS_ERR(emb)) return emb;
            ray_t* result = exec_cosine_sim(g, op, emb);
            ray_release(emb);
            return result;
        }
        case OP_EUCLIDEAN_DIST: {
            ray_t* emb = exec_node(g, op->inputs[0]);
            if (!emb || RAY_IS_ERR(emb)) return emb;
            ray_t* result = exec_euclidean_dist(g, op, emb);
            ray_release(emb);
            return result;
        }
        case OP_KNN: {
            ray_t* emb = exec_node(g, op->inputs[0]);
            if (!emb || RAY_IS_ERR(emb)) return emb;
            ray_t* result = exec_knn(g, op, emb);
            ray_release(emb);
            return result;
        }
        case OP_HNSW_KNN: {
            return exec_hnsw_knn(g, op);
        }

        default:
            return ray_error("nyi", NULL);
    }
}

/* ============================================================================
 * ray_execute -- top-level entry point (lazy pool init)
 * ============================================================================ */

ray_t* ray_execute(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return ray_error("nyi", NULL);

    /* Lazy-init the global thread pool on first call */
    ray_pool_t* pool = ray_pool_get();

    /* Reset cancellation flag at the start of each query */
    if (pool)
        atomic_store_explicit(&pool->cancelled, 0, memory_order_relaxed);

    ray_t* result = exec_node(g, root);

    /* Final compaction: if a lazy selection remains unconsumed (e.g., filter
     * followed directly by a terminal node), materialize it now. */
    if (g->selection && result && !RAY_IS_ERR(result)
        && result->type == RAY_TABLE) {
        ray_t* compacted = sel_compact(g, result, g->selection);
        ray_release(result);
        ray_release(g->selection);
        g->selection = NULL;
        result = compacted;
    }
    return result;
}
