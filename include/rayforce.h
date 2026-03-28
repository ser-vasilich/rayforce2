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

#ifndef RAY_H
#define RAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Type Constants ===== */

#define RAY_LIST       0
#define RAY_BOOL       1
#define RAY_U8         2
#define RAY_CHAR       3
#define RAY_I16        4
#define RAY_I32        5
#define RAY_I64        6
#define RAY_F64        7
#define RAY_F32        8    /* 32-bit float vector (also used for embeddings) */
#define RAY_DATE       9
#define RAY_TIME      10
#define RAY_TIMESTAMP 11
#define RAY_GUID      12
#define RAY_TABLE     13
#define RAY_SEL       16   /* selection bitmap (lazy filter) */

/* Unified dictionary-encoded string column (adaptive width) */
#define RAY_SYM       20

/* Variable-length string column (inline + pool) */
#define RAY_STR       21

/* Lazy DAG handle (atom-only; stored inline in nullmap region) */
#define RAY_LAZY      104

/* Function types (Rayforce-compatible) */
#define RAY_LAMBDA    100   /* User-defined function (compiled body + env) */
#define RAY_UNARY     101   /* Unary builtin: ray_t* (*)(ray_t*) */
#define RAY_BINARY    102   /* Binary builtin: ray_t* (*)(ray_t*, ray_t*) */
#define RAY_VARY      103   /* Variadic builtin: ray_t* (*)(ray_t**, int64_t) */

/* Function attribute flags (stored in attrs byte) */
#define RAY_FN_NONE          0x00
#define RAY_FN_LEFT_ATOMIC   0x01  /* auto-map left arg over vectors */
#define RAY_FN_RIGHT_ATOMIC  0x02  /* auto-map right arg over vectors */
#define RAY_FN_ATOMIC        0x04  /* auto-map all args over vectors */
#define RAY_FN_AGGR          0x08  /* aggregation function */
#define RAY_FN_SPECIAL_FORM  0x10  /* receives unevaluated args */

/* AST name flag (distinguishes symbol literal from variable reference) */
#define RAY_ATTR_NAME        0x20  /* ray_t SYM atom with this flag = name reference */

/* Vector literal flag (distinguishes [x y z] data from (f x y) calls in RAY_LIST) */
#define RAY_ATTR_VECTOR      0x01  /* RAY_LIST with this flag = data vector, not call */
#define RAY_ATTR_DICT        0x02  /* RAY_LIST with this flag = dict {k: v ...} */

/* Function type signatures (use union ray_t since ray_t typedef comes later) */
typedef union ray_t* (*ray_unary_fn)(union ray_t*);
typedef union ray_t* (*ray_binary_fn)(union ray_t*, union ray_t*);
typedef union ray_t* (*ray_vary_fn)(union ray_t**, int64_t);

/* Symbol width encoding (lower 2 bits of attrs when type == RAY_SYM) */
#define RAY_SYM_W_MASK   0x03
#define RAY_SYM_W8       0x00   /* uint8_t  indices — dict ≤ 255 entries */
#define RAY_SYM_W16      0x01   /* uint16_t indices — dict ≤ 65,535 */
#define RAY_SYM_W32      0x02   /* uint32_t indices — dict ≤ 4,294,967,295 */
#define RAY_SYM_W64      0x03   /* uint64_t indices — dict > 4B entries */

/* Helper macros */
#define RAY_IS_SYM(t)         ((t) == RAY_SYM)
#define RAY_SYM_ELEM(attrs)   (1u << ((attrs) & RAY_SYM_W_MASK))  /* 1,2,4,8 */

/* Number of types (positive range): must be > max type ID */
#define RAY_TYPE_COUNT 22

/* ===== Attribute Flags =====
 *
 * The `attrs` byte in ray_t is type-namespaced: the same bit positions carry
 * different meanings depending on the object's type tag.
 *
 *   Bits 0x01-0x03  RAY_SYM vectors:  sym index width (RAY_SYM_W8/W16/W32/W64)
 *   Bits 0x01-0x10  function objects (RAY_UNARY/BINARY/VARY): RAY_FN_* flags
 *   Bits 0x01-0x02  RAY_LIST atoms:   RAY_ATTR_VECTOR / RAY_ATTR_DICT
 *   Bit  0x10       vectors:         RAY_ATTR_SLICE
 *   Bit  0x20       vectors:         RAY_ATTR_NULLMAP_EXT
 *   Bit  0x20       -RAY_SYM:        RAY_ATTR_NAME (variable reference)
 *   Bit  0x40       vectors:         RAY_ATTR_HAS_NULLS
 *   Bit  0x80       all types:       RAY_ATTR_ARENA (arena-allocated, no refcount)
 *
 * Overlapping bit values are safe because consumers always check the type tag
 * before interpreting attrs.
 */

