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
#include <string.h>
#include <assert.h>
/* MSVC < 17.4 (cl 19.34) does not ship <stdatomic.h>; use Interlocked intrinsics
 * instead. _MSC_VER 1934 corresponds to VS 2022 17.4. */
#if !defined(_MSC_VER) || _MSC_VER >= 1934
  #include <stdatomic.h>
#else
  #include <windows.h>
  #define _Atomic(T)                          volatile T
  #define atomic_store_explicit(p, v, mo)     (*(p) = (v))
  #define atomic_load_explicit(p, mo)         (*(p))
  #define atomic_fetch_add_explicit(p, v, mo) _InterlockedExchangeAdd((volatile long*)(p), (long)(v))
  #define atomic_fetch_sub_explicit(p, v, mo) _InterlockedExchangeAdd((volatile long*)(p), -(long)(v))
  #define atomic_exchange_explicit(p, v, mo)  _InterlockedExchange((volatile long*)(p), (long)(v))
  #define atomic_compare_exchange_weak_explicit(p, exp, des, s, f) \
      (_InterlockedCompareExchange((volatile long*)(p), (long)(des), *(long*)(exp)) == *(long*)(exp))
  #define atomic_store(p, v)                  (*(p) = (v))
  #define atomic_thread_fence(mo)             MemoryBarrier()
  #define memory_order_relaxed 0
  #define memory_order_acquire 0
  #define memory_order_release 0
  #define memory_order_acq_rel 0
  #define memory_order_seq_cst 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Platform Macros ===== */

#ifndef RAY_LIKELY
#if defined(__GNUC__) || defined(__clang__)
  #define RAY_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define RAY_UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define RAY_ALIGN(n)    __attribute__((aligned(n)))
  #define RAY_INLINE      static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
  #define RAY_LIKELY(x)   (x)
  #define RAY_UNLIKELY(x) (x)
  #define RAY_ALIGN(n)    __declspec(align(n))
  #define RAY_INLINE      static __forceinline
#else
  #define RAY_LIKELY(x)   (x)
  #define RAY_UNLIKELY(x) (x)
  #define RAY_ALIGN(n)
  #define RAY_INLINE      static inline
#endif
#endif /* RAY_LIKELY */

#ifndef RAY_ASSUME_ALIGNED
#if defined(__GNUC__) || defined(__clang__)
  #define RAY_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#else
  #define RAY_ASSUME_ALIGNED(p, n) (p)
#endif
#endif

#if defined(_MSC_VER)
  #define RAY_TLS __declspec(thread)
#else
  #define RAY_TLS _Thread_local
#endif

/* ===== Atomic Helpers ===== */

#if defined(_MSC_VER)
  #define ray_atomic_inc(p)   _InterlockedIncrement((volatile long*)(p))
  #define ray_atomic_dec(p)   _InterlockedDecrement((volatile long*)(p))
  #define ray_atomic_load(p)  _InterlockedOr((volatile long*)(p), 0)
#else
  #define ray_atomic_inc(p)   atomic_fetch_add_explicit(p, 1, memory_order_relaxed)
  #define ray_atomic_dec(p)   atomic_fetch_sub_explicit(p, 1, memory_order_acq_rel)
  #define ray_atomic_load(p)  atomic_load_explicit(p, memory_order_acquire)
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

/* Function types (Rayforce-compatible) */
#define RAY_LAMBDA    100   /* User-defined function (compiled body + env) */
#define RAY_UNARY     101   /* Unary builtin: ray_t* (*)(ray_t*) */
#define RAY_BINARY    102   /* Binary builtin: ray_t* (*)(ray_t*, ray_t*) */
#define RAY_VARY      103   /* Variadic builtin: ray_t* (*)(ray_t**, int64_t) */

/* Function atom types (negative = atom) */
#define RAY_ATOM_LAMBDA    (-RAY_LAMBDA)
#define RAY_ATOM_UNARY     (-RAY_UNARY)
#define RAY_ATOM_BINARY    (-RAY_BINARY)
#define RAY_ATOM_VARY      (-RAY_VARY)

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

/* Parted types: composite of RAY_PARTED_BASE + base type */
#define RAY_PARTED_BASE   32
#define RAY_MAPCOMMON     64   /* virtual partition column */

/* MAPCOMMON inferred sub-types (stored in attrs field) */
#define RAY_MC_SYM    0   /* opaque partition key strings */
#define RAY_MC_DATE   1   /* YYYY.MM.DD partition directories */
#define RAY_MC_I64    2   /* pure integer partition keys */

#define RAY_IS_PARTED(t)       ((t) >= RAY_PARTED_BASE && (t) < RAY_MAPCOMMON)
#define RAY_PARTED_BASETYPE(t) ((t) - RAY_PARTED_BASE)

