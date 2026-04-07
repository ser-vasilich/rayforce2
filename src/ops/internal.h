/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Shared helpers for exec.c split — included by expr.c, filter.c, join.c, etc.
 *   Small hot-path helpers are static inline; larger functions that remain in
 *   exec.c are declared extern.
 */

#ifndef RAY_EXEC_INTERNAL_H
#define RAY_EXEC_INTERNAL_H

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "exec.h"
#include "hash.h"
#include "core/pool.h"
#include "core/profile.h"
#include "store/csr.h"
#include "store/hnsw.h"
#include "lftj.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "table/table.h"
#include "vec/str.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <ctype.h>

/* ══════════════════════════════════════════
 * Global profiler
 * ══════════════════════════════════════════ */

extern ray_profile_t g_ray_profile;

/* ══════════════════════════════════════════
 * Arena-based scratch allocation helpers
 * ══════════════════════════════════════════ */

/* Allocate zero-initialized scratch buffer, returns data pointer.
 * *hdr_out receives the ray_t* header for later ray_free(). */
static inline void* scratch_calloc(ray_t** hdr_out, size_t nbytes) {
    ray_t* h = ray_alloc(nbytes);
    if (!h) { *hdr_out = NULL; return NULL; }
    void* p = ray_data(h);
    memset(p, 0, nbytes);
    *hdr_out = h;
    return p;
}

/* Allocate uninitialized scratch buffer. */
static inline void* scratch_alloc(ray_t** hdr_out, size_t nbytes) {
    ray_t* h = ray_alloc(nbytes);
    if (!h) { *hdr_out = NULL; return NULL; }
    *hdr_out = h;
    return ray_data(h);
}

/* Reallocate: alloc new, copy old, free old. Returns new data pointer. */
static inline void* scratch_realloc(ray_t** hdr_out, size_t old_bytes, size_t new_bytes) {
    ray_t* old_h = *hdr_out;
    ray_t* new_h = ray_alloc(new_bytes);
    if (!new_h) return NULL;
    void* new_p = ray_data(new_h);
    if (old_h) {
        memcpy(new_p, ray_data(old_h), old_bytes < new_bytes ? old_bytes : new_bytes);
        ray_free(old_h);
    }
    *hdr_out = new_h;
    return new_p;
}

/* Free a scratch buffer (NULL-safe). */
static inline void scratch_free(ray_t* hdr) {
    if (!hdr) return;
    ray_free(hdr);
}

/* ══════════════════════════════════════════
 * Safe sym intern
 * ══════════════════════════════════════════ */

/* Safe sym intern for constant column names in graph algorithm result tables.
 * Falls back to 0 on failure (column name interning should never fail for
 * short constant strings unless ray_sym_init failed). */
static inline int64_t sym_intern_safe(const char* s, size_t len) {
    int64_t id = ray_sym_intern(s, len);
    return id >= 0 ? id : 0;
}

/* ══════════════════════════════════════════
 * Unified column read/write helpers
 * ══════════════════════════════════════════ */

static inline int64_t read_col_i64(const void* data, int64_t row,
                                    int8_t type, uint8_t attrs) {
    switch (type) {
    case RAY_I64: case RAY_TIMESTAMP:
        return ((const int64_t*)data)[row];
    case RAY_SYM:
        switch (attrs & RAY_SYM_W_MASK) {
        case RAY_SYM_W8:  return (int64_t)((const uint8_t*)data)[row];
        case RAY_SYM_W16: return (int64_t)((const uint16_t*)data)[row];
        case RAY_SYM_W32: return (int64_t)((const uint32_t*)data)[row];
        default:         return ((const int64_t*)data)[row];
        }
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        return (int64_t)((const int32_t*)data)[row];
    case RAY_I16:
        return (int64_t)((const int16_t*)data)[row];
    default: /* RAY_BOOL, RAY_U8 */
        return (int64_t)((const uint8_t*)data)[row];
    }
}