#define RAY_ATTR_SLICE        0x10
#define RAY_ATTR_NULLMAP_EXT  0x20
#define RAY_ATTR_HAS_NULLS    0x40
#define RAY_ATTR_ARENA        0x80

/* ===== Error Handling ===== */

typedef enum {
    RAY_OK = 0,
    RAY_ERR_OOM,
    RAY_ERR_TYPE,
    RAY_ERR_RANGE,
    RAY_ERR_LENGTH,
    RAY_ERR_RANK,
    RAY_ERR_DOMAIN,
    RAY_ERR_NYI,
    RAY_ERR_IO,
    RAY_ERR_SCHEMA,
    RAY_ERR_CORRUPT,
    RAY_ERR_CANCEL,
    RAY_ERR_PARSE,
    RAY_ERR_NAME,
    RAY_ERR_LIMIT
} ray_err_t;

#define RAY_ERR_PTR(e)   ((ray_t*)(uintptr_t)(e))
#define RAY_IS_ERR(p)    ((uintptr_t)(p) < 32)
#define RAY_ERR_CODE(p)  ((ray_err_t)(uintptr_t)(p))

const char* ray_err_str(ray_err_t e);

/* ===== Core Type: ray_t (32-byte block/object header) ===== */

typedef union ray_t {
    /* Allocated: object header */
    struct {
        /* Bytes 0-15: nullable bitmask / slice / ext nullmap */
        union {
            uint8_t  nullmap[16];
            struct { union ray_t* slice_parent; int64_t slice_offset; };
            struct { union ray_t* ext_nullmap;  union ray_t* sym_dict; };
            struct { union ray_t* str_ext_null; union ray_t* str_pool; };
        };
        /* Bytes 16-31: metadata + value */
        uint8_t  mmod;       /* 0=heap, 1=file-mmap */
        uint8_t  order;      /* block order (block size = 2^order) */
        int8_t   type;       /* negative=atom, positive=vector, 0=LIST */
        uint8_t  attrs;      /* attribute flags */
        uint32_t rc;         /* reference count (0=free) */
        union {
            uint8_t  b8;     /* BOOL atom */
            uint8_t  u8;     /* U8 atom */
            char     c8;     /* CHAR atom */
            int16_t  i16;    /* I16 atom */
            int32_t  i32;    /* I32 atom */
            uint32_t u32;
            int64_t  i64;    /* I64/SYMBOL/DATE/TIME/TIMESTAMP atom */
            double   f64;    /* F64 atom */
            union ray_t* obj; /* pointer to child (long strings, GUID) */
            struct { uint8_t slen; char sdata[7]; }; /* SSO string (<=7 bytes) */
            int64_t  len;    /* vector element count */
        };
        uint8_t  data[];     /* element data (flexible array member) */
    };
    /* Free: buddy allocator block (fl_prev/fl_next overlay bytes 0-15) */
    struct {
        union ray_t* fl_prev;
        union ray_t* fl_next;
    };
} ray_t;

/* Type sizes lookup table (defined in types.c) */
extern const uint8_t ray_type_sizes[RAY_TYPE_COUNT];

/* ===== Lazy DAG Handle Accessors =====
 *
 * A lazy handle is a ray_t with type == RAY_LAZY.  It stores two
 * pointers in the nullmap region (bytes 0-15), which is unused for atoms:
 *   Bytes 0-7:  ray_graph_t* (owns the graph)
 *   Bytes 8-15: ray_op_t*    (the output node)
 */
typedef struct ray_graph ray_graph_t;
typedef struct ray_op    ray_op_t;

static inline bool ray_is_lazy(ray_t* x) {
    return x && !RAY_IS_ERR(x) && x->type == RAY_LAZY;
}

/* ===== Accessor Macros ===== */

