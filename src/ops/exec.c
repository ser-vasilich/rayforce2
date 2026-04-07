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

#include "ops/internal.h"

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

/* ============================================================================
 * Partitioned gather — cache-conscious column rearrangement
 *
 * Standard gather:  dst[i] = src[idx[i]] — sequential writes, random reads.
 * With 10M rows the source data (~hundreds of MB) far exceeds L2 cache, so
 * every read is a main-memory miss (~60ns even with prefetching).
 *
 * Partitioned gather groups work by source ranges: for each 16K-row source
 * block, process all indices that point into it.  The block fits in L2, so
 * reads become L2 hits (~5ns).  Output writes become random but the CPU's
 * store buffer absorbs them without stalling (~20ns effective).
 *
 * Three phases:
 *   1. Histogram  — count indices per source block           (parallel)
 *   2. Route      — scatter (dest, src) pairs into buckets   (parallel)
 *   3. Block-gather — per block, source in L2 → fast reads   (parallel)
 * ============================================================================ */

/* Block = 16K source rows.  16K × 16 cols × 8B = 2MB ≈ L2 cache per core. */
#define PG_BSHIFT 14
#define PG_BSIZE  (1 << PG_BSHIFT)   /* 16384 */
#define PG_MIN    (PG_BSIZE * 8)     /* 131072 — below this, routing overhead > benefit */

/* Phase 1+2 use dispatch_n with explicit task-to-range mapping so that
 * histogram and scatter have consistent per-task assignments regardless
 * of which worker picks up each task (work-stealing is non-deterministic). */

typedef struct {
    const int64_t* idx;
    int64_t*       hist;      /* n_tasks × n_parts, row-major */
    int64_t        n_parts;
    int64_t        n;         /* total rows */
    uint32_t       n_tasks;
} pg_hist_ctx_t;

static void pg_hist_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    pg_hist_ctx_t* c = (pg_hist_ctx_t*)arg;
    int64_t task = start;

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) { memset(c->hist + task * c->n_parts, 0,
                           (size_t)c->n_parts * sizeof(int64_t)); return; }

    int64_t* h = c->hist + task * c->n_parts;
    memset(h, 0, (size_t)c->n_parts * sizeof(int64_t));
    const int64_t* idx = c->idx;
    for (int64_t i = lo; i < hi; i++)
        h[idx[i] >> PG_BSHIFT]++;
}

typedef struct {
    const int64_t* idx;
    int32_t*       rdest;
    int32_t*       rsrc;
    int64_t*       offsets;   /* n_tasks × n_parts write cursors */
    int64_t        n_parts;
    int64_t        n;
    uint32_t       n_tasks;
} pg_route_ctx_t;

static void pg_route_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    pg_route_ctx_t* c = (pg_route_ctx_t*)arg;
    int64_t task = start;

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;

    int64_t* off = c->offsets + task * c->n_parts;
    const int64_t* idx = c->idx;
    int32_t* rd = c->rdest;
    int32_t* rs = c->rsrc;
    for (int64_t i = lo; i < hi; i++) {
        int64_t src = idx[i];
        int64_t pos = off[src >> PG_BSHIFT]++;
        rd[pos] = (int32_t)i;
        rs[pos] = (int32_t)src;
    }
}

/* Phase 3: per-block gather — one task per source block */
typedef struct {
    const int32_t* rdest;
    const int32_t* rsrc;
    const int64_t* part_off;  /* partition start offsets (n_parts + 1) */
    char**         srcs;
    char**         dsts;
    const uint8_t* esz;
    int64_t        ncols;
} pg_block_ctx_t;