static inline void write_col_i64(void* data, int64_t row, int64_t val,
                                  int8_t type, uint8_t attrs) {
    switch (type) {
    case RAY_I64: case RAY_TIMESTAMP:
        ((int64_t*)data)[row] = val; return;
    case RAY_SYM:
        ray_write_sym(data, row, (uint64_t)val, type, attrs); return;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        ((int32_t*)data)[row] = (int32_t)val; return;
    case RAY_I16:
        ((int16_t*)data)[row] = (int16_t)val; return;
    default: /* RAY_BOOL, RAY_U8 */
        ((uint8_t*)data)[row] = (uint8_t)val; return;
    }
}

/* ══════════════════════════════════════════
 * RAY_SYM-aware column helpers
 * ══════════════════════════════════════════ */

static inline uint8_t col_esz(const ray_t* col) {
    return ray_sym_elem_size(col->type, col->attrs);
}

/* Fast key reader for DA/sort hot loops: elem_size is pre-computed and
 * loop-invariant, so the switch is always perfectly predicted.  Avoids the
 * ray_read_sym → type dispatch chain (3+ branches per element). */
static inline int64_t read_by_esz(const void* data, int64_t row, uint8_t esz) {
    switch (esz) {
    case 1:  return (int64_t)((const uint8_t*)data)[row];
    case 2:  return (int64_t)((const uint16_t*)data)[row];
    case 4:  return (int64_t)((const uint32_t*)data)[row];
    default: return ((const int64_t*)data)[row];
    }
}

static inline ray_t* col_vec_new(const ray_t* src, int64_t cap) {
    if (src->type == RAY_SYM)
        return ray_sym_vec_new(src->attrs & RAY_SYM_W_MASK, cap);
    return ray_vec_new(src->type, cap);
}

/* Propagate str_pool from source to gathered result.
 * Source may be a slice — resolve to owner's pool. */
static inline void col_propagate_str_pool(ray_t* dst, const ray_t* src) {
    if (src->type != RAY_STR || dst->type != RAY_STR) return;
    const ray_t* owner = (src->attrs & RAY_ATTR_SLICE) ? src->slice_parent : src;
    if (owner->str_pool) {
        if (dst->str_pool) ray_release(dst->str_pool);
        ray_retain(owner->str_pool);
        dst->str_pool = owner->str_pool;
    }
}

/* Same but from explicit type + attrs (for parted base type, etc.) */
static inline ray_t* typed_vec_new(int8_t type, uint8_t attrs, int64_t cap) {
    if (type == RAY_SYM)
        return ray_sym_vec_new(attrs & RAY_SYM_W_MASK, cap);
    return ray_vec_new(type, cap);
}

/* ══════════════════════════════════════════
 * Cancellation check
 * ══════════════════════════════════════════ */

static inline bool pool_cancelled(ray_pool_t* pool) {
    return pool && RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                                     memory_order_relaxed));
}

#define CHECK_CANCEL(pool)                                \
    do { if (pool_cancelled(pool))                        \
             return ray_error("cancel", NULL); } while(0)

#define CHECK_CANCEL_GOTO(pool, lbl)                      \
    do { if (pool_cancelled(pool)) {                      \
             result = ray_error("cancel", NULL);          \
             goto lbl;                                    \
         }                                                \
    } while(0)

/* ══════════════════════════════════════════
 * Graph helper: find extended node
 * ══════════════════════════════════════════ */

static inline ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id) {
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == node_id)
            return g->ext_nodes[i];
    }
    return NULL;
}

/* ══════════════════════════════════════════
 * String helpers
 * ══════════════════════════════════════════ */

