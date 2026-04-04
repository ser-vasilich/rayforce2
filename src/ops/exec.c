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
 * Reduction execution
 * ============================================================================ */

typedef struct {
    double sum_f, min_f, max_f, prod_f, first_f, last_f, sum_sq_f;
    int64_t sum_i, min_i, max_i, prod_i, first_i, last_i, sum_sq_i;
    int64_t cnt;
    int64_t null_count;
    bool has_first;
} reduce_acc_t;

static void reduce_acc_init(reduce_acc_t* acc) {
    acc->sum_f = 0; acc->min_f = DBL_MAX; acc->max_f = -DBL_MAX;
    acc->prod_f = 1.0; acc->first_f = 0; acc->last_f = 0; acc->sum_sq_f = 0;
    acc->sum_i = 0; acc->min_i = INT64_MAX; acc->max_i = INT64_MIN;
    acc->prod_i = 1; acc->first_i = 0; acc->last_i = 0; acc->sum_sq_i = 0;
    acc->cnt = 0; acc->null_count = 0; acc->has_first = false;
}

/* Integer reduction loop — reads native type T, accumulates as i64 */
#define REDUCE_LOOP_I(T, base, start, end, acc, has_nulls, null_bm) \
    do { \
        const T* d = (const T*)(base); \
        for (int64_t row = start; row < end; row++) { \
            if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { (acc)->null_count++; continue; } \
            int64_t v = (int64_t)d[row]; \
            (acc)->sum_i += v; (acc)->sum_sq_i += v * v; \
            (acc)->prod_i = (int64_t)((uint64_t)(acc)->prod_i * (uint64_t)v); \
            if (v < (acc)->min_i) (acc)->min_i = v; \
            if (v > (acc)->max_i) (acc)->max_i = v; \
            if (!(acc)->has_first) { (acc)->first_i = v; (acc)->has_first = true; } \
            (acc)->last_i = v; (acc)->cnt++; \
        } \
    } while (0)

/* Float reduction loop */
#define REDUCE_LOOP_F(base, start, end, acc, has_nulls, null_bm) \
    do { \
        const double* d = (const double*)(base); \
        for (int64_t row = start; row < end; row++) { \
            if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { (acc)->null_count++; continue; } \
            double v = d[row]; \
            (acc)->sum_f += v; (acc)->sum_sq_f += v * v; (acc)->prod_f *= v; \
            if (v < (acc)->min_f) (acc)->min_f = v; \
            if (v > (acc)->max_f) (acc)->max_f = v; \
            if (!(acc)->has_first) { (acc)->first_f = v; (acc)->has_first = true; } \
            (acc)->last_f = v; (acc)->cnt++; \
        } \
    } while (0)

static void reduce_range(ray_t* input, int64_t start, int64_t end,
                         reduce_acc_t* acc, bool has_nulls,
                         const uint8_t* null_bm) {
    void* base = ray_data(input);
    switch (input->type) {
    case RAY_BOOL: case RAY_U8:
        REDUCE_LOOP_I(uint8_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_I16:
        REDUCE_LOOP_I(int16_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        REDUCE_LOOP_I(int32_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_I64: case RAY_TIMESTAMP:
        REDUCE_LOOP_I(int64_t, base, start, end, acc, has_nulls, null_bm); break;
    case RAY_F64:
        REDUCE_LOOP_F(base, start, end, acc, has_nulls, null_bm); break;
    case RAY_SYM: {
        /* Adaptive-width SYM columns — use read_col_i64 */
        for (int64_t row = start; row < end; row++) {
            if (has_nulls && (null_bm[row/8] >> (row%8)) & 1) { acc->null_count++; continue; }
            int64_t v = read_col_i64(base, row, input->type, input->attrs);
            acc->sum_i += v; acc->sum_sq_i += v * v;
            acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
            if (v < acc->min_i) acc->min_i = v;
            if (v > acc->max_i) acc->max_i = v;
            if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
            acc->last_i = v; acc->cnt++;
        }
        break;
    }
    default: break;
    }
}

/* Context for parallel reduction */
typedef struct {
    ray_t*         input;
    reduce_acc_t*  accs;   /* one per worker */
    bool           has_nulls;
    const uint8_t* null_bm;
} par_reduce_ctx_t;

static void par_reduce_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    par_reduce_ctx_t* c = (par_reduce_ctx_t*)ctx;
    reduce_range(c->input, start, end, &c->accs[worker_id],
                 c->has_nulls, c->null_bm);
}

static void reduce_merge(reduce_acc_t* dst, const reduce_acc_t* src, int8_t in_type) {
    if (in_type == RAY_F64) {
        dst->sum_f += src->sum_f;
        dst->sum_sq_f += src->sum_sq_f;
        dst->prod_f *= src->prod_f;
        if (src->min_f < dst->min_f) dst->min_f = src->min_f;
        if (src->max_f > dst->max_f) dst->max_f = src->max_f;
    } else {
        dst->sum_i += src->sum_i;
        dst->sum_sq_i += src->sum_sq_i;
        dst->prod_i *= src->prod_i;
        if (src->min_i < dst->min_i) dst->min_i = src->min_i;
        if (src->max_i > dst->max_i) dst->max_i = src->max_i;
    }
    dst->cnt += src->cnt;
    dst->null_count += src->null_count;
    /* reduce_merge does not merge first/last; caller handles these separately.
     * Since workers process sequential ranges, worker 0's first is the global first,
     * and the last worker's last is the global last. */
}

/* Hash-based count distinct for integer/float columns */
static ray_t* exec_count_distinct(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g; (void)op;
    if (!input || RAY_IS_ERR(input)) return input;

    int8_t in_type = input->type;
    int64_t len = input->len;

    if (len == 0) return ray_i64(0);

    /* Only numeric/ordinal/sym column types are supported */
    switch (in_type) {
    case RAY_BOOL: case RAY_U8:
    case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_F64: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
    case RAY_SYM:
        break;
    default:
        return ray_error("type", NULL);
    }

    /* Use a simple open-addressing hash set for int64 values */
    uint64_t cap = (uint64_t)(len < 16 ? 32 : len) * 2;
    /* Round up to power of 2 */
    uint64_t c = 1;
    while (c && c < cap) c <<= 1;
    if (!c) return ray_error("oom", NULL); /* overflow: cap too large */
    cap = c;

    ray_t* set_hdr;
    int64_t* set = (int64_t*)scratch_calloc(&set_hdr,
                                             (size_t)cap * sizeof(int64_t));
    ray_t* used_hdr;
    uint8_t* used = (uint8_t*)scratch_calloc(&used_hdr,
                                              (size_t)cap * sizeof(uint8_t));
    if (!set || !used) {
        if (set_hdr) scratch_free(set_hdr);
        if (used_hdr) scratch_free(used_hdr);
        return ray_error("oom", NULL);
    }

    int64_t count = 0;
    uint64_t mask = cap - 1;
    void* base = ray_data(input);

    for (int64_t i = 0; i < len; i++) {
        int64_t val;
        if (in_type == RAY_F64) {
            double fv = ((double*)base)[i];
            /* Normalize: NaN → canonical NaN, -0.0 → +0.0 */
            if (fv != fv) fv = (double)NAN;        /* canonical NaN */
            else if (fv == 0.0) fv = 0.0;          /* +0.0 */
            memcpy(&val, &fv, sizeof(int64_t));
        } else {
            val = read_col_i64(base, i, in_type, input->attrs);
        }

        /* Open-addressing linear probe */
        uint64_t h = (uint64_t)val * 0x9E3779B97F4A7C15ULL;
        uint64_t slot = h & mask;
        while (used[slot]) {
            if (set[slot] == val) goto next_val;
            slot = (slot + 1) & mask;
        }
        /* New distinct value */
        set[slot] = val;
        used[slot] = 1;
        count++;
        next_val:;
    }

    scratch_free(set_hdr);
    scratch_free(used_hdr);
    return ray_i64(count);
}

static ray_t* exec_reduction(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g;
    if (!input || RAY_IS_ERR(input)) return input;

    /* TABLE input: COUNT returns row count, others need a column */
    if (input->type == RAY_TABLE) {
        if (op->opcode == OP_COUNT)
            return ray_i64(ray_table_nrows(input));
        return ray_error("type", NULL);
    }

    int8_t in_type = input->type;
    int64_t len = input->len;

    /* Resolve null bitmap once before dispatching */
    bool has_nulls = (input->attrs & RAY_ATTR_HAS_NULLS) != 0;
    const uint8_t* null_bm = NULL;
    if (has_nulls) {
        if (input->attrs & RAY_ATTR_NULLMAP_EXT)
            null_bm = (const uint8_t*)ray_data(input->ext_nullmap);
        else
            null_bm = input->nullmap;
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && len >= RAY_PARALLEL_THRESHOLD) {
        uint32_t nw = ray_pool_total_workers(pool);
        ray_t* accs_hdr;
        reduce_acc_t* accs = (reduce_acc_t*)scratch_calloc(&accs_hdr, nw * sizeof(reduce_acc_t));
        if (!accs) return ray_error("oom", NULL);
        for (uint32_t i = 0; i < nw; i++) reduce_acc_init(&accs[i]);

        par_reduce_ctx_t ctx = { .input = input, .accs = accs,
                                 .has_nulls = has_nulls, .null_bm = null_bm };
        ray_pool_dispatch(pool, par_reduce_fn, &ctx, len);

        /* Merge: worker 0 is the base, merge the rest in order */
        reduce_acc_t merged;
        reduce_acc_init(&merged);
        merged = accs[0];
        for (uint32_t i = 1; i < nw; i++) {
            if (!accs[i].has_first) continue;
            reduce_merge(&merged, &accs[i], in_type);
        }
        /* first = accs[first worker with data], last = accs[last worker with data] */
        for (uint32_t i = 0; i < nw; i++) {
            if (accs[i].has_first) {
                if (in_type == RAY_F64) merged.first_f = accs[i].first_f;
                else merged.first_i = accs[i].first_i;
                break;
            }
        }
        for (int32_t i = (int32_t)nw - 1; i >= 0; i--) {
            if (accs[i].has_first) {
                if (in_type == RAY_F64) merged.last_f = accs[i].last_f;
                else merged.last_i = accs[i].last_i;
                break;
            }
        }

        ray_t* result;
        switch (op->opcode) {
            case OP_SUM:   result = in_type == RAY_F64 ? ray_f64(merged.sum_f) : ray_i64(merged.sum_i); break;
            case OP_PROD:  result = in_type == RAY_F64 ? ray_f64(merged.prod_f) : ray_i64(merged.prod_i); break;
            case OP_MIN:   result = in_type == RAY_F64 ? ray_f64(merged.cnt > 0 ? merged.min_f : 0.0) : ray_i64(merged.cnt > 0 ? merged.min_i : 0); break;
            case OP_MAX:   result = in_type == RAY_F64 ? ray_f64(merged.cnt > 0 ? merged.max_f : 0.0) : ray_i64(merged.cnt > 0 ? merged.max_i : 0); break;
            case OP_COUNT: result = ray_i64(merged.cnt); break;
            case OP_AVG:   result = in_type == RAY_F64 ? ray_f64(merged.cnt > 0 ? merged.sum_f / merged.cnt : 0.0) : ray_f64(merged.cnt > 0 ? (double)merged.sum_i / merged.cnt : 0.0); break;
            case OP_FIRST: result = in_type == RAY_F64 ? ray_f64(merged.first_f) : ray_i64(merged.first_i); break;
            case OP_LAST:  result = in_type == RAY_F64 ? ray_f64(merged.last_f) : ray_i64(merged.last_i); break;
            case OP_VAR: case OP_VAR_POP:
            case OP_STDDEV: case OP_STDDEV_POP: {
                double mean, var_pop;
                if (in_type == RAY_F64) { mean = merged.sum_f / merged.cnt; var_pop = merged.sum_sq_f / merged.cnt - mean * mean; }
                else { mean = (double)merged.sum_i / merged.cnt; var_pop = (double)merged.sum_sq_i / merged.cnt - mean * mean; }
                if (var_pop < 0) var_pop = 0;
                double val;
                if (op->opcode == OP_VAR_POP) val = merged.cnt > 0 ? var_pop : NAN;
                else if (op->opcode == OP_VAR) val = merged.cnt > 1 ? var_pop * merged.cnt / (merged.cnt - 1) : NAN;
                else if (op->opcode == OP_STDDEV_POP) val = merged.cnt > 0 ? sqrt(var_pop) : NAN;
                else val = merged.cnt > 1 ? sqrt(var_pop * merged.cnt / (merged.cnt - 1)) : NAN;
                result = ray_f64(val);
                break;
            }
            default:       result = ray_error("nyi", NULL); break;
        }
        scratch_free(accs_hdr);
        return result;
    }

    reduce_acc_t acc;
    reduce_acc_init(&acc);
    reduce_range(input, 0, len, &acc, has_nulls, null_bm);

    switch (op->opcode) {
        case OP_SUM:   return in_type == RAY_F64 ? ray_f64(acc.sum_f) : ray_i64(acc.sum_i);
        case OP_PROD:  return in_type == RAY_F64 ? ray_f64(acc.prod_f) : ray_i64(acc.prod_i);
        case OP_MIN:   return in_type == RAY_F64 ? ray_f64(acc.cnt > 0 ? acc.min_f : 0.0) : ray_i64(acc.cnt > 0 ? acc.min_i : 0);
        case OP_MAX:   return in_type == RAY_F64 ? ray_f64(acc.cnt > 0 ? acc.max_f : 0.0) : ray_i64(acc.cnt > 0 ? acc.max_i : 0);
        case OP_COUNT: return ray_i64(acc.cnt);
        case OP_AVG:   return in_type == RAY_F64 ? ray_f64(acc.cnt > 0 ? acc.sum_f / acc.cnt : 0.0) : ray_f64(acc.cnt > 0 ? (double)acc.sum_i / acc.cnt : 0.0);
        case OP_FIRST: return in_type == RAY_F64 ? ray_f64(acc.first_f) : ray_i64(acc.first_i);
        case OP_LAST:  return in_type == RAY_F64 ? ray_f64(acc.last_f) : ray_i64(acc.last_i);
        case OP_VAR: case OP_VAR_POP:
        case OP_STDDEV: case OP_STDDEV_POP: {
            double mean, var_pop;
            if (in_type == RAY_F64) { mean = acc.sum_f / acc.cnt; var_pop = acc.sum_sq_f / acc.cnt - mean * mean; }
            else { mean = (double)acc.sum_i / acc.cnt; var_pop = (double)acc.sum_sq_i / acc.cnt - mean * mean; }
            if (var_pop < 0) var_pop = 0;
            double val;
            if (op->opcode == OP_VAR_POP) val = acc.cnt > 0 ? var_pop : NAN;
            else if (op->opcode == OP_VAR) val = acc.cnt > 1 ? var_pop * acc.cnt / (acc.cnt - 1) : NAN;
            else if (op->opcode == OP_STDDEV_POP) val = acc.cnt > 0 ? sqrt(var_pop) : NAN;
            else val = acc.cnt > 1 ? sqrt(var_pop * acc.cnt / (acc.cnt - 1)) : NAN;
            return ray_f64(val);
        }
        default:       return ray_error("nyi", NULL);
    }
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
 * Group-by execution — with parallel local hash tables + merge
 * ============================================================================ */

/* Hash using ray_t** (used by join code) */
static uint64_t hash_row_keys(ray_t** key_vecs, uint8_t n_keys, int64_t row) {
    uint64_t h = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* col = key_vecs[k];
        if (!col) continue;
        uint64_t kh;
        if (col->type == RAY_F64)
            kh = ray_hash_f64(((double*)ray_data(col))[row]);
        else
            kh = ray_hash_i64(read_col_i64(ray_data(col), row, col->type, col->attrs));
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    return h;
}

/* Extract salt from hash (upper 16 bits) for fast mismatch rejection */
#define HT_SALT(h) ((uint8_t)((h) >> 56))

/* Flags controlling which accumulator arrays are allocated */
#define GHT_NEED_SUM   0x01
#define GHT_NEED_MIN   0x02
#define GHT_NEED_MAX   0x04
#define GHT_NEED_SUMSQ 0x08

/* ── Row-layout HT ──────────────────────────────────────────────────────
 * Keys + accumulators stored inline in both radix entries and group rows.
 * After phase1 copies data from original columns, phase2 and phase3 never
 * touch column data again — all access is sequential/local.
 * ────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t entry_stride;    /* bytes per radix entry: 8 + n_keys*8 + n_agg_vals*8 */
    uint16_t row_stride;      /* bytes per group row: 8 + n_keys*8 + accum_bytes */
    uint8_t  n_keys;
    uint8_t  n_aggs;
    uint8_t  n_agg_vals;      /* non-NULL agg columns (excludes COUNT) */
    uint8_t  need_flags;
    uint8_t  agg_is_f64;      /* bitmask: bit a set => agg[a] source is f64 */
    uint8_t  agg_is_first;   /* bitmask: bit a set => agg[a] is OP_FIRST */
    uint8_t  agg_is_last;    /* bitmask: bit a set => agg[a] is OP_LAST  */
    int8_t   agg_val_slot[8]; /* agg_idx -> entry/accum slot (-1 = no value) */
    /* Unified accumulator offsets: each block is n_agg_vals * 8 bytes.
     * Each 8B slot is double or int64_t based on agg_is_f64 bitmask. */
    uint16_t off_sum;         /* 0 => not allocated */
    uint16_t off_min;
    uint16_t off_max;
    uint16_t off_sumsq;       /* sum-of-squares for STDDEV/VAR */
} ght_layout_t;

static ght_layout_t ght_compute_layout(uint8_t n_keys, uint8_t n_aggs,
                                        ray_t** agg_vecs, uint8_t need_flags,
                                        const uint16_t* agg_ops) {
    ght_layout_t ly;
    memset(&ly, 0, sizeof(ly));
    ly.n_keys = n_keys;
    ly.n_aggs = n_aggs;
    ly.need_flags = need_flags;

    uint8_t nv = 0;
    for (uint8_t a = 0; a < n_aggs && a < 8; a++) {
        if (agg_vecs[a]) {
            ly.agg_val_slot[a] = (int8_t)nv;
            if (agg_vecs[a]->type == RAY_F64)
                ly.agg_is_f64 |= (1u << a);
            nv++;
        } else {
            ly.agg_val_slot[a] = -1;
        }
        if (agg_ops) {
            if (agg_ops[a] == OP_FIRST) ly.agg_is_first |= (1u << a);
            if (agg_ops[a] == OP_LAST)  ly.agg_is_last  |= (1u << a);
        }
    }
    ly.n_agg_vals = nv;
    ly.entry_stride = (uint16_t)(8 + (uint16_t)n_keys * 8 + (uint16_t)nv * 8);

    uint16_t off = (uint16_t)(8 + (uint16_t)n_keys * 8);
    uint16_t block = (uint16_t)nv * 8;
    if (need_flags & GHT_NEED_SUM)   { ly.off_sum   = off; off += block; }
    if (need_flags & GHT_NEED_MIN)   { ly.off_min   = off; off += block; }
    if (need_flags & GHT_NEED_MAX)   { ly.off_max   = off; off += block; }
    if (need_flags & GHT_NEED_SUMSQ) { ly.off_sumsq = off; off += block; }
    ly.row_stride = off;
    return ly;
}

/* Packed HT slots: [salt:8 | gid:24] in 4 bytes.
 * Max groups per HT = 16M (24 bits) — ample for partitioned probes.
 * 4B slots halve cache footprint vs 8B, fitting HT in L2. */
#define HT_EMPTY    UINT32_MAX
#define HT_PACK(salt, gid)  (((uint32_t)(uint8_t)(salt) << 24) | ((gid) & 0xFFFFFF))
#define HT_GID(s)   ((s) & 0xFFFFFF)
#define HT_SALT_V(s) ((uint8_t)((s) >> 24))

typedef struct {
    uint32_t*    slots;       /* packed [salt:8|gid:24], HT_EMPTY=empty */
    uint32_t     ht_cap;
    char*        rows;        /* flat row store: rows + gid * layout.row_stride */
    uint32_t     grp_count;
    uint32_t     grp_cap;
    ght_layout_t layout;
    ray_t*        _h_slots;
    ray_t*        _h_rows;
} group_ht_t;

static bool group_ht_init_sized(group_ht_t* ht, uint32_t cap,
                                 const ght_layout_t* ly, uint32_t init_grp_cap) {
    ht->ht_cap = cap;
    ht->layout = *ly;
    ht->slots = (uint32_t*)scratch_alloc(&ht->_h_slots, (size_t)cap * sizeof(uint32_t));
    if (!ht->slots) return false;
    memset(ht->slots, 0xFF, (size_t)cap * sizeof(uint32_t)); /* HT_EMPTY = all-1s */
    ht->grp_cap = init_grp_cap;
    ht->grp_count = 0;
    ht->rows = (char*)scratch_alloc(&ht->_h_rows,
        (size_t)init_grp_cap * ly->row_stride);
    if (!ht->rows) return false;
    return true;
}

static bool group_ht_init(group_ht_t* ht, uint32_t cap, const ght_layout_t* ly) {
    return group_ht_init_sized(ht, cap, ly, 256);
}

static void group_ht_free(group_ht_t* ht) {
    scratch_free(ht->_h_slots);
    scratch_free(ht->_h_rows);
}

static bool group_ht_grow(group_ht_t* ht) {
    uint32_t old_cap = ht->grp_cap;
    uint32_t new_cap = old_cap * 2;
    uint16_t rs = ht->layout.row_stride;
    char* new_rows = (char*)scratch_realloc(
        &ht->_h_rows, (size_t)old_cap * rs, (size_t)new_cap * rs);
    if (!new_rows) return false;
    ht->rows = new_rows;
    ht->grp_cap = new_cap;
    return true;
}

/* Hash inline int64_t keys (for rehash — no original column access) */
static inline uint64_t hash_keys_inline(const int64_t* keys, const int8_t* key_types,
                                         uint8_t n_keys) {
    uint64_t h = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        uint64_t kh;
        if (key_types[k] == RAY_F64) {
            double dv;
            memcpy(&dv, &keys[k], 8);
            kh = ray_hash_f64(dv);
        } else {
            kh = ray_hash_i64(keys[k]);
        }
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    return h;
}

static void group_ht_rehash(group_ht_t* ht, const int8_t* key_types) {
    uint32_t new_cap = ht->ht_cap * 2;
    ray_t* new_h = NULL;
    uint32_t* new_slots = (uint32_t*)scratch_alloc(&new_h, (size_t)new_cap * sizeof(uint32_t));
    if (!new_slots) return; /* OOM: keep old HT, it still works (just slower) */
    scratch_free(ht->_h_slots);
    ht->_h_slots = new_h;
    ht->slots = new_slots;
    memset(ht->slots, 0xFF, (size_t)new_cap * sizeof(uint32_t));
    ht->ht_cap = new_cap;
    uint32_t mask = new_cap - 1;
    uint16_t rs = ht->layout.row_stride;
    uint8_t nk = ht->layout.n_keys;
    for (uint32_t gi = 0; gi < ht->grp_count; gi++) {
        const int64_t* row_keys = (const int64_t*)(ht->rows + (size_t)gi * rs + 8);
        uint64_t h = hash_keys_inline(row_keys, key_types, nk);
        uint32_t slot = (uint32_t)(h & mask);
        while (ht->slots[slot] != HT_EMPTY)
            slot = (slot + 1) & mask;
        ht->slots[slot] = HT_PACK(HT_SALT(h), gi);
    }
}

/* Initialize accumulators for a new group from entry's inline agg values.
 * Each unified block has n_agg_vals slots of 8 bytes, typed by agg_is_f64. */
static inline void init_accum_from_entry(char* row, const char* entry,
                                          const ght_layout_t* ly) {
    uint16_t accum_start = (uint16_t)(8 + (uint16_t)ly->n_keys * 8);
    if (ly->row_stride > accum_start)
        memset(row + accum_start, 0, ly->row_stride - accum_start);

    const char* agg_data = entry + 8 + ly->n_keys * 8;
    uint8_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;

    for (uint8_t a = 0; a < na; a++) {
        int8_t s = ly->agg_val_slot[a];
        if (s < 0) continue;
        /* Copy raw 8 bytes from entry into each enabled accumulator block */
        if (nf & GHT_NEED_SUM) memcpy(row + ly->off_sum + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MIN) memcpy(row + ly->off_min + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MAX) memcpy(row + ly->off_max + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_SUMSQ) {
            /* sumsq = v * v for the first entry */
            if (ly->agg_is_f64 & (1u << a)) {
                double v; memcpy(&v, agg_data + s * 8, 8);
                double sq = v * v;
                memcpy(row + ly->off_sumsq + s * 8, &sq, 8);
            } else {
                int64_t v; memcpy(&v, agg_data + s * 8, 8);
                double sq = (double)v * (double)v;
                memcpy(row + ly->off_sumsq + s * 8, &sq, 8);
            }
        }
    }
}

/* Row-layout accessors: cast through void* for strict-aliasing safety.
 * All row offsets are 8-byte aligned by construction. */
#define ROW_RD_F64(row, off, slot) (((const double*)((const void*)((row) + (off))))[(slot)])
#define ROW_RD_I64(row, off, slot) (((const int64_t*)((const void*)((row) + (off))))[(slot)])
#define ROW_WR_F64(row, off, slot) (((double*)((void*)((row) + (off))))[(slot)])
#define ROW_WR_I64(row, off, slot) (((int64_t*)((void*)((row) + (off))))[(slot)])

/* Accumulate into existing group from entry's inline agg values */
static inline void accum_from_entry(char* row, const char* entry,
                                     const ght_layout_t* ly) {
    const char* agg_data = entry + 8 + ly->n_keys * 8;
    uint8_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;

    for (uint8_t a = 0; a < na; a++) {
        int8_t s = ly->agg_val_slot[a];
        if (s < 0) continue;
        const char* val = agg_data + s * 8;

        uint8_t amask = (1u << a);
        if (ly->agg_is_f64 & amask) {
            double v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (ly->agg_is_first & amask) { /* keep init value */ }
                else if (ly->agg_is_last & amask) { memcpy(row + ly->off_sum + s * 8, val, 8); }
                else { ROW_WR_F64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { double* p = &ROW_WR_F64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { double* p = &ROW_WR_F64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += v * v; }
        } else {
            int64_t v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (ly->agg_is_first & amask) { /* keep init value */ }
                else if (ly->agg_is_last & amask) { memcpy(row + ly->off_sum + s * 8, val, 8); }
                else { ROW_WR_I64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { int64_t* p = &ROW_WR_I64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { int64_t* p = &ROW_WR_I64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += (double)v * (double)v; }
        }
    }
}

/* Probe + accumulate a single fat entry into the HT. Returns updated mask. */
static inline uint32_t group_probe_entry(group_ht_t* ht,
    const char* entry, const int8_t* key_types, uint32_t mask) {
    const ght_layout_t* ly = &ht->layout;
    uint64_t hash = *(const uint64_t*)entry;
    const char* ekeys = entry + 8;
    uint8_t salt = HT_SALT(hash);
    uint32_t slot = (uint32_t)(hash & mask);
    uint16_t key_bytes = ly->n_keys * 8;

    for (;;) {
        uint32_t sv = ht->slots[slot];
        if (sv == HT_EMPTY) {
            /* New group */
            if (ht->grp_count >= ht->grp_cap) {
                if (!group_ht_grow(ht)) return mask; /* OOM: stop adding groups */
            }
            uint32_t gid = ht->grp_count++;
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            *(int64_t*)row = 1;   /* count = 1 */
            memcpy(row + 8, ekeys, key_bytes);
            init_accum_from_entry(row, entry, ly);
            ht->slots[slot] = HT_PACK(salt, gid);
            if (ht->grp_count * 2 > ht->ht_cap) {
                group_ht_rehash(ht, key_types);
                mask = ht->ht_cap - 1;
            }
            return mask;
        }
        if (HT_SALT_V(sv) == salt) {
            uint32_t gid = HT_GID(sv);
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            if (memcmp(row + 8, ekeys, key_bytes) == 0) {
                (*(int64_t*)row)++;   /* count++ */
                accum_from_entry(row, entry, ly);
                return mask;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* Process rows [start, end) from original columns into a local hash table.
 * Converts each row to a fat entry on the stack, then probes. */
#define GROUP_PREFETCH_BATCH 16

static void group_rows_range(group_ht_t* ht, void** key_data, int8_t* key_types,
                              uint8_t* key_attrs, ray_t** agg_vecs,
                              int64_t start, int64_t end) {
    const ght_layout_t* ly = &ht->layout;
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint32_t mask = ht->ht_cap - 1;
    /* Stack buffer for one entry (max: 8 + 8*8 + 8*8 = 136 bytes) */
    char ebuf[8 + 8 * 8 + 8 * 8];

    for (int64_t row = start; row < end; row++) {
        uint64_t h = 0;
        int64_t* ek = (int64_t*)(ebuf + 8);
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            int64_t kv;
            if (t == RAY_F64)
                memcpy(&kv, &((double*)key_data[k])[row], 8);
            else
                kv = read_col_i64(key_data[k], row, t, key_attrs[k]);
            ek[k] = kv;
            uint64_t kh = (t == RAY_F64) ? ray_hash_f64(((double*)key_data[k])[row])
                                        : ray_hash_i64(kv);
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        *(uint64_t*)ebuf = h;

        int64_t* ev = (int64_t*)(ebuf + 8 + nk * 8);
        uint8_t vi = 0;
        for (uint8_t a = 0; a < na; a++) {
            ray_t* ac = agg_vecs[a];
            if (!ac) continue;
            if (ac->type == RAY_F64)
                memcpy(&ev[vi], &((double*)ray_data(ac))[row], 8);
            else
                ev[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
        }

        mask = group_probe_entry(ht, ebuf, key_types, mask);
    }
}

/* ============================================================================
 * Radix-partitioned parallel group-by
 *
 * Phase 1 (parallel): Each worker reads keys+agg values from original columns,
 *         packs into fat entries (hash, keys, agg_vals), scatters into
 *         thread-local per-partition buffers.
 * Phase 2 (parallel): Each partition is aggregated independently using
 *         inline data — no original column access needed.
 * Phase 3: Build result columns from inline group rows.
 * ============================================================================ */

#define RADIX_BITS  8
#define RADIX_P     (1u << RADIX_BITS)   /* 256 partitions */
#define RADIX_MASK  (RADIX_P - 1)
#define RADIX_PART(h) (((uint32_t)((h) >> 16)) & RADIX_MASK)

/* Per-worker, per-partition buffer of fat entries */
typedef struct {
    char*    data;           /* flat buffer: data[i * entry_stride] */
    uint32_t count;
    uint32_t cap;
    bool     oom;            /* set on realloc failure */
    ray_t*    _hdr;
} radix_buf_t;

static inline void radix_buf_push(radix_buf_t* buf, uint16_t entry_stride,
                                   uint64_t hash, const int64_t* keys, uint8_t n_keys,
                                   const int64_t* agg_vals, uint8_t n_agg_vals) {
    if (__builtin_expect(buf->count >= buf->cap, 0)) {
        uint32_t old_cap = buf->cap;
        uint32_t new_cap = old_cap * 2;
        char* new_data = (char*)scratch_realloc(
            &buf->_hdr, (size_t)old_cap * entry_stride,
            (size_t)new_cap * entry_stride);
        if (!new_data) { buf->oom = true; return; }
        buf->data = new_data;
        buf->cap = new_cap;
    }
    char* dst = buf->data + (size_t)buf->count * entry_stride;
    *(uint64_t*)dst = hash;
    memcpy(dst + 8, keys, (size_t)n_keys * 8);
    if (n_agg_vals)
        memcpy(dst + 8 + (size_t)n_keys * 8, agg_vals, (size_t)n_agg_vals * 8);
    buf->count++;
}

typedef struct {
    void**       key_data;
    int8_t*      key_types;
    uint8_t*     key_attrs;
    ray_t**       agg_vecs;
    uint32_t     n_workers;
    radix_buf_t* bufs;        /* [n_workers * RADIX_P] */
    ght_layout_t layout;
    const uint64_t* mask;
    const uint8_t*  sel_flags; /* per-segment RAY_SEL_NONE/ALL/MIX (NULL=all pass) */
} radix_phase1_ctx_t;

static void radix_phase1_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    radix_phase1_ctx_t* c = (radix_phase1_ctx_t*)ctx;
    const ght_layout_t* ly = &c->layout;
    radix_buf_t* my_bufs = &c->bufs[(size_t)worker_id * RADIX_P];
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint8_t nv = ly->n_agg_vals;
    uint16_t estride = ly->entry_stride;
    const uint64_t* mask = c->mask;
    const uint8_t* sel_flags = c->sel_flags;

    int64_t keys[8];
    int64_t agg_vals[8];

    for (int64_t row = start; row < end; ) {
        /* Segment-level skip for RAY_SEL_NONE */
        if (sel_flags) {
            uint32_t seg = (uint32_t)(row / RAY_MORSEL_ELEMS);
            int64_t seg_end = (int64_t)(seg + 1) * RAY_MORSEL_ELEMS;
            if (seg_end > end) seg_end = end;
            if (sel_flags[seg] == RAY_SEL_NONE) { row = seg_end; continue; }
        }

        if (RAY_UNLIKELY(mask && !RAY_SEL_BIT_TEST(mask, row))) { row++; continue; }
        uint64_t h = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = c->key_types[k];
            int64_t kv;
            if (t == RAY_F64)
                memcpy(&kv, &((double*)c->key_data[k])[row], 8);
            else
                kv = read_col_i64(c->key_data[k], row, t, c->key_attrs[k]);
            keys[k] = kv;
            uint64_t kh = (t == RAY_F64) ? ray_hash_f64(((double*)c->key_data[k])[row])
                                        : ray_hash_i64(kv);
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }

        uint8_t vi = 0;
        for (uint8_t a = 0; a < na; a++) {
            ray_t* ac = c->agg_vecs[a];
            if (!ac) continue;
            if (ac->type == RAY_F64)
                memcpy(&agg_vals[vi], &((double*)ray_data(ac))[row], 8);
            else
                agg_vals[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
        }

        uint32_t part = RADIX_PART(h);
        radix_buf_push(&my_bufs[part], estride, h, keys, nk, agg_vals, nv);
        row++;
    }
}

/* Process pre-partitioned fat entries into an HT with prefetch batching.
 * Two-phase prefetch: (1) prefetch HT slots, (2) prefetch group rows. */
static void group_rows_indirect(group_ht_t* ht, const int8_t* key_types,
                                 const char* entries, uint32_t n_entries,
                                 uint16_t entry_stride) {
    uint32_t mask = ht->ht_cap - 1;
    /* Stride-ahead prefetch: prefetch HT slot for entry i+D while processing i.
     * D=8 covers ~200ns L2/L3 latency at ~25ns per probe iteration. */
    enum { PF_DIST = 8 };
    /* Prime the prefetch pipeline */
    uint32_t pf_end = (n_entries < PF_DIST) ? n_entries : PF_DIST;
    for (uint32_t j = 0; j < pf_end; j++) {
        uint64_t h = *(const uint64_t*)(entries + (size_t)j * entry_stride);
        __builtin_prefetch(&ht->slots[(uint32_t)(h & mask)], 0, 1);
    }
    for (uint32_t i = 0; i < n_entries; i++) {
        /* Prefetch PF_DIST entries ahead */
        if (i + PF_DIST < n_entries) {
            uint64_t h = *(const uint64_t*)(entries + (size_t)(i + PF_DIST) * entry_stride);
            __builtin_prefetch(&ht->slots[(uint32_t)(h & mask)], 0, 1);
        }
        const char* e = entries + (size_t)i * entry_stride;
        mask = group_probe_entry(ht, e, key_types, mask);
    }
}

/* Phase 3: build result columns from inline group rows */
typedef struct {
    int8_t  out_type;
    bool    src_f64;
    uint16_t agg_op;
    bool    affine;
    double  bias_f64;
    int64_t bias_i64;
    void*   dst;
} agg_out_t;

typedef struct {
    group_ht_t*   part_hts;
    uint32_t*     part_offsets;
    char**        key_dsts;
    int8_t*       key_types;
    uint8_t*      key_attrs;
    uint8_t*      key_esizes;
    uint8_t       n_keys;
    agg_out_t*    agg_outs;
    uint8_t       n_aggs;
} radix_phase3_ctx_t;

static void radix_phase3_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase3_ctx_t* c = (radix_phase3_ctx_t*)ctx;
    uint8_t nk = c->n_keys;
    uint8_t na = c->n_aggs;

    for (int64_t p = start; p < end; p++) {
        group_ht_t* ph = &c->part_hts[p];
        uint32_t gc = ph->grp_count;
        if (gc == 0) continue;
        uint32_t off = c->part_offsets[p];
        const ght_layout_t* ly = &ph->layout;
        uint16_t rs = ly->row_stride;

        /* Single pass over group rows: read each row once, scatter keys + aggs.
         * Reduces memory traffic from nk+na passes over group data to 1 pass. */
        for (uint32_t gi = 0; gi < gc; gi++) {
            const char* row = ph->rows + (size_t)gi * rs;
            const int64_t* rkeys = (const int64_t*)(const void*)(row + 8);
            int64_t cnt = *(const int64_t*)(const void*)row;
            uint32_t di = off + gi;

            /* Scatter keys to result columns */
            for (uint8_t k = 0; k < nk; k++) {
                int64_t kv = rkeys[k];
                int8_t kt = c->key_types[k];
                char* dst = c->key_dsts[k];
                uint8_t esz = c->key_esizes[k];
                size_t doff = (size_t)di * esz;
                if (kt == RAY_F64)
                    memcpy(dst + doff, &kv, 8);
                else
                    write_col_i64(dst, di, kv, kt, c->key_attrs[k]);
            }

            /* Scatter agg results to result columns */
            for (uint8_t a = 0; a < na; a++) {
                agg_out_t* ao = &c->agg_outs[a];
                uint16_t op = ao->agg_op;
                bool sf = ao->src_f64;
                int8_t s = ly->agg_val_slot[a];
                if (ao->out_type == RAY_F64) {
                    double v;
                    switch (op) {
                        case OP_SUM:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_f64 * cnt;
                            break;
                        case OP_AVG:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s) / cnt
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / cnt;
                            if (ao->affine) v += ao->bias_f64;
                            break;
                        case OP_MIN:
                            v = sf ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                            break;
                        case OP_MAX:
                            v = sf ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                            break;
                        case OP_FIRST: case OP_LAST:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            break;
                        case OP_VAR: case OP_VAR_POP:
                        case OP_STDDEV: case OP_STDDEV_POP: {
                            double sum_val = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                            double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                            double mean = cnt > 0 ? sum_val / cnt : 0.0;
                            double var_pop = cnt > 0 ? sq_val / cnt - mean * mean : 0.0;
                            if (var_pop < 0) var_pop = 0;
                            if (op == OP_VAR_POP) v = cnt > 0 ? var_pop : NAN;
                            else if (op == OP_VAR) v = cnt > 1 ? var_pop * cnt / (cnt - 1) : NAN;
                            else if (op == OP_STDDEV_POP) v = cnt > 0 ? sqrt(var_pop) : NAN;
                            else v = cnt > 1 ? sqrt(var_pop * cnt / (cnt - 1)) : NAN;
                            break;
                        }
                        default: v = 0.0; break;
                    }
                    ((double*)(void*)ao->dst)[di] = v;
                } else {
                    int64_t v;
                    switch (op) {
                        case OP_SUM:
                            v = ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_i64 * cnt;
                            break;
                        case OP_COUNT: v = cnt; break;
                        case OP_MIN:   v = ROW_RD_I64(row, ly->off_min, s); break;
                        case OP_MAX:   v = ROW_RD_I64(row, ly->off_max, s); break;
                        case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly->off_sum, s); break;
                        default:       v = 0; break;
                    }
                    ((int64_t*)(void*)ao->dst)[di] = v;
                }
            }
        }
    }
}

/* Phase 2: aggregate each partition independently using inline data */
typedef struct {
    int8_t*      key_types;
    uint8_t      n_keys;
    uint32_t     n_workers;
    radix_buf_t* bufs;
    group_ht_t*  part_hts;
    ght_layout_t layout;
} radix_phase2_ctx_t;

static void radix_phase2_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase2_ctx_t* c = (radix_phase2_ctx_t*)ctx;
    uint16_t estride = c->layout.entry_stride;

    for (int64_t p = start; p < end; p++) {
        uint32_t total = 0;
        for (uint32_t w = 0; w < c->n_workers; w++)
            total += c->bufs[(size_t)w * RADIX_P + p].count;
        if (total == 0) continue;

        uint32_t part_ht_cap = 256;
        {
            uint64_t target = (uint64_t)total * 2;
            if (target < 256) target = 256;
            while (part_ht_cap < target) part_ht_cap *= 2;
        }
        /* Pre-size group store to avoid grows. Use next_pow2(total) as upper
         * bound on groups. Over-allocation is bounded: worst case total >> groups,
         * but total * row_stride is already committed via HT capacity anyway. */
        uint32_t init_grp = 256;
        while (init_grp < total) init_grp *= 2;
        if (!group_ht_init_sized(&c->part_hts[p], part_ht_cap, &c->layout, init_grp))
            continue;

        for (uint32_t w = 0; w < c->n_workers; w++) {
            radix_buf_t* buf = &c->bufs[(size_t)w * RADIX_P + p];
            if (buf->count == 0) continue;
            group_rows_indirect(&c->part_hts[p], c->key_types,
                                buf->data, buf->count, estride);
        }
    }
}

/* ============================================================================
 * Parallel direct-array accumulation for low-cardinality single integer key
 * ============================================================================ */

/* Parallel min/max scan for direct-array key range detection */
typedef struct {
    const void* key_data;
    int8_t      key_type;
    uint8_t     key_attrs;
    int64_t*    per_worker_min;  /* [n_workers] */
    int64_t*    per_worker_max;  /* [n_workers] */
    uint32_t    n_workers;
    const uint64_t* mask;
    const uint8_t*  sel_flags;   /* per-segment RAY_SEL_NONE/ALL/MIX (NULL=all pass) */
} minmax_ctx_t;

static void minmax_scan_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    minmax_ctx_t* c = (minmax_ctx_t*)ctx;
    uint32_t wid = worker_id % c->n_workers;
    const uint64_t* mask = c->mask;
    const uint8_t* sel_flags = c->sel_flags;
    int64_t kmin = INT64_MAX, kmax = INT64_MIN;
    int8_t t = c->key_type;

    #define MINMAX_SEG_LOOP(TYPE, CAST) \
        do { \
            const TYPE* kd = (const TYPE*)c->key_data; \
            for (int64_t r = start; r < end; ) { \
                if (sel_flags) { \
                    uint32_t seg = (uint32_t)(r / RAY_MORSEL_ELEMS); \
                    int64_t seg_end = (int64_t)(seg + 1) * RAY_MORSEL_ELEMS; \
                    if (seg_end > end) seg_end = end; \
                    if (sel_flags[seg] == RAY_SEL_NONE) { r = seg_end; continue; } \
                    bool need_bit = (sel_flags[seg] == RAY_SEL_MIX); \
                    for (; r < seg_end; r++) { \
                        if (need_bit && !RAY_SEL_BIT_TEST(mask, r)) continue; \
                        int64_t v = (int64_t)CAST kd[r]; \
                        if (v < kmin) kmin = v; \
                        if (v > kmax) kmax = v; \
                    } \
                } else if (mask) { \
                    if (!RAY_SEL_BIT_TEST(mask, r)) { r++; continue; } \
                    int64_t v = (int64_t)CAST kd[r]; \
                    if (v < kmin) kmin = v; \
                    if (v > kmax) kmax = v; \
                    r++; \
                } else { \
                    int64_t v = (int64_t)CAST kd[r]; \
                    if (v < kmin) kmin = v; \
                    if (v > kmax) kmax = v; \
                    r++; \
                } \
            } \
        } while (0)

    if (t == RAY_I64 || t == RAY_TIMESTAMP)
        MINMAX_SEG_LOOP(int64_t, );
    else if (RAY_IS_SYM(t)) {
        uint8_t w = c->key_attrs & RAY_SYM_W_MASK;
        if (w == RAY_SYM_W64) MINMAX_SEG_LOOP(int64_t, );
        else if (w == RAY_SYM_W32) MINMAX_SEG_LOOP(uint32_t, );
        else if (w == RAY_SYM_W16) MINMAX_SEG_LOOP(uint16_t, );
        else MINMAX_SEG_LOOP(uint8_t, );
    }
    else if (t == RAY_BOOL || t == RAY_U8)
        MINMAX_SEG_LOOP(uint8_t, );
    else if (t == RAY_I16)
        MINMAX_SEG_LOOP(int16_t, );
    else /* RAY_I32, RAY_DATE, RAY_TIME */
        MINMAX_SEG_LOOP(int32_t, );

    #undef MINMAX_SEG_LOOP

    /* Merge with existing per-worker values (a worker may process multiple morsels) */
    if (kmin < c->per_worker_min[wid]) c->per_worker_min[wid] = kmin;
    if (kmax > c->per_worker_max[wid]) c->per_worker_max[wid] = kmax;
}

typedef union { double f; int64_t i; } da_val_t;

typedef struct {
    da_val_t* sum;       /* SUM/AVG/FIRST/LAST [n_slots * n_aggs] */
    da_val_t* min_val;   /* MIN [n_slots * n_aggs] */
    da_val_t* max_val;   /* MAX [n_slots * n_aggs] */
    double*   sumsq_f64; /* sum-of-squares for STDDEV/VAR */
    int64_t*  count;     /* group counts [n_slots] */
    /* Arena headers */
    ray_t* _h_sum;
    ray_t* _h_min;
    ray_t* _h_max;
    ray_t* _h_sumsq;
    ray_t* _h_count;
} da_accum_t;

static inline void da_accum_free(da_accum_t* a) {
    scratch_free(a->_h_sum);
    scratch_free(a->_h_min);
    scratch_free(a->_h_max);
    scratch_free(a->_h_sumsq);
    scratch_free(a->_h_count);
}

/* Unified agg result emitter — used by both DA and HT paths.
 * Arrays indexed by [gi * n_aggs + a], counts by [gi]. */
static void emit_agg_columns(ray_t** result, ray_graph_t* g, const ray_op_ext_t* ext,
                              ray_t* const* agg_vecs, uint32_t grp_count,
                              uint8_t n_aggs,
                              const double*  sum_f64,  const int64_t* sum_i64,
                              const double*  min_f64,  const double*  max_f64,
                              const int64_t* min_i64,  const int64_t* max_i64,
                              const int64_t* counts,
                              const agg_affine_t* affine,
                              const double*  sumsq_f64) {
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
                out_type = RAY_F64; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col = ray_vec_new(out_type, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            size_t idx = (size_t)gi * n_aggs + a;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64 * counts[gi];
                        break;
                    case OP_AVG:
                        v = is_f64 ? sum_f64[idx] / counts[gi] : (double)sum_i64[idx] / counts[gi];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64;
                        break;
                    case OP_MIN: v = is_f64 ? min_f64[idx] : (double)min_i64[idx]; break;
                    case OP_MAX: v = is_f64 ? max_f64[idx] : (double)max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx]; break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        int64_t cnt = counts[gi];
                        double sum_val = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        double sq_val = sumsq_f64 ? sumsq_f64[idx] : 0.0;
                        double mean = cnt > 0 ? sum_val / cnt : 0.0;
                        double var_pop = cnt > 0 ? sq_val / cnt - mean * mean : 0.0;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = cnt > 0 ? var_pop : NAN;
                        else if (agg_op == OP_VAR) v = cnt > 1 ? var_pop * cnt / (cnt - 1) : NAN;
                        else if (agg_op == OP_STDDEV_POP) v = cnt > 0 ? sqrt(var_pop) : NAN;
                        else v = cnt > 1 ? sqrt(var_pop * cnt / (cnt - 1)) : NAN;
                        break;
                    }
                    default:     v = 0.0; break;
                }
                ((double*)ray_data(new_col))[gi] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:
                        v = sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_i64 * counts[gi];
                        break;
                    case OP_COUNT: v = counts[gi]; break;
                    case OP_MIN:   v = min_i64[idx]; break;
                    case OP_MAX:   v = max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST: v = sum_i64[idx]; break;
                    default:       v = 0; break;
                }
                ((int64_t*)ray_data(new_col))[gi] = v;
            }
        }
        /* Generate unique column name: base_name + agg suffix (e.g. "v1_sum") */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            ray_t* name_atom = ray_sym_str(agg_ext->sym);
            const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
            size_t blen = base ? ray_str_len(name_atom) : 0;
            const char* sfx = "";
            size_t slen = 0;
            switch (agg_op) {
                case OP_SUM:   sfx = "_sum";   slen = 4; break;
                case OP_COUNT: sfx = "_count"; slen = 6; break;
                case OP_AVG:   sfx = "_mean";  slen = 5; break;
                case OP_MIN:   sfx = "_min";   slen = 4; break;
                case OP_MAX:   sfx = "_max";   slen = 4; break;
                case OP_FIRST: sfx = "_first"; slen = 6; break;
                case OP_LAST:  sfx = "_last";  slen = 5; break;
                case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                case OP_VAR:        sfx = "_var";        slen = 4; break;
                case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
            }
            char buf[256];
            if (base && blen + slen < sizeof(buf)) {
                memcpy(buf, base, blen);
                memcpy(buf + blen, sfx, slen);
                name_id = ray_sym_intern(buf, blen + slen);
            } else {
                name_id = agg_ext->sym;
            }
        } else {
            /* Expression agg input — synthetic name like "_e0_sum" */
            char nbuf[32];
            int np = 0;
            nbuf[np++] = '_'; nbuf[np++] = 'e';
            /* Multi-digit agg index */
            { uint8_t v = a; char dig[3]; int nd = 0;
              do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
              while (nd--) nbuf[np++] = dig[nd]; }
            const char* nsfx = "";
            size_t nslen = 0;
            switch (agg_op) {
                case OP_SUM:   nsfx = "_sum";   nslen = 4; break;
                case OP_COUNT: nsfx = "_count"; nslen = 6; break;
                case OP_AVG:   nsfx = "_mean";  nslen = 5; break;
                case OP_MIN:   nsfx = "_min";   nslen = 4; break;
                case OP_MAX:   nsfx = "_max";   nslen = 4; break;
                case OP_FIRST: nsfx = "_first"; nslen = 6; break;
                case OP_LAST:  nsfx = "_last";  nslen = 5; break;
                case OP_STDDEV:     nsfx = "_stddev";     nslen = 7; break;
                case OP_STDDEV_POP: nsfx = "_stddev_pop"; nslen = 11; break;
                case OP_VAR:        nsfx = "_var";        nslen = 4; break;
                case OP_VAR_POP:    nsfx = "_var_pop";    nslen = 8; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        *result = ray_table_add_col(*result, name_id, new_col);
        ray_release(new_col);
    }
}