static void pg_block_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    pg_block_ctx_t* c = (pg_block_ctx_t*)arg;
    int64_t blk = start;  /* dispatch_n: one task per call */

    int64_t lo = c->part_off[blk];
    int64_t hi = c->part_off[blk + 1];
    if (lo >= hi) return;

    const int32_t* rd = c->rdest + lo;
    const int32_t* rs = c->rsrc  + lo;
    int64_t cnt = hi - lo;

    /* Column-at-a-time: keeps the source block hot in L2.
     * After the first few reads, the entire 16K-row source slice
     * is cache-resident, so subsequent reads are L2 hits. */
    for (int64_t col = 0; col < c->ncols; col++) {
        uint8_t e = c->esz[col];
        const char* src = c->srcs[col];
        char* dst = c->dsts[col];
        if (e == 8) {
            const uint64_t* s8 = (const uint64_t*)src;
            uint64_t* d8 = (uint64_t*)dst;
            for (int64_t j = 0; j < cnt; j++)
                d8[rd[j]] = s8[rs[j]];
        } else if (e == 4) {
            const uint32_t* s4 = (const uint32_t*)src;
            uint32_t* d4 = (uint32_t*)dst;
            for (int64_t j = 0; j < cnt; j++)
                d4[rd[j]] = s4[rs[j]];
        } else if (e == 2) {
            const uint16_t* s2 = (const uint16_t*)src;
            uint16_t* d2 = (uint16_t*)dst;
            for (int64_t j = 0; j < cnt; j++)
                d2[rd[j]] = s2[rs[j]];
        } else if (e == 1) {
            for (int64_t j = 0; j < cnt; j++)
                dst[rd[j]] = src[rs[j]];
        } else {
            for (int64_t j = 0; j < cnt; j++)
                memcpy(dst + (int64_t)rd[j] * e,
                       src + (int64_t)rs[j] * e, e);
        }
    }
}

/* Public entry point: partitioned gather for n > PG_MIN, fallback otherwise.
 * n:        number of index entries (output rows)
 * src_rows: number of rows in the source columns (indices may reference [0, src_rows)) */
void partitioned_gather(ray_pool_t* pool, const int64_t* idx, int64_t n,
                        int64_t src_rows, char** srcs, char** dsts,
                        const uint8_t* esz, int64_t ncols) {
    /* Fallback for small arrays or no pool */
    if (!pool || n < PG_MIN || n > INT32_MAX || src_rows > INT32_MAX) {
        multi_gather_ctx_t mg = { .idx = idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols && c < MGATHER_MAX_COLS; c++) {
            mg.srcs[c] = srcs[c]; mg.dsts[c] = dsts[c]; mg.esz[c] = esz[c];
            mg.ncols++;
        }
        if (pool) ray_pool_dispatch(pool, multi_gather_fn, &mg, n);
        else      multi_gather_fn(&mg, 0, 0, n);
        return;
    }

    /* Partition by SOURCE range — indices can reference any row in [0, src_rows) */
    int64_t n_parts = (src_rows + PG_BSIZE - 1) >> PG_BSHIFT;
    uint32_t nw = ray_pool_total_workers(pool);

    /* Allocate routing buffers */
    ray_t *hist_hdr = NULL, *off_hdr = NULL;
    ray_t *rdest_hdr = NULL, *rsrc_hdr = NULL, *poff_hdr = NULL;

    int64_t* hist    = (int64_t*)scratch_alloc(&hist_hdr,
                           (size_t)nw * (size_t)n_parts * sizeof(int64_t));
    int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                           (size_t)nw * (size_t)n_parts * sizeof(int64_t));
    int32_t* rdest   = (int32_t*)scratch_alloc(&rdest_hdr,
                           (size_t)n * sizeof(int32_t));
    int32_t* rsrc    = (int32_t*)scratch_alloc(&rsrc_hdr,
                           (size_t)n * sizeof(int32_t));
    int64_t* part_off = (int64_t*)scratch_alloc(&poff_hdr,
                            (size_t)(n_parts + 1) * sizeof(int64_t));

    if (!hist || !offsets || !rdest || !rsrc || !part_off) {
        scratch_free(hist_hdr); scratch_free(off_hdr);
        scratch_free(rdest_hdr); scratch_free(rsrc_hdr);
        scratch_free(poff_hdr);
        /* Fallback to regular gather on allocation failure */
        multi_gather_ctx_t mg = { .idx = idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols && c < MGATHER_MAX_COLS; c++) {
            mg.srcs[c] = srcs[c]; mg.dsts[c] = dsts[c]; mg.esz[c] = esz[c];
            mg.ncols++;
        }
        ray_pool_dispatch(pool, multi_gather_fn, &mg, n);
        return;
    }

    /* Phase 1: parallel histogram (dispatch_n for deterministic task→range) */
    pg_hist_ctx_t hctx = {
        .idx = idx, .hist = hist, .n_parts = n_parts,
        .n = n, .n_tasks = nw,
    };
    ray_pool_dispatch_n(pool, pg_hist_fn, &hctx, nw);

    /* Phase 2: prefix sum → per-task scatter offsets + partition boundaries */
    int64_t running = 0;
    for (int64_t p = 0; p < n_parts; p++) {
        part_off[p] = running;
        for (uint32_t t = 0; t < nw; t++) {
            offsets[t * n_parts + p] = running;
            running += hist[t * n_parts + p];
        }
    }
    part_off[n_parts] = running;

    /* Phase 3: parallel route (same task→range mapping as histogram) */
    pg_route_ctx_t rctx = {
        .idx = idx, .rdest = rdest, .rsrc = rsrc,
        .offsets = offsets, .n_parts = n_parts,
        .n = n, .n_tasks = nw,
    };
    ray_pool_dispatch_n(pool, pg_route_fn, &rctx, nw);

    /* Phase 4: parallel per-block gather */
    pg_block_ctx_t bctx = {
        .rdest = rdest, .rsrc = rsrc, .part_off = part_off,
        .srcs = srcs, .dsts = dsts, .esz = esz, .ncols = ncols,
    };
    ray_pool_dispatch_n(pool, pg_block_fn, &bctx, (uint32_t)n_parts);

    scratch_free(hist_hdr);
    scratch_free(off_hdr);
    scratch_free(rdest_hdr);
    scratch_free(rsrc_hdr);
    scratch_free(poff_hdr);
}