/* Convert an atom (-RAY_STR or RAY_SYM scalar) to ray_str_t for comparison */
static inline void atom_to_str_t(ray_t* atom, ray_str_t* out, const char** out_pool) {
    const char* sp;
    size_t sl;
    if (atom->type == -RAY_STR) {
        sp = ray_str_ptr(atom);
        sl = ray_str_len(atom);
    } else if (atom->type == RAY_STR) {
        /* Length-1 RAY_STR vector used as scalar */
        if (atom->len < 1) {
            memset(out, 0, sizeof(ray_str_t));
            *out_pool = NULL;
            return;
        }
        const ray_str_t* elems = (const ray_str_t*)ray_data(atom);
        *out = elems[0];
        *out_pool = atom->str_pool ? (const char*)ray_data(atom->str_pool) : NULL;
        return;
    } else if (RAY_IS_SYM(atom->type) && ray_is_atom(atom)) {
        /* SAFETY: ray_sym_str returns a borrowed pointer into the append-only
         * sym table.  The pointer is valid for the lifetime of the sym table
         * (i.e., the entire query execution).  If the sym table ever gains
         * eviction, this must retain the returned atom. */
        ray_t* s = ray_sym_str(atom->i64);
        sp = s ? ray_str_ptr(s) : "";
        sl = s ? ray_str_len(s) : 0;
    } else {
        sp = ""; sl = 0;
    }
    memset(out, 0, sizeof(ray_str_t));
    out->len = (uint32_t)sl;
    if (sl <= RAY_STR_INLINE_MAX) {
        if (sl > 0) memcpy(out->data, sp, sl);
        *out_pool = NULL;
    } else {
        memcpy(out->prefix, sp, 4);
        out->pool_off = 0;
        *out_pool = sp; /* point directly at atom's string data */
    }
}

/* Resolve RAY_STR vec to data owner, accounting for slices.
 * Returns element pointer (already offset for slices) and pool pointer. */
static inline void str_resolve(const ray_t* v, const ray_str_t** elems,
                               const char** pool) {
    const ray_t* owner = (v->attrs & RAY_ATTR_SLICE) ? v->slice_parent : v;
    int64_t base = (v->attrs & RAY_ATTR_SLICE) ? v->slice_offset : 0;
    *elems = (const ray_str_t*)ray_data((ray_t*)owner) + base;
    *pool = owner->str_pool ? (const char*)ray_data(owner->str_pool) : NULL;
}

/* Helper: resolve sym/enum element to string */
static inline void sym_elem(const ray_t* input, int64_t i,
                            const char** out_str, size_t* out_len) {
    int64_t sym_id = ray_read_sym(ray_data((ray_t*)input), i, input->type, input->attrs);
    ray_t* atom = ray_sym_str(sym_id);
    if (!atom) { *out_str = ""; *out_len = 0; return; }
    *out_str = ray_str_ptr(atom);
    *out_len = ray_str_len(atom);
}

/* ══════════════════════════════════════════
 * Shared types — used by expr.c and exec.c
 * ══════════════════════════════════════════ */

typedef struct {
    bool    enabled;
    double  bias_f64;
    int64_t bias_i64;
} agg_affine_t;

#define AGG_LINEAR_MAX_TERMS 8

typedef struct {
    bool    enabled;
    uint8_t n_terms;
    void*   term_ptrs[AGG_LINEAR_MAX_TERMS];
    int8_t  term_types[AGG_LINEAR_MAX_TERMS];
    int64_t coeff_i64[AGG_LINEAR_MAX_TERMS];
    int64_t bias_i64;
} agg_linear_t;

typedef struct {
    uint8_t n_terms;
    int64_t syms[AGG_LINEAR_MAX_TERMS];
    int64_t coeff_i64[AGG_LINEAR_MAX_TERMS];
    int64_t bias_i64;
} linear_expr_i64_t;

/* ── Expression compiler types ── */

#define EXPR_MAX_REGS 16
#define EXPR_MAX_INS  48
#define EXPR_MORSEL   RAY_MORSEL_ELEMS

typedef struct {
    uint8_t opcode;     /* OP_ADD, OP_NEG, OP_CAST, etc. */
    uint8_t dst;        /* destination register */
    uint8_t src1;       /* source 1 register */
    uint8_t src2;       /* source 2 register (0xFF for unary) */
} expr_ins_t;