/* Bitmask for which accumulator arrays are actually needed */
#define DA_NEED_SUM   0x01  /* da_val_t sum array */
#define DA_NEED_MIN   0x02  /* da_val_t min_val array */
#define DA_NEED_MAX   0x04  /* da_val_t max_val array */
#define DA_NEED_COUNT 0x08  /* count array */
#define DA_NEED_SUMSQ 0x10  /* sumsq_f64 array (for STDDEV/VAR) */

typedef struct {
    da_accum_t*    accums;
    uint32_t       n_accums;     /* number of accumulator sets (may < pool workers) */
    void**         key_ptrs;     /* key data pointers [n_keys] */
    int8_t*        key_types;    /* key type codes [n_keys] */
    uint8_t*       key_attrs;    /* key attrs for RAY_SYM width [n_keys] */
    uint8_t*       key_esz;      /* pre-computed per-key elem size [n_keys] */
    int64_t*       key_mins;     /* per-key minimum [n_keys] */
    int64_t*       key_strides;  /* per-key stride [n_keys] */
    uint8_t        n_keys;
    void**         agg_ptrs;
    int8_t*        agg_types;
    uint16_t*      agg_ops;      /* per-agg operation code */
    uint8_t        n_aggs;
    uint8_t        need_flags;   /* DA_NEED_* bitmask */
    uint32_t       agg_f64_mask; /* bitmask: bit a set if agg[a] is RAY_F64 */
    bool           all_sum;      /* true when all ops are SUM/AVG/COUNT (no MIN/MAX/FIRST/LAST) */
    uint32_t       n_slots;
    const uint64_t* mask;
    const uint8_t*  sel_flags;   /* per-segment RAY_SEL_NONE/ALL/MIX (NULL=all pass) */
} da_ctx_t;

/* Composite GID from multi-key.  Arithmetic overflow is prevented in practice
 * by the DA budget check (DA_PER_WORKER_MAX) which limits total_slots to 262K. */
static inline int32_t da_composite_gid(da_ctx_t* c, int64_t r) {
    int32_t gid = 0;
    for (uint8_t k = 0; k < c->n_keys; k++) {
        int64_t val = read_by_esz(c->key_ptrs[k], r, c->key_esz[k]);
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]);
    }
    return gid;
}

/* Typed composite GID: eliminates per-element switch when all keys share width */
#define DEFINE_DA_COMPOSITE_GID_TYPED(SUFFIX, KTYPE) \
static inline int32_t da_composite_gid_##SUFFIX(da_ctx_t* c, int64_t r) { \
    int32_t gid = 0; \
    for (uint8_t k = 0; k < c->n_keys; k++) { \
        int64_t val = (int64_t)((const KTYPE*)c->key_ptrs[k])[r]; \
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]); \
    } \
    return gid; \
}
DEFINE_DA_COMPOSITE_GID_TYPED(u8,  uint8_t)
DEFINE_DA_COMPOSITE_GID_TYPED(u16, uint16_t)
DEFINE_DA_COMPOSITE_GID_TYPED(u32, uint32_t)
DEFINE_DA_COMPOSITE_GID_TYPED(i64, int64_t)
#undef DEFINE_DA_COMPOSITE_GID_TYPED

static inline void da_read_val(const void* ptr, int8_t type, uint8_t attrs,
                               int64_t r, double* out_f64, int64_t* out_i64) {
    if (type == RAY_F64) {
        *out_f64 = ((const double*)ptr)[r];
        *out_i64 = (int64_t)*out_f64;
    } else {
        *out_i64 = read_col_i64(ptr, r, type, attrs);
        *out_f64 = (double)*out_i64;
    }
}

/* Materialize a scalar (atom or len-1 vector) into a full-length vector so
 * group-aggregation loops can read row-wise without out-of-bounds access. */