/* Atom variants (negative type tags) */
#define RAY_ATOM_BOOL       (-RAY_BOOL)
#define RAY_ATOM_U8         (-RAY_U8)
#define RAY_ATOM_CHAR       (-RAY_CHAR)
#define RAY_ATOM_I16        (-RAY_I16)
#define RAY_ATOM_I32        (-RAY_I32)
#define RAY_ATOM_I64        (-RAY_I64)
#define RAY_ATOM_F64        (-RAY_F64)
#define RAY_ATOM_F32        (-RAY_F32)
#define RAY_ATOM_STR        (-RAY_STR)
#define RAY_ATOM_DATE       (-RAY_DATE)
#define RAY_ATOM_TIME       (-RAY_TIME)
#define RAY_ATOM_TIMESTAMP  (-RAY_TIMESTAMP)
#define RAY_ATOM_GUID       (-RAY_GUID)
#define RAY_ATOM_SYM        (-RAY_SYM)

/* Number of types (positive range): must be > max type ID */
#define RAY_TYPE_COUNT 22

/* ===== Attribute Flags =====
 *
 * The `attrs` byte in ray_t is type-namespaced: the same bit positions carry
 * different meanings depending on the object's type tag.
 *
 *   Bits 0x01-0x03  RAY_SYM vectors:  sym index width (RAY_SYM_W8/W16/W32/W64)
 *   Bits 0x01-0x10  function atoms (RAY_ATOM_UNARY/BINARY/VARY): RAY_FN_* flags
 *   Bits 0x01-0x02  RAY_LIST atoms:   RAY_ATTR_VECTOR / RAY_ATTR_DICT
 *   Bit  0x10       vectors:         RAY_ATTR_SLICE
 *   Bit  0x20       vectors:         RAY_ATTR_NULLMAP_EXT
 *   Bit  0x20       RAY_ATOM_SYM:     RAY_ATTR_NAME (variable reference)
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

/* ===== Morsel Constants ===== */

#define RAY_MORSEL_ELEMS  1024

/* ===== Slab Cache Constants ===== */

#define RAY_SLAB_CACHE_SIZE  64
#define RAY_SLAB_ORDERS      5

/* ===== Heap Allocator Constants ===== */

#define RAY_ORDER_MIN  6
#define RAY_ORDER_MAX  30

/* ===== Parallel Threshold ===== */

#define RAY_PARALLEL_THRESHOLD  (64 * RAY_MORSEL_ELEMS)
#define RAY_DISPATCH_MORSELS    8

/* Radix-partitioned hash join tuning.
 * L2_TARGET: per-partition HT working set limit (tuned for L1d/L2).     */
#define RAY_JOIN_L2_TARGET   (256 * 1024)   /* target partition HT size in bytes */
#define RAY_JOIN_MIN_RADIX   2              /* min radix bits (4 partitions)   */
#define RAY_JOIN_MAX_RADIX   14             /* max radix bits (16K partitions) */

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