#define ray_type(v)       ((v)->type)
#define ray_is_atom(v)    ((v)->type < 0 || (v)->type >= RAY_LAMBDA)
#define ray_is_vec(v)     ((v)->type > 0 && (v)->type < RAY_LAMBDA)
#define ray_len(v)        ((v)->len)
static inline void* ray_data_fn(ray_t* v) { return (void*)v->data; }
#define ray_data(v)       ray_data_fn(v)
#define ray_elem_size(t)  (ray_type_sizes[(t)])

/* ===== Inline String Element (16 bytes) ===== */

typedef union {
    struct { uint32_t len; char     data[12]; };      /* inline: len <= 12 */
    struct { uint32_t len_; char    prefix[4];        /* pooled: len > 12  */
             uint32_t pool_off; uint32_t _pad; };
} ray_str_t;

#define RAY_STR_INLINE_MAX 12

static inline bool ray_str_is_inline(const ray_str_t* s) {
    return s->len <= RAY_STR_INLINE_MAX;
}

/* Resolve string data pointer for a ray_str_t element.
 * pool_base: base of string pool (NULL if all strings are inline) */
static inline const char* ray_str_t_ptr(const ray_str_t* s, const char* pool_base) {
    if (s->len == 0) return "";
    if (ray_str_is_inline(s)) return s->data;
    assert(pool_base != NULL && "ray_str_t_ptr: pooled string requires non-NULL pool_base");
    return pool_base + s->pool_off;
}

/* ===== Forward Declarations (types referenced in public API) ===== */

typedef struct ray_pool      ray_pool_t;
typedef struct ray_csr       ray_csr_t;
typedef struct ray_rel       ray_rel_t;
typedef struct ray_hnsw      ray_hnsw_t;

/* ===== Memory Allocator API ===== */

ray_t*    ray_alloc(size_t data_size);
/* NOTE: ray_free supports cross-thread free via foreign_blocks list.
 * Blocks freed from a non-owning thread are deferred and coalesced
 * when the owning heap flushes foreign blocks. */
void     ray_free(ray_t* v);
ray_t*    ray_alloc_copy(ray_t* v);
ray_t*    ray_scratch_alloc(size_t data_size);
ray_t*    ray_scratch_realloc(ray_t* v, size_t new_data_size);

/* ===== COW / Ref Counting API ===== */

void     ray_retain(ray_t* v);
void     ray_release(ray_t* v);
ray_t*    ray_cow(ray_t* v);

/* ===== Atom Constructors ===== */

ray_t* ray_bool(bool val);
ray_t* ray_u8(uint8_t val);
ray_t* ray_char(char val);
ray_t* ray_i16(int16_t val);
ray_t* ray_i32(int32_t val);
ray_t* ray_i64(int64_t val);
ray_t* ray_f64(double val);
ray_t* ray_str(const char* s, size_t len);
ray_t* ray_sym(int64_t id);
ray_t* ray_date(int64_t val);
ray_t* ray_time(int64_t val);
ray_t* ray_timestamp(int64_t val);
ray_t* ray_guid(const uint8_t* bytes);

/* ===== Vector API ===== */

ray_t* ray_vec_new(int8_t type, int64_t capacity);
ray_t* ray_sym_vec_new(uint8_t sym_width, int64_t capacity);  /* RAY_SYM with adaptive width */
ray_t* ray_vec_append(ray_t* vec, const void* elem);
ray_t* ray_vec_set(ray_t* vec, int64_t idx, const void* elem);
void* ray_vec_get(ray_t* vec, int64_t idx);
ray_t* ray_vec_slice(ray_t* vec, int64_t offset, int64_t len);
ray_t* ray_vec_concat(ray_t* a, ray_t* b);
ray_t* ray_vec_from_raw(int8_t type, const void* data, int64_t count);

/* Null bitmap ops */
void     ray_vec_set_null(ray_t* vec, int64_t idx, bool is_null);
ray_err_t ray_vec_set_null_checked(ray_t* vec, int64_t idx, bool is_null);
bool     ray_vec_is_null(ray_t* vec, int64_t idx);

/* ===== String Vector API ===== */

ray_t* ray_str_vec_append(ray_t* vec, const char* s, size_t len);
const char* ray_str_vec_get(ray_t* vec, int64_t idx, size_t* out_len);
ray_t* ray_str_vec_set(ray_t* vec, int64_t idx, const char* s, size_t len);
ray_t* ray_str_vec_compact(ray_t* vec);

/* ===== String API ===== */

const char* ray_str_ptr(ray_t* s);
size_t      ray_str_len(ray_t* s);
int         ray_str_cmp(ray_t* a, ray_t* b);