static ray_t* materialize_broadcast_input(ray_t* src, int64_t nrows) {
    if (!src || RAY_IS_ERR(src) || nrows < 0) return NULL;

    int8_t out_type = ray_is_atom(src) ? (int8_t)-src->type : src->type;
    if (out_type <= 0 || out_type >= RAY_TYPE_COUNT) return NULL;

    ray_t* out = ray_vec_new(out_type, nrows);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = nrows;
    if (nrows == 0) return out;

    if (!ray_is_atom(src)) {
        uint8_t esz = col_esz(src);
        const char* s = (const char*)ray_data(src);
        char* d = (char*)ray_data(out);
        for (int64_t i = 0; i < nrows; i++)
            memcpy(d + (size_t)i * esz, s, esz);
        return out;
    }

    switch (src->type) {
        case -RAY_F64: {
            double v = src->f64;
            for (int64_t i = 0; i < nrows; i++) ((double*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I64:
        case -RAY_SYM:
        case -RAY_TIMESTAMP: {
            int64_t v = src->i64;
            for (int64_t i = 0; i < nrows; i++) ((int64_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_DATE:
        case -RAY_TIME: {
            int32_t v = (int32_t)src->i64;
            for (int64_t i = 0; i < nrows; i++) ((int32_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I32: {
            int32_t v = src->i32;
            for (int64_t i = 0; i < nrows; i++) ((int32_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I16: {
            int16_t v = src->i16;
            for (int64_t i = 0; i < nrows; i++) ((int16_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_U8:
        case -RAY_BOOL: {
            uint8_t v = src->u8;
            for (int64_t i = 0; i < nrows; i++) ((uint8_t*)ray_data(out))[i] = v;
            return out;
        }
        default:
            ray_release(out);
            return NULL;
    }
}

/* ---- Scalar aggregate (n_keys==0): one flat scan, no GID, no hash ---- */
typedef struct {
    void**         agg_ptrs;
    int8_t*        agg_types;
    uint16_t*      agg_ops;
    agg_linear_t*  agg_linear;
    uint8_t        n_aggs;
    uint8_t        need_flags;
    const uint64_t* mask;
    const uint8_t*  sel_flags;   /* per-segment RAY_SEL_NONE/ALL/MIX (NULL=all pass) */
    /* per-worker accumulators (1 slot each) */
    da_accum_t*    accums;
    uint32_t       n_accums;
} scalar_ctx_t;

static inline int64_t scalar_i64_at(const void* ptr, int8_t type, int64_t r) {
    return read_col_i64(ptr, r, type, 0);  /* attrs=0: agg columns are numeric, never SYM */
}

/* Tight SIMD-friendly loop for single SUM/AVG on i64 (no mask).
 * Note: int64 sum can overflow; caller responsibility to use appropriate types. */
static void scalar_sum_i64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const int64_t* restrict data = (const int64_t*)c->agg_ptrs[0];
    int64_t sum = 0;
    for (int64_t r = start; r < end; r++)
        sum += data[r];
    acc->sum[0].i += sum;
    acc->count[0] += end - start;
}

/* Tight SIMD-friendly loop for single SUM/AVG on f64 (no mask) */
static void scalar_sum_f64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const double* restrict data = (const double*)c->agg_ptrs[0];
    double sum = 0.0;
    for (int64_t r = start; r < end; r++)
        sum += data[r];
    acc->sum[0].f += sum;
    acc->count[0] += end - start;
}

/* Tight loop for single SUM/AVG on integer linear expression (no mask). */
static void scalar_sum_linear_i64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const agg_linear_t* lin = &c->agg_linear[0];
    int64_t n = end - start;

    int64_t sum = lin->bias_i64 * n;
    for (uint8_t t = 0; t < lin->n_terms; t++) {
        int64_t coeff = lin->coeff_i64[t];
        if (coeff == 0) continue;
        const void* ptr = lin->term_ptrs[t];
        int8_t type = lin->term_types[t];
        int64_t term_sum = 0;
        for (int64_t r = start; r < end; r++)
            term_sum += scalar_i64_at(ptr, type, r);
        sum += coeff * term_sum;
    }

    acc->sum[0].i += sum;
    acc->count[0] += n;
}

/* Generic scalar accumulation: handles all ops, all types, mask */
/* Inner scalar accumulation for a single row */
static inline void scalar_accum_row(scalar_ctx_t* c, da_accum_t* acc, int64_t r) {
    uint8_t n_aggs = c->n_aggs;
    acc->count[0]++;
    for (uint8_t a = 0; a < n_aggs; a++) {
        double fv; int64_t iv;
        if (c->agg_linear && c->agg_linear[a].enabled) {
            const agg_linear_t* lin = &c->agg_linear[a];
            iv = lin->bias_i64;
            for (uint8_t t = 0; t < lin->n_terms; t++) {
                iv += lin->coeff_i64[t] *
                      scalar_i64_at(lin->term_ptrs[t], lin->term_types[t], r);
            }
            fv = (double)iv;
        } else {
            if (!c->agg_ptrs[a]) continue;
            da_read_val(c->agg_ptrs[a], c->agg_types[a], 0, r, &fv, &iv);
        }
        uint16_t op = c->agg_ops[a];
        bool is_f = (c->agg_types[a] == RAY_F64);
        if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (is_f) acc->sum[a].f += fv;
            else acc->sum[a].i += iv;
            if (acc->sumsq_f64) acc->sumsq_f64[a] += fv * fv;
        } else if (op == OP_FIRST) {
            if (acc->count[0] == 1) {
                if (is_f) acc->sum[a].f = fv; else acc->sum[a].i = iv;
            }
        } else if (op == OP_LAST) {
            if (is_f) acc->sum[a].f = fv; else acc->sum[a].i = iv;
        } else if (op == OP_MIN) {
            if (is_f) { if (fv < acc->min_val[a].f) acc->min_val[a].f = fv; }
            else      { if (iv < acc->min_val[a].i) acc->min_val[a].i = iv; }
        } else if (op == OP_MAX) {
            if (is_f) { if (fv > acc->max_val[a].f) acc->max_val[a].f = fv; }
            else      { if (iv > acc->max_val[a].i) acc->max_val[a].i = iv; }
        }
    }
}

static void scalar_accum_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const uint64_t* mask = c->mask;
    const uint8_t* sel_flags = c->sel_flags;

    for (int64_t r = start; r < end; ) {
        /* Segment-level skip */
        if (sel_flags) {
            uint32_t seg = (uint32_t)(r / RAY_MORSEL_ELEMS);
            int64_t seg_end = (int64_t)(seg + 1) * RAY_MORSEL_ELEMS;
            if (seg_end > end) seg_end = end;
            if (sel_flags[seg] == RAY_SEL_NONE) { r = seg_end; continue; }
            bool need_bit = (sel_flags[seg] == RAY_SEL_MIX);

            for (; r < seg_end; r++) {
                if (need_bit && !RAY_SEL_BIT_TEST(mask, r)) continue;
                scalar_accum_row(c, acc, r);
            }
            continue;
        }

        if (RAY_UNLIKELY(mask && !RAY_SEL_BIT_TEST(mask, r))) { r++; continue; }
        scalar_accum_row(c, acc, r);
        r++;
    }
}

/* Inner DA accumulation for a single row — shared by single-key and multi-key paths.
 * Fast path for SUM/AVG-only queries: eliminates op-code dispatch and da_read_val
 * dual-write overhead.  The branch on c->all_sum is perfectly predicted (invariant
 * across all rows). */
static inline void da_accum_row(da_ctx_t* c, da_accum_t* acc, int32_t gid, int64_t r) {
    uint8_t n_aggs = c->n_aggs;
    acc->count[gid]++;
    size_t base = (size_t)gid * n_aggs;

    if (RAY_LIKELY(c->all_sum)) {
        /* SUM/AVG/COUNT fast path — no op-code dispatch, typed read only.
         * COUNT-only queries have acc->sum==NULL; count[gid]++ above suffices. */
        if (!acc->sum) return;
        uint32_t f64m = c->agg_f64_mask;
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!c->agg_ptrs[a]) continue;
            size_t idx = base + a;
            if (f64m & (1u << a))
                acc->sum[idx].f += ((const double*)c->agg_ptrs[a])[r];
            else
                acc->sum[idx].i += read_col_i64(c->agg_ptrs[a], r,
                                                c->agg_types[a], 0);
        }
        return;
    }

    for (uint8_t a = 0; a < n_aggs; a++) {
        if (!c->agg_ptrs[a]) continue;
        size_t idx = base + a;
        double fv; int64_t iv;
        da_read_val(c->agg_ptrs[a], c->agg_types[a], 0, r, &fv, &iv);
        uint16_t op = c->agg_ops[a];
        if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (c->agg_types[a] == RAY_F64) acc->sum[idx].f += fv;
            else acc->sum[idx].i = (int64_t)((uint64_t)acc->sum[idx].i + (uint64_t)iv);
            if (acc->sumsq_f64) acc->sumsq_f64[idx] += fv * fv;
        } else if (op == OP_FIRST) {
            if (acc->count[gid] == 1) {
                if (c->agg_types[a] == RAY_F64) acc->sum[idx].f = fv;
                else acc->sum[idx].i = iv;
            }
        } else if (op == OP_LAST) {
            if (c->agg_types[a] == RAY_F64) acc->sum[idx].f = fv;
            else acc->sum[idx].i = iv;
        } else if (op == OP_MIN) {
            if (c->agg_types[a] == RAY_F64) {
                if (fv < acc->min_val[idx].f) acc->min_val[idx].f = fv;
            } else {
                if (iv < acc->min_val[idx].i) acc->min_val[idx].i = iv;
            }
        } else if (op == OP_MAX) {
            if (c->agg_types[a] == RAY_F64) {
                if (fv > acc->max_val[idx].f) acc->max_val[idx].f = fv;
            } else {
                if (iv > acc->max_val[idx].i) acc->max_val[idx].i = iv;
            }
        }
    }
}

static void da_accum_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    da_ctx_t* c = (da_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    uint8_t n_aggs = c->n_aggs;
    uint8_t n_keys = c->n_keys;
    const uint64_t* mask = c->mask;
    const uint8_t* sel_flags = c->sel_flags;

    /* Fast path: single key — avoid composite GID loop overhead.
     * Templated by key element size: the entire loop is stamped out per width
     * so the compiler generates direct movzbl/movzwl/movl/movq — zero dispatch. */
    #define DA_PF_DIST 8
    #define DA_SINGLE_KEY_LOOP(KTYPE, KCAST) \
    do { \
        const KTYPE* kp = (const KTYPE*)c->key_ptrs[0]; \
        int64_t kmin = c->key_mins[0]; \
        bool da_pf = c->n_slots >= 4096; \
        for (int64_t r = start; r < end; ) { \
            if (sel_flags) { \
                uint32_t seg = (uint32_t)(r / RAY_MORSEL_ELEMS); \
                int64_t seg_end = (int64_t)(seg + 1) * RAY_MORSEL_ELEMS; \
                if (seg_end > end) seg_end = end; \
                if (sel_flags[seg] == RAY_SEL_NONE) { r = seg_end; continue; } \
                bool need_bit = (sel_flags[seg] == RAY_SEL_MIX); \
                for (; r < seg_end; r++) { \
                    if (need_bit && !RAY_SEL_BIT_TEST(mask, r)) continue; \
                    if (da_pf && RAY_LIKELY(r + DA_PF_DIST < end)) { \
                        int64_t pfk = (int64_t)KCAST kp[r + DA_PF_DIST]; \
                        __builtin_prefetch(&acc->count[(int32_t)(pfk - kmin)], 1, 1); \
                        if (acc->sum) __builtin_prefetch( \
                            &acc->sum[(size_t)(int32_t)(pfk - kmin) * n_aggs], 1, 1); \
                    } \
                    int64_t kv = (int64_t)KCAST kp[r]; \
                    da_accum_row(c, acc, (int32_t)(kv - kmin), r); \
                } \
                continue; \
            } \
            if (RAY_UNLIKELY(mask && !RAY_SEL_BIT_TEST(mask, r))) { r++; continue; } \
            if (da_pf && RAY_LIKELY(r + DA_PF_DIST < end)) { \
                int64_t pfk = (int64_t)KCAST kp[r + DA_PF_DIST]; \
                __builtin_prefetch(&acc->count[(int32_t)(pfk - kmin)], 1, 1); \
                if (acc->sum) __builtin_prefetch( \
                    &acc->sum[(size_t)(int32_t)(pfk - kmin) * n_aggs], 1, 1); \
            } \
            int64_t kv = (int64_t)KCAST kp[r]; \
            da_accum_row(c, acc, (int32_t)(kv - kmin), r); \
            r++; \
        } \
    } while (0)

    if (n_keys == 1) {
        switch (c->key_esz[0]) {
        case 1: DA_SINGLE_KEY_LOOP(uint8_t, ); break;
        case 2: DA_SINGLE_KEY_LOOP(uint16_t, ); break;
        case 4: DA_SINGLE_KEY_LOOP(uint32_t, (int64_t)); break;
        default: DA_SINGLE_KEY_LOOP(int64_t, ); break;
        }
        #undef DA_SINGLE_KEY_LOOP
        return;
    }

    /* Multi-key composite GID — typed inner loop eliminates read_by_esz switch.
     * When all keys share the same element size, use da_composite_gid_XX(). */
    #define DA_MULTI_KEY_LOOP(GID_FN) \
    do { \
        bool _da_pf = c->n_slots >= 4096; \
        for (int64_t r = start; r < end; ) { \
            if (sel_flags) { \
                uint32_t seg = (uint32_t)(r / RAY_MORSEL_ELEMS); \
                int64_t seg_end = (int64_t)(seg + 1) * RAY_MORSEL_ELEMS; \
                if (seg_end > end) seg_end = end; \
                if (sel_flags[seg] == RAY_SEL_NONE) { r = seg_end; continue; } \
                bool need_bit = (sel_flags[seg] == RAY_SEL_MIX); \
                for (; r < seg_end; r++) { \
                    if (need_bit && !RAY_SEL_BIT_TEST(mask, r)) continue; \
                    if (_da_pf && RAY_LIKELY(r + DA_PF_DIST < end)) { \
                        int32_t pf_gid = GID_FN(r + DA_PF_DIST); \
                        __builtin_prefetch(&acc->count[pf_gid], 1, 1); \
                        if (acc->sum) __builtin_prefetch(&acc->sum[(size_t)pf_gid * n_aggs], 1, 1); \
                    } \
                    da_accum_row(c, acc, GID_FN(r), r); \
                } \
                continue; \
            } \
            if (RAY_UNLIKELY(mask && !RAY_SEL_BIT_TEST(mask, r))) { r++; continue; } \
            if (_da_pf && RAY_LIKELY(r + DA_PF_DIST < end)) { \
                int32_t pf_gid = GID_FN(r + DA_PF_DIST); \
                __builtin_prefetch(&acc->count[pf_gid], 1, 1); \
                if (acc->sum) __builtin_prefetch(&acc->sum[(size_t)pf_gid * n_aggs], 1, 1); \
            } \
            da_accum_row(c, acc, GID_FN(r), r); \
            r++; \
        } \
    } while (0)

    /* Check if all keys share the same element size */
    bool uniform_esz = true;
    for (uint8_t k = 1; k < n_keys; k++)
        if (c->key_esz[k] != c->key_esz[0]) { uniform_esz = false; break; }

    if (uniform_esz) {
        switch (c->key_esz[0]) {
        case 1:
#define GID_FN(R) da_composite_gid_u8(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        case 2:
#define GID_FN(R) da_composite_gid_u16(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        case 4:
#define GID_FN(R) da_composite_gid_u32(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        default:
#define GID_FN(R) da_composite_gid_i64(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        }
    } else {
#define GID_FN(R) da_composite_gid(c, (R))
        DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
    }
    #undef DA_MULTI_KEY_LOOP
    #undef DA_PF_DIST
}

/* Parallel DA merge: merge per-worker accumulators into accums[0] by
 * dispatching disjoint slot ranges across pool workers. */
typedef struct {
    da_accum_t* accums;
    uint32_t    n_src_workers; /* number of source workers to merge (1..n) */
    uint8_t     need_flags;
    uint8_t     n_aggs;
    const int8_t* agg_types;  /* per-agg value type (for typed merge) */
    const uint16_t* agg_ops;  /* per-agg opcode (for FIRST/LAST merge) */
} da_merge_ctx_t;

static void da_merge_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    da_merge_ctx_t* c = (da_merge_ctx_t*)ctx;
    da_accum_t* merged = &c->accums[0];
    uint8_t n_aggs = c->n_aggs;
    const int8_t* agg_types = c->agg_types;
    for (uint32_t w = 1; w < c->n_src_workers; w++) {
        da_accum_t* wa = &c->accums[w];
        for (int64_t s = start; s < end; s++) {
            size_t base = (size_t)s * n_aggs;
            if (c->need_flags & DA_NEED_SUMSQ) {
                for (uint8_t a = 0; a < n_aggs; a++)
                    merged->sumsq_f64[base + a] += wa->sumsq_f64[base + a];
            }
            if (c->need_flags & DA_NEED_SUM) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    uint16_t aop = c->agg_ops ? c->agg_ops[a] : OP_SUM;
                    if (aop == OP_FIRST) {
                        /* Keep worker 0 value; take from w only if merged has no data */
                        if (merged->count[s] == 0 && wa->count[s] > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (aop == OP_LAST) {
                        /* Overwrite with last worker that has data */
                        if (wa->count[s] > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (agg_types[a] == RAY_F64)
                        merged->sum[idx].f += wa->sum[idx].f;
                    else
                        merged->sum[idx].i += wa->sum[idx].i;
                }
            }
            if (c->need_flags & DA_NEED_MIN) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[idx].f < merged->min_val[idx].f)
                            merged->min_val[idx].f = wa->min_val[idx].f;
                    } else {
                        if (wa->min_val[idx].i < merged->min_val[idx].i)
                            merged->min_val[idx].i = wa->min_val[idx].i;
                    }
                }
            }
            if (c->need_flags & DA_NEED_MAX) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[idx].f > merged->max_val[idx].f)
                            merged->max_val[idx].f = wa->max_val[idx].f;
                    } else {
                        if (wa->max_val[idx].i > merged->max_val[idx].i)
                            merged->max_val[idx].i = wa->max_val[idx].i;
                    }
                }
            }
            merged->count[s] += wa->count[s];
        }
    }
}

/* ============================================================================
 * Partition-aware group-by: detect parted columns, concatenate segments into
 * a flat table, then run standard exec_group once.
 * ============================================================================ */
static ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                        int64_t group_limit); /* forward decl */

/* Forward declaration — defined below exec_group */
static ray_t* exec_group_per_partition(ray_t* parted_tbl, ray_op_ext_t* ext,
                                       int32_t n_parts, const int64_t* key_syms,
                                       const int64_t* agg_syms, int has_avg,
                                       int has_stddev, int64_t group_limit);

/* --------------------------------------------------------------------------
 * exec_group_parted — dispatch per-partition or concat-fallback
 * -------------------------------------------------------------------------- */
static ray_t* exec_group_parted(ray_graph_t* g, ray_op_t* op, ray_t* parted_tbl,
                               int64_t group_limit) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    if (ncols <= 0) return ray_error("nyi", NULL);

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Find partition count and total rows from first parted column */
    int32_t n_parts = 0;
    int64_t total_rows = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (col && RAY_IS_PARTED(col->type)) {
            n_parts = (int32_t)col->len;
            total_rows = ray_parted_nrows(col);
            break;
        }
    }
    if (n_parts <= 0 || total_rows <= 0) return ray_error("nyi", NULL);

    /* Check eligibility for per-partition exec + merge:
     * - All keys and agg inputs must be simple SCANs
     * - Supported agg ops: SUM, COUNT, MIN, MAX, AVG, FIRST, LAST,
     *   STDDEV, STDDEV_POP, VAR, VAR_POP */
    int can_partition = 1;
    int has_avg = 0;
    int has_stddev = 0;
    int64_t key_syms[8];
    for (uint8_t k = 0; k < n_keys && can_partition; k++) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[k]->id);
        if (!ke || ke->base.opcode != OP_SCAN) { can_partition = 0; break; }
        key_syms[k] = ke->sym;
    }
    int64_t agg_syms[8];
    for (uint8_t a = 0; a < n_aggs && can_partition; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop != OP_SUM && aop != OP_COUNT && aop != OP_MIN &&
            aop != OP_MAX && aop != OP_AVG && aop != OP_FIRST &&
            aop != OP_LAST && aop != OP_STDDEV && aop != OP_STDDEV_POP &&
            aop != OP_VAR && aop != OP_VAR_POP) { can_partition = 0; break; }
        if (aop == OP_AVG) has_avg = 1;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
            aop == OP_VAR || aop == OP_VAR_POP) has_stddev = 1;
        ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]->id);
        if (!ae || ae->base.opcode != OP_SCAN) { can_partition = 0; break; }
        agg_syms[a] = ae->sym;
    }

    /* Cardinality gate: estimate groups from first partition.
     * Per-partition only wins when #groups << partition_size. */
    if (can_partition) {
        int64_t rows_per_part = total_rows / n_parts;
        int64_t est_groups = 1;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
            if (!pcol) { est_groups = rows_per_part; break; }
            /* MAPCOMMON key: constant per partition — excluded from
             * per-partition sub-GROUP-BY, contributes 0 to cardinality. */
            if (pcol->type == RAY_MAPCOMMON) { continue; }
            if (!RAY_IS_PARTED(pcol->type)) { est_groups = rows_per_part; break; }
            ray_t* seg0 = ((ray_t**)ray_data(pcol))[0];
            if (!seg0 || seg0->len <= 0) { est_groups = rows_per_part; break; }
            int8_t bt = RAY_PARTED_BASETYPE(pcol->type);
            int64_t card;
            if (RAY_IS_SYM(bt)) {
                uint32_t sym_n = ray_sym_count();
                if (sym_n == 0 || sym_n > 4194304) { est_groups = rows_per_part; break; }
                size_t bwords = ((size_t)sym_n + 63) / 64;
                ray_t* bits_hdr = NULL;
                uint64_t* bits = (uint64_t*)scratch_calloc(&bits_hdr, bwords * 8);
                if (!bits) { est_groups = rows_per_part; break; }
                for (int64_t r = 0; r < seg0->len; r++) {
                    uint32_t id = (uint32_t)ray_read_sym(ray_data(seg0), r, seg0->type, seg0->attrs);
                    bits[id / 64] |= 1ULL << (id % 64);
                }
                card = 0;
                for (size_t i = 0; i < bwords; i++)
                    card += __builtin_popcountll(bits[i]);
                scratch_free(bits_hdr);
            } else if (bt == RAY_I64) {
                const int64_t* v = (const int64_t*)ray_data(seg0);
                int64_t lo = v[0], hi = v[0];
                for (int64_t r = 1; r < seg0->len; r++) {
                    if (v[r] < lo) lo = v[r];
                    if (v[r] > hi) hi = v[r];
                }
                card = hi - lo + 1;
            } else if (bt == RAY_I32) {
                const int32_t* v = (const int32_t*)ray_data(seg0);
                int32_t lo = v[0], hi = v[0];
                for (int64_t r = 1; r < seg0->len; r++) {
                    if (v[r] < lo) lo = v[r];
                    if (v[r] > hi) hi = v[r];
                }
                card = (int64_t)(hi - lo + 1);
            } else {
                card = seg0->len;
            }
            est_groups *= card;
            if (est_groups > rows_per_part) { est_groups = rows_per_part; break; }
        }
        /* Block per-partition when cardinality is high AND the concat
         * fallback would fit in memory (< 4 GB estimated).  When concat is
         * too large, per-partition with batched merge is the only option. */
        int64_t concat_bytes = total_rows * 8LL * (int64_t)(n_keys + n_aggs);
        if (est_groups * 100 > rows_per_part &&
            concat_bytes < 4LL * 1024 * 1024 * 1024)
            can_partition = 0;
    }

    /* Try per-partition path (separate noinline function to avoid I-cache pressure) */
    if (can_partition) {
        ray_t* result = exec_group_per_partition(parted_tbl, ext, n_parts,
                                                 key_syms, agg_syms, has_avg,
                                                 has_stddev, group_limit);
        if (result) return result;
        /* NULL = per-partition failed, fall through to concat */
    }

    /* ---- Concat fallback ---- */
    /* ---- Concat-only-needed-columns fallback ----
     * Used when query has AVG or expression keys/aggs.
     * Only concatenates the columns actually referenced by the GROUP BY. */
    {
        /* Collect needed column sym IDs (keys + agg inputs) */
        int64_t needed[16];
        int n_needed = 0;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_op_ext_t* ke = find_ext(g, ext->keys[k]->id);
            if (ke && ke->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ke->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ke->sym;
            }
        }
        for (uint8_t a = 0; a < n_aggs; a++) {
            ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]->id);
            if (ae && ae->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ae->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ae->sym;
            } else {
                /* Expression agg input — need all columns for evaluation.
                 * Fall back to copying everything. */
                n_needed = 0;
                break;
            }
        }

        /* Build flat table with only needed columns (or all if n_needed==0) */
        ray_t* flat_tbl = ray_table_new(n_needed > 0 ? (int64_t)n_needed : ncols);
        if (!flat_tbl || RAY_IS_ERR(flat_tbl)) return flat_tbl;

        int64_t cols_to_iter = n_needed > 0 ? (int64_t)n_needed : ncols;
        for (int64_t ci = 0; ci < cols_to_iter; ci++) {
            ray_t* col;
            int64_t name_id;
            if (n_needed > 0) {
                col = ray_table_get_col(parted_tbl, needed[ci]);
                name_id = needed[ci];
            } else {
                col = ray_table_get_col_idx(parted_tbl, ci);
                name_id = ray_table_col_name(parted_tbl, ci);
            }
            if (!col) continue;
            if (col->type == RAY_MAPCOMMON) {
                ray_t* mc_flat = materialize_mapcommon(col);
                if (mc_flat && !RAY_IS_ERR(mc_flat)) {
                    flat_tbl = ray_table_add_col(flat_tbl, name_id, mc_flat);
                    ray_release(mc_flat);
                }
                continue;
            }

            if (!RAY_IS_PARTED(col->type)) {
                ray_retain(col);
                flat_tbl = ray_table_add_col(flat_tbl, name_id, col);
                ray_release(col);
                continue;
            }

            int8_t base_type = (int8_t)RAY_PARTED_BASETYPE(col->type);
            ray_t** segs = (ray_t**)ray_data(col);
            uint8_t base_attrs = (base_type == RAY_SYM && col->len > 0 && segs[0])
                               ? segs[0]->attrs : 0;
            ray_t* flat = typed_vec_new(base_type, base_attrs, total_rows);
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(flat_tbl);
                return ray_error("oom", NULL);
            }
            flat->len = total_rows;

            size_t elem_size = (size_t)ray_sym_elem_size(base_type, base_attrs);
            int64_t offset = 0;
            for (int32_t p = 0; p < n_parts; p++) {
                ray_t* seg = segs[p];
                if (!seg || seg->len <= 0) continue;
                memcpy((char*)ray_data(flat) + (size_t)offset * elem_size,
                       ray_data(seg), (size_t)seg->len * elem_size);
                offset += seg->len;
            }

            flat_tbl = ray_table_add_col(flat_tbl, name_id, flat);
            ray_release(flat);
        }

        ray_t* saved = g->table;
        g->table = flat_tbl;
        ray_t* result = exec_group(g, op, flat_tbl, 0);
        g->table = saved;
        ray_release(flat_tbl);
        return result;
    }
}

static ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                        int64_t group_limit) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    /* Parted dispatch: detect parted input columns */
    {
        int64_t nc = ray_table_ncols(tbl);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                return exec_group_parted(g, op, tbl, group_limit);
            }
        }
    }

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t nrows = ray_table_nrows(tbl);
    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Factorized shortcut: if input is a factorized expand result with
     * (_src, _count) columns, and GROUP BY _src with COUNT/SUM(_count),
     * return the pre-aggregated table directly without re-scanning. */
    if (n_keys == 1 && n_aggs > 0 && nrows > 0) {
        int64_t cnt_sym = ray_sym_intern("_count", 6);
        ray_t* cnt_col = ray_table_get_col(tbl, cnt_sym);
        if (cnt_col && cnt_col->type == RAY_I64) {
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]->id);
            int64_t src_sym = ray_sym_intern("_src", 4);
            if (key_ext && key_ext->base.opcode == OP_SCAN &&
                key_ext->sym == src_sym) {
                /* Verify all aggs are compatible with factorized data:
                 * COUNT(*) → use _count directly
                 * SUM(_count) → use _count directly */
                bool all_compat = true;
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
                    if (aop == OP_COUNT) continue;
                    if (aop == OP_SUM && agg_ext &&
                        agg_ext->base.opcode == OP_SCAN &&
                        agg_ext->sym == cnt_sym) continue;
                    all_compat = false;
                    break;
                }
                if (all_compat) {
                    /* The factorized table already has one row per group.
                     * Build result with _src key + agg columns from _count. */
                    ray_t* src_col = ray_table_get_col(tbl, src_sym);
                    if (src_col) {
                        int64_t out_nkeys = 1;
                        int64_t out_ncols = out_nkeys + n_aggs;
                        ray_t* result = ray_table_new((int64_t)out_ncols);
                        if (!result || RAY_IS_ERR(result))
                            return ray_error("oom", NULL);
                        ray_retain(src_col);
                        ray_t* tmp_r = ray_table_add_col(result, src_sym, src_col);
                        ray_release(src_col);
                        if (!tmp_r || RAY_IS_ERR(tmp_r)) {
                            ray_release(result);
                            return ray_error("oom", NULL);
                        }
                        result = tmp_r;
                        for (uint8_t a = 0; a < n_aggs; a++) {
                            ray_retain(cnt_col);
                            int64_t agg_name = ray_sym_intern("_agg", 4);
                            if (n_aggs > 1) {
                                char buf[16];
                                int n = snprintf(buf, sizeof(buf), "_agg%d", a);
                                agg_name = ray_sym_intern(buf, (size_t)n);
                            }
                            tmp_r = ray_table_add_col(result, agg_name, cnt_col);
                            ray_release(cnt_col);
                            if (!tmp_r || RAY_IS_ERR(tmp_r)) {
                                ray_release(result);
                                return ray_error("oom", NULL);
                            }
                            result = tmp_r;
                        }
                        return result;
                    }
                }
            }
        }
    }

    /* Extract selection bitmap for pushdown (skip filtered rows in scan loops) */
    const uint64_t* mask = NULL;
    const uint8_t* sel_flags = NULL;
    if (g->selection && g->selection->type == RAY_SEL
        && g->selection->len == nrows) {
        mask = ray_sel_bits(g->selection);
        sel_flags = ray_sel_flags(g->selection);
    }

    if (n_keys > 8 || n_aggs > 8) return ray_error("nyi", NULL);

    /* Resolve key columns (VLA — n_keys ≤ 8; use ≥1 to avoid zero-size VLA UB) */
    uint8_t vla_keys = n_keys > 0 ? n_keys : 1;
    ray_t* key_vecs[vla_keys];
    memset(key_vecs, 0, vla_keys * sizeof(ray_t*));

    uint8_t key_owned[vla_keys]; /* 1 = we allocated via exec_node, must free */
    memset(key_owned, 0, vla_keys * sizeof(uint8_t));
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_t* key_op = ext->keys[k];
        ray_op_ext_t* key_ext = find_ext(g, key_op->id);
        if (key_ext && key_ext->base.opcode == OP_SCAN) {
            key_vecs[k] = ray_table_get_col(tbl, key_ext->sym);
        } else {
            /* Expression key (CASE WHEN etc) — evaluate against current tbl */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, key_op);
            g->table = saved_table;
            if (vec && !RAY_IS_ERR(vec)) {
                key_vecs[k] = vec;
                key_owned[k] = 1;
            }
        }
    }

    /* Resolve agg input columns (VLA — n_aggs ≤ 8; use ≥1 to avoid zero-size VLA UB) */
    uint8_t vla_aggs = n_aggs > 0 ? n_aggs : 1;
    ray_t* agg_vecs[vla_aggs];
    uint8_t agg_owned[vla_aggs]; /* 1 = we allocated via exec_node, must free */
    agg_affine_t agg_affine[vla_aggs];
    agg_linear_t agg_linear[vla_aggs];
    memset(agg_vecs, 0, vla_aggs * sizeof(ray_t*));
    memset(agg_owned, 0, vla_aggs * sizeof(uint8_t));
    memset(agg_affine, 0, vla_aggs * sizeof(agg_affine_t));
    memset(agg_linear, 0, vla_aggs * sizeof(agg_linear_t));

    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_op_t* agg_input_op = ext->agg_ins[a];
        ray_op_ext_t* agg_ext = find_ext(g, agg_input_op->id);

        /* SUM/AVG(scan +/- const): aggregate base scan and apply bias at emit. */
        uint16_t agg_kind = ext->agg_ops[a];
        if ((agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_affine_sumavg_input(g, tbl, agg_input_op, &agg_vecs[a], &agg_affine[a])) {
            continue;
        }

        /* SUM/AVG(integer-linear expr): scalar path can aggregate directly
         * without materializing the expression vector. */
        if (n_keys == 0 && nrows > 0 &&
            (agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_linear_sumavg_input_i64(g, tbl, agg_input_op, &agg_linear[a])) {
            continue;
        }

        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            agg_vecs[a] = ray_table_get_col(tbl, agg_ext->sym);
        } else if (agg_ext && agg_ext->base.opcode == OP_CONST && agg_ext->literal) {
            agg_vecs[a] = agg_ext->literal;
        } else {
            /* Expression node (ADD/MUL etc) — try compiled expression first */
            ray_expr_t agg_expr;
            if (expr_compile(g, tbl, agg_input_op, &agg_expr)) {
                ray_t* vec = expr_eval_full(&agg_expr, nrows);
                if (vec && !RAY_IS_ERR(vec)) {
                    agg_vecs[a] = vec;
                    agg_owned[a] = 1;
                    continue;
                }
            }
            /* Fallback: full recursive evaluation */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, agg_input_op);
            g->table = saved_table;
            if (vec && !RAY_IS_ERR(vec)) {
                agg_vecs[a] = vec;
                agg_owned[a] = 1;
            }
        }
    }

    /* Normalize scalar agg inputs to full-length vectors.
     * Constants and scalar sub-expressions (len=1) must be broadcast to nrows
     * before row-wise aggregation loops. */
    for (uint8_t a = 0; a < n_aggs; a++) {
        if (!agg_vecs[a] || RAY_IS_ERR(agg_vecs[a])) continue;
        if (ext->agg_ops[a] == OP_COUNT) continue; /* value is ignored for COUNT */

        bool needs_broadcast = ray_is_atom(agg_vecs[a]) ||
                               (agg_vecs[a]->type > 0 && agg_vecs[a]->len == 1 && nrows > 1);
        if (!needs_broadcast) continue;

        ray_t* bcast = materialize_broadcast_input(agg_vecs[a], nrows);
        if (!bcast || RAY_IS_ERR(bcast)) {
            for (uint8_t i = 0; i < n_aggs; i++) {
                if (agg_owned[i] && agg_vecs[i]) ray_release(agg_vecs[i]);
            }
            for (uint8_t k = 0; k < n_keys; k++) {
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            }
            return bcast && RAY_IS_ERR(bcast) ? bcast : ray_error("oom", NULL);
        }

        if (agg_owned[a]) ray_release(agg_vecs[a]);
        agg_vecs[a] = bcast;
        agg_owned[a] = 1;
    }

    /* Pre-compute key metadata (VLA — n_keys ≤ 8; vla_keys ≥ 1) */
    void* key_data[vla_keys];
    int8_t key_types[vla_keys];
    uint8_t key_attrs[vla_keys];
    for (uint8_t k = 0; k < n_keys; k++) {
        if (key_vecs[k]) {
            key_data[k]  = ray_data(key_vecs[k]);
            key_types[k] = key_vecs[k]->type;
            key_attrs[k] = key_vecs[k]->attrs;
        } else {
            key_data[k]  = NULL;
            key_types[k] = 0;
            key_attrs[k] = 0;
        }
    }

    /* ---- Scalar aggregate fast path (n_keys == 0): flat vector scan ---- */
    if (n_keys == 0 && nrows > 0) {
        uint8_t need_flags = DA_NEED_COUNT;
        for (uint8_t a = 0; a < n_aggs; a++) {
            uint16_t aop = ext->agg_ops[a];
            if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST)
                need_flags |= DA_NEED_SUM;
            else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
            else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
            else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
        }

        void* agg_ptrs[vla_aggs];
        int8_t agg_types[vla_aggs];
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (agg_vecs[a]) {
                agg_ptrs[a]  = ray_data(agg_vecs[a]);
                agg_types[a] = agg_vecs[a]->type;
            } else {
                agg_ptrs[a]  = NULL;
                agg_types[a] = 0;
            }
        }

        ray_pool_t* sc_pool = ray_pool_get();
        uint32_t sc_n = (sc_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                        ? ray_pool_total_workers(sc_pool) : 1;

        ray_t* sc_hdr;
        da_accum_t* sc_acc = (da_accum_t*)scratch_calloc(&sc_hdr,
            sc_n * sizeof(da_accum_t));
        if (!sc_acc) goto da_path;

        /* Allocate 1-slot accumulators per worker (n_aggs entries) */
        bool alloc_ok = true;
        for (uint32_t w = 0; w < sc_n; w++) {
            if (need_flags & DA_NEED_SUM) {
                sc_acc[w].sum = (da_val_t*)scratch_calloc(&sc_acc[w]._h_sum,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].sum) { alloc_ok = false; break; }
            }
            if (need_flags & DA_NEED_MIN) {
                sc_acc[w].min_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_min,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].min_val) { alloc_ok = false; break; }
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].min_val[a].f = DBL_MAX;
                    else sc_acc[w].min_val[a].i = INT64_MAX;
                }
            }
            if (need_flags & DA_NEED_MAX) {
                sc_acc[w].max_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_max,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].max_val) { alloc_ok = false; break; }
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].max_val[a].f = -DBL_MAX;
                    else sc_acc[w].max_val[a].i = INT64_MIN;
                }
            }
            if (need_flags & DA_NEED_SUMSQ) {
                sc_acc[w].sumsq_f64 = (double*)scratch_calloc(&sc_acc[w]._h_sumsq,
                    n_aggs * sizeof(double));
                if (!sc_acc[w].sumsq_f64) { alloc_ok = false; break; }
            }
            sc_acc[w].count = (int64_t*)scratch_calloc(&sc_acc[w]._h_count,
                1 * sizeof(int64_t));
            if (!sc_acc[w].count) { alloc_ok = false; break; }
        }
        if (!alloc_ok) {
            for (uint32_t w = 0; w < sc_n; w++) da_accum_free(&sc_acc[w]);
            scratch_free(sc_hdr);
            goto da_path;
        }

        scalar_ctx_t sc_ctx = {
            .agg_ptrs   = agg_ptrs,
            .agg_types  = agg_types,
            .agg_ops    = ext->agg_ops,
            .agg_linear = agg_linear,
            .n_aggs     = n_aggs,
            .need_flags = need_flags,
            .mask       = mask,
            .sel_flags  = sel_flags,
            .accums     = sc_acc,
            .n_accums   = sc_n,
        };

        /* Pick specialized tight loop when possible, else generic */
        typedef void (*scalar_fn_t)(void*, uint32_t, int64_t, int64_t);
        scalar_fn_t sc_fn = scalar_accum_fn;
        if (n_aggs == 1 && !mask && agg_ptrs[0] != NULL) {
            uint16_t op0 = ext->agg_ops[0];
            int8_t   t0  = agg_types[0];
            if ((op0 == OP_SUM || op0 == OP_AVG) &&
                (t0 == RAY_I64 || t0 == RAY_SYM || t0 == RAY_TIMESTAMP))
                sc_fn = scalar_sum_i64_fn;
            else if ((op0 == OP_SUM || op0 == OP_AVG) && t0 == RAY_F64)
                sc_fn = scalar_sum_f64_fn;
        } else if (n_aggs == 1 && !mask && agg_linear[0].enabled) {
            uint16_t op0 = ext->agg_ops[0];
            if (op0 == OP_SUM || op0 == OP_AVG)
                sc_fn = scalar_sum_linear_i64_fn;
        }

        if (sc_n > 1)
            ray_pool_dispatch(sc_pool, sc_fn, &sc_ctx, nrows);
        else
            sc_fn(&sc_ctx, 0, 0, nrows);

        /* Merge per-worker accumulators into sc_acc[0] */
        da_accum_t* m = &sc_acc[0];
        for (uint32_t w = 1; w < sc_n; w++) {
            da_accum_t* wa = &sc_acc[w];
            if (need_flags & DA_NEED_SUM) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t merge_op = ext->agg_ops[a];
                    if (merge_op == OP_FIRST) {
                        if (m->count[0] == 0 && wa->count[0] > 0)
                            m->sum[a] = wa->sum[a];
                    } else if (merge_op == OP_LAST) {
                        if (wa->count[0] > 0)
                            m->sum[a] = wa->sum[a];
                    } else {
                        if (agg_types[a] == RAY_F64)
                            m->sum[a].f += wa->sum[a].f;
                        else
                            m->sum[a].i += wa->sum[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_SUMSQ) {
                for (uint8_t a = 0; a < n_aggs; a++)
                    m->sumsq_f64[a] += wa->sumsq_f64[a];
            }
            if (need_flags & DA_NEED_MIN) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[a].f < m->min_val[a].f)
                            m->min_val[a].f = wa->min_val[a].f;
                    } else {
                        if (wa->min_val[a].i < m->min_val[a].i)
                            m->min_val[a].i = wa->min_val[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_MAX) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[a].f > m->max_val[a].f)
                            m->max_val[a].f = wa->max_val[a].f;
                    } else {
                        if (wa->max_val[a].i > m->max_val[a].i)
                            m->max_val[a].i = wa->max_val[a].i;
                    }
                }
            }
            m->count[0] += wa->count[0];
        }
        for (uint32_t w = 1; w < sc_n; w++) da_accum_free(&sc_acc[w]);

        /* Emit 1-row result with no key columns */
        ray_t* result = ray_table_new(n_aggs);
        if (!result || RAY_IS_ERR(result)) {
            da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            return result ? result : ray_error("oom", NULL);
        }

        emit_agg_columns(&result, g, ext, agg_vecs, 1, n_aggs,
                         (double*)m->sum, (int64_t*)m->sum,
                         (double*)m->min_val, (double*)m->max_val,
                         (int64_t*)m->min_val, (int64_t*)m->max_val,
                         m->count, agg_affine, m->sumsq_f64);

        da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
        for (uint8_t a = 0; a < n_aggs; a++)
            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
        for (uint8_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        return result;
    }

da_path:;
    /* ---- Direct-array fast path for low-cardinality integer keys ---- */
    /* Supports multi-key via composite index: product of ranges <= MAX */
    #define DA_MAX_COMPOSITE_SLOTS 262144  /* 256K slots max */
    #define DA_MEM_BUDGET      (256ULL << 20)  /* 256 MB total across all workers */
    #define DA_PER_WORKER_MAX  (6ULL << 20)    /* 6 MB per-worker max */
    {
        bool da_eligible = (nrows > 0 && n_keys > 0 && n_keys <= 8);
        for (uint8_t k = 0; k < n_keys && da_eligible; k++) {
            if (!key_data[k]) { da_eligible = false; break; }
            int8_t t = key_types[k];
            if (t != RAY_I64 && t != RAY_SYM && t != RAY_I32
                && t != RAY_TIMESTAMP && t != RAY_DATE && t != RAY_TIME
                && t != RAY_BOOL && t != RAY_U8 && t != RAY_I16) {
                da_eligible = false;
            }
        }

        int64_t da_key_min[8], da_key_range[8], da_key_stride[8];
        uint64_t total_slots = 1;
        bool da_fits = false;


        if (da_eligible) {
            da_fits = true;
            ray_pool_t* mm_pool = ray_pool_get();
            uint32_t mm_n = (mm_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                            ? ray_pool_total_workers(mm_pool) : 1;
            /* VLA bounded by worker count — max ~2KB per key even on 256-core systems. */
            int64_t mm_mins[mm_n], mm_maxs[mm_n];
            for (uint8_t k = 0; k < n_keys && da_fits; k++) {
                int64_t kmin, kmax;
                for (uint32_t w = 0; w < mm_n; w++) {
                    mm_mins[w] = INT64_MAX;
                    mm_maxs[w] = INT64_MIN;
                }
                minmax_ctx_t mm_ctx = {
                    .key_data       = key_data[k],
                    .key_type       = key_types[k],
                    .key_attrs      = key_attrs[k],
                    .per_worker_min = mm_mins,
                    .per_worker_max = mm_maxs,
                    .n_workers      = mm_n,
                    .mask           = mask,
                    .sel_flags      = sel_flags,
                };
                if (mm_n > 1) {
                    ray_pool_dispatch(mm_pool, minmax_scan_fn, &mm_ctx, nrows);
                } else {
                    minmax_scan_fn(&mm_ctx, 0, 0, nrows);
                }
                kmin = INT64_MAX; kmax = INT64_MIN;
                for (uint32_t w = 0; w < mm_n; w++) {
                    if (mm_mins[w] < kmin) kmin = mm_mins[w];
                    if (mm_maxs[w] > kmax) kmax = mm_maxs[w];
                }
                da_key_min[k]   = kmin;
                da_key_range[k] = kmax - kmin + 1;
                if (da_key_range[k] <= 0) { da_fits = false; break; }
                total_slots *= (uint64_t)da_key_range[k];
                if (total_slots > DA_MAX_COMPOSITE_SLOTS) da_fits = false;
            }
        }

        if (da_fits) {
            /* Compute which accumulator arrays we actually need */
            uint8_t need_flags = DA_NEED_COUNT; /* always need count */
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
                else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                    { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
                else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
                else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
            }

            /* Compute per-worker memory budget.  Actual allocation is 1 union
             * array per type, but MIN/MAX use conditional random writes that
             * perform worse than radix-partitioned HT at high group counts.
             * Weight MIN/MAX at 2x to keep those queries on the HT path. */
            uint32_t arrays_per_agg = 0;
            if (need_flags & DA_NEED_SUM) arrays_per_agg += 1;
            if (need_flags & DA_NEED_MIN) arrays_per_agg += 2; /* 2x: DA MIN slow at high cardinality */
            if (need_flags & DA_NEED_MAX) arrays_per_agg += 2; /* 2x: DA MAX slow at high cardinality */
            if (need_flags & DA_NEED_SUMSQ) arrays_per_agg += 1;
            uint64_t per_worker = total_slots * (arrays_per_agg * n_aggs + 1u) * 8u;
            if (per_worker > DA_PER_WORKER_MAX)
                da_fits = false;
        }

        if (da_fits) {
            /* Recompute need_flags (da_fits may have changed scope) */
            uint8_t need_flags = DA_NEED_COUNT;
            bool all_sum = true;
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
                else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                    { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
                else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
                else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
                if (aop != OP_SUM && aop != OP_AVG && aop != OP_COUNT)
                    all_sum = false;
            }

            /* Compute strides: stride[k] = product of ranges[k+1..n_keys-1]
             * Guard against overflow: if any product exceeds INT64_MAX,
             * fall through to HT path. */
            bool stride_overflow = false;
            for (uint8_t k = 0; k < n_keys; k++) {
                int64_t s = 1;
                for (uint8_t j = k + 1; j < n_keys; j++) {
                    if (da_key_range[j] != 0 && s > INT64_MAX / da_key_range[j]) {
                        stride_overflow = true; break;
                    }
                    s *= da_key_range[j];
                }
                if (stride_overflow) break;
                da_key_stride[k] = s;
            }
            if (stride_overflow) da_fits = false;

            uint32_t n_slots = (uint32_t)total_slots;
            size_t total = (size_t)n_slots * n_aggs;

            void* agg_ptrs[vla_aggs];
            int8_t agg_types[vla_aggs];
            uint32_t agg_f64_mask = 0;
            for (uint8_t a = 0; a < n_aggs; a++) {
                if (agg_vecs[a]) {
                    agg_ptrs[a]  = ray_data(agg_vecs[a]);
                    agg_types[a] = agg_vecs[a]->type;
                    if (agg_vecs[a]->type == RAY_F64)
                        agg_f64_mask |= (1u << a);
                } else {
                    agg_ptrs[a]  = NULL;
                    agg_types[a] = 0;
                }
            }

            ray_pool_t* da_pool = ray_pool_get();
            uint32_t da_n_workers = (da_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                                    ? ray_pool_total_workers(da_pool) : 1;

            /* Check memory budget — need one accumulator set per worker.
             * Weight MIN/MAX at 2x in budget (same as eligibility check) to
             * keep MIN/MAX-heavy queries on the faster radix-HT path. */
            uint32_t arrays_per_agg = 0;
            if (need_flags & DA_NEED_SUM) arrays_per_agg += 1;
            if (need_flags & DA_NEED_MIN) arrays_per_agg += 2;
            if (need_flags & DA_NEED_MAX) arrays_per_agg += 2;
            if (need_flags & DA_NEED_SUMSQ) arrays_per_agg += 1;
            uint64_t per_worker_bytes = (uint64_t)n_slots * (arrays_per_agg * n_aggs + 1u) * 8u;
            if ((uint64_t)da_n_workers * per_worker_bytes > DA_MEM_BUDGET)
                da_n_workers = 1;

            ray_t* accums_hdr;
            da_accum_t* accums = (da_accum_t*)scratch_calloc(&accums_hdr,
                da_n_workers * sizeof(da_accum_t));
            if (!accums) goto ht_path;

            bool alloc_ok = true;
            for (uint32_t w = 0; w < da_n_workers; w++) {
                if (need_flags & DA_NEED_SUM) {
                    accums[w].sum = (da_val_t*)scratch_calloc(&accums[w]._h_sum,
                        total * sizeof(da_val_t));
                    if (!accums[w].sum) { alloc_ok = false; break; }
                }
                if (need_flags & DA_NEED_SUMSQ) {
                    accums[w].sumsq_f64 = (double*)scratch_calloc(&accums[w]._h_sumsq,
                        total * sizeof(double));
                    if (!accums[w].sumsq_f64) { alloc_ok = false; break; }
                }
                if (need_flags & DA_NEED_MIN) {
                    accums[w].min_val = (da_val_t*)scratch_alloc(&accums[w]._h_min,
                        total * sizeof(da_val_t));
                    if (!accums[w].min_val) { alloc_ok = false; break; }
                    for (size_t i = 0; i < total; i++) {
                        uint8_t a = (uint8_t)(i % n_aggs);
                        if (agg_types[a] == RAY_F64) accums[w].min_val[i].f = DBL_MAX;
                        else accums[w].min_val[i].i = INT64_MAX;
                    }
                }
                if (need_flags & DA_NEED_MAX) {
                    accums[w].max_val = (da_val_t*)scratch_alloc(&accums[w]._h_max,
                        total * sizeof(da_val_t));
                    if (!accums[w].max_val) { alloc_ok = false; break; }
                    for (size_t i = 0; i < total; i++) {
                        uint8_t a = (uint8_t)(i % n_aggs);
                        if (agg_types[a] == RAY_F64) accums[w].max_val[i].f = -DBL_MAX;
                        else accums[w].max_val[i].i = INT64_MIN;
                    }
                }
                accums[w].count = (int64_t*)scratch_calloc(&accums[w]._h_count,
                    n_slots * sizeof(int64_t));
                if (!accums[w].count) { alloc_ok = false; break; }
            }
            if (!alloc_ok) {
                for (uint32_t w = 0; w < da_n_workers; w++)
                    da_accum_free(&accums[w]);
                scratch_free(accums_hdr);
                goto ht_path;
            }


            /* Pre-compute per-key element sizes for fast DA reads */
            uint8_t da_key_esz[n_keys];
            for (uint8_t k = 0; k < n_keys; k++)
                da_key_esz[k] = ray_sym_elem_size(key_types[k], key_attrs[k]);

            da_ctx_t da_ctx = {
                .accums      = accums,
                .n_accums    = da_n_workers,
                .key_ptrs    = key_data,
                .key_types   = key_types,
                .key_attrs   = key_attrs,
                .key_esz     = da_key_esz,
                .key_mins    = da_key_min,
                .key_strides = da_key_stride,
                .n_keys      = n_keys,
                .agg_ptrs    = agg_ptrs,
                .agg_types   = agg_types,
                .agg_ops     = ext->agg_ops,
                .n_aggs      = n_aggs,
                .need_flags  = need_flags,
                .agg_f64_mask = agg_f64_mask,
                .all_sum     = all_sum,
                .n_slots     = n_slots,
                .mask        = mask,
                .sel_flags   = sel_flags,
            };

            if (da_n_workers > 1)
                ray_pool_dispatch(da_pool, da_accum_fn, &da_ctx, nrows);
            else
                da_accum_fn(&da_ctx, 0, 0, nrows);

            /* Merge target is always accums[0] */
            da_accum_t* merged = &accums[0];

            /* Check if any agg is FIRST/LAST (needs ordered per-worker merge) */
            bool has_first_last = false;
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_FIRST || aop == OP_LAST) { has_first_last = true; break; }
            }

            /* Merge per-worker accumulators into accums[0].
             * FIRST/LAST require worker-order-dependent merge (sequential).
             * All other ops are commutative — dispatch over disjoint slot
             * ranges for parallel merge. */
            if (has_first_last) {
                for (uint32_t w = 1; w < da_n_workers; w++) {
                    da_accum_t* wa = &accums[w];
                    if (need_flags & DA_NEED_SUMSQ) {
                        for (size_t i = 0; i < total; i++)
                            merged->sumsq_f64[i] += wa->sumsq_f64[i];
                    }
                    if (need_flags & DA_NEED_SUM) {
                        for (uint32_t s = 0; s < n_slots; s++) {
                            size_t base = (size_t)s * n_aggs;
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_SUM || aop == OP_AVG || aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP) {
                                    if (agg_types[a] == RAY_F64) merged->sum[idx].f += wa->sum[idx].f;
                                    else merged->sum[idx].i += wa->sum[idx].i;
                                } else if (aop == OP_FIRST) {
                                    if (merged->count[s] == 0 && wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                }
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MIN) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->min_val[i].f < merged->min_val[i].f)
                                    merged->min_val[i].f = wa->min_val[i].f;
                            } else {
                                if (wa->min_val[i].i < merged->min_val[i].i)
                                    merged->min_val[i].i = wa->min_val[i].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MAX) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->max_val[i].f > merged->max_val[i].f)
                                    merged->max_val[i].f = wa->max_val[i].f;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    for (uint32_t s = 0; s < n_slots; s++)
                        merged->count[s] += wa->count[s];
                }
            } else if (da_n_workers > 1 && n_slots >= 1024 && da_pool) {
                /* Parallel merge: dispatch over disjoint slot ranges */
                da_merge_ctx_t merge_ctx = {
                    .accums        = accums,
                    .n_src_workers = da_n_workers,
                    .need_flags    = need_flags,
                    .n_aggs        = n_aggs,
                    .agg_types     = agg_types,
                    .agg_ops       = ext->agg_ops,
                };
                ray_pool_dispatch(da_pool, da_merge_fn, &merge_ctx, (int64_t)n_slots);
            } else {
                /* Sequential merge for small slot counts */
                for (uint32_t w = 1; w < da_n_workers; w++) {
                    da_accum_t* wa = &accums[w];
                    if (need_flags & DA_NEED_SUMSQ) {
                        for (size_t i = 0; i < total; i++)
                            merged->sumsq_f64[i] += wa->sumsq_f64[i];
                    }
                    if (need_flags & DA_NEED_SUM) {
                        for (uint32_t s = 0; s < n_slots; s++) {
                            size_t base = (size_t)s * n_aggs;
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_FIRST) {
                                    if (merged->count[s] == 0 && wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (agg_types[a] == RAY_F64)
                                    merged->sum[idx].f += wa->sum[idx].f;
                                else
                                    merged->sum[idx].i += wa->sum[idx].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MIN) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->min_val[i].f < merged->min_val[i].f)
                                    merged->min_val[i].f = wa->min_val[i].f;
                            } else {
                                if (wa->min_val[i].i < merged->min_val[i].i)
                                    merged->min_val[i].i = wa->min_val[i].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MAX) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->max_val[i].f > merged->max_val[i].f)
                                    merged->max_val[i].f = wa->max_val[i].f;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    for (uint32_t s = 0; s < n_slots; s++)
                        merged->count[s] += wa->count[s];
                }
            }



            for (uint32_t w = 1; w < da_n_workers; w++)
                da_accum_free(&accums[w]);

            da_val_t* da_sum      = merged->sum;      /* may be NULL if !DA_NEED_SUM */
            da_val_t* da_min_val  = merged->min_val;  /* may be NULL if !DA_NEED_MIN */
            da_val_t* da_max_val  = merged->max_val;  /* may be NULL if !DA_NEED_MAX */
            double*   da_sumsq   = merged->sumsq_f64; /* may be NULL if !DA_NEED_SUMSQ */
            int64_t*  da_count   = merged->count;

            uint32_t grp_count = 0;
            for (uint32_t s = 0; s < n_slots; s++)
                if (da_count[s] > 0) grp_count++;

            int64_t total_cols = n_keys + n_aggs;
            ray_t* result = ray_table_new(total_cols);
            if (!result || RAY_IS_ERR(result)) {
                da_accum_free(&accums[0]); scratch_free(accums_hdr);
                for (uint8_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint8_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                return result ? result : ray_error("oom", NULL);
            }

            /* Key columns — decompose composite slot back to per-key values */
            for (uint8_t k = 0; k < n_keys; k++) {
                ray_t* src_col = key_vecs[k];
                if (!src_col) continue;
                ray_t* key_col = col_vec_new(src_col, (int64_t)grp_count);
                if (!key_col || RAY_IS_ERR(key_col)) continue;
                key_col->len = (int64_t)grp_count;
                uint32_t gi = 0;
                for (uint32_t s = 0; s < n_slots; s++) {
                    if (da_count[s] == 0) continue;
                    int64_t offset = ((int64_t)s / da_key_stride[k]) % da_key_range[k];
                    int64_t key_val = da_key_min[k] + offset;
                    write_col_i64(ray_data(key_col), gi, key_val, src_col->type, key_col->attrs);
                    gi++;
                }
                ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
                int64_t name_id = key_ext ? key_ext->sym : (int64_t)k;
                result = ray_table_add_col(result, name_id, key_col);
                ray_release(key_col);
            }

            /* Agg columns — compact sparse DA arrays into dense, then emit */
            size_t dense_total = (size_t)grp_count * n_aggs;
            ray_t *_h_dsum = NULL, *_h_dmin = NULL, *_h_dmax = NULL;
            ray_t *_h_dsq = NULL, *_h_dcnt = NULL;
            da_val_t* dense_sum     = da_sum     ? (da_val_t*)scratch_alloc(&_h_dsum, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_min_val = da_min_val ? (da_val_t*)scratch_alloc(&_h_dmin, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_max_val = da_max_val ? (da_val_t*)scratch_alloc(&_h_dmax, dense_total * sizeof(da_val_t)) : NULL;
            double*   dense_sumsq   = da_sumsq   ? (double*)scratch_alloc(&_h_dsq, dense_total * sizeof(double)) : NULL;
            int64_t*  dense_counts  = (int64_t*)scratch_alloc(&_h_dcnt, grp_count * sizeof(int64_t));

            uint32_t gi = 0;
            for (uint32_t s = 0; s < n_slots; s++) {
                if (da_count[s] == 0) continue;
                dense_counts[gi] = da_count[s];
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t si = (size_t)s * n_aggs + a;
                    size_t di = (size_t)gi * n_aggs + a;
                    if (dense_sum)     dense_sum[di]     = da_sum[si];
                    if (dense_min_val) dense_min_val[di] = da_min_val[si];
                    if (dense_max_val) dense_max_val[di] = da_max_val[si];
                    if (dense_sumsq)   dense_sumsq[di]   = da_sumsq[si];
                }
                gi++;
            }

            emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                             (double*)dense_sum, (int64_t*)dense_sum,
                             (double*)dense_min_val, (double*)dense_max_val,
                             (int64_t*)dense_min_val, (int64_t*)dense_max_val,
                             dense_counts, agg_affine, dense_sumsq);

            scratch_free(_h_dsum); scratch_free(_h_dmin);
            scratch_free(_h_dmax);
            scratch_free(_h_dsq); scratch_free(_h_dcnt);

            da_accum_free(&accums[0]); scratch_free(accums_hdr);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            return result;
        }
    }