enum { REG_SCAN = 0, REG_CONST = 1, REG_SCRATCH = 2 };

typedef struct {
    uint8_t n_ins;
    uint8_t n_regs;
    uint8_t n_scratch;      /* scratch registers needed */
    uint8_t out_reg;
    int8_t  out_type;       /* RAY_F64, RAY_I64, or RAY_BOOL */
    bool    has_parted;     /* true if any REG_SCAN refs a parted column */
    struct {
        uint8_t     kind;       /* REG_SCAN / REG_CONST / REG_SCRATCH */
        int8_t      type;       /* computational type: RAY_F64 / RAY_I64 / RAY_BOOL */
        int8_t      col_type;   /* original column type (REG_SCAN only) */
        uint8_t     col_attrs;  /* column attrs — RAY_SYM width (REG_SCAN only) */
        bool        is_parted;  /* true if this SCAN refs a parted column */
        const void* data;       /* column data pointer (REG_SCAN only) */
        ray_t*       parted_col; /* parted wrapper (is_parted only) */
        double      const_f64;  /* scalar value (REG_CONST) */
        int64_t     const_i64;  /* scalar value (REG_CONST) */
    } regs[EXPR_MAX_REGS];
    expr_ins_t ins[EXPR_MAX_INS];
} ray_expr_t;

/* ══════════════════════════════════════════
 * Shared gather types — used by filter.c, exec.c (sort, join)
 * ══════════════════════════════════════════ */

#define MGATHER_MAX_COLS 16

typedef struct {
    const int64_t* idx;
    char*          srcs[MGATHER_MAX_COLS];
    char*          dsts[MGATHER_MAX_COLS];
    uint8_t        esz[MGATHER_MAX_COLS];
    int64_t        ncols;
} multi_gather_ctx_t;

typedef struct {
    int64_t*     idx;
    ray_t*        src_col;
    ray_t*        dst_col;
    uint8_t      esz;
    bool         nullable;  /* true = idx may contain -1 (LEFT JOIN nulls) */
} gather_ctx_t;

/* ══════════════════════════════════════════
 * Shared sort types and constants — used by sort_exec.c, exec.c (window)
 * ══════════════════════════════════════════ */

#define RADIX_SORT_THRESHOLD 4096  /* switch from comparison to radix sort */
#define SMALL_POOL_THRESHOLD 8192  /* skip pool dispatch below this size */
#define NEARLY_SORTED_FRAC   0.05  /* threshold for nearly-sorted detection */
#define MK_PRESCAN_MAX_KEYS  8     /* max sort keys for stack allocation */

typedef struct {
    ray_t**       vecs;
    uint8_t*     desc;
    uint8_t*     nulls_first;
    uint8_t      n_sort;
} sort_cmp_ctx_t;

/* Radix pass context (shared across histogram + scatter phases) */
typedef struct {
    const uint64_t*  keys;
    const int64_t*   idx;
    uint64_t*        keys_out;
    int64_t*         idx_out;
    int64_t          n;
    uint8_t          shift;
    uint32_t         n_tasks;
    uint32_t*        hist;       /* flat [n_tasks * 256] */
    int64_t*         offsets;    /* flat [n_tasks * 256] */
} radix_pass_ctx_t;

/* Key-encoding context for parallel encode phase */
typedef struct {
    uint64_t*       keys;      /* output */
    int64_t*        indices;   /* if non-NULL, initialize indices[i]=i (fused iota) */
    /* Single-key fields: */
    const void*     data;      /* raw column data */
    int8_t          type;      /* column type */
    uint8_t         col_attrs; /* RAY_SYM width attrs */
    bool            desc;
    bool            nulls_first; /* for single-key F64: 1=nulls first */
    /* SYM rank mapping (NULL if not sym): */
    const uint32_t* enum_rank; /* intern_id → sort rank */
    /* Composite-key fields (n_keys > 1): */
    uint8_t         n_keys;
    ray_t**          vecs;
    int64_t         mins[16];
    int64_t         ranges[16];
    uint8_t         bit_shifts[16]; /* bit offset for key k in composite */
    uint8_t         descs[16];
    const uint32_t* enum_ranks[16]; /* per-key rank mappings */
} radix_encode_ctx_t;