/* ===== List API ===== */

ray_t* ray_list_new(int64_t capacity);
ray_t* ray_list_append(ray_t* list, ray_t* item);
ray_t* ray_list_get(ray_t* list, int64_t idx);
ray_t* ray_list_set(ray_t* list, int64_t idx, ray_t* item);

/* ===== Symbol Intern Table API ===== */

ray_err_t ray_sym_init(void);
void     ray_sym_destroy(void);
int64_t  ray_sym_intern(const char* str, size_t len);
int64_t  ray_sym_find(const char* str, size_t len);
ray_t*    ray_sym_str(int64_t id);
uint32_t ray_sym_count(void);
bool     ray_sym_ensure_cap(uint32_t needed);
ray_err_t ray_sym_save(const char* path);
ray_err_t ray_sym_load(const char* path);

/* ===== Table API ===== */

ray_t*       ray_table_new(int64_t ncols);
ray_t*       ray_table_add_col(ray_t* tbl, int64_t name_id, ray_t* col_vec);
ray_t*       ray_table_get_col(ray_t* tbl, int64_t name_id);
ray_t*       ray_table_get_col_idx(ray_t* tbl, int64_t idx);
int64_t     ray_table_col_name(ray_t* tbl, int64_t idx);
void        ray_table_set_col_name(ray_t* tbl, int64_t idx, int64_t name_id);
int64_t     ray_table_ncols(ray_t* tbl);
int64_t     ray_table_nrows(ray_t* tbl);
int64_t     ray_parted_nrows(ray_t* parted_col);
ray_t*       ray_table_schema(ray_t* tbl);

/* ===== Lazy DAG Handle API (Public) ===== */

ray_t*    ray_lazy_materialize(ray_t* val);

/* ===== Storage API ===== */

/* Cross-platform file I/O (locking, sync, atomic rename) */
#ifdef _WIN32
  typedef HANDLE ray_fd_t;
  #define RAY_FD_INVALID INVALID_HANDLE_VALUE
#else
  typedef int ray_fd_t;
  #define RAY_FD_INVALID (-1)
#endif

#define RAY_OPEN_READ   0x01
#define RAY_OPEN_WRITE  0x02
#define RAY_OPEN_CREATE 0x04

ray_fd_t  ray_file_open(const char* path, int flags);
void     ray_file_close(ray_fd_t fd);
ray_err_t ray_file_lock_ex(ray_fd_t fd);
ray_err_t ray_file_lock_sh(ray_fd_t fd);
ray_err_t ray_file_unlock(ray_fd_t fd);
ray_err_t ray_file_sync(ray_fd_t fd);
ray_err_t ray_file_sync_dir(const char* path);
ray_err_t ray_file_rename(const char* old_path, const char* new_path);

/* Column file I/O */
ray_err_t ray_col_save(ray_t* vec, const char* path);
ray_t*    ray_col_load(const char* path);
ray_t*    ray_col_mmap(const char* path);

/* Splayed table I/O */
ray_err_t ray_splay_save(ray_t* tbl, const char* dir, const char* sym_path);
ray_t*    ray_splay_load(const char* dir, const char* sym_path);
ray_t*    ray_read_splayed(const char* dir, const char* sym_path);

/* Partitioned table */
ray_t*    ray_part_load(const char* db_root, const char* table_name);
ray_t*    ray_read_parted(const char* db_root, const char* table_name);

/* Metadata */
ray_err_t ray_meta_save_d(ray_t* schema, const char* path);
ray_t*    ray_meta_load_d(const char* path);

/* ===== CSV API ===== */

ray_t* ray_read_csv(const char* path);
ray_t* ray_read_csv_opts(const char* path, char delimiter, bool header,
                        const int8_t* col_types, int32_t n_types);
ray_err_t ray_write_csv(ray_t* table, const char* path);


/* ===== Pool / Cancel API ===== */

void     ray_cancel(void);

/* ===== Rayfall Builtin Functions ===== */
/* Public builtin implementations for the Rayfall language.
 * Functions with _fn suffix are disambiguated from DAG op constructors
 * of the same base name. */

/* Arithmetic (binary, FN_ATOMIC) */
ray_t* ray_add_fn(ray_t* a, ray_t* b);
ray_t* ray_sub_fn(ray_t* a, ray_t* b);
ray_t* ray_mul_fn(ray_t* a, ray_t* b);
ray_t* ray_div_fn(ray_t* a, ray_t* b);
ray_t* ray_mod_fn(ray_t* a, ray_t* b);