ht_path:;
    /* Compute which accumulator arrays the HT needs based on agg ops.
     * COUNT only reads group row's count field — no accumulator needed. */
    uint8_t ght_need = 0;
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_SUM || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST)
            ght_need |= GHT_NEED_SUM;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
            { ght_need |= GHT_NEED_SUM; ght_need |= GHT_NEED_SUMSQ; }
        if (aop == OP_MIN) ght_need |= GHT_NEED_MIN;
        if (aop == OP_MAX) ght_need |= GHT_NEED_MAX;
    }

    /* RAY_STR keys not yet supported in HT path (16-byte elements vs 8-byte slots) */
    for (uint8_t k = 0; k < n_keys; k++) {
        if (key_types[k] == RAY_STR) {
            for (uint8_t kk = 0; kk < n_keys; kk++)
                if (key_owned[kk] && key_vecs[kk]) ray_release(key_vecs[kk]);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            return ray_error("nyi", NULL);
        }
    }

    /* Compute row-layout: keys + agg values inline */
    ght_layout_t ght_layout = ght_compute_layout(n_keys, n_aggs, agg_vecs, ght_need, ext->agg_ops);

    /* Right-sized hash table: start small, rehash on load > 0.5 */
    uint32_t ht_cap = 256;
    {
        uint64_t target = (uint64_t)nrows < 65536 ? (uint64_t)nrows : 65536;
        if (target < 256) target = 256;
        while (ht_cap < target) ht_cap *= 2;
    }

    /* Parallel path: radix-partitioned group-by */
    ray_pool_t* pool = ray_pool_get();
    uint32_t n_total = pool ? ray_pool_total_workers(pool) : 1;

    group_ht_t single_ht;
    group_ht_t* final_ht = NULL;
    ray_t* result = NULL;

    ray_t* radix_bufs_hdr = NULL;
    radix_buf_t* radix_bufs = NULL;
    ray_t* part_hts_hdr = NULL;
    group_ht_t*  part_hts   = NULL;

    if (pool && nrows >= RAY_PARALLEL_THRESHOLD && n_total > 1) {
        size_t n_bufs = (size_t)n_total * RADIX_P;
        radix_bufs = (radix_buf_t*)scratch_calloc(&radix_bufs_hdr,
            n_bufs * sizeof(radix_buf_t));
        if (!radix_bufs) goto sequential_fallback;

        /* Pre-size each buffer: 1.5x expected, capped so total ≤ 2 GB.
         * Buffers grow on demand via radix_buf_push doubling. */
        uint32_t buf_init = (uint32_t)((uint64_t)nrows / (RADIX_P * n_total));
        if (buf_init < 64) buf_init = 64;
        buf_init = buf_init + buf_init / 2;  /* 1.5x headroom */
        uint16_t estride = ght_layout.entry_stride;
        {
            /* Cap: total pre-alloc ≤ 2 GB */
            size_t total_pre = (size_t)n_bufs * buf_init * estride;
            if (total_pre > (size_t)2 << 30) {
                buf_init = (uint32_t)(((size_t)2 << 30) / ((size_t)n_bufs * estride));
                if (buf_init < 64) buf_init = 64;
            }
        }
        for (size_t i = 0; i < n_bufs; i++) {
            radix_bufs[i].data = (char*)scratch_alloc(
                &radix_bufs[i]._hdr, (size_t)buf_init * estride);
            radix_bufs[i].count = 0;
            radix_bufs[i].cap = buf_init;
        }

        /* Phase 1: parallel hash + copy keys/agg values into fat entries */
        radix_phase1_ctx_t p1ctx = {
            .key_data  = key_data,
            .key_types = key_types,
            .key_attrs = key_attrs,
            .agg_vecs  = agg_vecs,
            .n_workers = n_total,
            .bufs      = radix_bufs,
            .layout    = ght_layout,
            .mask      = mask,
            .sel_flags = sel_flags,
        };
        ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, nrows);
        CHECK_CANCEL_GOTO(pool, cleanup);

        /* Check for OOM during phase 1 radix buffer growth */
        {
            bool phase1_oom = false;
            for (size_t i = 0; i < n_bufs; i++) {
                if (radix_bufs[i].oom) { phase1_oom = true; break; }
            }
            if (phase1_oom) {
                for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
                scratch_free(radix_bufs_hdr);
                radix_bufs = NULL;
                goto sequential_fallback;
            }
        }

        /* Phase 2: parallel per-partition aggregation (no column access) */
        part_hts = (group_ht_t*)scratch_calloc(&part_hts_hdr,
            RADIX_P * sizeof(group_ht_t));
        if (!part_hts) {
            for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
            scratch_free(radix_bufs_hdr);
            radix_bufs = NULL;
            goto sequential_fallback;
        }

        radix_phase2_ctx_t p2ctx = {
            .key_types   = key_types,
            .n_keys      = n_keys,
            .n_workers   = n_total,
            .bufs        = radix_bufs,
            .part_hts    = part_hts,
            .layout      = ght_layout,
        };
        ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
        CHECK_CANCEL_GOTO(pool, cleanup);

        /* Prefix offsets */
        uint32_t part_offsets[RADIX_P + 1];
        part_offsets[0] = 0;
        for (uint32_t p = 0; p < RADIX_P; p++)
            part_offsets[p + 1] = part_offsets[p] + part_hts[p].grp_count;
        uint32_t total_grps = part_offsets[RADIX_P];

        /* Build result directly from partition HTs */
        int64_t total_cols = n_keys + n_aggs;
        result = ray_table_new(total_cols);
        if (!result || RAY_IS_ERR(result)) goto cleanup;

        /* Pre-allocate key columns */
        ray_t* key_cols[n_keys];
        char* key_dsts[n_keys];
        int8_t key_out_types[n_keys];
        uint8_t key_esizes[n_keys];
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* src_col = key_vecs[k];
            key_cols[k] = NULL;
            key_dsts[k] = NULL;
            key_out_types[k] = 0;
            key_esizes[k] = 0;
            if (!src_col) continue;
            uint8_t esz = ray_sym_elem_size(src_col->type, src_col->attrs);
            ray_t* new_col;
            if (src_col->type == RAY_SYM)
                new_col = ray_sym_vec_new(src_col->attrs & RAY_SYM_W_MASK, (int64_t)total_grps);
            else
                new_col = ray_vec_new(src_col->type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = (int64_t)total_grps;
            key_cols[k] = new_col;
            key_dsts[k] = (char*)ray_data(new_col);
            key_out_types[k] = src_col->type;
            key_esizes[k] = esz;
        }

        /* Pre-allocate agg result vectors */
        agg_out_t agg_outs[n_aggs];
        ray_t* agg_cols[n_aggs];
        for (uint8_t a = 0; a < n_aggs; a++) {
            uint16_t agg_op = ext->agg_ops[a];
            ray_t* agg_col = agg_vecs[a];
            bool is_f64 = agg_col && agg_col->type == RAY_F64;
            int8_t out_type;
            switch (agg_op) {
                case OP_AVG:
                case OP_STDDEV: case OP_STDDEV_POP:
                case OP_VAR: case OP_VAR_POP:
                    out_type = RAY_F64; break;
                case OP_COUNT: out_type = RAY_I64; break;
                case OP_SUM: case OP_PROD:
                    out_type = is_f64 ? RAY_F64 : RAY_I64; break;
                default:
                    out_type = agg_col ? agg_col->type : RAY_I64; break;
            }
            ray_t* new_col = ray_vec_new(out_type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) { agg_cols[a] = NULL; continue; }
            new_col->len = (int64_t)total_grps;
            agg_cols[a] = new_col;
            agg_outs[a] = (agg_out_t){
                .out_type = out_type, .src_f64 = is_f64,
                .agg_op = agg_op,
                .affine = agg_affine[a].enabled,
                .bias_f64 = agg_affine[a].bias_f64,
                .bias_i64 = agg_affine[a].bias_i64,
                .dst = ray_data(new_col),
            };
        }

        /* Phase 3: parallel key gather + agg result building from inline rows */
        {
            radix_phase3_ctx_t p3ctx = {
                .part_hts     = part_hts,
                .part_offsets = part_offsets,
                .key_dsts     = key_dsts,
                .key_types    = key_out_types,
                .key_attrs    = key_attrs,
                .key_esizes   = key_esizes,
                .n_keys       = n_keys,
                .agg_outs     = agg_outs,
                .n_aggs       = n_aggs,
            };
            ray_pool_dispatch_n(pool, radix_phase3_fn, &p3ctx, RADIX_P);
        }

        /* Add key columns to result */
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
            int64_t name_id = key_ext ? key_ext->sym : k;
            result = ray_table_add_col(result, name_id, key_cols[k]);
            ray_release(key_cols[k]);
        }

        /* Add agg columns to result */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            uint16_t agg_op = ext->agg_ops[a];
            ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
            int64_t name_id;
            if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
                ray_t* name_atom = ray_sym_str(agg_ext->sym);
                const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
                size_t blen = base ? ray_str_len(name_atom) : 0;
                const char* sfx = "";
                size_t slen = 0;
                switch (agg_op) {
                    case OP_SUM:   sfx = "_sum";   slen = 4; break;
                    case OP_COUNT: sfx = "_count"; slen = 6; break;
                    case OP_AVG:   sfx = "_mean";  slen = 5; break;
                    case OP_MIN:   sfx = "_min";   slen = 4; break;
                    case OP_MAX:   sfx = "_max";   slen = 4; break;
                    case OP_FIRST: sfx = "_first"; slen = 6; break;
                    case OP_LAST:  sfx = "_last";  slen = 5; break;
                    case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                    case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                    case OP_VAR:        sfx = "_var";        slen = 4; break;
                    case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
                }
                char buf[256];
                ray_t* name_dyn_hdr = NULL;
                char* nbp = buf;
                size_t nbc = sizeof(buf);
                if (base && blen + slen >= sizeof(buf)) {
                    nbp = (char*)scratch_alloc(&name_dyn_hdr, blen + slen + 1);
                    if (nbp) nbc = blen + slen + 1;
                    else { nbp = buf; nbc = sizeof(buf); }
                }
                if (base && blen + slen < nbc) {
                    memcpy(nbp, base, blen);
                    memcpy(nbp + blen, sfx, slen);
                    name_id = ray_sym_intern(nbp, blen + slen);
                } else {
                    name_id = agg_ext->sym;
                }
                scratch_free(name_dyn_hdr);
            } else {
                name_id = (int64_t)(n_keys + a);
            }
            result = ray_table_add_col(result, name_id, agg_cols[a]);
            ray_release(agg_cols[a]);
        }

        goto cleanup;
    }

sequential_fallback:;
    /* Sequential path using row-layout HT */
    if (!group_ht_init(&single_ht, ht_cap, &ght_layout)) {
        result = ray_error("oom", NULL);
        goto cleanup;
    }
    group_rows_range(&single_ht, key_data, key_types, key_attrs, agg_vecs, 0, nrows);

    final_ht = &single_ht;

    /* Build result from sequential HT (inline row layout) */
    {
    uint32_t grp_count = final_ht->grp_count;
    const ght_layout_t* ly = &final_ht->layout;
    int64_t total_cols = n_keys + n_aggs;
    result = ray_table_new(total_cols);
    if (!result || RAY_IS_ERR(result)) goto cleanup;

    /* Key columns: read from inline group rows, narrow to original type */
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* src_col = key_vecs[k];
        if (!src_col) continue;
        uint8_t esz = col_esz(src_col);
        int8_t kt = src_col->type;

        ray_t* new_col = col_vec_new(src_col, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;

        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            int64_t kv = ((const int64_t*)(row + 8))[k];
            if (kt == RAY_F64) {
                char* dst = (char*)ray_data(new_col) + (size_t)gi * esz;
                memcpy(dst, &kv, 8);
            } else
                write_col_i64(ray_data(new_col), gi, kv, kt, new_col->attrs);
        }

        ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
        int64_t name_id = key_ext ? key_ext->sym : k;
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }

    /* Agg columns from inline accumulators */
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
                out_type = RAY_F64; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col = ray_vec_new(out_type, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;

        int8_t s = ly->agg_val_slot[a]; /* unified accum slot */
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            int64_t cnt = *(const int64_t*)(const void*)row;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64 * cnt;
                        break;
                    case OP_AVG:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s) / cnt
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / cnt;
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64;
                        break;
                    case OP_MIN:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                        break;
                    case OP_MAX:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        double sum_val = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                        double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                        double mean = cnt > 0 ? sum_val / cnt : 0.0;
                        double var_pop = cnt > 0 ? sq_val / cnt - mean * mean : 0.0;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = cnt > 0 ? var_pop : NAN;
                        else if (agg_op == OP_VAR) v = cnt > 1 ? var_pop * cnt / (cnt - 1) : NAN;
                        else if (agg_op == OP_STDDEV_POP) v = cnt > 0 ? sqrt(var_pop) : NAN;
                        else v = cnt > 1 ? sqrt(var_pop * cnt / (cnt - 1)) : NAN;
                        break;
                    }
                    default: v = 0.0; break;
                }
                ((double*)ray_data(new_col))[gi] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:
                        v = ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_i64 * cnt;
                        break;
                    case OP_COUNT: v = cnt; break;
                    case OP_MIN:   v = ROW_RD_I64(row, ly->off_min, s); break;
                    case OP_MAX:   v = ROW_RD_I64(row, ly->off_max, s); break;
                    case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly->off_sum, s); break;
                    default:       v = 0; break;
                }
                ((int64_t*)ray_data(new_col))[gi] = v;
            }
        }

        /* Generate unique column name */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            ray_t* name_atom = ray_sym_str(agg_ext->sym);
            const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
            size_t blen = base ? ray_str_len(name_atom) : 0;
            const char* sfx = "";
            size_t slen = 0;
            switch (agg_op) {
                case OP_SUM:   sfx = "_sum";   slen = 4; break;
                case OP_COUNT: sfx = "_count"; slen = 6; break;
                case OP_AVG:   sfx = "_mean";  slen = 5; break;
                case OP_MIN:   sfx = "_min";   slen = 4; break;
                case OP_MAX:   sfx = "_max";   slen = 4; break;
                case OP_FIRST: sfx = "_first"; slen = 6; break;
                case OP_LAST:  sfx = "_last";  slen = 5; break;
                case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                case OP_VAR:        sfx = "_var";        slen = 4; break;
                case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
            }
            char buf[256];
            if (base && blen + slen < sizeof(buf)) {
                memcpy(buf, base, blen);
                memcpy(buf + blen, sfx, slen);
                name_id = ray_sym_intern(buf, blen + slen);
            } else {
                name_id = agg_ext->sym;
            }
        } else {
            /* Expression agg input — synthetic name like "_e0_sum" */
            char nbuf[32];
            int np = 0;
            nbuf[np++] = '_'; nbuf[np++] = 'e';
            /* Multi-digit agg index */
            { uint8_t v = a; char dig[3]; int nd = 0;
              do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
              while (nd--) nbuf[np++] = dig[nd]; }
            const char* nsfx = "";
            size_t nslen = 0;
            switch (agg_op) {
                case OP_SUM:   nsfx = "_sum";   nslen = 4; break;
                case OP_COUNT: nsfx = "_count"; nslen = 6; break;
                case OP_AVG:   nsfx = "_mean";  nslen = 5; break;
                case OP_MIN:   nsfx = "_min";   nslen = 4; break;
                case OP_MAX:   nsfx = "_max";   nslen = 4; break;
                case OP_FIRST: nsfx = "_first"; nslen = 6; break;
                case OP_LAST:  nsfx = "_last";  nslen = 5; break;
                case OP_STDDEV:     nsfx = "_stddev";     nslen = 7; break;
                case OP_STDDEV_POP: nsfx = "_stddev_pop"; nslen = 11; break;
                case OP_VAR:        nsfx = "_var";        nslen = 4; break;
                case OP_VAR_POP:    nsfx = "_var_pop";    nslen = 8; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }
    }

cleanup:
    if (final_ht == &single_ht) {
        group_ht_free(&single_ht);
    }
    if (radix_bufs) {
        size_t n_bufs = (size_t)n_total * RADIX_P;
        for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
        scratch_free(radix_bufs_hdr);
    }
    if (part_hts) {
        for (uint32_t p = 0; p < RADIX_P; p++) {
            if (part_hts[p].rows) group_ht_free(&part_hts[p]);
        }
        scratch_free(part_hts_hdr);
    }
    for (uint8_t a = 0; a < n_aggs; a++)
        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
    for (uint8_t k = 0; k < n_keys; k++)
        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_group_per_partition — per-partition GROUP BY with merge
 *
 * Runs exec_group on each partition independently (zero-copy mmap segments),
 * then merges the small partial results via a second exec_group pass.
 *
 * Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX, FIRST→FIRST, LAST→LAST.
 * AVG: decomposed into SUM+COUNT per partition, merged, then divided.
 * STDDEV/VAR: decomposed into SUM(x)+SUM(x²)+COUNT(x) per partition,
 *   merged with SUM, then final variance/stddev computed from merged totals.
 *
 * Returns NULL if any step fails (caller falls through to concat path).
 * -------------------------------------------------------------------------- */
static ray_t* __attribute__((noinline))
exec_group_per_partition(ray_t* parted_tbl, ray_op_ext_t* ext,
                         int32_t n_parts, const int64_t* key_syms,
                         const int64_t* agg_syms, int has_avg,
                         int has_stddev, int64_t group_limit) {

    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Guard: fixed-size arrays below cap at 24 agg ops.
     * Each AVG adds 1 extra (COUNT), each STDDEV/VAR adds 2 (SUM_SQ + COUNT).
     * n_aggs + n_avg + 2*n_std must stay within 24. */
    if (n_aggs > 8 || n_keys > 8) return NULL;

    /* Identify MAPCOMMON vs PARTED keys.  MAPCOMMON keys are constant
     * within a partition, so they are excluded from per-partition GROUP BY
     * and reconstructed after concat. */
    uint8_t  n_mc_keys = 0;
    int64_t  mc_sym_ids[8];
    uint8_t  n_part_keys = 0;
    int64_t  pk_syms[8];       /* non-MAPCOMMON key sym IDs */

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
        if (pcol && pcol->type == RAY_MAPCOMMON) {
            mc_sym_ids[n_mc_keys++] = key_syms[k];
        } else {
            pk_syms[n_part_keys++] = key_syms[k];
        }
    }

    /* LIMIT pushdown: when all GROUP BY keys are MAPCOMMON (n_part_keys==0),
     * each partition produces exactly 1 group.  Limit the partition loop. */
    if (group_limit > 0 && n_part_keys == 0 && group_limit < n_parts)
        n_parts = (int32_t)group_limit;

    /* Decomposition: AVG(x) → SUM(x) + COUNT(x).
     * STDDEV/VAR(x) → SUM(x) + SUM(x²) + COUNT(x).
     * Build per-partition agg_ops with decomposed ops, then merge ops. */
    uint16_t part_ops[24];   /* per-partition agg ops */
    uint16_t merge_ops[24];  /* merge agg ops */
    uint8_t  avg_idx[8];     /* which original agg slots are AVG */
    uint8_t  std_idx[8];     /* which original agg slots are STDDEV/VAR */
    uint16_t std_orig_op[8]; /* original op for each std slot */
    uint8_t  n_avg = 0;
    uint8_t  n_std = 0;
    uint8_t  part_n_aggs = n_aggs;
    /* stddev_needs_sq[a]: index into part_ops for the SUM(x²) slot */
    uint8_t  std_sq_slot[8];
    uint8_t  std_cnt_slot[8];

    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_AVG) {
            part_ops[a] = OP_SUM;     /* partition: compute SUM */
            avg_idx[n_avg++] = a;
        } else if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
                   aop == OP_VAR || aop == OP_VAR_POP) {
            part_ops[a] = OP_SUM;     /* partition: compute SUM(x) */
            std_orig_op[n_std] = aop;
            std_idx[n_std++] = a;
        } else {
            part_ops[a] = aop;
        }
    }
    /* Guard: total decomposed slots must fit */
    if (n_aggs + n_avg + 2 * n_std > 24) return NULL;

    /* Append SUM(x²) for each STDDEV/VAR slot */
    for (uint8_t i = 0; i < n_std; i++) {
        std_sq_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_SUM;  /* SUM(x²) */
    }
    /* Append COUNT for each AVG column */
    for (uint8_t i = 0; i < n_avg; i++)
        part_ops[part_n_aggs++] = OP_COUNT;
    /* Append COUNT for each STDDEV/VAR column */
    for (uint8_t i = 0; i < n_std; i++) {
        std_cnt_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_COUNT;
    }

    /* Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX,
     * FIRST→FIRST, LAST→LAST, all appended slots → SUM */
    for (uint8_t a = 0; a < part_n_aggs; a++) {
        merge_ops[a] = part_ops[a];
        if (merge_ops[a] == OP_COUNT) merge_ops[a] = OP_SUM;
    }

    /* Agg input syms for the decomposed ops.
     * AVG's COUNT uses same input column as the AVG itself.
     * STDDEV's SUM(x²) and COUNT use same input column as the STDDEV. */
    int64_t part_agg_syms[24];
    /* Flag: slot needs x*x graph node (for SUM(x²)) */
    int part_needs_sq[24];
    memset(part_needs_sq, 0, sizeof(part_needs_sq));

    for (uint8_t a = 0; a < n_aggs; a++)
        part_agg_syms[a] = agg_syms[a];
    /* SUM(x²) slots for STDDEV/VAR */
    for (uint8_t i = 0; i < n_std; i++) {
        part_agg_syms[std_sq_slot[i]] = agg_syms[std_idx[i]];
        part_needs_sq[std_sq_slot[i]] = 1;
    }
    /* COUNT slots for AVG */
    for (uint8_t i = 0; i < n_avg; i++)
        part_agg_syms[n_aggs + n_std + i] = agg_syms[avg_idx[i]];
    /* COUNT slots for STDDEV/VAR */
    for (uint8_t i = 0; i < n_std; i++)
        part_agg_syms[std_cnt_slot[i]] = agg_syms[std_idx[i]];

    /* ---- Batched incremental merge ----
     * Process partitions in batches of MERGE_BATCH.  After each batch:
     *   Phase 1: exec_group each partition in batch → batch_partials[]
     *   Phase 2: concat (running + batch_partials + MAPCOMMON) → merge_tbl
     *   Phase 3: merge GROUP BY → new running
     * Bounds peak memory to O(MERGE_BATCH × groups_per_partition). */
