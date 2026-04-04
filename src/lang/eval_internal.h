/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Shared helpers for eval.c split — included by arith.c, cmp.c, agg.c, etc.
 *   Small hot-path helpers are static inline; larger functions that remain in
 *   eval.c are declared extern.
 */

#ifndef RAY_EVAL_INTERNAL_H
#define RAY_EVAL_INTERNAL_H

#include "lang/eval.h"
#include "lang/format.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════
 * Atom constructors
 * ══════════════════════════════════════════ */

static inline ray_t* make_i64(int64_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_I64;
    obj->i64 = v;
    return obj;
}

static inline ray_t* make_f64(double v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_F64;
    obj->f64 = v;
    return obj;
}

static inline ray_t* make_i16(int16_t v) {
    return ray_i16(v);
}

static inline ray_t* make_i32(int32_t v) {
    return ray_i32(v);
}

static inline ray_t* make_u8(uint8_t v) {
    return ray_u8(v);
}

static inline ray_t* make_bool(uint8_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_BOOL;
    obj->b8 = v;
    return obj;
}

/* ══════════════════════════════════════════
 * Type checks and numeric extraction
 * ══════════════════════════════════════════ */

/* Helpers to extract numeric value as double */
static inline int is_numeric(ray_t* x) {
    return x->type == -RAY_I64 || x->type == -RAY_F64 ||
           x->type == -RAY_I16 || x->type == -RAY_I32 ||
           x->type == -RAY_U8  || x->type == -RAY_BOOL;
}

/* Check if an atom is a temporal type */
static inline int is_temporal(ray_t* x) {
    return x->type == -RAY_DATE || x->type == -RAY_TIME || x->type == -RAY_TIMESTAMP;
}

/* Convert temporal atom to nanoseconds for cross-temporal comparison.
 * DATE = days since epoch -> ns, TIME = ms since midnight -> ns, TIMESTAMP = ns */
static inline int64_t temporal_as_ns(ray_t* x) {
    if (x->type == -RAY_TIMESTAMP) return x->i64;
    if (x->type == -RAY_DATE)      return (int64_t)x->i32 * 86400000000000LL;
    if (x->type == -RAY_TIME)      return (int64_t)x->i32 * 1000000LL;
    return 0;
}

/* Extract integer value from any integer atom as int64_t */
static inline int64_t as_i64(ray_t* x) {
    if (x->type == -RAY_I64)  return x->i64;
    if (x->type == -RAY_I32)  return (int64_t)x->i32;
    if (x->type == -RAY_I16)  return (int64_t)x->i16;
    if (x->type == -RAY_U8)   return (int64_t)x->u8;
    return x->i64; /* fallback */
}

static inline double as_f64(ray_t* x) {
    if (x->type == -RAY_F64) return x->f64;
    if (x->type == -RAY_I64) return (double)x->i64;
    if (x->type == -RAY_I32) return (double)x->i32;
    if (x->type == -RAY_I16) return (double)x->i16;
    if (x->type == -RAY_U8)  return (double)x->u8;
    if (x->type == -RAY_STR && ray_str_len(x) == 1) return (double)(unsigned char)x->sdata[0];
    if (x->type == -RAY_BOOL) return (double)x->b8;
    if (x->type == -RAY_DATE || x->type == -RAY_TIME) return (double)x->i32;
    if (x->type == -RAY_TIMESTAMP) return (double)x->i64;
    return (double)x->i64;
}

static inline int is_float_op(ray_t* a, ray_t* b) {
    return a->type == -RAY_F64 || b->type == -RAY_F64;
}

/* ══════════════════════════════════════════
 * Null/type helpers
 * ══════════════════════════════════════════ */

/* Null sentinel checks */
static inline int is_null_atom(ray_t* x) {
    if (x->type == -RAY_I64)  return x->i64 == INT64_MIN;
    if (x->type == -RAY_I32)  return x->i32 == INT32_MIN;
    if (x->type == -RAY_I16)  return x->i16 == INT16_MIN;
    if (x->type == -RAY_F64)  return isnan(x->f64);
    return 0;
}