typedef union RAY_ALIGN(32) ray_t {
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
        _Atomic(uint32_t) rc; /* reference count (0=free) */
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

/* ===== Accessor Macros ===== */

#define ray_type(v)       ((v)->type)
#define ray_is_atom(v)    ((v)->type < 0)
#define ray_is_vec(v)     ((v)->type > 0)
#define ray_len(v)        ((v)->len)
static inline void* ray_data_fn(ray_t* v) {
    return RAY_ASSUME_ALIGNED((void*)v->data, 32);
}
#define ray_data(v)       ray_data_fn(v)
#define ray_elem_size(t)  (ray_type_sizes[(t)])

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

/* Determine optimal SYM width for a given dictionary size */
static inline uint8_t ray_sym_dict_width(int64_t dict_size) {
    if (dict_size <= 255)        return RAY_SYM_W8;
    if (dict_size <= 65535)      return RAY_SYM_W16;
    if (dict_size <= 4294967295) return RAY_SYM_W32;
    return RAY_SYM_W64;
}

/* ===== Operation Graph ===== */

/* Opcodes — Sources */
#define OP_SCAN          1
#define OP_CONST         2

/* Opcodes — Unary element-wise (fuseable) */
#define OP_NEG          10
#define OP_ABS          11
#define OP_NOT          12
#define OP_SQRT         13
#define OP_LOG          14
#define OP_EXP          15
#define OP_CEIL         16
#define OP_FLOOR        17
#define OP_ISNULL       18
#define OP_CAST         19

/* Opcodes — Binary element-wise (fuseable) */
#define OP_ADD          20
#define OP_SUB          21
#define OP_MUL          22
#define OP_DIV          23
#define OP_MOD          24
#define OP_EQ           25
#define OP_NE           26
#define OP_LT           27
#define OP_LE           28
#define OP_GT           29
#define OP_GE           30
#define OP_AND          31
#define OP_OR           32
#define OP_MIN2         33
#define OP_MAX2         34
#define OP_IF           35
#define OP_LIKE         36
#define OP_UPPER        37
#define OP_LOWER        38
#define OP_STRLEN       39
#define OP_SUBSTR       40
#define OP_REPLACE      41
#define OP_TRIM         42
#define OP_CONCAT       43
#define OP_EXTRACT      45
#define OP_DATE_TRUNC   46

/* EXTRACT / DATE_TRUNC field identifiers */
#define RAY_EXTRACT_YEAR    0
#define RAY_EXTRACT_MONTH   1
#define RAY_EXTRACT_DAY     2
#define RAY_EXTRACT_HOUR    3
#define RAY_EXTRACT_MINUTE  4
#define RAY_EXTRACT_SECOND  5
#define RAY_EXTRACT_DOW     6
#define RAY_EXTRACT_DOY     7
#define RAY_EXTRACT_EPOCH   8

/* Opcodes — Reductions (pipeline breakers) */
#define OP_SUM          50
#define OP_PROD         51
#define OP_MIN          52
#define OP_MAX          53
#define OP_COUNT        54
#define OP_AVG          55
#define OP_FIRST        56
#define OP_LAST         57
#define OP_COUNT_DISTINCT 58
#define OP_STDDEV       59

/* Opcodes — Structural (pipeline breakers) */
#define OP_FILTER       60
#define OP_SORT         61
#define OP_GROUP        62
#define OP_JOIN         63
#define OP_WINDOW_JOIN  64
#define OP_SELECT       66
#define OP_HEAD         67
#define OP_TAIL         68

/* Opcodes — Window */
#define OP_WINDOW       72

/* Opcodes — Statistical aggregates */
#define OP_STDDEV_POP   73
#define OP_VAR          74
#define OP_VAR_POP      75
#define OP_ILIKE        76

/* Opcodes — Graph */
#define OP_EXPAND        80   /* 1-hop CSR neighbor expansion       */
#define OP_VAR_EXPAND    81   /* variable-length BFS/DFS            */
#define OP_SHORTEST_PATH 82   /* BFS shortest path                  */
#define OP_WCO_JOIN      83   /* worst-case optimal join (LFTJ)     */
#define OP_PAGERANK        84   /* iterative PageRank                 */
#define OP_CONNECTED_COMP  85   /* connected components (label prop)  */
#define OP_DIJKSTRA        86   /* weighted shortest path (Dijkstra)  */
#define OP_LOUVAIN         87   /* community detection (Louvain)      */

/* Opcodes — Graph algorithms (batch 1) */
#define OP_DEGREE_CENT     92   /* degree centrality                  */
#define OP_TOPSORT         93   /* topological sort (Kahn's)          */
#define OP_DFS             94   /* depth-first search traversal       */

/* Opcodes — Graph algorithms (batch 2) */
#define OP_ASTAR           95   /* A* shortest path (coordinate heuristic) */
#define OP_K_SHORTEST      96   /* Yen's k-shortest paths                 */
#define OP_CLUSTER_COEFF   97   /* clustering coefficients                */
#define OP_RANDOM_WALK     98   /* random walk traversal                  */
#define OP_BETWEENNESS     99   /* betweenness centrality (Brandes)       */
#define OP_CLOSENESS      100   /* closeness centrality                   */
#define OP_MST            101   /* minimum spanning forest (Kruskal)      */

/* Opcodes — Vector similarity */
#define OP_COSINE_SIM      88   /* cosine similarity between embeddings   */
#define OP_EUCLIDEAN_DIST  89   /* euclidean distance between embeddings  */
#define OP_KNN             90   /* brute-force K nearest neighbors        */
#define OP_HNSW_KNN        91   /* HNSW approximate K nearest neighbors   */

/* Opcodes — Misc */
#define OP_ALIAS        70
#define OP_MATERIALIZE  71

/* Window function kinds (stored in func_kinds[]) */
#define RAY_WIN_ROW_NUMBER    0
#define RAY_WIN_RANK          1
#define RAY_WIN_DENSE_RANK    2
#define RAY_WIN_NTILE         3
#define RAY_WIN_SUM           4
#define RAY_WIN_AVG           5
#define RAY_WIN_MIN           6
#define RAY_WIN_MAX           7
#define RAY_WIN_COUNT         8
#define RAY_WIN_LAG           9
#define RAY_WIN_LEAD         10
#define RAY_WIN_FIRST_VALUE  11
#define RAY_WIN_LAST_VALUE   12
#define RAY_WIN_NTH_VALUE    13

/* Frame types */
#define RAY_FRAME_ROWS    0
#define RAY_FRAME_RANGE   1

/* Frame bounds */
#define RAY_BOUND_UNBOUNDED_PRECEDING  0
#define RAY_BOUND_N_PRECEDING          1
#define RAY_BOUND_CURRENT_ROW          2
#define RAY_BOUND_N_FOLLOWING          3
#define RAY_BOUND_UNBOUNDED_FOLLOWING  4

/* Op flags */
#define OP_FLAG_FUSED        0x01
#define OP_FLAG_DEAD         0x02

/* Operation node (32 bytes, fits one cache line) */
typedef struct ray_op {
    uint16_t       opcode;     /* OP_ADD, OP_SCAN, OP_FILTER, etc. */
    uint8_t        arity;      /* 0, 1, or 2 */
    uint8_t        flags;      /* FUSED, DEAD */
    int8_t         out_type;   /* inferred output type */
    uint8_t        pad[3];
    uint32_t       id;         /* unique node ID */
    uint32_t       est_rows;   /* estimated row count */
    struct ray_op*  inputs[2];  /* NULL if unused */
} ray_op_t;

/* Extended operation node for N-ary ops (heap-allocated, variable size) */
typedef struct ray_op_ext {
    ray_op_t base;              /* 32 bytes standard node */
    union {
        ray_t*   literal;       /* OP_CONST: inline literal value */
        int64_t sym;           /* OP_SCAN: column name symbol ID */
        struct {               /* OP_GROUP: group-by specification */
            ray_op_t**  keys;
            uint8_t    n_keys;
            uint8_t    n_aggs;
            uint16_t*  agg_ops;
            ray_op_t**  agg_ins;
        };
        struct {               /* OP_SORT: multi-column sort */
            ray_op_t**  columns;
            uint8_t*   desc;
            uint8_t*   nulls_first; /* 1=nulls first, 0=nulls last */
            uint8_t    n_cols;
        } sort;
        struct {               /* OP_JOIN: join specification */
            ray_op_t**  left_keys;
            ray_op_t**  right_keys;
            uint8_t    n_join_keys;
            uint8_t    join_type;  /* 0=inner, 1=left, 2=full */
        } join;
        struct {               /* OP_WINDOW_JOIN: ASOF join */
            ray_op_t*   time_key;      /* time/ordered key column */
            ray_op_t**  eq_keys;       /* equality partition keys */
            uint8_t    n_eq_keys;     /* number of equality keys */
            uint8_t    join_type;     /* 0=inner, 1=left outer */
        } asof;
        struct {               /* OP_WINDOW: window functions */
            ray_op_t**  part_keys;
            ray_op_t**  order_keys;
            uint8_t*   order_descs;
            ray_op_t**  func_inputs;
            uint8_t*   func_kinds;    /* RAY_WIN_ROW_NUMBER etc. */
            int64_t*   func_params;   /* NTILE(n), LAG offset, etc. */
            uint8_t    n_part_keys;
            uint8_t    n_order_keys;
            uint8_t    n_funcs;
            uint8_t    frame_type;    /* RAY_FRAME_ROWS / RAY_FRAME_RANGE */
            uint8_t    frame_start;   /* RAY_BOUND_* */
            uint8_t    frame_end;     /* RAY_BOUND_* */
            int64_t    frame_start_n;
            int64_t    frame_end_n;
        } window;
        struct {  /* OP_EXPAND / OP_VAR_EXPAND / OP_SHORTEST_PATH / graph algos */
            void*     rel;            /* ray_rel_t* (opaque to public header) */
            void*     sip_sel;        /* ray_t* RAY_SEL bitmap for SIP source-side skip */
            uint8_t   direction;      /* 0=fwd, 1=rev, 2=both */
            uint8_t   min_depth;
            uint8_t   max_depth;
            uint8_t   path_tracking;
            uint8_t   factorized;     /* 1 = emit factorized output (fvec) */
            uint16_t  max_iter;       /* PageRank/Louvain iterations  */
            double    damping;        /* PageRank damping factor      */
            int64_t   weight_col_sym; /* Dijkstra/Astar/Yen weight column   */
            int64_t   coord_col_syms[2]; /* A*: lat/lon property column names */
            void*     node_props;       /* ray_t* node property table (A*: coords) */
        } graph;
        struct {  /* OP_WCO_JOIN */
            void**    rels;           /* ray_rel_t** array */
            uint8_t   n_rels;
            uint8_t   n_vars;
        } wco;
        struct {  /* OP_COSINE_SIM / OP_EUCLIDEAN_DIST / OP_KNN */
            float*    query_vec;      /* query embedding (caller-owned, must outlive graph) */
            int32_t   dim;            /* embedding dimension */
            int64_t   k;              /* top-K for KNN */
        } vector;
        struct {  /* OP_HNSW_KNN */
            void*     hnsw_idx;       /* ray_hnsw_t* (opaque, must outlive graph) */
            float*    query_vec;
            int32_t   dim;
            int64_t   k;
            int32_t   ef_search;
        } hnsw;
    };
} ray_op_ext_t;

/* Operation graph */
typedef struct ray_graph {
    ray_op_t*       nodes;       /* array of op nodes (malloc'd) */
    uint32_t       node_count;  /* number of nodes */
    uint32_t       node_cap;    /* allocated capacity */
    ray_t*          table;       /* bound table (provides columns for OP_SCAN) */
    ray_t**         tables;      /* table registry (indexed by table_id) */
    uint16_t       n_tables;    /* number of registered tables */
    ray_op_ext_t**  ext_nodes;   /* tracked extended nodes for cleanup */
    uint32_t       ext_count;   /* number of extended nodes */
    uint32_t       ext_cap;     /* capacity of ext_nodes array */
    ray_t*          selection;   /* RAY_SEL bitmap — lazy filter (NULL = all pass) */
} ray_graph_t;

/* ===== Morsel Iterator ===== */

typedef struct {
    ray_t*    vec;          /* source vector */
    int64_t  offset;       /* current position (element index) */
    int64_t  len;          /* total length of vector */
    uint32_t elem_size;    /* bytes per element */
    int64_t  morsel_len;   /* elements in current morsel (<=RAY_MORSEL_ELEMS) */
    void*    morsel_ptr;   /* pointer to current morsel data */
    uint8_t* null_bits;    /* current morsel null bitmap (or NULL) */
} ray_morsel_t;

/* ===== Selection Bitmap (RAY_SEL) ===== */

/* Segment flags — one per morsel (RAY_MORSEL_ELEMS rows) */
#define RAY_SEL_NONE  0   /* all bits 0 — skip entire morsel           */
#define RAY_SEL_ALL   1   /* all bits 1 — process without bitmap check */
#define RAY_SEL_MIX   2   /* mixed bits — must check per-row           */

/* Words per morsel segment: 1024 rows / 64 bits = 16 uint64_t */
#define RAY_SEL_WORDS_PER_SEG  (RAY_MORSEL_ELEMS / 64)

/* Inline metadata at ray_data(sel) */
typedef struct {
    int64_t   total_pass;  /* total passing rows                      */
    uint32_t  n_segs;      /* ceil(nrows / RAY_MORSEL_ELEMS)           */
    uint32_t  _pad;
} ray_sel_meta_t;

/*
 * RAY_SEL block layout (ray_data offset 0):
 *
 *   ray_sel_meta_t  meta        (16 bytes)
 *   uint8_t        seg_flags[] (n_segs, padded to 8-byte alignment)
 *   uint16_t       seg_popcnt[](n_segs, padded to 8-byte alignment)
 *   uint64_t       bits[]      (ceil(nrows/64) words)
 */

static inline ray_sel_meta_t* ray_sel_meta(ray_t* s) {
    return (ray_sel_meta_t*)ray_data(s);
}
static inline uint8_t* ray_sel_flags(ray_t* s) {
    return (uint8_t*)ray_data(s) + sizeof(ray_sel_meta_t);
}
static inline uint16_t* ray_sel_popcnt(ray_t* s) {
    uint32_t n = ray_sel_meta(s)->n_segs;
    return (uint16_t*)(ray_sel_flags(s) + ((n + 7u) & ~7u));
}
static inline uint64_t* ray_sel_bits(ray_t* s) {
    uint32_t n = ray_sel_meta(s)->n_segs;
    uint16_t* pc = ray_sel_popcnt(s);
    return (uint64_t*)(pc + ((n + 3u) & ~3u));
}

/* Bit ops */
#define RAY_SEL_BIT_TEST(bits, r)  ((bits)[(r) >> 6] & (1ULL << ((r) & 63)))
#define RAY_SEL_BIT_SET(bits, r)   ((bits)[(r) >> 6] |= (1ULL << ((r) & 63)))
#define RAY_SEL_BIT_CLR(bits, r)   ((bits)[(r) >> 6] &= ~(1ULL << ((r) & 63)))

/* ===== Executor Pipeline ===== */

typedef struct ray_pipe {
    ray_op_t*          op;            /* operation node */
    struct ray_pipe*   inputs[2];     /* upstream pipes */
    ray_morsel_t       state;         /* current morsel state */
    ray_t*             materialized;  /* materialized intermediate (or NULL) */
    int               spill_fd;      /* file descriptor for spill (-1 if none) */
} ray_pipe_t;

/* ===== Memory Statistics ===== */

typedef struct {
    size_t alloc_count;      /* ray_alloc calls */
    size_t free_count;       /* ray_free calls */
    size_t bytes_allocated;  /* currently allocated */
    size_t peak_bytes;       /* high-water mark */
    size_t slab_hits;        /* slab cache hits */
    size_t direct_count;     /* active direct mmaps */
    size_t direct_bytes;     /* bytes in direct mmaps */
    size_t sys_current;      /* sys allocator: current mmap'd bytes */
    size_t sys_peak;         /* sys allocator: peak mmap'd bytes */
} ray_mem_stats_t;

/* ===== Forward Declarations (internal types) ===== */

typedef struct ray_heap      ray_heap_t;
typedef struct ray_sym_table ray_sym_table_t;
typedef struct ray_sym_map   ray_sym_map_t;
typedef struct ray_pool      ray_pool_t;
typedef struct ray_task      ray_task_t;
typedef struct ray_dispatch  ray_dispatch_t;
typedef struct ray_csr       ray_csr_t;
typedef struct ray_rel       ray_rel_t;
typedef struct ray_hnsw      ray_hnsw_t;

/* ===== Thread Types ===== */

#if defined(_WIN32)
  typedef void* ray_thread_t;
#else
  typedef unsigned long ray_thread_t;
#endif

typedef void (*ray_thread_fn)(void* arg);

/* ===== Platform API ===== */

void* ray_vm_alloc(size_t size);
void  ray_vm_free(void* ptr, size_t size);
void* ray_vm_map_file(const char* path, size_t* out_size);
void  ray_vm_unmap_file(void* ptr, size_t size);
void  ray_vm_advise_seq(void* ptr, size_t size);
void  ray_vm_advise_willneed(void* ptr, size_t size);
void  ray_vm_release(void* ptr, size_t size);
void* ray_vm_alloc_aligned(size_t size, size_t alignment);

/* ===== Threading API ===== */

ray_err_t ray_thread_create(ray_thread_t* t, ray_thread_fn fn, void* arg);
ray_err_t ray_thread_join(ray_thread_t t);
uint32_t ray_thread_count(void);

void ray_parallel_begin(void);
void ray_parallel_end(void);
extern _Atomic(uint32_t) ray_parallel_flag;

/* Reclaim fully-free pools by munmapping their regions. Called at
 * control points (e.g. between queries, end of parallel sections). */
void ray_heap_gc(void);

/* Release physical pages for large free blocks (madvise DONTNEED).
 * Explicit opt-in — NOT called automatically by ray_heap_gc(). Use
 * after long idle periods to reduce RSS. */
void ray_heap_release_pages(void);

/* ===== Memory Allocator API ===== */

ray_t*    ray_alloc(size_t data_size);
/* NOTE: ray_free supports cross-thread free via foreign_blocks list.
 * Blocks freed from a non-owning thread are deferred and coalesced
 * when the owning heap flushes foreign blocks. */
void     ray_free(ray_t* v);
ray_t*    ray_alloc_copy(ray_t* v);
ray_t*    ray_scratch_alloc(size_t data_size);
ray_t*    ray_scratch_realloc(ray_t* v, size_t new_data_size);

void     ray_heap_init(void);
void     ray_heap_destroy(void);
void     ray_heap_merge(ray_heap_t* src);
void     ray_heap_flush_foreign(void);
void     ray_heap_push_pending(ray_heap_t* heap);
void     ray_heap_drain_pending(void);

uint8_t  ray_order_for_size(size_t data_size);
void     ray_mem_stats(ray_mem_stats_t* out);

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

/* ===== Selection API ===== */

ray_t* ray_sel_new(int64_t nrows);              /* all-zero (no rows pass)       */
ray_t* ray_sel_from_pred(ray_t* bool_vec);       /* convert RAY_BOOL vec → RAY_SEL  */
ray_t* ray_sel_and(ray_t* a, ray_t* b);           /* AND two selections            */
void  ray_sel_recompute(ray_t* sel);             /* rebuild seg_flags + popcounts */

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

/* ===== Morsel Iterator API ===== */

void ray_morsel_init(ray_morsel_t* m, ray_t* vec);
void ray_morsel_init_range(ray_morsel_t* m, ray_t* vec, int64_t start, int64_t end);
bool ray_morsel_next(ray_morsel_t* m);

/* ===== Operation Graph API ===== */

ray_graph_t* ray_graph_new(ray_t* tbl);
void        ray_graph_free(ray_graph_t* g);

/* Source ops */
ray_op_t* ray_scan(ray_graph_t* g, const char* col_name);
ray_op_t* ray_const_f64(ray_graph_t* g, double val);
ray_op_t* ray_const_i64(ray_graph_t* g, int64_t val);
ray_op_t* ray_const_bool(ray_graph_t* g, bool val);
ray_op_t* ray_const_str(ray_graph_t* g, const char* s, size_t len);
ray_op_t* ray_const_vec(ray_graph_t* g, ray_t* vec);
ray_op_t* ray_const_table(ray_graph_t* g, ray_t* table);

/* Unary element-wise ops */
ray_op_t* ray_neg(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_abs(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_not(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_sqrt_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_log_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_exp_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_ceil_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_floor_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_isnull(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_cast(ray_graph_t* g, ray_op_t* a, int8_t target_type);

/* Binary element-wise ops */
ray_op_t* ray_add(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_sub(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_mul(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_div(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_mod(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_eq(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_ne(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_lt(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_le(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_gt(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_ge(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_and(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_or(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_min2(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_max2(ray_graph_t* g, ray_op_t* a, ray_op_t* b);
ray_op_t* ray_if(ray_graph_t* g, ray_op_t* cond, ray_op_t* then_val, ray_op_t* else_val);
ray_op_t* ray_like(ray_graph_t* g, ray_op_t* input, ray_op_t* pattern);
ray_op_t* ray_ilike(ray_graph_t* g, ray_op_t* input, ray_op_t* pattern);
ray_op_t* ray_upper(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_lower(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_strlen(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_substr(ray_graph_t* g, ray_op_t* str, ray_op_t* start, ray_op_t* len);
ray_op_t* ray_replace(ray_graph_t* g, ray_op_t* str, ray_op_t* from, ray_op_t* to);
ray_op_t* ray_trim_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_concat(ray_graph_t* g, ray_op_t** args, int n);

/* Date/time extraction and truncation */
ray_op_t* ray_extract(ray_graph_t* g, ray_op_t* col, int64_t field);
ray_op_t* ray_date_trunc(ray_graph_t* g, ray_op_t* col, int64_t field);

/* Reduction ops */
ray_op_t* ray_sum(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_prod(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_min_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_max_op(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_count(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_avg(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_first(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_last(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_count_distinct(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_stddev(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_stddev_pop(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_var(ray_graph_t* g, ray_op_t* a);
ray_op_t* ray_var_pop(ray_graph_t* g, ray_op_t* a);

/* Structural ops */
ray_op_t* ray_filter(ray_graph_t* g, ray_op_t* input, ray_op_t* predicate);
ray_op_t* ray_sort_op(ray_graph_t* g, ray_op_t* table_node,
                     ray_op_t** keys, uint8_t* descs, uint8_t* nulls_first,
                     uint8_t n_cols);
ray_op_t* ray_group(ray_graph_t* g, ray_op_t** keys, uint8_t n_keys,
                   uint16_t* agg_ops, ray_op_t** agg_ins, uint8_t n_aggs);
ray_op_t* ray_distinct(ray_graph_t* g, ray_op_t** keys, uint8_t n_keys);
ray_op_t* ray_join(ray_graph_t* g,
                  ray_op_t* left_table, ray_op_t** left_keys,
                  ray_op_t* right_table, ray_op_t** right_keys,
                  uint8_t n_keys, uint8_t join_type);
ray_op_t* ray_asof_join(ray_graph_t* g,
                       ray_op_t* left_table, ray_op_t* right_table,
                       ray_op_t* time_key,
                       ray_op_t** eq_keys, uint8_t n_eq_keys,
                       uint8_t join_type);
ray_op_t* ray_window_op(ray_graph_t* g, ray_op_t* table_node,
                       ray_op_t** part_keys, uint8_t n_part,
                       ray_op_t** order_keys, uint8_t* order_descs, uint8_t n_order,
                       uint8_t* func_kinds, ray_op_t** func_inputs,
                       int64_t* func_params, uint8_t n_funcs,
                       uint8_t frame_type, uint8_t frame_start, uint8_t frame_end,
                       int64_t frame_start_n, int64_t frame_end_n);
ray_op_t* ray_select(ray_graph_t* g, ray_op_t* input,
                    ray_op_t** cols, uint8_t n_cols);
ray_op_t* ray_head(ray_graph_t* g, ray_op_t* input, int64_t n);
ray_op_t* ray_tail(ray_graph_t* g, ray_op_t* input, int64_t n);
ray_op_t* ray_alias(ray_graph_t* g, ray_op_t* input, const char* name);
ray_op_t* ray_materialize(ray_graph_t* g, ray_op_t* input);

/* ===== Graph Ops ===== */

/* Multi-table support */
uint16_t ray_graph_add_table(ray_graph_t* g, ray_t* table);
ray_op_t* ray_scan_table(ray_graph_t* g, uint16_t table_id, const char* col_name);

/* Graph traversal */
ray_op_t* ray_expand(ray_graph_t* g, ray_op_t* src_nodes,
                    ray_rel_t* rel, uint8_t direction);
ray_op_t* ray_var_expand(ray_graph_t* g, ray_op_t* start_nodes,
                        ray_rel_t* rel, uint8_t direction,
                        uint8_t min_depth, uint8_t max_depth,
                        bool track_path);
ray_op_t* ray_shortest_path(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                           ray_rel_t* rel, uint8_t max_depth);
ray_op_t* ray_wco_join(ray_graph_t* g,
                      ray_rel_t** rels, uint8_t n_rels,
                      uint8_t n_vars);

/* Graph algorithms */
ray_op_t* ray_pagerank(ray_graph_t* g, ray_rel_t* rel,
                      uint16_t max_iter, double damping);
ray_op_t* ray_connected_comp(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_dijkstra(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                      ray_rel_t* rel, const char* weight_col,
                      uint8_t max_depth);
ray_op_t* ray_louvain(ray_graph_t* g, ray_rel_t* rel,
                     uint16_t max_iter);
ray_op_t* ray_degree_cent(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_topsort(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_dfs(ray_graph_t* g, ray_op_t* src, ray_rel_t* rel, uint8_t max_depth);
ray_op_t* ray_astar(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                  ray_rel_t* rel, const char* weight_col,
                  const char* lat_col, const char* lon_col,
                  ray_t* node_props, uint8_t max_depth);
ray_op_t* ray_k_shortest(ray_graph_t* g, ray_op_t* src, ray_op_t* dst,
                       ray_rel_t* rel, const char* weight_col, uint16_t k);
ray_op_t* ray_cluster_coeff(ray_graph_t* g, ray_rel_t* rel);
ray_op_t* ray_random_walk(ray_graph_t* g, ray_op_t* src, ray_rel_t* rel,
                        uint16_t walk_length);
ray_op_t* ray_betweenness(ray_graph_t* g, ray_rel_t* rel, uint16_t sample_size);
ray_op_t* ray_closeness(ray_graph_t* g, ray_rel_t* rel, uint16_t sample_size);
ray_op_t* ray_mst(ray_graph_t* g, ray_rel_t* rel, const char* weight_col);

/* Vector similarity ops */
ray_op_t* ray_cosine_sim(ray_graph_t* g, ray_op_t* emb_col,
                        const float* query_vec, int32_t dim);
ray_op_t* ray_euclidean_dist(ray_graph_t* g, ray_op_t* emb_col,
                            const float* query_vec, int32_t dim);
ray_op_t* ray_knn(ray_graph_t* g, ray_op_t* emb_col,
                 const float* query_vec, int32_t dim, int64_t k);

/* HNSW-accelerated KNN (uses pre-built index instead of brute-force) */
ray_op_t* ray_hnsw_knn(ray_graph_t* g, ray_hnsw_t* idx,
                       const float* query_vec, int32_t dim,
                       int64_t k, int32_t ef_search);

/* CSR / Relationship API */
ray_rel_t* ray_rel_build(ray_t* from_table, const char* fk_col,
                         int64_t n_target_nodes, bool sort_targets);
ray_rel_t* ray_rel_from_edges(ray_t* edge_table,
                             const char* src_col, const char* dst_col,
                             int64_t n_src_nodes, int64_t n_dst_nodes,
                             bool sort_targets);
ray_err_t  ray_rel_save(ray_rel_t* rel, const char* dir);
ray_rel_t* ray_rel_load(const char* dir);
ray_rel_t* ray_rel_mmap(const char* dir);
void      ray_rel_set_props(ray_rel_t* rel, ray_t* props);
void      ray_rel_free(ray_rel_t* rel);
const int64_t* ray_rel_neighbors(ray_rel_t* rel, int64_t node,
                                uint8_t direction, int64_t* out_count);
int64_t   ray_rel_n_nodes(ray_rel_t* rel, uint8_t direction);

/* ===== Optimizer API ===== */

ray_op_t* ray_optimize(ray_graph_t* g, ray_op_t* root);
void     ray_fuse_pass(ray_graph_t* g, ray_op_t* root);

/* ===== Plan Printer ===== */

/* Print human-readable query plan rooted at `root` to `out`.
 * `out` is a FILE* (caller must include <stdio.h>).
 * If out is NULL, prints to stderr. */
void ray_graph_dump(ray_graph_t* g, ray_op_t* root, void* out);

/* ===== Executor API ===== */

ray_t* ray_execute(ray_graph_t* g, ray_op_t* root);

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


/* ===== Embedding Column Helpers ===== */

/* An embedding column is a RAY_F32 vector of length N*D where D is the
 * embedding dimension.  D is stored in a separate I32 atom that the
 * caller keeps alongside the column.  Access helpers: */

/* Create an embedding column for N rows of D-dimensional vectors. */
ray_t* ray_embedding_new(int64_t nrows, int32_t dim);

/* Get the raw float pointer for row `row` (0-indexed). */
static inline float* ray_embedding_row(ray_t* col, int32_t dim, int64_t row) {
    return (float*)ray_data(col) + row * dim;
}

/* Set one row's embedding from a float array. */
static inline void ray_embedding_set(ray_t* col, int32_t dim,
                                     int64_t row, const float* vec) {
    float* dst = ray_embedding_row(col, dim, row);
    memcpy(dst, vec, (size_t)dim * sizeof(float));
}

/* Number of rows in an embedding column. */
static inline int64_t ray_embedding_nrows(ray_t* col, int32_t dim) {
    return col->len / dim;
}

/* ===== Pool / Parallel API ===== */

ray_err_t ray_pool_init(uint32_t n_workers);
void     ray_pool_destroy(void);
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

#ifdef __cplusplus
}
#endif

#endif /* RAY_H */
