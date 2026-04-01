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

#ifndef RAY_H
#define RAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Semantic Versioning ===== */

#define RAY_VERSION_MAJOR 2
#define RAY_VERSION_MINOR 1
#define RAY_VERSION_PATCH 0

/* Packed version number: 0xMMmmpp (MM=major, mm=minor, pp=patch) */
#define RAY_VERSION_NUMBER \
    ((RAY_VERSION_MAJOR * 10000) + (RAY_VERSION_MINOR * 100) + RAY_VERSION_PATCH)

/* Compile-time version check: true if lib version >= (major, minor, patch) */
#define RAY_VERSION_AT_LEAST(major, minor, patch) \
    (RAY_VERSION_NUMBER >= ((major) * 10000 + (minor) * 100 + (patch)))

/* Runtime version query */
int  ray_version_major(void);
int  ray_version_minor(void);
int  ray_version_patch(void);
const char* ray_version_string(void);

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
/* Unified dictionary-encoded string column (adaptive width) */
#define RAY_SYM       13

/* Variable-length string column (inline + pool) */
#define RAY_STR       14

/* Compound types */
#define RAY_TABLE     98
#define RAY_DICT      99

/* Function types (Rayforce-compatible) */
#define RAY_LAMBDA    100   /* User-defined function (compiled body + env) */
#define RAY_UNARY     101   /* Unary builtin: ray_t* (*)(ray_t*) */
#define RAY_BINARY    102   /* Binary builtin: ray_t* (*)(ray_t*, ray_t*) */
#define RAY_VARY      103   /* Variadic builtin: ray_t* (*)(ray_t**, int64_t) */
#define RAY_ERROR     127   /* Error object: 8-byte packed ASCII code in sdata */

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

/* DEPRECATED — legacy sentinel-pointer error macro (migration in progress) */
#define RAY_ERR_CODE(p)  ((ray_err_t)(uintptr_t)(p)) /* DEPRECATED */

#define RAY_IS_ERR(p)    ((p) != NULL && (uintptr_t)(p) > 31 && ((ray_t*)(p))->type == RAY_ERROR)

const char* ray_err_str(ray_err_t e); /* DEPRECATED */

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

/* Error object creation (defined in core/runtime.c) */
ray_t* ray_error(const char* code, const char* fmt, ...);
const char* ray_err_code_str(ray_err_t e);
const char* ray_err_code(ray_t* err);

/* ===== Accessor Macros ===== */

#define ray_type(v)       ((v)->type)
#define ray_is_atom(v)    ((v)->type < 0 || (v)->type >= RAY_LAMBDA)
#define ray_is_vec(v)     ((v)->type >= RAY_BOOL && (v)->type <= RAY_STR)
#define ray_len(v)        ((v)->len)
static inline void* ray_data_fn(ray_t* v) { return (void*)v->data; }
#define ray_data(v)       ray_data_fn(v)

/* ===== Memory Allocator API ===== */

ray_t*    ray_alloc(size_t data_size);
/* NOTE: ray_free supports cross-thread free via foreign_blocks list.
 * Blocks freed from a non-owning thread are deferred and coalesced
 * when the owning heap flushes foreign blocks. */
void     ray_free(ray_t* v);

/* ===== COW / Ref Counting API ===== */

void     ray_retain(ray_t* v);
void     ray_release(ray_t* v);

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
ray_t*       ray_table_schema(ray_t* tbl);

#ifdef __cplusplus
}
#endif

#endif /* RAY_H */