#define MERGE_BATCH 8

    /* Capture agg column name IDs from first partition result */
    int64_t agg_name_ids[24];
    int agg_names_captured = 0;

    ray_t* running = NULL;
    ray_t* merge_tbl = NULL;      /* last merge table (for column name fixup) */

    for (int32_t batch_start = 0; batch_start < n_parts;
         batch_start += MERGE_BATCH) {

        int32_t batch_end = batch_start + MERGE_BATCH;
        if (batch_end > n_parts) batch_end = n_parts;
        int32_t batch_n = batch_end - batch_start;

        /* Phase 1: exec_group each partition in this batch */
        ray_t* bp[MERGE_BATCH];
        memset(bp, 0, sizeof(bp));

        for (int32_t bi = 0; bi < batch_n; bi++) {
            int32_t p = batch_start + bi;

            /* Collect unique agg input sym IDs (avoid duplicate columns) */
            int64_t unique_agg[24];
            int n_unique_agg = 0;
            for (uint8_t a = 0; a < part_n_aggs; a++) {
                int dup = 0;
                for (int j = 0; j < n_unique_agg; j++)
                    if (unique_agg[j] == part_agg_syms[a]) { dup = 1; break; }
                if (!dup) {
                    for (uint8_t k = 0; k < n_keys; k++)
                        if (key_syms[k] == part_agg_syms[a]) { dup = 1; break; }
                    if (!dup) unique_agg[n_unique_agg++] = part_agg_syms[a];
                }
            }

            ray_t* sub = ray_table_new((int64_t)(n_part_keys + n_unique_agg));
            if (!sub || RAY_IS_ERR(sub)) goto batch_fail;

            for (uint8_t k = 0; k < n_part_keys; k++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, pk_syms[k]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                ray_retain(seg);
                sub = ray_table_add_col(sub, pk_syms[k], seg);
                ray_release(seg);
            }
            for (int j = 0; j < n_unique_agg; j++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, unique_agg[j]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                ray_retain(seg);
                sub = ray_table_add_col(sub, unique_agg[j], seg);
                ray_release(seg);
            }

            ray_graph_t* pg = ray_graph_new(sub);
            if (!pg) { ray_release(sub); goto batch_fail; }

            ray_op_t* pkeys[8];
            for (uint8_t k = 0; k < n_part_keys; k++) {
                ray_t* sym_atom = ray_sym_str(pk_syms[k]);
                pkeys[k] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            ray_op_t* pagg_ins[24];
            for (uint8_t a = 0; a < part_n_aggs; a++) {
                ray_t* sym_atom = ray_sym_str(part_agg_syms[a]);
                pagg_ins[a] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            for (uint8_t j = 0; j < n_std; j++) {
                uint8_t sq = std_sq_slot[j];
                ray_op_t* x = pagg_ins[sq];
                pagg_ins[sq] = ray_mul(pg, x, x);
            }

            ray_op_t* proot = ray_group(pg, pkeys, n_part_keys,
                                       part_ops, pagg_ins, part_n_aggs);
            proot = ray_optimize(pg, proot);
            bp[bi] = ray_execute(pg, proot);
            ray_graph_free(pg);
            ray_release(sub);

            if (!bp[bi] || RAY_IS_ERR(bp[bi])) goto batch_fail;

            /* Capture agg column name IDs once (all partials share names) */
            if (!agg_names_captured) {
                for (uint8_t a = 0; a < part_n_aggs; a++)
                    agg_name_ids[a] = ray_table_col_name(
                        bp[bi], (int64_t)n_part_keys + a);
                agg_names_captured = 1;
            }
        }

        /* Phase 2: concat (running + batch_partials + MAPCOMMON) */
        int64_t mrows = running ? ray_table_nrows(running) : 0;
        for (int32_t i = 0; i < batch_n; i++)
            mrows += ray_table_nrows(bp[i]);

        if (merge_tbl) { ray_release(merge_tbl); merge_tbl = NULL; }
        merge_tbl = ray_table_new((int64_t)(n_keys + part_n_aggs));
        if (!merge_tbl || RAY_IS_ERR(merge_tbl)) {
            merge_tbl = NULL; goto batch_fail;
        }

        /* Key columns */
        for (uint8_t k = 0; k < n_keys; k++) {
            int is_mc = 0;
            for (uint8_t m = 0; m < n_mc_keys; m++)
                if (mc_sym_ids[m] == key_syms[k]) { is_mc = 1; break; }

            /* Type reference for column allocation */
            ray_t* tref = NULL;
            if (running) {
                tref = ray_table_get_col(running, key_syms[k]);
            } else if (is_mc) {
                ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                tref = ((ray_t**)ray_data(mc_col))[0];
            } else {
                tref = ray_table_get_col(bp[0], key_syms[k]);
            }
            if (!tref) goto batch_fail;

            size_t esz = (size_t)col_esz(tref);
            ray_t* flat = col_vec_new(tref, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            /* Copy from running result */
            if (running) {
                ray_t* rc = ray_table_get_col(running, key_syms[k]);
                if (rc && rc->len > 0) {
                    memcpy(out, ray_data(rc), (size_t)rc->len * esz);
                    off = rc->len;
                }
            }

            /* Copy from batch partials */
            for (int32_t i = 0; i < batch_n; i++) {
                int64_t pnrows = ray_table_nrows(bp[i]);
                if (is_mc) {
                    /* MAPCOMMON: replicate this partition's key value */
                    int32_t p = batch_start + i;
                    ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                    ray_t* mc_kv = ((ray_t**)ray_data(mc_col))[0];
                    const char* kdata = (const char*)ray_data(mc_kv);
                    for (int64_t r = 0; r < pnrows; r++)
                        memcpy(out + (size_t)(off + r) * esz,
                               kdata + (size_t)p * esz, esz);
                    off += pnrows;
                } else {
                    ray_t* pc = ray_table_get_col(bp[i], key_syms[k]);
                    if (pc && pc->len > 0) {
                        memcpy(out + (size_t)off * esz,
                               ray_data(pc), (size_t)pc->len * esz);
                        off += pc->len;
                    }
                }
            }

            merge_tbl = ray_table_add_col(merge_tbl, key_syms[k], flat);
            ray_release(flat);
        }

        /* Agg columns */
        for (uint8_t a = 0; a < part_n_aggs; a++) {
            ray_t* tref = running
                ? ray_table_get_col_idx(running, (int64_t)n_keys + a)
                : ray_table_get_col_idx(bp[0], (int64_t)n_part_keys + a);
            if (!tref) goto batch_fail;

            size_t esz = (size_t)col_esz(tref);
            ray_t* flat = col_vec_new(tref, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            if (running) {
                ray_t* rc = ray_table_get_col_idx(running, (int64_t)n_keys + a);
                if (rc && rc->len > 0) {
                    memcpy(out, ray_data(rc), (size_t)rc->len * esz);
                    off = rc->len;
                }
            }

            for (int32_t i = 0; i < batch_n; i++) {
                ray_t* pc = ray_table_get_col_idx(bp[i],
                                                 (int64_t)n_part_keys + a);
                if (pc && pc->len > 0) {
                    memcpy(out + (size_t)off * esz,
                           ray_data(pc), (size_t)pc->len * esz);
                    off += pc->len;
                }
            }

            merge_tbl = ray_table_add_col(merge_tbl, agg_name_ids[a], flat);
            ray_release(flat);
        }

        /* Free batch partials */
        for (int32_t i = 0; i < batch_n; i++) {
            ray_release(bp[i]);
            bp[i] = NULL;
        }

        /* Phase 3: merge GROUP BY */
        ray_graph_t* mg = ray_graph_new(merge_tbl);
        if (!mg) goto batch_fail;

        ray_op_t* mkeys[8];
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* sym_atom = ray_sym_str(key_syms[k]);
            mkeys[k] = ray_scan(mg, ray_str_ptr(sym_atom));
        }

        ray_op_t* magg_ins[24];
        for (uint8_t a = 0; a < part_n_aggs; a++) {
            ray_t* agg_name = ray_sym_str(agg_name_ids[a]);
            magg_ins[a] = ray_scan(mg, ray_str_ptr(agg_name));
        }

        ray_op_t* mroot = ray_group(mg, mkeys, n_keys,
                                   merge_ops, magg_ins, part_n_aggs);
        mroot = ray_optimize(mg, mroot);
        ray_t* new_running = ray_execute(mg, mroot);
        ray_graph_free(mg);

        if (running) ray_release(running);
        running = new_running;

        if (!running || RAY_IS_ERR(running)) {
            ray_release(merge_tbl);
            return NULL;
        }

        /* Rename running's agg columns back to the original partial names.
         * Without this, each merge adds an extra suffix (e.g. v1_sum → v1_sum_sum). */
        for (uint8_t a = 0; a < part_n_aggs; a++)
            ray_table_set_col_name(running, (int64_t)n_keys + a, agg_name_ids[a]);

        continue;

batch_fail:
        for (int32_t i = 0; i < batch_n; i++)
            if (bp[i]) ray_release(bp[i]);
        if (running) ray_release(running);
        if (merge_tbl) ray_release(merge_tbl);
        return NULL;
    }

    ray_t* result = running;

    if (!result || RAY_IS_ERR(result)) {
        if (merge_tbl) ray_release(merge_tbl);
        return NULL;
    }

    int64_t rncols = ray_table_ncols(result);

    /* AVG/STDDEV post-processing: build trimmed table (n_keys + n_aggs cols),
     * computing final AVG = SUM/COUNT and STDDEV/VAR from SUM, SUM_SQ, COUNT. */
    if (has_avg || has_stddev) {
        ray_t* trimmed = ray_table_new((int64_t)(n_keys + n_aggs));
        if (!trimmed || RAY_IS_ERR(trimmed)) {
            ray_release(result);
            if (merge_tbl) ray_release(merge_tbl);
            return NULL;
        }

        for (int64_t c = 0; c < (int64_t)(n_keys + n_aggs) && c < rncols; c++) {
            int64_t nm = ray_table_col_name(result, c);

            /* Check if this agg column is an AVG or STDDEV/VAR slot */
            int is_avg_slot = 0, is_std_slot = 0;
            uint8_t avg_i = 0, std_i = 0;
            if (c >= n_keys) {
                uint8_t a = (uint8_t)(c - n_keys);
                for (uint8_t j = 0; j < n_avg; j++) {
                    if (avg_idx[j] == a) { is_avg_slot = 1; avg_i = j; break; }
                }
                for (uint8_t j = 0; j < n_std; j++) {
                    if (std_idx[j] == a) { is_std_slot = 1; std_i = j; break; }
                }
            }

            if (is_avg_slot) {
                /* AVG = SUM(x) / COUNT(x) */
                int64_t sum_ci = c;
                /* AVG COUNT slots: after n_aggs + n_std SUM_SQ slots */
                int64_t cnt_ci = (int64_t)n_keys + n_aggs + n_std + avg_i;
                ray_t* sum_col = ray_table_get_col_idx(result, sum_ci);
                ray_t* cnt_col = (cnt_ci < rncols) ? ray_table_get_col_idx(result, cnt_ci) : NULL;
                if (!sum_col || !cnt_col) {
                    if (sum_col) {
                        ray_retain(sum_col);
                        trimmed = ray_table_add_col(trimmed, nm, sum_col);
                        ray_release(sum_col);
                    }
                    continue;
                }

                int64_t nrows = sum_col->len;
                ray_t* avg_col = ray_vec_new(RAY_F64, nrows);
                if (!avg_col || RAY_IS_ERR(avg_col)) {
                    ray_release(trimmed); ray_release(result);
                    if (merge_tbl) ray_release(merge_tbl);
                    return NULL;
                }
                avg_col->len = nrows;

                double* out = (double*)ray_data(avg_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? sv[r] / (double)cv[r] : 0.0;
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? (double)sv[r] / (double)cv[r] : 0.0;
                }
                trimmed = ray_table_add_col(trimmed, nm, avg_col);
                ray_release(avg_col);
            } else if (is_std_slot) {
                /* STDDEV/VAR from merged SUM(x), SUM(x²), COUNT(x):
                 * var_pop = SUM_SQ/N - (SUM/N)²
                 * var_samp = var_pop * N/(N-1)
                 * stddev_pop = sqrt(var_pop), stddev_samp = sqrt(var_samp) */
                int64_t sum_ci = c;
                int64_t sq_ci  = (int64_t)n_keys + std_sq_slot[std_i];
                int64_t cnt_ci = (int64_t)n_keys + std_cnt_slot[std_i];
                ray_t* sum_col = ray_table_get_col_idx(result, sum_ci);
                ray_t* sq_col  = (sq_ci < rncols) ? ray_table_get_col_idx(result, sq_ci) : NULL;
                ray_t* cnt_col = (cnt_ci < rncols) ? ray_table_get_col_idx(result, cnt_ci) : NULL;
                if (!sum_col || !sq_col || !cnt_col) {
                    if (sum_col) {
                        ray_retain(sum_col);
                        trimmed = ray_table_add_col(trimmed, nm, sum_col);
                        ray_release(sum_col);
                    }
                    continue;
                }

                int64_t nrows = sum_col->len;
                ray_t* out_col = ray_vec_new(RAY_F64, nrows);
                if (!out_col || RAY_IS_ERR(out_col)) {
                    ray_release(trimmed); ray_release(result);
                    if (merge_tbl) ray_release(merge_tbl);
                    return NULL;
                }
                out_col->len = nrows;
                double* out = (double*)ray_data(out_col);

                uint16_t orig_op = std_orig_op[std_i];
                /* SUM(x) is always F64 after merge (SUM produces F64 for F64 input,
                 * I64 for integer input; SUM(x²) via ray_mul always produces F64). */
                const double* sq = (const double*)ray_data(sq_col);
                const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = NAN; continue; }
                        double mean = sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (orig_op == OP_VAR_POP)         out[r] = var_pop;
                        else if (orig_op == OP_VAR)         out[r] = n > 1 ? var_pop * n / (n - 1) : NAN;
                        else if (orig_op == OP_STDDEV_POP)  out[r] = sqrt(var_pop);
                        else /* OP_STDDEV */                out[r] = n > 1 ? sqrt(var_pop * n / (n - 1)) : NAN;
                    }
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = NAN; continue; }
                        double mean = (double)sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (orig_op == OP_VAR_POP)         out[r] = var_pop;
                        else if (orig_op == OP_VAR)         out[r] = n > 1 ? var_pop * n / (n - 1) : NAN;
                        else if (orig_op == OP_STDDEV_POP)  out[r] = sqrt(var_pop);
                        else /* OP_STDDEV */                out[r] = n > 1 ? sqrt(var_pop * n / (n - 1)) : NAN;
                    }
                }
                trimmed = ray_table_add_col(trimmed, nm, out_col);
                ray_release(out_col);
            } else {
                ray_t* col = ray_table_get_col_idx(result, c);
                if (col) {
                    ray_retain(col);
                    trimmed = ray_table_add_col(trimmed, nm, col);
                    ray_release(col);
                }
            }
        }
        ray_release(result);
        result = trimmed;
        rncols = ray_table_ncols(result);
    }

    /* Agg column names already fixed by ray_table_set_col_name inside batch loop.
     * Apply final name fixup for the user-facing n_aggs columns (trim decomposed extras). */
    for (uint8_t a = 0; a < n_aggs && (int64_t)(n_keys + a) < rncols; a++)
        ray_table_set_col_name(result, (int64_t)n_keys + a, agg_name_ids[a]);

    if (merge_tbl) ray_release(merge_tbl);
    return result;
}

/* ============================================================================
 * Radix-partitioned hash join
 *
 * Four-phase pipeline:
 *   Phase 1: Partition both sides by radix bits of hash (parallel)
 *   Phase 2: Per-partition build + probe with open-addressing HT (parallel)
 *   Phase 3: Gather output columns from matched pairs (parallel)
 *   Phase 4: Fallback to chained HT for small joins (< RAY_PARALLEL_THRESHOLD)
 * ============================================================================ */

/* Partition entry: row index + cached hash */
typedef struct {
    uint32_t row_idx;
    uint32_t hash;
} join_radix_entry_t;

/* Per-partition descriptor */
typedef struct {
    join_radix_entry_t* entries;     /* partition buffer (from ray_alloc) */
    ray_t*               entries_hdr; /* ray_alloc header for freeing */
    uint32_t            count;       /* number of entries in partition */
} join_radix_part_t;

/* Choose radix bits so each partition's HT working set fits in cache.
 * HT working set per partition ≈ 2x right entries × 8B = 16B per right row. */
static uint8_t radix_join_bits(int64_t right_rows) {
    /* HT working set: 2x capacity × 8B slot = 16B per right row */
    size_t right_bytes = (size_t)right_rows * 16;
    if (right_bytes <= RAY_JOIN_L2_TARGET)
        return RAY_JOIN_MIN_RADIX;

    /* R = ceil(log2(right_bytes / L2_TARGET)) */
    uint8_t r = 0;
    size_t target = RAY_JOIN_L2_TARGET;
    while (target < right_bytes && r < RAY_JOIN_MAX_RADIX) {
        target *= 2;
        r++;
    }
    if (r < RAY_JOIN_MIN_RADIX) r = RAY_JOIN_MIN_RADIX;
    return r;
}

/* Context for parallel hash pre-computation */
typedef struct {
    ray_t**    key_vecs;
    uint8_t   n_keys;
    uint32_t* hashes;    /* output: hash[row] */
} join_radix_hash_ctx_t;

static void join_radix_hash_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    join_radix_hash_ctx_t* c = (join_radix_hash_ctx_t*)raw;
    for (int64_t r = start; r < end; r++)
        c->hashes[r] = (uint32_t)hash_row_keys(c->key_vecs, c->n_keys, r);
}

/* Context for parallel partition histogram + scatter (pre-computed hashes).
 * Uses fixed row assignment: task i processes rows [i*chunk, (i+1)*chunk).
 * This ensures histogram and scatter see the same row ranges per task,
 * enabling non-atomic per-worker scatter offsets. */
typedef struct {
    uint32_t* hashes;
    uint32_t  radix_mask;
    uint8_t   radix_shift;
    uint32_t  n_parts;
    uint32_t  n_workers;
    int64_t   nrows;
    uint32_t* histograms;   /* [n_workers][n_parts] flat array */
} join_radix_hist_ctx_t;

static void join_radix_hist_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_hist_ctx_t* c = (join_radix_hist_ctx_t*)raw;
    /* Fixed row range for this task */
    uint32_t tid = (uint32_t)task_start;
    int64_t chunk = (c->nrows + (int64_t)c->n_workers - 1) / (int64_t)c->n_workers;
    int64_t start = (int64_t)tid * chunk;
    int64_t end = start + chunk;
    if (end > c->nrows) end = c->nrows;
    if (start >= c->nrows) return;

    uint32_t* hist = c->histograms + tid * c->n_parts;
    uint32_t mask = c->radix_mask;
    uint8_t shift = c->radix_shift;

    for (int64_t r = start; r < end; r++) {
        uint32_t part = (c->hashes[r] >> shift) & mask;
        hist[part]++;
    }
}

/* Context for parallel partition scatter with write-combining buffers.
 * Each worker writes to small local buffers (one per partition). When
 * a buffer fills, it flushes to the partition in a burst memcpy.
 * This converts random writes into sequential bursts, dramatically
 * improving cache utilization.
 *
 * Uses fixed per-worker row assignments (dispatch_n with n_workers tasks)
 * to match histogram phase, eliminating atomic operations. */
#define WCB_SIZE 64  /* entries per write-combine buffer */
typedef struct {
    uint32_t*           hashes;
    uint32_t            radix_mask;
    uint8_t             radix_shift;
    uint32_t            n_parts;
    join_radix_part_t*  parts;
    uint32_t*           offsets;     /* [n_workers][n_parts] per-worker write positions */
    int64_t             nrows;
    uint32_t            n_workers;
    _Atomic(uint8_t)    had_error;   /* set by any worker on OOM */
} join_radix_scatter_ctx_t;

static void join_radix_scatter_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_scatter_ctx_t* c = (join_radix_scatter_ctx_t*)raw;
    uint32_t mask = c->radix_mask;
    uint8_t shift = c->radix_shift;
    uint32_t n_parts = c->n_parts;

    /* Fixed row range for this task (matches histogram) */
    uint32_t tid = (uint32_t)task_start;
    int64_t chunk = (c->nrows + (int64_t)c->n_workers - 1) / (int64_t)c->n_workers;
    int64_t ws = (int64_t)tid * chunk;
    int64_t we = ws + chunk;
    if (we > c->nrows) we = c->nrows;
    if (ws >= c->nrows) return;

    uint32_t* off = c->offsets + tid * n_parts;

    /* Write-combining: per-partition local buffers, flushed in bursts */
    uint32_t wcb_cnt_stack[1024];
    uint32_t* wcb_cnt_p = wcb_cnt_stack;
    ray_t* wcb_cnt_hdr = NULL;
    if (n_parts > 1024) {
        wcb_cnt_p = (uint32_t*)scratch_calloc(&wcb_cnt_hdr, (size_t)n_parts * sizeof(uint32_t));
        if (!wcb_cnt_p) {
            atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
            return;
        }
    } else {
        memset(wcb_cnt_stack, 0, (size_t)n_parts * sizeof(uint32_t));
    }

    /* Allocate per-partition local buffers */
    ray_t* local_hdr = NULL;
    join_radix_entry_t* local_buf = (join_radix_entry_t*)scratch_alloc(&local_hdr,
        (size_t)n_parts * WCB_SIZE * sizeof(join_radix_entry_t));
    if (!local_buf) {
        /* Fallback: direct write without buffering */
        for (int64_t r = ws; r < we; r++) {
            uint32_t h = c->hashes[r];
            uint32_t part = (h >> shift) & mask;
            uint32_t pos = off[part]++;
            c->parts[part].entries[pos].row_idx = (uint32_t)r;
            c->parts[part].entries[pos].hash = h;
        }
        if (wcb_cnt_hdr) scratch_free(wcb_cnt_hdr);
        return;
    }

    for (int64_t r = ws; r < we; r++) {
        uint32_t h = c->hashes[r];
        uint32_t part = (h >> shift) & mask;
        uint32_t idx = wcb_cnt_p[part];
        local_buf[part * WCB_SIZE + idx].row_idx = (uint32_t)r;
        local_buf[part * WCB_SIZE + idx].hash = h;
        idx++;
        if (idx == WCB_SIZE) {
            /* Flush buffer to partition */
            memcpy(&c->parts[part].entries[off[part]],
                   &local_buf[part * WCB_SIZE],
                   WCB_SIZE * sizeof(join_radix_entry_t));
            off[part] += WCB_SIZE;
            idx = 0;
        }
        wcb_cnt_p[part] = idx;
    }

    /* Flush remaining entries */
    for (uint32_t p = 0; p < n_parts; p++) {
        uint32_t cnt = wcb_cnt_p[p];
        if (cnt > 0) {
            memcpy(&c->parts[p].entries[off[p]],
                   &local_buf[p * WCB_SIZE],
                   (size_t)cnt * sizeof(join_radix_entry_t));
            off[p] += cnt;
        }
    }

    scratch_free(local_hdr);
    if (wcb_cnt_hdr) scratch_free(wcb_cnt_hdr);
}

/* Partition one side of the join. Returns array of join_radix_part_t[n_parts].
 * Caller must free each partition's entries_hdr and the parts array itself. */
static join_radix_part_t* join_radix_partition(ray_pool_t* pool, int64_t nrows,
                                      uint8_t radix_bits,
                                      uint32_t* hashes,
                                      ray_t** parts_hdr_out) {
    uint32_t n_parts = (uint32_t)1 << radix_bits;
    uint32_t mask = n_parts - 1;
    /* Use upper bits of hash for radix (lower bits used inside partition HT) */
    uint8_t shift = 32 - radix_bits;

    /* Allocate partition descriptor array */
    ray_t* parts_hdr;
    join_radix_part_t* parts = (join_radix_part_t*)scratch_calloc(&parts_hdr,
                            (size_t)n_parts * sizeof(join_radix_part_t));
    if (!parts) { *parts_hdr_out = NULL; return NULL; }
    *parts_hdr_out = parts_hdr;

    /* Step 1: Histogram — count rows per partition per worker.
     * n_workers must match dispatch: 1 when running serially so that the
     * single hist/scatter call covers all rows (chunk = nrows / 1). */
    uint32_t n_workers = (pool && nrows > RAY_PARALLEL_THRESHOLD) ? pool->n_workers + 1 : 1;
    ray_t* hist_hdr;
    uint32_t* histograms = (uint32_t*)scratch_calloc(&hist_hdr,
                             (size_t)n_workers * n_parts * sizeof(uint32_t));
    if (!histograms) { scratch_free(parts_hdr); *parts_hdr_out = NULL; return NULL; }

    join_radix_hist_ctx_t hctx = {
        .hashes = hashes,
        .radix_mask = mask, .radix_shift = shift,
        .n_parts = n_parts, .n_workers = n_workers,
        .nrows = nrows,
        .histograms = histograms,
    };
    if (pool && nrows > RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch_n(pool, join_radix_hist_fn, &hctx, n_workers);
    else
        join_radix_hist_fn(&hctx, 0, 0, 1);

    /* Compute partition sizes (sum across workers) */
    for (uint32_t p = 0; p < n_parts; p++) {
        uint32_t total = 0;
        for (uint32_t w = 0; w < n_workers; w++)
            total += histograms[w * n_parts + p];
        parts[p].count = total;
    }

    /* Allocate partition buffers */
    bool oom = false;
    for (uint32_t p = 0; p < n_parts; p++) {
        if (parts[p].count == 0) continue;
        parts[p].entries = (join_radix_entry_t*)scratch_alloc(&parts[p].entries_hdr,
                             (size_t)parts[p].count * sizeof(join_radix_entry_t));
        if (!parts[p].entries) {
            ray_heap_gc();
            ray_heap_release_pages();
            parts[p].entries = (join_radix_entry_t*)scratch_alloc(&parts[p].entries_hdr,
                                 (size_t)parts[p].count * sizeof(join_radix_entry_t));
            if (!parts[p].entries) { oom = true; break; }
        }
    }
    if (oom) {
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].entries_hdr) scratch_free(parts[p].entries_hdr);
        scratch_free(hist_hdr);
        scratch_free(parts_hdr);
        *parts_hdr_out = NULL;
        return NULL;
    }

    /* Step 2: Compute per-worker write offsets (prefix sum of histograms).
     * For each partition p, worker w's write offset =
     *   sum(histograms[0..w-1][p]) = global prefix for workers before w. */
    ray_t* off_hdr;
    uint32_t* offsets = (uint32_t*)scratch_alloc(&off_hdr,
                            (size_t)n_workers * n_parts * sizeof(uint32_t));
    if (!offsets) {
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].entries_hdr) scratch_free(parts[p].entries_hdr);
        scratch_free(hist_hdr);
        scratch_free(parts_hdr);
        *parts_hdr_out = NULL;
        return NULL;
    }
    for (uint32_t p = 0; p < n_parts; p++) {
        uint32_t running = 0;
        for (uint32_t w = 0; w < n_workers; w++) {
            offsets[w * n_parts + p] = running;
            running += histograms[w * n_parts + p];
        }
    }

    /* Step 3: Scatter rows into partition buffers (fixed row assignment, no atomics) */
    join_radix_scatter_ctx_t sctx = {
        .hashes = hashes,
        .radix_mask = mask, .radix_shift = shift,
        .n_parts = n_parts, .parts = parts,
        .offsets = offsets,
        .nrows = nrows, .n_workers = n_workers,
        .had_error = 0,
    };
    if (pool && nrows > RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch_n(pool, join_radix_scatter_fn, &sctx, n_workers);
    else
        join_radix_scatter_fn(&sctx, 0, 0, 1);

    scratch_free(off_hdr);
    scratch_free(hist_hdr);

    if (atomic_load_explicit(&sctx.had_error, memory_order_relaxed)) {
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].entries_hdr) scratch_free(parts[p].entries_hdr);
        scratch_free(parts_hdr);
        *parts_hdr_out = NULL;
        return NULL;
    }

    return parts;
}

/* ============================================================================
 * Join execution (parallel hash join)
 *
 * Three-phase pipeline:
 *   Phase 1 (sequential): Build chained hash table on right side
 *   Phase 2 (parallel):   Two-pass probe — count matches, prefix-sum, fill
 *   Phase 3 (parallel):   Column gather — assemble result columns
 * ============================================================================ */

/* Key equality helper — shared by count + fill phases */
static inline bool join_keys_eq(ray_t* const* l_vecs, ray_t* const* r_vecs, uint8_t n_keys,
                                 int64_t l, int64_t r) {
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* lc = l_vecs[k];
        ray_t* rc = r_vecs[k];
        if (!lc || !rc) return false;
        if (lc->type == RAY_F64) {
            if (((double*)ray_data(lc))[l] != ((double*)ray_data(rc))[r]) return false;
        } else {
            if (read_col_i64(ray_data(lc), l, lc->type, lc->attrs) !=
                read_col_i64(ray_data(rc), r, rc->type, rc->attrs)) return false;
        }
    }
    return true;
}

/* ── Per-partition open-addressing build + probe ─────────────────────── */

#define RADIX_HT_EMPTY UINT32_MAX

/* Per-partition single-pass build+probe context.
 * Each partition writes to its own local output buffer, then results
 * are consolidated into contiguous arrays afterward. */
typedef struct {
    join_radix_part_t*  l_parts;
    join_radix_part_t*  r_parts;
    ray_t**         l_key_vecs;
    ray_t**         r_key_vecs;
    uint8_t        n_keys;
    uint8_t        join_type;
    /* Per-partition output: pp_l[p], pp_r[p] are local buffers */
    int32_t**      pp_l;         /* per-partition left indices (int32_t) */
    int32_t**      pp_r;         /* per-partition right indices (int32_t) */
    ray_t**         pp_l_hdr;     /* allocation headers for freeing */
    ray_t**         pp_r_hdr;
    int64_t*       part_counts;  /* actual output count per partition */
    uint32_t*      pp_cap;       /* capacity per partition */
    _Atomic(uint8_t)* matched_right;
    _Atomic(uint8_t)  had_error;  /* set by any partition on OOM */
} join_radix_bp_ctx_t;

/* Grow per-partition output buffers (matched pair arrays).
 * Returns true on success, false on OOM (sets had_error). */
static inline bool bp_grow_bufs(join_radix_bp_ctx_t* c, uint32_t p,
                                 int32_t** pl, int32_t** pr,
                                 uint32_t* cap, uint32_t cnt) {
    if (cnt < *cap) return true;
    if (*cap > UINT32_MAX / 2) {
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        return false;
    }
    uint32_t new_cap = *cap * 2;
    ray_t* nl_hdr; ray_t* nr_hdr;
    int32_t* nl = (int32_t*)scratch_alloc(&nl_hdr, (size_t)new_cap * sizeof(int32_t));
    int32_t* nr = (int32_t*)scratch_alloc(&nr_hdr, (size_t)new_cap * sizeof(int32_t));
    if (!nl || !nr) {
        if (nl_hdr) scratch_free(nl_hdr);
        if (nr_hdr) scratch_free(nr_hdr);
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        return false;
    }
    memcpy(nl, *pl, (size_t)cnt * sizeof(int32_t));
    memcpy(nr, *pr, (size_t)cnt * sizeof(int32_t));
    scratch_free(c->pp_l_hdr[p]); scratch_free(c->pp_r_hdr[p]);
    *pl = nl; *pr = nr;
    c->pp_l_hdr[p] = nl_hdr; c->pp_r_hdr[p] = nr_hdr;
    *cap = new_cap;
    return true;
}