/* Parallel multi-key min/max prescan context */
typedef struct {
    ray_t*     const* vecs;
    uint32_t* const* enum_ranks;
    uint8_t          n_keys;
    int64_t          nrows;
    uint32_t         n_workers;
    int64_t*         pw_mins;
    int64_t*         pw_maxs;
} mk_prescan_ctx_t;

/* Parallel sort phase 1 context */
typedef struct {
    const sort_cmp_ctx_t* cmp_ctx;
    int64_t*  indices;
    int64_t*  tmp;
    int64_t   nrows;
    uint32_t  n_chunks;
} sort_phase1_ctx_t;

/* Parallel merge pass context */
typedef struct {
    const sort_cmp_ctx_t* cmp_ctx;
    const int64_t*  src;
    int64_t*        dst;
    int64_t         nrows;
    int64_t         run_size;
} sort_merge_ctx_t;

/* Compute the number of significant bytes for radix sort based on type.
 * Returns 1..8: the number of byte passes radix_sort_run needs. */
static inline uint8_t radix_key_bytes(int8_t type) {
    switch (type) {
    case RAY_BOOL: case RAY_U8:   return 1;
    case RAY_I16:                return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: return 4;
    default:                    return 8;  /* I64, F64, TIMESTAMP, SYM */
    }
}

/* ══════════════════════════════════════════
 * Extern forward declarations — larger functions in exec.c
 * ══════════════════════════════════════════ */

/* ── exec.c (gather helpers) ── */
void multi_gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end);
void gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end);
void partitioned_gather(ray_pool_t* pool, const int64_t* idx, int64_t n,
                        int64_t src_rows, char** srcs, char** dsts,
                        const uint8_t* esz, int64_t ncols);

/* ── filter.c ── */
ray_t* exec_filter(ray_graph_t* g, ray_op_t* op, ray_t* input, ray_t* pred);
ray_t* exec_filter_head(ray_t* input, ray_t* pred, int64_t limit);
ray_t* sel_compact(ray_graph_t* g, ray_t* tbl, ray_t* sel);

/* ── expr.c ── */
bool try_affine_sumavg_input(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                             ray_t** out_vec, agg_affine_t* out_affine);
bool try_linear_sumavg_input_i64(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                                 agg_linear_t* out_plan);
bool expr_compile(ray_graph_t* g, ray_t* tbl, ray_op_t* root, ray_expr_t* out);
ray_t* expr_eval_full(const ray_expr_t* expr, int64_t nrows);
ray_t* exec_elementwise_unary(ray_graph_t* g, ray_op_t* op, ray_t* input);
ray_t* exec_elementwise_binary(ray_graph_t* g, ray_op_t* op, ray_t* lhs, ray_t* rhs);

/* ── sort_exec.c ── */
int sort_cmp(const sort_cmp_ctx_t* ctx, int64_t a, int64_t b);
void sort_insertion(const sort_cmp_ctx_t* ctx, int64_t* arr, int64_t n);
void sort_merge_recursive(const sort_cmp_ctx_t* ctx,
                          int64_t* arr, int64_t* tmp, int64_t n);
void sort_phase1_fn(void* arg, uint32_t worker_id, int64_t start, int64_t end);
void sort_merge_fn(void* arg, uint32_t worker_id, int64_t start, int64_t end);
void key_introsort(uint64_t* keys, int64_t* idx, int64_t n);
double detect_sortedness(ray_pool_t* pool, const uint64_t* keys, int64_t n);
uint8_t compute_key_nbytes(ray_pool_t* pool, const uint64_t* keys,
                            int64_t n, uint8_t type_max);