/* Return the null value for the promoted result type of two operands */
static inline ray_t* null_for_promoted(ray_t* a, ray_t* b) {
    /* If either is f64, or one is f64 and other is int, result is f64 null */
    if (a->type == -RAY_F64 || b->type == -RAY_F64)
        return make_f64(NAN);
    /* Promote: i16 < i32 < i64.  Result type is the wider of the two */
    if (a->type == -RAY_I64 || b->type == -RAY_I64)
        return make_i64(INT64_MIN);
    if (a->type == -RAY_I32 || b->type == -RAY_I32)
        return make_i32(INT32_MIN);
    if (a->type == -RAY_I16 || b->type == -RAY_I16)
        return make_i16(INT16_MIN);
    if (a->type == -RAY_U8 || b->type == -RAY_U8)
        return make_i64(INT64_MIN);
    return make_i64(INT64_MIN);
}

/* ══════════════════════════════════════════
 * Type promotion
 * ══════════════════════════════════════════ */

/* Determine the promoted integer result type for two numeric operands.
 * Returns atom type code (negative). */
static inline int8_t promote_int_type(ray_t* a, ray_t* b) {
    if (a->type == -RAY_I64 || b->type == -RAY_I64) return -RAY_I64;
    if (a->type == -RAY_I32 || b->type == -RAY_I32) return -RAY_I32;
    if (a->type == -RAY_U8 || b->type == -RAY_U8) {
        /* u8 op u8 -> u8, but u8 op i16 -> i16 etc */
        if (a->type == -RAY_U8 && b->type == -RAY_U8) return -RAY_U8;
        return (a->type == -RAY_I16 || b->type == -RAY_I16) ? -RAY_I16 : -RAY_I64;
    }
    if (a->type == -RAY_I16 || b->type == -RAY_I16) return -RAY_I16;
    return -RAY_I64;
}

/* Promote integer type following right-operand's type (K/q semantics for sub) */
static inline int8_t promote_int_type_right(ray_t* a, ray_t* b) {
    (void)a;
    int8_t bt = b->type;
    if (bt == -RAY_I32 || bt == -RAY_I16 || bt == -RAY_U8 || bt == -RAY_I64)
        return bt;
    int8_t at = a->type;
    if (at == -RAY_I32 || at == -RAY_I16 || at == -RAY_U8 || at == -RAY_I64)
        return at;
    return -RAY_I64;
}

/* Create a result atom of the given type from an int64_t value */
static inline ray_t* make_typed_int(int8_t atom_type, int64_t val) {
    switch (atom_type) {
    case -RAY_I16: return make_i16((int16_t)val);
    case -RAY_I32: return make_i32((int32_t)val);
    case -RAY_U8:  return make_u8((uint8_t)val);
    default:       return make_i64(val);
    }
}

/* ══════════════════════════════════════════
 * Truthiness
 * ══════════════════════════════════════════ */

/* Logical -- coerce to truthiness (0/nil/false = falsy, else truthy) */
static inline int is_truthy(ray_t* x) {
    if (x->type == -RAY_BOOL) return x->b8;
    if (x->type == -RAY_I64)  return x->i64 != 0;
    if (x->type == -RAY_F64)  return x->f64 != 0.0;
    return 1; /* non-null objects are truthy */
}

/* ══════════════════════════════════════════
 * Collection helpers
 * ══════════════════════════════════════════ */

static inline int is_list(ray_t* x) {
    return x && !RAY_IS_ERR(x) && x->type == RAY_LIST;
}

/* Check if x is a collection: boxed list OR typed vector */
static inline int is_collection(ray_t* x) {
    return x && !RAY_IS_ERR(x) && (x->type == RAY_LIST || ray_is_vec(x));
}

/* Extract the i-th element of a collection as a ray_t* atom.
 * For boxed lists, returns the stored pointer (no alloc).
 * For typed vectors, allocates a new atom.  Caller must release
 * atoms obtained from typed vectors (allocated == 1). */