/* (filter execution moved to filter.c) */


/* ============================================================================
 * Sort execution (simple insertion sort)
 * ============================================================================ */

/* Forward declarations — exec_node wraps exec_node_inner with profiling */
/* exec_node declared extern in exec_internal.h */
static ray_t* exec_node_inner(ray_graph_t* g, ray_op_t* op);



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

/* Merge two partial results from partition-streamed execution.
 * Concatenates table columns or vectors across segments. */
ray_t* ray_result_merge(ray_t* accum, ray_t* partial) {
    if (!accum || RAY_IS_ERR(accum)) {
        if (partial && !RAY_IS_ERR(partial)) ray_retain(partial);
        return partial;
    }
    if (!partial || RAY_IS_ERR(partial)) {
        ray_retain(accum);
        return accum;
    }

    /* Table merge: concatenate each column */
    if (accum->type == RAY_TABLE && partial->type == RAY_TABLE) {
        int64_t ncols = ray_table_ncols(accum);
        ray_t* merged = ray_table_new(ncols);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t name_id = ray_table_col_name(accum, c);
            ray_t* a_col = ray_table_get_col_idx(accum, c);
            ray_t* p_col = ray_table_get_col_idx(partial, c);
            if (!a_col || !p_col) {
                ray_release(merged);
                return ray_error("schema", NULL);
            }
            ray_t* combined = ray_vec_concat(a_col, p_col);
            if (!combined || RAY_IS_ERR(combined)) {
                ray_release(merged);
                return combined;
            }
            merged = ray_table_add_col(merged, name_id, combined);
            ray_release(combined);
        }
        return merged;
    }

    /* Vector merge: concatenate directly */
    if (accum->type != RAY_TABLE && partial->type != RAY_TABLE) {
        return ray_vec_concat(accum, partial);
    }

    return ray_error("type", NULL);
}

/* Build a flat table containing one segment's columns from a parted table.
 * For each parted column, extracts segs[seg_idx] as a flat vector.
 * MAPCOMMON columns are materialized for segment seg_idx: the partition key
 * value is broadcast to fill seg_rows elements.
 * Non-parted columns are retained as-is. */