int64_t* radix_sort_run(ray_pool_t* pool, uint64_t* keys, int64_t* indices,
                         uint64_t* keys_tmp, int64_t* idx_tmp,
                         int64_t n, uint8_t n_bytes,
                         uint64_t** sorted_keys_out);
uint64_t* packed_radix_sort_run(ray_pool_t* pool, uint64_t* data,
                                 uint64_t* tmp, int64_t n, uint8_t n_bytes);
int64_t* msd_radix_sort_run(ray_pool_t* pool, uint64_t* keys, int64_t* indices,
                              uint64_t* keys_tmp, int64_t* idx_tmp,
                              int64_t n, uint8_t n_bytes,
                              uint64_t** sorted_keys_out);
void radix_encode_fn(void* arg, uint32_t wid, int64_t start, int64_t end);
void mk_prescan_fn(void* arg, uint32_t wid, int64_t start, int64_t end);
uint32_t* build_enum_rank(ray_t* col, int64_t nrows, ray_t** hdr_out);
ray_t* exec_sort(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t limit);

/* ── join.c ── */
ray_t* exec_join(ray_graph_t* g, ray_op_t* op, ray_t* left_table, ray_t* right_table);
ray_t* exec_antijoin(ray_graph_t* g, ray_op_t* op,
                     ray_t* left_table, ray_t* right_table);
ray_t* exec_window_join(ray_graph_t* g, ray_op_t* op,
                        ray_t* left_table, ray_t* right_table);

/* ── group.c ── */
ray_t* exec_reduction(ray_graph_t* g, ray_op_t* op, ray_t* input);
ray_t* exec_count_distinct(ray_graph_t* g, ray_op_t* op, ray_t* input);
ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t group_limit);

/* Group HT types and helpers — shared with pivot (exec.c) */
#define GHT_NEED_SUM   0x01
#define GHT_NEED_MIN   0x02
#define GHT_NEED_MAX   0x04
#define GHT_NEED_SUMSQ 0x08

typedef struct {
    uint16_t entry_stride;
    uint16_t row_stride;
    uint8_t  n_keys;
    uint8_t  n_aggs;
    uint8_t  n_agg_vals;
    uint8_t  need_flags;
    uint8_t  agg_is_f64;
    uint8_t  agg_is_first;
    uint8_t  agg_is_last;
    int8_t   agg_val_slot[8];
    uint16_t off_sum;
    uint16_t off_min;
    uint16_t off_max;
    uint16_t off_sumsq;
} ght_layout_t;

typedef struct {
    uint32_t*    slots;
    uint32_t     ht_cap;
    char*        rows;
    uint32_t     grp_count;
    uint32_t     grp_cap;
    ght_layout_t layout;
    ray_t*        _h_slots;
    ray_t*        _h_rows;
} group_ht_t;

/* Row-level accessors for group HT rows */
#define HT_SALT(h)  ((uint8_t)((h) >> 56))
#define HT_EMPTY    UINT32_MAX
#define HT_PACK(salt, gid)  (((uint32_t)(uint8_t)(salt) << 24) | ((gid) & 0xFFFFFF))
#define HT_GID(s)   ((s) & 0xFFFFFF)
#define HT_SALT_V(s) ((uint8_t)((s) >> 24))

#define ROW_RD_F64(row, off, slot) (((const double*)((const void*)((row) + (off))))[(slot)])
#define ROW_RD_I64(row, off, slot) (((const int64_t*)((const void*)((row) + (off))))[(slot)])
#define ROW_WR_F64(row, off, slot) (((double*)((void*)((row) + (off))))[(slot)])
#define ROW_WR_I64(row, off, slot) (((int64_t*)((void*)((row) + (off))))[(slot)])

ght_layout_t ght_compute_layout(uint8_t n_keys, uint8_t n_aggs,
                                ray_t** agg_vecs, uint8_t need_flags,
                                const uint16_t* agg_ops);