static inline ray_t* collection_elem(ray_t* coll, int64_t i, int *allocated) {
    if (coll->type == RAY_LIST) {
        *allocated = 0;
        return ((ray_t**)ray_data(coll))[i];
    }
    *allocated = 1;
    switch (coll->type) {
        case RAY_I64:       return ray_i64(((int64_t*)ray_data(coll))[i]);
        case RAY_F64:       return ray_f64(((double*)ray_data(coll))[i]);
        case RAY_I32:       return ray_i32(((int32_t*)ray_data(coll))[i]);
        case RAY_I16:       return ray_i16(((int16_t*)ray_data(coll))[i]);
        case RAY_BOOL:      return ray_bool(((bool*)ray_data(coll))[i]);
        case RAY_SYM:       return ray_sym(((int64_t*)ray_data(coll))[i]);
        case RAY_U8:        return ray_u8(((uint8_t*)ray_data(coll))[i]);
        case RAY_DATE:      return ray_date((int64_t)((int32_t*)ray_data(coll))[i]);
        case RAY_TIME:      return ray_time((int64_t)((int32_t*)ray_data(coll))[i]);
        case RAY_TIMESTAMP: return ray_timestamp(((int64_t*)ray_data(coll))[i]);
        case RAY_GUID: {
            const uint8_t* gd = ((uint8_t*)ray_data(coll)) + i * 16;
            return ray_guid(gd);
        }
        /* RAY_CHAR removed -- char vectors no longer exist */
        case RAY_STR: {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(coll, i, &slen);
            return ray_str(sp ? sp : "", sp ? slen : 0);
        }
        default:            *allocated = 0; return ray_error("type", NULL);
    }
}

/* Extract a value from an atom for storage, handling cross-type casting.
 * Returns the value as int64_t (for integer/temporal types). */
static inline int64_t elem_as_i64(ray_t* elem) {
    if (elem->type == -RAY_I64 || elem->type == -RAY_TIMESTAMP ||
        elem->type == -RAY_DATE || elem->type == -RAY_TIME ||
        elem->type == -RAY_SYM) return elem->i64;
    if (elem->type == -RAY_I32)  return (int64_t)elem->i32;
    if (elem->type == -RAY_I16)  return (int64_t)elem->i16;
    if (elem->type == -RAY_U8)   return (int64_t)elem->u8;
    if (elem->type == -RAY_F64)  return (int64_t)elem->f64;
    return elem->i64;
}

/* Store a scalar result into a typed vector at position i.
 * Returns 0 on success, -1 if the element type doesn't match. */
static inline int store_typed_elem(ray_t* vec, int64_t i, ray_t* elem) {
    switch (vec->type) {
        case RAY_I64:       ((int64_t*)ray_data(vec))[i]  = elem_as_i64(elem); return 0;
        case RAY_F64:       ((double*)ray_data(vec))[i]    = (elem->type == -RAY_F64) ? elem->f64 : (double)elem_as_i64(elem); return 0;
        case RAY_I32:       ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_I16:       ((int16_t*)ray_data(vec))[i]   = (int16_t)elem_as_i64(elem); return 0;
        case RAY_BOOL:      ((bool*)ray_data(vec))[i]      = elem->b8;  return 0;
        case RAY_U8:        ((uint8_t*)ray_data(vec))[i]   = (uint8_t)elem_as_i64(elem); return 0;
        /* RAY_CHAR removed -- char vectors no longer exist */
        case RAY_DATE:      ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_TIME:      ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_TIMESTAMP: ((int64_t*)ray_data(vec))[i]   = elem_as_i64(elem); return 0;
        case RAY_SYM:       ((int64_t*)ray_data(vec))[i]   = elem->i64; return 0;
        case RAY_GUID:      if (elem->obj) memcpy(((uint8_t*)ray_data(vec)) + i * 16, ray_data(elem->obj), 16); return 0;
        default: return -1;
    }
}

/* ══════════════════════════════════════════
 * Extern forward declarations — larger functions that stay in eval.c
 * ══════════════════════════════════════════ */

ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right);
ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg);
ray_t* to_boxed_list(ray_t* x);
ray_t* unbox_vec_arg(ray_t* x, ray_t** _bx);
ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc);
ray_t* call_fn1(ray_t* fn, ray_t* arg);
ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b);
ray_t* gather_by_idx(ray_t* vec, int64_t* idx, int64_t n);
int    char_str_cmp(ray_t* a, ray_t* b, int *out);
int    is_comparable(ray_t* x);

/* Arithmetic builtins (formerly static in eval.c, now in arith.c) */
ray_t* ray_round_fn(ray_t* x);
ray_t* ray_floor_fn(ray_t* x);
ray_t* ray_ceil_fn(ray_t* x);

/* Convenience wrapper: atomic_map_binary with no DAG opcode */
static inline ray_t* atomic_map_binary(ray_binary_fn fn, ray_t* left, ray_t* right) {
    return atomic_map_binary_op(fn, 0, left, right);
}

#endif /* RAY_EVAL_INTERNAL_H */