static ray_t* build_segment_table(ray_t* parted_tbl, int32_t seg_idx) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    ray_t* seg_tbl = ray_table_new(ncols);
    if (!seg_tbl || RAY_IS_ERR(seg_tbl)) return seg_tbl;

    /* Find segment row count from first parted column */
    int64_t seg_rows = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (col && RAY_IS_PARTED(col->type)) {
            ray_t** segs = (ray_t**)ray_data(col);
            if (seg_idx < col->len && segs[seg_idx])
                seg_rows = segs[seg_idx]->len;
            break;
        }
    }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(parted_tbl, c);
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (!col) continue;

        if (col->type == RAY_MAPCOMMON) {
            /* Materialize partition key for this segment: broadcast key
             * value across seg_rows elements. */
            ray_t** mc_ptrs = (ray_t**)ray_data(col);
            ray_t* kv = mc_ptrs[0];  /* key_values */
            if (!kv || seg_idx >= kv->len) {
                ray_release(seg_tbl);
                return ray_error("schema", NULL);
            }
            int8_t kv_type = kv->type;
            size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
            ray_t* flat = ray_vec_new(kv_type, seg_rows);
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(seg_tbl);
                return ray_error("oom", NULL);
            }
            flat->len = seg_rows;
            const char* src = (const char*)ray_data(kv) + (size_t)seg_idx * esz;
            char* dst = (char*)ray_data(flat);
            if (esz == 8) {
                uint64_t v; memcpy(&v, src, 8);
                for (int64_t r = 0; r < seg_rows; r++)
                    ((uint64_t*)dst)[r] = v;
            } else if (esz == 4) {
                uint32_t v; memcpy(&v, src, 4);
                for (int64_t r = 0; r < seg_rows; r++)
                    ((uint32_t*)dst)[r] = v;
            } else {
                for (int64_t r = 0; r < seg_rows; r++)
                    memcpy(dst + r * esz, src, esz);
            }
            seg_tbl = ray_table_add_col(seg_tbl, name_id, flat);
            ray_release(flat);
        } else if (RAY_IS_PARTED(col->type)) {
            ray_t** segs = (ray_t**)ray_data(col);
            if (seg_idx < col->len && segs[seg_idx]) {
                ray_retain(segs[seg_idx]);
                seg_tbl = ray_table_add_col(seg_tbl, name_id, segs[seg_idx]);
                ray_release(segs[seg_idx]);
            }
        } else {
            ray_retain(col);
            seg_tbl = ray_table_add_col(seg_tbl, name_id, col);
            ray_release(col);
        }
    }
    return seg_tbl;
}

/* Is this opcode safe for segment streaming with concatenation merge?
 * Only element-wise, scan, filter, project, and alias ops produce
 * results that can be correctly concatenated across segments.
 * Everything else (joins, aggregations, sorts, graph ops, etc.)
 * requires specialized merge or global state. */
static bool op_streamable(uint16_t opc) {
    switch (opc) {
        /* Data access (OP_CONST excluded: vector constants have total-row
         * length and produce length mismatches with per-segment data.
         * Scalar constants are checked separately in dag_can_stream.) */
        case OP_SCAN:
        /* Element-wise unary */
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_SQRT:
        case OP_LOG: case OP_EXP: case OP_CEIL: case OP_FLOOR:
        case OP_ISNULL: case OP_CAST:
        /* Element-wise binary */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR:
        case OP_MIN2: case OP_MAX2: case OP_IF:
        /* String element-wise */
        case OP_LIKE: case OP_ILIKE: case OP_UPPER: case OP_LOWER:
        case OP_STRLEN: case OP_SUBSTR: case OP_REPLACE: case OP_TRIM:
        case OP_CONCAT:
        /* Temporal element-wise */
        case OP_EXTRACT: case OP_DATE_TRUNC:
        /* Structure */
        case OP_FILTER: case OP_SELECT: case OP_ALIAS:
        case OP_MATERIALIZE:
            return true;
        default:
            return false;
    }
}

/* Check whether a DAG can be correctly executed via segment streaming
 * with simple concatenation merge. Every live node must be streamable.
 * OP_CONST is allowed only for scalar (atom) literals — vector constants
 * have total-row length and would mismatch per-segment data. */
static bool dag_can_stream(ray_graph_t* g) {
    for (uint32_t i = 0; i < g->node_count; i++) {
        if (g->nodes[i].flags & OP_FLAG_DEAD) continue;
        uint16_t opc = g->nodes[i].opcode;
        if (opc == OP_CONST) {
            /* Scalar constants are safe; vector constants are not */
            ray_op_ext_t* ext = find_ext(g, g->nodes[i].id);
            if (ext && ext->literal && !ray_is_atom(ext->literal))
                return false;
            continue;
        }
        if (!op_streamable(opc))
            return false;
    }
    return true;
}