static void join_radix_build_probe_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_bp_ctx_t* c = (join_radix_bp_ctx_t*)raw;
    uint32_t p = (uint32_t)task_start;

    join_radix_part_t* rp = &c->r_parts[p];
    join_radix_part_t* lp = &c->l_parts[p];

    if (rp->count == 0) {
        /* No right rows — emit unmatched left rows for LEFT/FULL */
        if (c->join_type >= 1 && lp->count > 0) {
            uint32_t cap = lp->count;
            int32_t* pl = (int32_t*)scratch_alloc(&c->pp_l_hdr[p], (size_t)cap * sizeof(int32_t));
            int32_t* pr = (int32_t*)scratch_alloc(&c->pp_r_hdr[p], (size_t)cap * sizeof(int32_t));
            if (pl && pr) {
                for (uint32_t i = 0; i < lp->count; i++) {
                    pl[i] = (int32_t)lp->entries[i].row_idx;
                    pr[i] = -1;
                }
                c->pp_l[p] = pl; c->pp_r[p] = pr;
                c->part_counts[p] = lp->count;
                c->pp_cap[p] = cap;
            } else {
                if (c->pp_l_hdr[p]) scratch_free(c->pp_l_hdr[p]);
                if (c->pp_r_hdr[p]) scratch_free(c->pp_r_hdr[p]);
                c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
                atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
            }
        }
        return;
    }

    /* Allocate per-partition output buffer.
     * Capacity = max(left, right) handles 1:1 and 1:N joins.
     * For N:M (overflow), we grow by re-allocating. */
    uint32_t init_cap = lp->count > rp->count ? lp->count : rp->count;
    if (init_cap < 64) init_cap = 64;
    int32_t* pl = (int32_t*)scratch_alloc(&c->pp_l_hdr[p], (size_t)init_cap * sizeof(int32_t));
    int32_t* pr = (int32_t*)scratch_alloc(&c->pp_r_hdr[p], (size_t)init_cap * sizeof(int32_t));
    if (!pl || !pr) {
        if (c->pp_l_hdr[p]) scratch_free(c->pp_l_hdr[p]);
        if (c->pp_r_hdr[p]) scratch_free(c->pp_r_hdr[p]);
        c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
        c->part_counts[p] = 0;
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        return;
    }
    uint32_t cap = init_cap;
    uint32_t cnt = 0;

    /* Build open-addressing HT for right partition */
    uint32_t ht_cap = 256;
    uint64_t ht_target = (uint64_t)rp->count * 2;
    while ((uint64_t)ht_cap < ht_target && ht_cap <= (UINT32_MAX >> 1)) ht_cap *= 2;
    if ((uint64_t)ht_cap < ht_target) {
        /* Partition too large for open-addressing HT — signal error */
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        c->part_counts[p] = 0;
        scratch_free(c->pp_l_hdr[p]); scratch_free(c->pp_r_hdr[p]);
        c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
        return;
    }
    uint32_t ht_mask = ht_cap - 1;

    ray_t* ht_hdr;
    uint32_t* ht = (uint32_t*)scratch_calloc(&ht_hdr, (size_t)ht_cap * 2 * sizeof(uint32_t));
    if (!ht) {
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        scratch_free(c->pp_l_hdr[p]); scratch_free(c->pp_r_hdr[p]);
        c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
        c->part_counts[p] = 0;
        return;
    }
    for (uint32_t s = 0; s < ht_cap; s++)
        ht[s * 2 + 1] = RADIX_HT_EMPTY;

    for (uint32_t i = 0; i < rp->count; i++) {
        uint32_t h = rp->entries[i].hash;
        uint32_t slot = h & ht_mask;
        if (i + 4 < rp->count)
            __builtin_prefetch(&ht[(rp->entries[i + 4].hash & ht_mask) * 2], 1, 1);
        while (ht[slot * 2 + 1] != RADIX_HT_EMPTY)
            slot = (slot + 1) & ht_mask;
        ht[slot * 2] = h;
        ht[slot * 2 + 1] = rp->entries[i].row_idx;
    }

    /* Single-pass probe + fill */
    for (uint32_t i = 0; i < lp->count; i++) {
        uint32_t h = lp->entries[i].hash;
        uint32_t lr = lp->entries[i].row_idx;
        uint32_t slot = h & ht_mask;
        if (i + 4 < lp->count)
            __builtin_prefetch(&ht[(lp->entries[i + 4].hash & ht_mask) * 2], 0, 1);
        bool matched = false;
        while (ht[slot * 2 + 1] != RADIX_HT_EMPTY) {
            if (ht[slot * 2] == h) {
                uint32_t rr = ht[slot * 2 + 1];
                if (join_keys_eq(c->l_key_vecs, c->r_key_vecs, c->n_keys,
                                 (int64_t)lr, (int64_t)rr)) {
                    if (!bp_grow_bufs(c, p, &pl, &pr, &cap, cnt))
                        goto done;
                    pl[cnt] = (int32_t)lr;
                    pr[cnt] = (int32_t)rr;
                    cnt++;
                    matched = true;
                    if (c->matched_right)
                        atomic_store_explicit(&c->matched_right[rr], 1, memory_order_relaxed);
                }
            }
            slot = (slot + 1) & ht_mask;
        }
        if (!matched && c->join_type >= 1) {
            if (!bp_grow_bufs(c, p, &pl, &pr, &cap, cnt))
                goto done;
            pl[cnt] = (int32_t)lr;
            pr[cnt] = -1;
            cnt++;
        }
    }

done:
    scratch_free(ht_hdr);
    c->pp_l[p] = pl; c->pp_r[p] = pr;
    c->part_counts[p] = cnt;
    c->pp_cap[p] = cap;
}

/* ── Parallel join HT build ─────────────────────────────────────────────
 * Workers hash right-side rows in parallel and insert into the shared
 * chain-linked hash table using atomic CAS on ht_heads[slot].
 * ht_next[r] is per-row (no contention). Load factor ~0.3 → negligible
 * CAS contention.
 * ──────────────────────────────────────────────────────────────────── */

/* ht_heads is accessed atomically from multiple workers during join build.
 * Using _Atomic(uint32_t)* for C11-compliant atomic access. */
#define JHT_EMPTY UINT32_MAX  /* sentinel for empty HT slot/chain end */

typedef struct {
    _Atomic(uint32_t)* ht_heads;  /* shared, protected by atomic CAS */
    uint32_t* ht_next;            /* per-row, no contention */
    uint32_t ht_mask;       /* ht_cap - 1 */
    ray_t**   r_key_vecs;
    uint8_t  n_keys;
    /* ASP-Join: semijoin filter from factorized left side (NULL if N/A) */
    uint64_t* asp_bits;
    int64_t   asp_key_max;
} join_build_ctx_t;

static void join_build_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    join_build_ctx_t* c = (join_build_ctx_t*)raw;
    _Atomic(uint32_t)* heads = c->ht_heads;
    uint32_t* restrict next  = c->ht_next;
    uint32_t mask  = c->ht_mask;

    /* ASP-Join: precompute pointer for right-side build filtering */
    uint64_t* asp_bits = c->asp_bits;
    int64_t asp_max = c->asp_key_max;
    int64_t* rk0 = (asp_bits && c->n_keys == 1) ? (int64_t*)ray_data(c->r_key_vecs[0]) : NULL;

    for (int64_t r = start; r < end; r++) {
        /* ASP-Join skip: if right key not in left-side bitmap, skip insert */
        if (rk0 && rk0[r] >= 0 && rk0[r] <= asp_max &&
            !RAY_SEL_BIT_TEST(asp_bits, rk0[r])) {
            next[(uint32_t)r] = JHT_EMPTY;  /* mark as unused */
            continue;
        }
        if (r + 8 < end) {
            uint64_t pf_h = hash_row_keys(c->r_key_vecs, c->n_keys, r + 8);
            __builtin_prefetch(&heads[(uint32_t)(pf_h & mask)], 1, 1);
        }
        uint64_t h = hash_row_keys(c->r_key_vecs, c->n_keys, r);
        uint32_t slot = (uint32_t)(h & mask);
        uint32_t row32 = (uint32_t)r;
        uint32_t old = atomic_load_explicit(&heads[slot], memory_order_relaxed);
        do {
            next[row32] = old;
        } while (!atomic_compare_exchange_weak_explicit(&heads[slot], &old, row32,
                    memory_order_release, memory_order_relaxed));
    }
}

#define JOIN_MORSEL 8192

typedef struct {
    _Atomic(uint32_t)* ht_heads;
    uint32_t*    ht_next;
    uint32_t     ht_cap;
    ray_t**       l_key_vecs;
    ray_t**       r_key_vecs;
    uint8_t      n_keys;
    uint8_t      join_type;
    int64_t      left_rows;
    /* Per-morsel counts/offsets (allocated by main thread) */
    int64_t*     morsel_counts;
    int64_t*     morsel_offsets;
    /* Shared output arrays (phase 2 fill) */
    int64_t*     l_idx;
    int64_t*     r_idx;
    /* FULL OUTER: track which right rows were matched (NULL if not full) */
    _Atomic(uint8_t)* matched_right;
    /* S-Join: semijoin filter bitmap (NULL if not applicable) */
    uint64_t*    sjoin_bits;
    int64_t      sjoin_key_max;
} join_probe_ctx_t;

/* Phase 2a: count matches per morsel */
static void join_count_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_probe_ctx_t* c = (join_probe_ctx_t*)raw;
    uint32_t tid = (uint32_t)task_start;
    int64_t row_start = (int64_t)tid * JOIN_MORSEL;
    int64_t row_end = row_start + JOIN_MORSEL;
    if (row_end > c->left_rows) row_end = c->left_rows;

    /* S-Join: precompute pointer for fast semijoin check */
    uint64_t* sjbits = c->sjoin_bits;
    int64_t sjmax = c->sjoin_key_max;
    int64_t* lk0 = (sjbits && c->n_keys == 1) ? (int64_t*)ray_data(c->l_key_vecs[0]) : NULL;

    int64_t count = 0;
    uint32_t ht_mask = c->ht_cap - 1;
    for (int64_t l = row_start; l < row_end; l++) {
        /* S-Join skip: if left key not in right-side bitmap, skip probe */
        if (lk0 && lk0[l] >= 0 && lk0[l] <= sjmax &&
            !RAY_SEL_BIT_TEST(sjbits, lk0[l])) {
            if (c->join_type >= 1) count++;  /* LEFT/FULL: emit unmatched */
            continue;
        }

        if (l + 8 < row_end) {
            uint64_t pf_h = hash_row_keys(c->l_key_vecs, c->n_keys, l + 8);
            __builtin_prefetch(&c->ht_heads[(uint32_t)(pf_h & ht_mask)], 0, 1);
        }
        uint64_t h = hash_row_keys(c->l_key_vecs, c->n_keys, l);
        uint32_t slot = (uint32_t)(h & ht_mask);
        bool matched = false;
        for (uint32_t r = c->ht_heads[slot]; r != JHT_EMPTY; r = c->ht_next[r]) {
            if (join_keys_eq(c->l_key_vecs, c->r_key_vecs, c->n_keys, l, (int64_t)r)) {
                count++;
                matched = true;
            }
        }
        if (!matched && c->join_type >= 1) count++;
    }
    c->morsel_counts[tid] = count;
}

/* Phase 2b: fill match pairs using pre-computed offsets */
static void join_fill_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_probe_ctx_t* c = (join_probe_ctx_t*)raw;
    uint32_t tid = (uint32_t)task_start;
    int64_t row_start = (int64_t)tid * JOIN_MORSEL;
    int64_t row_end = row_start + JOIN_MORSEL;
    if (row_end > c->left_rows) row_end = c->left_rows;

    int64_t off = c->morsel_offsets[tid];
    int64_t* restrict li = c->l_idx;
    int64_t* restrict ri = c->r_idx;

    /* S-Join: precompute pointer for fast semijoin check */
    uint64_t* sjbits = c->sjoin_bits;
    int64_t sjmax = c->sjoin_key_max;
    int64_t* lk0 = (sjbits && c->n_keys == 1) ? (int64_t*)ray_data(c->l_key_vecs[0]) : NULL;

    uint32_t ht_mask = c->ht_cap - 1;
    for (int64_t l = row_start; l < row_end; l++) {
        /* S-Join skip: if left key not in right-side bitmap, skip probe */
        if (lk0 && lk0[l] >= 0 && lk0[l] <= sjmax &&
            !RAY_SEL_BIT_TEST(sjbits, lk0[l])) {
            if (c->join_type >= 1) {
                li[off] = l;
                ri[off] = -1;
                off++;
            }
            continue;
        }

        if (l + 8 < row_end) {
            uint64_t pf_h = hash_row_keys(c->l_key_vecs, c->n_keys, l + 8);
            __builtin_prefetch(&c->ht_heads[(uint32_t)(pf_h & ht_mask)], 0, 1);
        }
        uint64_t h = hash_row_keys(c->l_key_vecs, c->n_keys, l);
        uint32_t slot = (uint32_t)(h & ht_mask);
        bool matched = false;
        for (uint32_t r = c->ht_heads[slot]; r != JHT_EMPTY; r = c->ht_next[r]) {
            if (join_keys_eq(c->l_key_vecs, c->r_key_vecs, c->n_keys, l, (int64_t)r)) {
                li[off] = l;
                ri[off] = (int64_t)r;
                off++;
                matched = true;
                /* Monotonic 0→1 store from multiple workers. */
                if (c->matched_right) atomic_store_explicit(&c->matched_right[r], 1, memory_order_relaxed);
            }
        }
        if (!matched && c->join_type >= 1) {
            li[off] = l;
            ri[off] = -1;
            off++;
        }
    }
}

static ray_t* exec_join(ray_graph_t* g, ray_op_t* op, ray_t* left_table, ray_t* right_table) {
    if (!left_table || RAY_IS_ERR(left_table)) return left_table;
    if (!right_table || RAY_IS_ERR(right_table)) return right_table;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t left_rows = ray_table_nrows(left_table);
    int64_t right_rows = ray_table_nrows(right_table);
    /* Guard: radix path stores row indices as int32_t (widened to int64_t on gather).
     * Chained HT path uses uint32_t.  Cap at INT32_MAX for correctness. */
    if (right_rows > (int64_t)INT32_MAX || left_rows > (int64_t)INT32_MAX)
        return ray_error("nyi", NULL);
    uint8_t n_keys = ext->join.n_join_keys;
    uint8_t join_type = ext->join.join_type;

    ray_t* l_key_vecs[n_keys];
    ray_t* r_key_vecs[n_keys];
    memset(l_key_vecs, 0, n_keys * sizeof(ray_t*));
    memset(r_key_vecs, 0, n_keys * sizeof(ray_t*));

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_ext_t* lk = find_ext(g, ext->join.left_keys[k]->id);
        ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]->id);
        if (lk && lk->base.opcode == OP_SCAN)
            l_key_vecs[k] = ray_table_get_col(left_table, lk->sym);
        if (rk && rk->base.opcode == OP_SCAN)
            r_key_vecs[k] = ray_table_get_col(right_table, rk->sym);
        if (rk && rk->base.opcode == OP_CONST && rk->literal)
            r_key_vecs[k] = rk->literal;
    }

    /* RAY_STR keys not yet supported (16-byte elements vs 8-byte hash/eq slots) */
    for (uint8_t k = 0; k < n_keys; k++) {
        if ((l_key_vecs[k] && l_key_vecs[k]->type == RAY_STR) ||
            (r_key_vecs[k] && r_key_vecs[k]->type == RAY_STR))
            return ray_error("nyi", NULL);
    }

    ray_pool_t* pool = ray_pool_get();

    /* Shared output state — used by both radix and chained HT paths */
    ray_t* result = NULL;
    ray_t* counts_hdr = NULL;
    ray_t* l_idx_hdr = NULL;
    ray_t* r_idx_hdr = NULL;
    ray_t* matched_right_hdr = NULL;
    ray_t* sjoin_sel = NULL;
    ray_t* asp_sel = NULL;
    ray_t* ht_next_hdr = NULL;
    ray_t* ht_heads_hdr = NULL;
    int64_t* l_idx = NULL;
    int64_t* r_idx = NULL;
    int64_t pair_count = 0;
    _Atomic(uint8_t)* matched_right = NULL;

    /* ── Radix-partitioned path (large joins) ──────────────────────── */
    if (right_rows > RAY_PARALLEL_THRESHOLD) {
        uint8_t radix_bits = radix_join_bits(right_rows);
        uint32_t n_rparts = (uint32_t)1 << radix_bits;

        /* Pre-compute hashes for both sides (once, reused by histogram+scatter) */
        ray_t* r_hash_hdr = NULL;
        uint32_t* r_hashes = (uint32_t*)scratch_alloc(&r_hash_hdr,
                                (size_t)right_rows * sizeof(uint32_t));
        ray_t* l_hash_hdr = NULL;
        uint32_t* l_hashes = (uint32_t*)scratch_alloc(&l_hash_hdr,
                                (size_t)left_rows * sizeof(uint32_t));
        if (!r_hashes || !l_hashes) {
            if (r_hash_hdr) scratch_free(r_hash_hdr);
            if (l_hash_hdr) scratch_free(l_hash_hdr);
            goto chained_ht_fallback;
        }
        join_radix_hash_ctx_t rhctx = { .key_vecs = r_key_vecs, .n_keys = n_keys, .hashes = r_hashes };
        join_radix_hash_ctx_t lhctx = { .key_vecs = l_key_vecs, .n_keys = n_keys, .hashes = l_hashes };
        if (pool) {
            ray_pool_dispatch(pool, join_radix_hash_fn, &rhctx, right_rows);
            ray_pool_dispatch(pool, join_radix_hash_fn, &lhctx, left_rows);
        } else {
            join_radix_hash_fn(&rhctx, 0, 0, right_rows);
            join_radix_hash_fn(&lhctx, 0, 0, left_rows);
        }

        if (pool_cancelled(pool)) {
            scratch_free(r_hash_hdr); scratch_free(l_hash_hdr);
            return ray_error("cancel", NULL);
        }

        /* Partition both sides using cached hashes */
        ray_t* r_parts_hdr = NULL;
        join_radix_part_t* r_parts = join_radix_partition(pool, right_rows,
                                                          radix_bits, r_hashes, &r_parts_hdr);
        ray_t* l_parts_hdr = NULL;
        join_radix_part_t* l_parts = join_radix_partition(pool, left_rows,
                                                          radix_bits, l_hashes, &l_parts_hdr);
        scratch_free(r_hash_hdr);
        scratch_free(l_hash_hdr);
        if (!r_parts || !l_parts) {
            /* OOM during partitioning — fall through to chained HT path */
            if (r_parts) {
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                    if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                scratch_free(r_parts_hdr);
            }
            if (l_parts) {
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                    if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
                scratch_free(l_parts_hdr);
            }
            goto chained_ht_fallback;
        }

        if (pool_cancelled(pool)) {
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
            }
            scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
            return ray_error("cancel", NULL);
        }

        /* FULL OUTER: allocate matched_right tracker */
        if (join_type == 2 && right_rows > 0) {
            matched_right = (_Atomic(uint8_t)*)scratch_calloc(&matched_right_hdr,
                                                               (size_t)right_rows);
            if (!matched_right) {
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                    if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                    if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
                }
                scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
                matched_right_hdr = NULL;
                goto chained_ht_fallback;
            }
        }

        /* Single-pass per-partition build+probe with local output buffers */
        ray_t* pcounts_hdr = NULL;
        int64_t* part_counts = (int64_t*)scratch_calloc(&pcounts_hdr,
                                  (size_t)n_rparts * sizeof(int64_t));
        ray_t* pp_meta_hdr = NULL;
        /* Allocate per-partition pointer arrays */
        size_t pp_alloc_sz = (size_t)n_rparts * (2 * sizeof(int32_t*) + 2 * sizeof(ray_t*) + sizeof(uint32_t));
        char* pp_mem = (char*)scratch_calloc(&pp_meta_hdr, pp_alloc_sz);
        if (!part_counts || !pp_mem) {
            if (pcounts_hdr) scratch_free(pcounts_hdr);
            if (pp_meta_hdr) scratch_free(pp_meta_hdr);
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
            }
            scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
            if (matched_right_hdr) { scratch_free(matched_right_hdr); matched_right_hdr = NULL; }
            matched_right = NULL;
            goto chained_ht_fallback;
        }
        int32_t** pp_l = (int32_t**)pp_mem;
        int32_t** pp_r = (int32_t**)(pp_mem + (size_t)n_rparts * sizeof(int32_t*));
        ray_t** pp_l_hdr = (ray_t**)(pp_mem + (size_t)n_rparts * 2 * sizeof(int32_t*));
        ray_t** pp_r_hdr = (ray_t**)(pp_mem + (size_t)n_rparts * (2 * sizeof(int32_t*) + sizeof(ray_t*)));
        uint32_t* pp_cap = (uint32_t*)(pp_mem + (size_t)n_rparts * (2 * sizeof(int32_t*) + 2 * sizeof(ray_t*)));

        join_radix_bp_ctx_t bp_ctx = {
            .l_parts = l_parts, .r_parts = r_parts,
            .l_key_vecs = l_key_vecs, .r_key_vecs = r_key_vecs,
            .n_keys = n_keys, .join_type = join_type,
            .pp_l = pp_l, .pp_r = pp_r,
            .pp_l_hdr = pp_l_hdr, .pp_r_hdr = pp_r_hdr,
            .part_counts = part_counts, .pp_cap = pp_cap,
            .matched_right = matched_right,
            .had_error = 0,
        };
        if (pool && n_rparts > 1)
            ray_pool_dispatch_n(pool, join_radix_build_probe_fn, &bp_ctx, n_rparts);
        else
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                join_radix_build_probe_fn(&bp_ctx, 0, rp2, rp2 + 1);

        /* Check cancellation and errors during build+probe */
        bool bp_cancelled = pool_cancelled(pool);
        bool bp_error = atomic_load_explicit(&bp_ctx.had_error, memory_order_relaxed);
        if (bp_cancelled || bp_error) {
            /* Free all per-partition buffers */
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
                if (pp_l_hdr[rp2]) scratch_free(pp_l_hdr[rp2]);
                if (pp_r_hdr[rp2]) scratch_free(pp_r_hdr[rp2]);
            }
            scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
            scratch_free(pp_meta_hdr); scratch_free(pcounts_hdr);
            if (matched_right_hdr) { scratch_free(matched_right_hdr); matched_right_hdr = NULL; }
            matched_right = NULL;
            if (bp_cancelled) return ray_error("cancel", NULL);
            goto chained_ht_fallback;
        }

        /* Free partition buffers — no longer needed */
        for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
            if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
            if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
        }
        scratch_free(r_parts_hdr);
        scratch_free(l_parts_hdr);

        /* Compute total output size and consolidate per-partition buffers */
        for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
            pair_count += part_counts[rp2];

        /* FULL OUTER: count unmatched right rows */
        int64_t unmatched_right = 0;
        if (join_type == 2 && matched_right) {
            for (int64_t r = 0; r < right_rows; r++)
                if (!matched_right[r]) unmatched_right++;
        }
        int64_t total_out = pair_count + unmatched_right;

        if (total_out > 0) {
            l_idx = (int64_t*)scratch_alloc(&l_idx_hdr, (size_t)total_out * sizeof(int64_t));
            r_idx = (int64_t*)scratch_alloc(&r_idx_hdr, (size_t)total_out * sizeof(int64_t));
            if (!l_idx || !r_idx) {
                scratch_free(l_idx_hdr); scratch_free(r_idx_hdr);
                l_idx_hdr = NULL; r_idx_hdr = NULL;
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                    if (pp_l_hdr[rp2]) scratch_free(pp_l_hdr[rp2]);
                    if (pp_r_hdr[rp2]) scratch_free(pp_r_hdr[rp2]);
                }
                scratch_free(pp_meta_hdr);
                scratch_free(pcounts_hdr);
                if (matched_right_hdr) scratch_free(matched_right_hdr);
                matched_right_hdr = NULL;
                return ray_error("oom", NULL);
            }

            /* Copy per-partition results into contiguous arrays (int32→int64 widen) */
            int64_t off = 0;
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                int64_t cnt = part_counts[rp2];
                if (cnt > 0 && pp_l[rp2] && pp_r[rp2]) {
                    for (int64_t j = 0; j < cnt; j++) {
                        l_idx[off + j] = (int64_t)pp_l[rp2][j];
                        r_idx[off + j] = (int64_t)pp_r[rp2][j];
                    }
                    off += cnt;
                }
            }

            /* FULL OUTER: append unmatched right rows */
            if (unmatched_right > 0) {
                for (int64_t r = 0; r < right_rows; r++) {
                    if (!matched_right[r]) {
                        l_idx[off] = -1;
                        r_idx[off] = r;
                        off++;
                    }
                }
            }
            pair_count = total_out;
        }

        /* Free per-partition buffers allocated by worker threads.
         * Safe: ray_pool_dispatch_n has completed (workers are back on semaphore),
         * ray_parallel_flag is 0, and ray_free handles cross-heap deallocation
         * via the foreign-block list flushed by ray_heap_gc at ray_parallel_end. */
        for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
            if (pp_l_hdr[rp2]) scratch_free(pp_l_hdr[rp2]);
            if (pp_r_hdr[rp2]) scratch_free(pp_r_hdr[rp2]);
        }
        scratch_free(pp_meta_hdr);
        scratch_free(pcounts_hdr);
        goto join_gather;
    }

chained_ht_fallback:;
    /* ── Chained HT path (small joins / radix OOM fallback) ────────── */
    uint64_t ht_cap64 = 256;
    uint64_t target = (uint64_t)right_rows * 2;
    while (ht_cap64 < target) ht_cap64 *= 2;
    if (ht_cap64 > UINT32_MAX) ht_cap64 = (uint64_t)1 << 31;
    uint32_t ht_cap = (uint32_t)ht_cap64;

    uint32_t* ht_next = (uint32_t*)scratch_alloc(&ht_next_hdr, (size_t)right_rows * sizeof(uint32_t));
    // cppcheck-suppress internalAstError
    // Valid C11/C17 _Atomic(T)* declaration; cppcheck parser may mis-handle this syntax.
    _Atomic(uint32_t)* ht_heads = (_Atomic(uint32_t)*)scratch_alloc(&ht_heads_hdr, ht_cap * sizeof(uint32_t));
    if (!ht_next || !ht_heads) {
        scratch_free(ht_next_hdr); scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }
    memset(ht_heads, 0xFF, ht_cap * sizeof(uint32_t));  /* JHT_EMPTY = 0xFFFFFFFF */

    /* Phase 0.5: ASP-Join — extract semijoin filter from factorized left side.
     * When the left input comes from a factorized expand (_count column present),
     * build a RAY_SEL bitmap of left-side key values to skip right-side rows
     * during hash-build whose keys can't match any left-side row. */
    uint64_t* asp_bits = NULL;
    int64_t asp_key_max = 0;
    if (n_keys == 1 && join_type == 0 && l_key_vecs[0] &&
        l_key_vecs[0]->type == RAY_I64 && right_rows > left_rows * 2) {
        int64_t cnt_sym = ray_sym_intern("_count", 6);
        ray_t* cnt_col = ray_table_get_col(left_table, cnt_sym);
        if (cnt_col) {  /* left is factorized */
            int64_t* lk = (int64_t*)ray_data(l_key_vecs[0]);
            int64_t lk_max = 0;
            for (int64_t i = 0; i < left_rows; i++)
                if (lk[i] > lk_max) lk_max = lk[i];

            if (lk_max < (int64_t)1 << 24) {
                asp_sel = ray_sel_new(lk_max + 1);
                if (asp_sel && !RAY_IS_ERR(asp_sel)) {
                    asp_bits = ray_sel_bits(asp_sel);
                    asp_key_max = lk_max;
                    for (int64_t i = 0; i < left_rows; i++) {
                        int64_t k = lk[i];
                        if (k >= 0 && k <= lk_max)
                            RAY_SEL_BIT_SET(asp_bits, k);
                    }
                }
            }
        }
    }

    {
        join_build_ctx_t bctx = {
            .ht_heads   = ht_heads,
            .ht_next    = ht_next,
            .ht_mask    = ht_cap - 1,
            .r_key_vecs = r_key_vecs,
            .n_keys     = n_keys,
            .asp_bits   = asp_bits,
            .asp_key_max = asp_key_max,
        };
        if (pool && right_rows > RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, join_build_fn, &bctx, right_rows);
        else
            join_build_fn(&bctx, 0, 0, right_rows);
    }
    CHECK_CANCEL_GOTO(pool, join_cleanup);

    /* Phase 1.5: S-Join semijoin filter extraction.
     * Build a RAY_SEL bitmap of all distinct right-side key values that
     * appear in the hash table. This can be used to skip left-side rows
     * whose key cannot match any right-side row.
     *
     * Applied when: single I64 key, inner join, left side is large enough
     * to benefit from filtering (> 2x right side). */
    if (n_keys == 1 && join_type == 0 && l_key_vecs[0] && r_key_vecs[0] &&
        l_key_vecs[0]->type == RAY_I64 && r_key_vecs[0]->type == RAY_I64 &&
        left_rows > right_rows * 2) {
        /* Determine key range to size the bitmap */
        int64_t* rk = (int64_t*)ray_data(r_key_vecs[0]);
        int64_t key_max = 0;
        for (int64_t i = 0; i < right_rows; i++)
            if (rk[i] > key_max) key_max = rk[i];

        if (key_max < (int64_t)1 << 24) {  /* only for reasonably bounded keys */
            sjoin_sel = ray_sel_new(key_max + 1);
            if (sjoin_sel && !RAY_IS_ERR(sjoin_sel)) {
                uint64_t* bits = ray_sel_bits(sjoin_sel);
                for (int64_t i = 0; i < right_rows; i++) {
                    int64_t k = rk[i];
                    if (k >= 0 && k <= key_max)
                        RAY_SEL_BIT_SET(bits, k);
                }
            }
        }
    }

    /* Phase 2: Parallel probe (two-pass: count → prefix-sum → fill) */
    uint32_t n_tasks = (uint32_t)((left_rows + JOIN_MORSEL - 1) / JOIN_MORSEL);
    if (n_tasks == 0) n_tasks = 1;

    int64_t* morsel_counts = (int64_t*)scratch_calloc(&counts_hdr,
                              (size_t)(n_tasks + 1) * sizeof(int64_t));
    if (!morsel_counts) {
        scratch_free(ht_next_hdr); scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }

    /* For FULL OUTER JOIN, allocate matched_right tracker */
    if (join_type == 2 && right_rows > 0) {
        matched_right = (_Atomic(uint8_t)*)scratch_calloc(&matched_right_hdr,
                                                           (size_t)right_rows);
        if (!matched_right) goto join_cleanup;
    }

    /* Prepare S-Join fields for probe context */
    uint64_t* sjoin_bits = NULL;
    int64_t sjoin_key_max = 0;
    if (sjoin_sel && !RAY_IS_ERR(sjoin_sel)) {
        sjoin_bits = ray_sel_bits(sjoin_sel);
        sjoin_key_max = sjoin_sel->len - 1;
    }

    join_probe_ctx_t probe_ctx = {
        .ht_heads    = ht_heads,
        .ht_next     = ht_next,
        .ht_cap      = ht_cap,
        .l_key_vecs  = l_key_vecs,
        .r_key_vecs  = r_key_vecs,
        .n_keys      = n_keys,
        .join_type   = join_type,
        .left_rows   = left_rows,
        .morsel_counts = morsel_counts,
        .matched_right = matched_right,
        .sjoin_bits  = sjoin_bits,
        .sjoin_key_max = sjoin_key_max,
    };

    /* 2a: Count matches per morsel */
    if (pool && n_tasks > 1)
        ray_pool_dispatch_n(pool, join_count_fn, &probe_ctx, n_tasks);
    else
        for (uint32_t t = 0; t < n_tasks; t++)
            join_count_fn(&probe_ctx, 0, t, t + 1);

    /* Prefix sum → morsel_offsets (reuse counts array as offsets) */
    pair_count = 0;
    for (uint32_t t = 0; t < n_tasks; t++) {
        int64_t cnt = morsel_counts[t];
        morsel_counts[t] = pair_count;
        pair_count += cnt;
    }

    /* Allocate output pair arrays */
    if (pair_count > 0) {
        l_idx = (int64_t*)scratch_alloc(&l_idx_hdr, (size_t)pair_count * sizeof(int64_t));
        r_idx = (int64_t*)scratch_alloc(&r_idx_hdr, (size_t)pair_count * sizeof(int64_t));
        if (!l_idx || !r_idx) goto join_cleanup;
    }

    /* 2b: Fill match pairs */
    probe_ctx.morsel_offsets = morsel_counts;  /* now holds prefix sums */
    probe_ctx.l_idx = l_idx;
    probe_ctx.r_idx = r_idx;

    if (pair_count > 0) {
        if (pool && n_tasks > 1)
            ray_pool_dispatch_n(pool, join_fill_fn, &probe_ctx, n_tasks);
        else
            for (uint32_t t = 0; t < n_tasks; t++)
                join_fill_fn(&probe_ctx, 0, t, t + 1);
    }

    CHECK_CANCEL_GOTO(pool, join_cleanup);

    /* FULL OUTER: append unmatched right rows (l_idx=-1, r_idx=r) */
    if (join_type == 2 && matched_right) {
        int64_t unmatched_right = 0;
        for (int64_t r = 0; r < right_rows; r++)
            if (!matched_right[r]) unmatched_right++;

        if (unmatched_right > 0) {
            int64_t total = pair_count + unmatched_right;
            ray_t* new_l_hdr;
            ray_t* new_r_hdr;
            int64_t* new_l = (int64_t*)scratch_alloc(&new_l_hdr,
                                (size_t)total * sizeof(int64_t));
            int64_t* new_r = (int64_t*)scratch_alloc(&new_r_hdr,
                                (size_t)total * sizeof(int64_t));
            if (!new_l || !new_r) {
                scratch_free(new_l_hdr); scratch_free(new_r_hdr);
                goto join_cleanup;
            }
            if (pair_count > 0) {
                memcpy(new_l, l_idx, (size_t)pair_count * sizeof(int64_t));
                memcpy(new_r, r_idx, (size_t)pair_count * sizeof(int64_t));
            }
            scratch_free(l_idx_hdr);
            scratch_free(r_idx_hdr);
            int64_t off = pair_count;
            for (int64_t r = 0; r < right_rows; r++) {
                if (!matched_right[r]) {
                    new_l[off] = -1;
                    new_r[off] = r;
                    off++;
                }
            }
            l_idx = new_l;  r_idx = new_r;
            l_idx_hdr = new_l_hdr;  r_idx_hdr = new_r_hdr;
            pair_count = total;
        }
    }