/* Comparison (binary, FN_ATOMIC) */
ray_t* ray_gt_fn(ray_t* a, ray_t* b);
ray_t* ray_lt_fn(ray_t* a, ray_t* b);
ray_t* ray_gte(ray_t* a, ray_t* b);
ray_t* ray_lte(ray_t* a, ray_t* b);
ray_t* ray_eq_fn(ray_t* a, ray_t* b);
ray_t* ray_neq(ray_t* a, ray_t* b);

/* Logic (binary/unary) */
ray_t* ray_and_fn(ray_t* a, ray_t* b);
ray_t* ray_or_fn(ray_t* a, ray_t* b);
ray_t* ray_not_fn(ray_t* x);
ray_t* ray_neg_fn(ray_t* x);

/* Aggregation (unary, FN_AGGR) */
ray_t* ray_sum_fn(ray_t* x);
ray_t* ray_count_fn(ray_t* x);
ray_t* ray_avg_fn(ray_t* x);
ray_t* ray_min(ray_t* x);
ray_t* ray_max(ray_t* x);
ray_t* ray_first_fn(ray_t* x);
ray_t* ray_last_fn(ray_t* x);
ray_t* ray_med(ray_t* x);
ray_t* ray_dev(ray_t* x);

/* Higher-order functions (variadic/binary) */
ray_t* ray_map(ray_t** args, int64_t n);
ray_t* ray_pmap(ray_t** args, int64_t n);
ray_t* ray_fold(ray_t** args, int64_t n);
ray_t* ray_scan_fn(ray_t** args, int64_t n);
ray_t* ray_filter_fn(ray_t* vec, ray_t* mask);
ray_t* ray_apply(ray_t** args, int64_t n);

/* Collection operations */
ray_t* ray_distinct_fn(ray_t* x);
ray_t* ray_in(ray_t* val, ray_t* vec);
ray_t* ray_except(ray_t* vec1, ray_t* vec2);
ray_t* ray_union(ray_t* vec1, ray_t* vec2);
ray_t* ray_sect(ray_t* vec1, ray_t* vec2);
ray_t* ray_take(ray_t* vec, ray_t* n_obj);
ray_t* ray_at(ray_t* vec, ray_t* idx);
ray_t* ray_find(ray_t* vec, ray_t* val);
ray_t* ray_til(ray_t* x);
ray_t* ray_reverse(ray_t* x);

/* Table construction and access */
ray_t* ray_list(ray_t** args, int64_t n);
ray_t* ray_table(ray_t* names, ray_t* cols);
ray_t* ray_key(ray_t* x);
ray_t* ray_value(ray_t* x);

/* Query operations */
ray_t* ray_select_fn(ray_t** args, int64_t n);
ray_t* ray_update(ray_t** args, int64_t n);
ray_t* ray_insert(ray_t** args, int64_t n);
ray_t* ray_upsert(ray_t** args, int64_t n);
ray_t* ray_xbar(ray_t* col, ray_t* bucket);

/* Join operations */
ray_t* ray_left_join(ray_t** args, int64_t n);
ray_t* ray_inner_join(ray_t** args, int64_t n);
ray_t* ray_window_join(ray_t** args, int64_t n);

/* I/O builtins */
ray_t* ray_println(ray_t** args, int64_t n);
ray_t* ray_read_csv_fn(ray_t** args, int64_t n);
ray_t* ray_write_csv_fn(ray_t** args, int64_t n);
ray_t* ray_read_file(ray_t* path_obj);
ray_t* ray_write_file(ray_t* path_obj, ray_t* content);

/* Cast and type */
ray_t* ray_cast_fn(ray_t* type_sym, ray_t* val);
ray_t* ray_type_fn(ray_t* val);

/* Special forms */
ray_t* ray_set(ray_t* name_obj, ray_t* val_expr);
ray_t* ray_let(ray_t* name_obj, ray_t* val_expr);
ray_t* ray_cond(ray_t** args, int64_t n);
ray_t* ray_do(ray_t** args, int64_t n);
ray_t* ray_fn(ray_t** args, int64_t n);
ray_t* ray_raise(ray_t* val);
ray_t* ray_try(ray_t* expr, ray_t* handler_expr);

#ifdef __cplusplus
}
#endif

#endif /* RAY_H */