ray_t* ray_execute(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return ray_error("nyi", NULL);

    /* Lazy-init the global thread pool on first call */
    ray_pool_t* pool = ray_pool_get();

    /* Reset cancellation flag at the start of each query */
    if (pool)
        atomic_store_explicit(&pool->cancelled, 0, memory_order_relaxed);

    /* Detect streaming mode: check if g->table has parted columns */
    int32_t seg_count = 0;
    if (g->table) {
        for (int64_t c = 0; c < ray_table_ncols(g->table); c++) {
            ray_t* col = ray_table_get_col_idx(g->table, c);
            if (col && RAY_IS_PARTED(col->type)) {
                seg_count = (int32_t)col->len;
                break;
            }
        }
    }

    if (seg_count == 0 || !dag_can_stream(g)) {
        /* Non-parted table or DAG contains ops that need specialized merge:
         * use existing flat-materialization path. */
        ray_t* result = exec_node(g, root);
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

    /* Streaming mode: find seg_mask from optimizer (if any) */
    uint64_t* seg_mask = NULL;
    for (uint32_t e = 0; e < g->ext_count; e++) {
        if (g->ext_nodes[e] && g->ext_nodes[e]->seg_mask) {
            seg_mask = g->ext_nodes[e]->seg_mask;
            break;
        }
    }

    ray_t* saved_table = g->table;
    ray_t* result = NULL;

    for (int32_t s = 0; s < seg_count; s++) {
        /* Check pruning mask */
        if (seg_mask && !(seg_mask[s / 64] & (1ULL << (s % 64))))
            continue;

        /* Check cancellation */
        if (pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) {
            g->table = saved_table;
            ray_release(result);
            return ray_error("cancel", NULL);
        }

        /* Build flat table for this segment and swap g->table.
         * All operators (OP_SCAN, GROUP, expr_compile, etc.) see flat
         * columns via g->table, so no special-casing is needed. */
        ray_t* seg_tbl = build_segment_table(saved_table, s);
        if (!seg_tbl || RAY_IS_ERR(seg_tbl)) {
            g->table = saved_table;
            ray_release(result);
            return seg_tbl;
        }
        g->table = seg_tbl;
        g->selection = NULL;

        ray_t* partial = exec_node(g, root);

        /* Compact lazy selection for this segment */
        if (g->selection && partial && !RAY_IS_ERR(partial)
            && partial->type == RAY_TABLE) {
            ray_t* compacted = sel_compact(g, partial, g->selection);
            ray_release(partial);
            ray_release(g->selection);
            g->selection = NULL;
            partial = compacted;
        }

        g->table = saved_table;
        ray_release(seg_tbl);

        if (!partial || RAY_IS_ERR(partial)) {
            ray_release(result);
            return partial;
        }

        /* Merge partial into accumulator */
        ray_t* merged = ray_result_merge(result, partial);
        ray_release(result);
        ray_release(partial);
        if (!merged || RAY_IS_ERR(merged)) return merged;
        result = merged;
    }

    /* All segments pruned: execute DAG on empty table to get correct
     * output schema (handles SELECT/PROJECT that reshape columns).
     * Build a fresh 0-row table — do not mutate shared source vectors. */
    if (!result) {
        int64_t ncols = ray_table_ncols(saved_table);
        ray_t* empty_tbl = ray_table_new(ncols);
        if (empty_tbl && !RAY_IS_ERR(empty_tbl)) {
            for (int64_t c = 0; c < ncols; c++) {
                int64_t name_id = ray_table_col_name(saved_table, c);
                ray_t* col = ray_table_get_col_idx(saved_table, c);
                if (!col) continue;
                int8_t base = col->type;
                if (col->type == RAY_MAPCOMMON) {
                    ray_t** mc = (ray_t**)ray_data(col);
                    base = mc[0] ? mc[0]->type : RAY_I64;
                } else if (RAY_IS_PARTED(col->type)) {
                    base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                }
                ray_t* ecol = ray_vec_new(base, 0);
                if (!ecol || RAY_IS_ERR(ecol)) {
                    /* ray_vec_new rejects RAY_LIST (type 0) and other
                     * non-standard types; fall back to a raw 0-length
                     * block with the correct type tag. */
                    ecol = ray_alloc(0);
                    if (!ecol || RAY_IS_ERR(ecol)) continue;
                    ecol->type = base;
                    ecol->len = 0;
                }
                empty_tbl = ray_table_add_col(empty_tbl, name_id, ecol);
                ray_release(ecol);
            }
            g->table = empty_tbl;
            g->selection = NULL;
            result = exec_node(g, root);
            if (g->selection) {
                ray_release(g->selection);
                g->selection = NULL;
            }
            g->table = saved_table;
            ray_release(empty_tbl);
        }
    }

    return result;
}