join_gather:;
    /* Phase 3: Build result table with parallel column gather.
     * Use multi_gather for batched column access when possible (non-nullable
     * indices), falling back to per-column gather for nullable RIGHT columns. */
    int64_t left_ncols = ray_table_ncols(left_table);
    int64_t right_ncols = ray_table_ncols(right_table);
    result = ray_table_new(left_ncols + right_ncols);
    if (!result || RAY_IS_ERR(result)) goto join_cleanup;

    /* Allocate all output columns upfront for batched gather */
    ray_t* l_out_cols[MGATHER_MAX_COLS];
    int64_t l_out_names[MGATHER_MAX_COLS];
    int64_t l_out_count = 0;
    for (int64_t c = 0; c < left_ncols && l_out_count < MGATHER_MAX_COLS; c++) {
        ray_t* col = ray_table_get_col_idx(left_table, c);
        if (!col) continue;
        ray_t* new_col = col_vec_new(col, pair_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = pair_count;
        l_out_cols[l_out_count] = new_col;
        l_out_names[l_out_count] = ray_table_col_name(left_table, c);
        l_out_count++;
    }

    ray_t* r_out_cols[MGATHER_MAX_COLS];
    ray_t* r_src_cols[MGATHER_MAX_COLS];
    int64_t r_out_names[MGATHER_MAX_COLS];
    int64_t r_out_count = 0;
    for (int64_t c = 0; c < right_ncols; c++) {
        ray_t* col = ray_table_get_col_idx(right_table, c);
        int64_t name_id = ray_table_col_name(right_table, c);
        if (!col) continue;
        bool is_key = false;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]->id);
            if (rk && rk->base.opcode == OP_SCAN && rk->sym == name_id) {
                is_key = true; break;
            }
        }
        if (is_key) continue;
        if (r_out_count >= MGATHER_MAX_COLS) continue;
        ray_t* new_col = col_vec_new(col, pair_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = pair_count;
        r_out_cols[r_out_count] = new_col;
        r_src_cols[r_out_count] = col;
        r_out_names[r_out_count] = name_id;
        r_out_count++;
    }

    if (pair_count > 0) {
        /* Left columns: multi_gather (non-nullable for INNER/LEFT) */
        bool l_nullable = (join_type == 2);  /* only FULL OUTER */
        if (!l_nullable && l_out_count > 1 && l_out_count <= MGATHER_MAX_COLS) {
            multi_gather_ctx_t mgctx = { .idx = l_idx, .ncols = l_out_count };
            int64_t si = 0;
            for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
                ray_t* col = ray_table_get_col_idx(left_table, c);
                if (!col) continue;
                mgctx.srcs[si] = (char*)ray_data(col);
                mgctx.dsts[si] = (char*)ray_data(l_out_cols[si]);
                mgctx.esz[si] = col_esz(col);
                si++;
            }
            if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pair_count);
            else
                multi_gather_fn(&mgctx, 0, 0, pair_count);
        } else {
            /* Fall back to per-column gather for nullable or single column */
            int64_t si = 0;
            for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
                ray_t* col = ray_table_get_col_idx(left_table, c);
                if (!col) continue;
                gather_ctx_t gctx = {
                    .idx = l_idx, .src_col = col, .dst_col = l_out_cols[si],
                    .esz = col_esz(col), .nullable = l_nullable,
                };
                if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                    ray_pool_dispatch(pool, gather_fn, &gctx, pair_count);
                else
                    gather_fn(&gctx, 0, 0, pair_count);
                si++;
            }
        }

        /* Right columns: per-column gather (nullable for LEFT/FULL OUTER) */
        bool r_nullable = (join_type >= 1);
        if (!r_nullable && r_out_count > 1 && r_out_count <= MGATHER_MAX_COLS) {
            multi_gather_ctx_t mgctx = { .idx = r_idx, .ncols = r_out_count };
            for (int64_t i = 0; i < r_out_count; i++) {
                mgctx.srcs[i] = (char*)ray_data(r_src_cols[i]);
                mgctx.dsts[i] = (char*)ray_data(r_out_cols[i]);
                mgctx.esz[i] = col_esz(r_out_cols[i]);
            }
            if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pair_count);
            else
                multi_gather_fn(&mgctx, 0, 0, pair_count);
        } else {
            for (int64_t i = 0; i < r_out_count; i++) {
                gather_ctx_t gctx = {
                    .idx = r_idx, .src_col = r_src_cols[i], .dst_col = r_out_cols[i],
                    .esz = col_esz(r_src_cols[i]), .nullable = r_nullable,
                };
                if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                    ray_pool_dispatch(pool, gather_fn, &gctx, pair_count);
                else
                    gather_fn(&gctx, 0, 0, pair_count);
            }
        }
    }

    /* Propagate RAY_STR string pools from source to gathered columns */
    {
        int64_t si = 0;
        for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
            ray_t* col = ray_table_get_col_idx(left_table, c);
            if (!col) continue;
            col_propagate_str_pool(l_out_cols[si], col);
            si++;
        }
    }
    for (int64_t i = 0; i < r_out_count; i++) {
        col_propagate_str_pool(r_out_cols[i], r_src_cols[i]);
    }

    /* Add columns to result */
    for (int64_t i = 0; i < l_out_count; i++) {
        result = ray_table_add_col(result, l_out_names[i], l_out_cols[i]);
        ray_release(l_out_cols[i]);
    }
    for (int64_t i = 0; i < r_out_count; i++) {
        result = ray_table_add_col(result, r_out_names[i], r_out_cols[i]);
        ray_release(r_out_cols[i]);
    }

join_cleanup:
    if (ht_next_hdr) scratch_free(ht_next_hdr);
    if (ht_heads_hdr) scratch_free(ht_heads_hdr);
    scratch_free(l_idx_hdr);
    scratch_free(r_idx_hdr);
    if (counts_hdr) scratch_free(counts_hdr);
    scratch_free(matched_right_hdr);
    if (sjoin_sel) ray_release(sjoin_sel);
    if (asp_sel) ray_release(asp_sel);

    return result;
}

/* ============================================================================
 * OP_ANTIJOIN: anti-semi-join — keep left rows with NO matching right row
 * Build hash set from right keys, probe left, emit non-matching left rows.
 * ============================================================================ */

static ray_t* exec_antijoin(ray_graph_t* g, ray_op_t* op,
                            ray_t* left_table, ray_t* right_table) {
    if (!left_table || RAY_IS_ERR(left_table)) return left_table;
    if (!right_table || RAY_IS_ERR(right_table)) return right_table;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t left_rows  = ray_table_nrows(left_table);
    int64_t right_rows = ray_table_nrows(right_table);

    if (right_rows > (int64_t)INT32_MAX || left_rows > (int64_t)INT32_MAX)
        return ray_error("nyi", NULL);

    uint8_t n_keys = ext->join.n_join_keys;

    /* Trivial case: empty right → all left rows pass */
    if (right_rows == 0) {
        ray_retain(left_table);
        return left_table;
    }
    /* Trivial case: empty left → empty result */
    if (left_rows == 0) {
        ray_retain(left_table);
        return left_table;
    }

    ray_t* l_key_vecs[16];
    ray_t* r_key_vecs[16];
    memset(l_key_vecs, 0, n_keys * sizeof(ray_t*));
    memset(r_key_vecs, 0, n_keys * sizeof(ray_t*));

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_ext_t* lk = find_ext(g, ext->join.left_keys[k]->id);
        ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]->id);
        if (lk && lk->base.opcode == OP_SCAN)
            l_key_vecs[k] = ray_table_get_col(left_table, lk->sym);
        if (rk && rk->base.opcode == OP_SCAN)
            r_key_vecs[k] = ray_table_get_col(right_table, rk->sym);
        if (rk && rk->base.opcode == OP_CONST && rk->literal)
            r_key_vecs[k] = rk->literal;
    }

    /* RAY_STR keys not yet supported */
    for (uint8_t k = 0; k < n_keys; k++) {
        if ((l_key_vecs[k] && l_key_vecs[k]->type == RAY_STR) ||
            (r_key_vecs[k] && r_key_vecs[k]->type == RAY_STR))
            return ray_error("nyi", NULL);
    }

    /* Build chained hash table from right side */
    ray_t* ht_next_hdr = NULL;
    ray_t* ht_heads_hdr = NULL;

    uint64_t ht_cap64 = 256;
    uint64_t target = (uint64_t)right_rows * 2;
    while (ht_cap64 < target) ht_cap64 *= 2;
    if (ht_cap64 > UINT32_MAX) ht_cap64 = (uint64_t)1 << 31;
    uint32_t ht_cap = (uint32_t)ht_cap64;

    uint32_t* ht_next = (uint32_t*)scratch_alloc(&ht_next_hdr,
                            (size_t)right_rows * sizeof(uint32_t));
    _Atomic(uint32_t)* ht_heads = (_Atomic(uint32_t)*)scratch_alloc(&ht_heads_hdr,
                            ht_cap * sizeof(uint32_t));
    if (!ht_next || !ht_heads) {
        if (ht_next_hdr) scratch_free(ht_next_hdr);
        if (ht_heads_hdr) scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }
    memset(ht_heads, 0xFF, ht_cap * sizeof(uint32_t));  /* JHT_EMPTY */

    /* Build: insert right rows into HT */
    ray_pool_t* pool = ray_pool_get();
    {
        join_build_ctx_t bctx = {
            .ht_heads   = ht_heads,
            .ht_next    = ht_next,
            .ht_mask    = ht_cap - 1,
            .r_key_vecs = r_key_vecs,
            .n_keys     = n_keys,
            .asp_bits   = NULL,
            .asp_key_max = 0,
        };
        if (pool && right_rows > RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, join_build_fn, &bctx, right_rows);
        else
            join_build_fn(&bctx, 0, 0, right_rows);
    }

    if (pool_cancelled(pool)) {
        scratch_free(ht_next_hdr);
        scratch_free(ht_heads_hdr);
        return ray_error("cancel", NULL);
    }

    /* Probe: scan left rows, collect indices of those with NO match */
    ray_t* out_idx_hdr = NULL;
    int64_t* out_idx = (int64_t*)scratch_alloc(&out_idx_hdr,
                            (size_t)left_rows * sizeof(int64_t));
    if (!out_idx) {
        scratch_free(ht_next_hdr);
        scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }

    uint32_t ht_mask = ht_cap - 1;
    int64_t out_count = 0;
    for (int64_t l = 0; l < left_rows; l++) {
        uint64_t h = hash_row_keys(l_key_vecs, n_keys, l);
        uint32_t slot = (uint32_t)(h & ht_mask);
        bool matched = false;
        for (uint32_t r = ht_heads[slot]; r != JHT_EMPTY; r = ht_next[r]) {
            if (join_keys_eq(l_key_vecs, r_key_vecs, n_keys, l, (int64_t)r)) {
                matched = true;
                break;  /* anti-join: one match is enough to exclude */
            }
        }
        if (!matched) {
            out_idx[out_count++] = l;
        }
    }

    scratch_free(ht_next_hdr);
    scratch_free(ht_heads_hdr);

    /* Gather: build result table with only left columns */
    int64_t left_ncols = ray_table_ncols(left_table);
    ray_t* result = ray_table_new(left_ncols);
    if (!result || RAY_IS_ERR(result)) {
        scratch_free(out_idx_hdr);
        return result;
    }

    if (out_count > 0) {
        for (int64_t c = 0; c < left_ncols; c++) {
            ray_t* col = ray_table_get_col_idx(left_table, c);
            if (!col) continue;
            ray_t* new_col = col_vec_new(col, out_count);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = out_count;

            gather_ctx_t gctx = {
                .idx = out_idx, .src_col = col, .dst_col = new_col,
                .esz = col_esz(col), .nullable = false,
            };
            if (pool && out_count > RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, gather_fn, &gctx, out_count);
            else
                gather_fn(&gctx, 0, 0, out_count);

            col_propagate_str_pool(new_col, col);

            int64_t name_id = ray_table_col_name(left_table, c);
            result = ray_table_add_col(result, name_id, new_col);
            ray_release(new_col);
        }
    }

    scratch_free(out_idx_hdr);
    return result;
}

/* ============================================================================
 * OP_WINDOW_JOIN: ASOF join (DuckDB-style sort-merge)
 * For each left row, find the most recent right row where right.time <= left.time,
 * optionally partitioned by equality keys. O(N+M) after sorting.
 * ============================================================================ */

static ray_t* exec_window_join(ray_graph_t* g, ray_op_t* op,
                               ray_t* left_table, ray_t* right_table) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_eq      = ext->asof.n_eq_keys;
    uint8_t join_type = ext->asof.join_type;

    int64_t left_n  = ray_table_nrows(left_table);
    int64_t right_n = ray_table_nrows(right_table);

    /* Resolve time key */
    ray_op_ext_t* time_ext = find_ext(g, ext->asof.time_key->id);
    if (!time_ext || time_ext->base.opcode != OP_SCAN)
        return ray_error("nyi", NULL);
    int64_t time_sym = time_ext->sym;

    /* Resolve equality keys */
    int64_t eq_syms[256];
    for (uint8_t k = 0; k < n_eq; k++) {
        ray_op_ext_t* ek = find_ext(g, ext->asof.eq_keys[k]->id);
        if (!ek || ek->base.opcode != OP_SCAN)
            return ray_error("nyi", NULL);
        eq_syms[k] = ek->sym;
    }

    /* Get time vectors — use int64 representation for comparison.
     * TIME uses 4-byte i32 (ms), TIMESTAMP uses 8-byte i64 (ns).
     * We expand to a temporary i64 array for uniform comparison. */
    ray_t* lt_time_vec = ray_table_get_col(left_table, time_sym);
    ray_t* rt_time_vec = ray_table_get_col(right_table, time_sym);
    if (!lt_time_vec || !rt_time_vec) return ray_error("schema", NULL);
    int8_t time_type = lt_time_vec->type;

    /* Helper macro to read time value as int64_t regardless of storage type */
    #define READ_TIME(vec, idx) \
        ((time_type == RAY_TIME || time_type == RAY_DATE) \
            ? (int64_t)((int32_t*)ray_data(vec))[(idx)] \
            : ((int64_t*)ray_data(vec))[(idx)])

    /* Build i64 time arrays for efficient comparison */
    ray_t* lt_time_hdr = NULL, *rt_time_hdr = NULL;
    int64_t* lt_time = (int64_t*)scratch_alloc(&lt_time_hdr, (size_t)left_n * sizeof(int64_t));
    int64_t* rt_time = (int64_t*)scratch_alloc(&rt_time_hdr, (size_t)right_n * sizeof(int64_t));
    if ((!lt_time && left_n > 0) || (!rt_time && right_n > 0)) {
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < left_n; i++) lt_time[i] = READ_TIME(lt_time_vec, i);
    for (int64_t i = 0; i < right_n; i++) rt_time[i] = READ_TIME(rt_time_vec, i);
    #undef READ_TIME

    /* Get eq key vectors */
    int64_t* lt_eq[256], *rt_eq[256];
    for (uint8_t k = 0; k < n_eq; k++) {
        ray_t* lv = ray_table_get_col(left_table, eq_syms[k]);
        ray_t* rv = ray_table_get_col(right_table, eq_syms[k]);
        if (!lv || !rv) return ray_error("schema", NULL);
        lt_eq[k] = (int64_t*)ray_data(lv);
        rt_eq[k] = (int64_t*)ray_data(rv);
    }

    /* Sort both tables by (eq_keys, time_key) using index arrays */
    ray_t* li_hdr = NULL, *ri_hdr = NULL;
    int64_t* li_idx = (int64_t*)scratch_alloc(&li_hdr, (size_t)left_n * sizeof(int64_t));
    int64_t* ri_idx = (int64_t*)scratch_alloc(&ri_hdr, (size_t)right_n * sizeof(int64_t));
    if ((!li_idx && left_n > 0) || (!ri_idx && right_n > 0)) {
        if (li_hdr) scratch_free(li_hdr);
        if (ri_hdr) scratch_free(ri_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < left_n; i++) li_idx[i] = i;
    for (int64_t i = 0; i < right_n; i++) ri_idx[i] = i;

    /* Bottom-up mergesort on index arrays — O(N log N) */
    {
        int64_t max_n = left_n > right_n ? left_n : right_n;
        ray_t* tmp_hdr = NULL;
        int64_t* tmp = max_n > 0
            ? (int64_t*)scratch_alloc(&tmp_hdr, (size_t)max_n * sizeof(int64_t))
            : NULL;
        if (!tmp && max_n > 0) {
            scratch_free(li_hdr); scratch_free(ri_hdr);
            return ray_error("oom", NULL);
        }

        /* Sort left indices by (eq_keys, time) */
        for (int64_t width = 1; width < left_n; width *= 2) {
            for (int64_t lo = 0; lo < left_n; lo += 2 * width) {
                int64_t mid = lo + width;
                int64_t hi = lo + 2 * width;
                if (mid > left_n) mid = left_n;
                if (hi > left_n) hi = left_n;
                int64_t a = lo, b = mid, t = lo;
                while (a < mid && b < hi) {
                    int64_t ai = li_idx[a], bi = li_idx[b];
                    int cmp = 0;
                    for (uint8_t k2 = 0; k2 < n_eq && cmp == 0; k2++) {
                        if (lt_eq[k2][ai] < lt_eq[k2][bi]) cmp = -1;
                        else if (lt_eq[k2][ai] > lt_eq[k2][bi]) cmp = 1;
                    }
                    if (cmp == 0) {
                        if (lt_time[ai] < lt_time[bi]) cmp = -1;
                        else if (lt_time[ai] > lt_time[bi]) cmp = 1;
                    }
                    tmp[t++] = (cmp <= 0) ? li_idx[a++] : li_idx[b++];
                }
                while (a < mid) tmp[t++] = li_idx[a++];
                while (b < hi) tmp[t++] = li_idx[b++];
                for (int64_t c = lo; c < hi; c++) li_idx[c] = tmp[c];
            }
        }

        /* Sort right indices by (eq_keys, time) */
        for (int64_t width = 1; width < right_n; width *= 2) {
            for (int64_t lo = 0; lo < right_n; lo += 2 * width) {
                int64_t mid = lo + width;
                int64_t hi = lo + 2 * width;
                if (mid > right_n) mid = right_n;
                if (hi > right_n) hi = right_n;
                int64_t a = lo, b = mid, t = lo;
                while (a < mid && b < hi) {
                    int64_t ai = ri_idx[a], bi = ri_idx[b];
                    int cmp = 0;
                    for (uint8_t k2 = 0; k2 < n_eq && cmp == 0; k2++) {
                        if (rt_eq[k2][ai] < rt_eq[k2][bi]) cmp = -1;
                        else if (rt_eq[k2][ai] > rt_eq[k2][bi]) cmp = 1;
                    }
                    if (cmp == 0) {
                        if (rt_time[ai] < rt_time[bi]) cmp = -1;
                        else if (rt_time[ai] > rt_time[bi]) cmp = 1;
                    }
                    tmp[t++] = (cmp <= 0) ? ri_idx[a++] : ri_idx[b++];
                }
                while (a < mid) tmp[t++] = ri_idx[a++];
                while (b < hi) tmp[t++] = ri_idx[b++];
                for (int64_t c = lo; c < hi; c++) ri_idx[c] = tmp[c];
            }
        }

        if (tmp_hdr) scratch_free(tmp_hdr);
    }

    /* Build match array: for each left row (sorted), find best right match */
    ray_t* match_hdr = NULL;
    int64_t* match = (int64_t*)scratch_alloc(&match_hdr, (size_t)left_n * sizeof(int64_t));
    if (!match && left_n > 0) {
        scratch_free(li_hdr); scratch_free(ri_hdr);
        return ray_error("oom", NULL);
    }

    /* Two-pointer merge with best-match carry-forward */
    int64_t rp = 0;        /* right pointer (only advances) */
    int64_t best_ri = -1;  /* best right match in current partition */
    for (int64_t lp = 0; lp < left_n; lp++) {
        int64_t li = li_idx[lp];

        /* Detect partition change — reset best match */
        if (lp > 0) {
            int64_t prev_li = li_idx[lp - 1];
            int changed = 0;
            for (uint8_t k = 0; k < n_eq; k++) {
                if (lt_eq[k][li] != lt_eq[k][prev_li]) { changed = 1; break; }
            }
            if (changed) best_ri = -1;
        }

        /* Advance right pointer, accumulating best match */
        while (rp < right_n) {
            int64_t ri = ri_idx[rp];
            int eq_cmp = 0;
            for (uint8_t k = 0; k < n_eq && eq_cmp == 0; k++) {
                if (rt_eq[k][ri] < lt_eq[k][li]) eq_cmp = -1;
                else if (rt_eq[k][ri] > lt_eq[k][li]) eq_cmp = 1;
            }
            if (eq_cmp > 0) break;  /* right partition past left */
            if (eq_cmp == 0) {
                if (rt_time[ri] <= lt_time[li])
                    best_ri = ri;  /* valid candidate */
                else
                    break;  /* right time past left time */
            }
            rp++;
        }
        match[lp] = best_ri;
    }

    /* Remap match[] from sorted order to original left-row order.
     * match[lp] gives the best right row for sorted left position lp.
     * We need match_orig[li] = best right row for original left row li. */
    ray_t* mo_hdr = NULL;
    int64_t* match_orig = (int64_t*)scratch_alloc(&mo_hdr, (size_t)left_n * sizeof(int64_t));
    if (!match_orig && left_n > 0) {
        scratch_free(match_hdr); scratch_free(li_hdr); scratch_free(ri_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t lp = 0; lp < left_n; lp++)
        match_orig[li_idx[lp]] = match[lp];

    /* Count output rows */
    int64_t out_n = 0;
    if (join_type == 1) {
        out_n = left_n;  /* left outer: all left rows */
    } else {
        for (int64_t i = 0; i < left_n; i++)
            if (match_orig[i] >= 0) out_n++;
    }

    /* Build output table */
    int64_t left_ncols  = ray_table_ncols(left_table);
    int64_t right_ncols = ray_table_ncols(right_table);

    /* Collect right column indices, excluding duplicate key columns */
    int64_t right_out_idx[256];
    int64_t right_out_count = 0;
    for (int64_t c = 0; c < right_ncols; c++) {
        int64_t rname = ray_table_col_name(right_table, c);
        int skip = 0;
        if (rname == time_sym) skip = 1;
        for (uint8_t k = 0; k < n_eq && !skip; k++)
            if (rname == eq_syms[k]) skip = 1;
        if (!skip) right_out_idx[right_out_count++] = c;
    }

    ray_t* out = ray_table_new(left_ncols + right_out_count);

    /* Gather left columns — iterate in original row order */
    for (int64_t c = 0; c < left_ncols; c++) {
        int64_t col_name = ray_table_col_name(left_table, c);
        ray_t* src_col = ray_table_get_col_idx(left_table, c);
        int8_t ctype = src_col->type;
        ray_t* dst_col = ray_vec_new(ctype, out_n);

        uint8_t esz = ray_type_sizes[ctype];
        char* src = (char*)ray_data(src_col);
        char* dst = (char*)ray_data(dst_col);
        int64_t wi = 0;
        for (int64_t li = 0; li < left_n; li++) {
            if (join_type == 0 && match_orig[li] < 0) continue;
            memcpy(dst + wi * esz, src + li * esz, esz);
            wi++;
        }
        dst_col->len = out_n;
        col_propagate_str_pool(dst_col, src_col);
        out = ray_table_add_col(out, col_name, dst_col);
        ray_release(dst_col);
    }

    /* Gather right columns (excluding key duplicates) — original left-row order */
    for (int64_t rc = 0; rc < right_out_count; rc++) {
        int64_t cidx = right_out_idx[rc];
        int64_t col_name = ray_table_col_name(right_table, cidx);
        ray_t* src_col = ray_table_get_col_idx(right_table, cidx);
        int8_t ctype = src_col->type;
        ray_t* dst_col = ray_vec_new(ctype, out_n);

        uint8_t esz = ray_type_sizes[ctype];
        char* src = (char*)ray_data(src_col);
        char* dst = (char*)ray_data(dst_col);
        int64_t wi = 0;
        for (int64_t li = 0; li < left_n; li++) {
            if (join_type == 0 && match_orig[li] < 0) continue;
            if (match_orig[li] >= 0) {
                memcpy(dst + wi * esz, src + match_orig[li] * esz, esz);
            } else {
                memset(dst + wi * esz, 0, esz);  /* NULL fill for left outer */
            }
            wi++;
        }
        dst_col->len = out_n;
        col_propagate_str_pool(dst_col, src_col);
        out = ray_table_add_col(out, col_name, dst_col);
        ray_release(dst_col);
    }

    scratch_free(mo_hdr);
    scratch_free(match_hdr);
    scratch_free(li_hdr);
    scratch_free(ri_hdr);
    if (lt_time_hdr) scratch_free(lt_time_hdr);
    if (rt_time_hdr) scratch_free(rt_time_hdr);
    return out;
}

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