bool group_ht_init(group_ht_t* ht, uint32_t cap, const ght_layout_t* ly);
void group_ht_free(group_ht_t* ht);
void group_rows_range(group_ht_t* ht, void** key_data, int8_t* key_types,
                      uint8_t* key_attrs, ray_t** agg_vecs,
                      int64_t start, int64_t end);

/* ── window.c ── */
ray_t* exec_window(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* ── graph_exec.c ── */
ray_t* exec_expand(ray_graph_t* g, ray_op_t* op, ray_t* src_vec);
ray_t* exec_var_expand(ray_graph_t* g, ray_op_t* op, ray_t* start_vec);
ray_t* exec_shortest_path(ray_graph_t* g, ray_op_t* op,
                          ray_t* src_val, ray_t* dst_val);
ray_t* exec_pagerank(ray_graph_t* g, ray_op_t* op);
ray_t* exec_connected_comp(ray_graph_t* g, ray_op_t* op);
ray_t* exec_dijkstra(ray_graph_t* g, ray_op_t* op,
                     ray_t* src_val, ray_t* dst_val);
ray_t* exec_wco_join(ray_graph_t* g, ray_op_t* op);
ray_t* exec_louvain(ray_graph_t* g, ray_op_t* op);
ray_t* exec_degree_cent(ray_graph_t* g, ray_op_t* op);
ray_t* exec_topsort(ray_graph_t* g, ray_op_t* op);
ray_t* exec_cluster_coeff(ray_graph_t* g, ray_op_t* op);
ray_t* exec_betweenness(ray_graph_t* g, ray_op_t* op);
ray_t* exec_closeness(ray_graph_t* g, ray_op_t* op);
ray_t* exec_mst(ray_graph_t* g, ray_op_t* op);
ray_t* exec_random_walk(ray_graph_t* g, ray_op_t* op, ray_t* src_val);
ray_t* exec_dfs(ray_graph_t* g, ray_op_t* op, ray_t* src_val);
ray_t* exec_astar(ray_graph_t* g, ray_op_t* op,
                  ray_t* src_val, ray_t* dst_val);
ray_t* exec_k_shortest(ray_graph_t* g, ray_op_t* op,
                       ray_t* src_val, ray_t* dst_val);

/* ── pivot_exec.c ── */
ray_t* exec_if(ray_graph_t* g, ray_op_t* op);
ray_t* exec_pivot(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* ── embedding_exec.c ── */
ray_t* exec_cosine_sim(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec);
ray_t* exec_euclidean_dist(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec);
ray_t* exec_knn(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec);
ray_t* exec_hnsw_knn(ray_graph_t* g, ray_op_t* op);

/* ── temporal_exec.c ── */
ray_t* exec_extract(ray_graph_t* g, ray_op_t* op);
ray_t* exec_date_trunc(ray_graph_t* g, ray_op_t* op);

/* ── string_exec.c ── */
ray_t* exec_like(ray_graph_t* g, ray_op_t* op);
ray_t* exec_ilike(ray_graph_t* g, ray_op_t* op);
ray_t* exec_string_unary(ray_graph_t* g, ray_op_t* op);
ray_t* exec_strlen(ray_graph_t* g, ray_op_t* op);
ray_t* exec_substr(ray_graph_t* g, ray_op_t* op);
ray_t* exec_replace(ray_graph_t* g, ray_op_t* op);
ray_t* exec_concat(ray_graph_t* g, ray_op_t* op);

/* ── exec.c ── */
ray_t* materialize_mapcommon(ray_t* mc);
ray_t* materialize_mapcommon_head(ray_t* mc, int64_t n);
ray_t* materialize_mapcommon_filter(ray_t* mc, ray_t* pred, int64_t pass_count);
ray_t* broadcast_scalar(ray_t* atom, int64_t nrows);
ray_t* exec_node(ray_graph_t* g, ray_op_t* op);
ray_t* ray_result_merge(ray_t* accum, ray_t* partial, uint16_t root_opcode);

#endif /* RAY_EXEC_INTERNAL_H */
