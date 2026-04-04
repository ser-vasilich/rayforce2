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
