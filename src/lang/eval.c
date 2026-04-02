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

#include "lang/eval.h"
#include "lang/env.h"
#include "lang/nfo.h"
#include "lang/parse.h"
#include "core/types.h"
#include "io/csv.h"
#include "ops/ops.h"
#include "ops/pool.h"
#include "table/sym.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "app/format.h"
/* ray_error() is declared in <rayforce.h> (included via eval.h) */

#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Maximum recursion depth for ray_eval() to prevent stack overflow */
#define RAY_EVAL_MAX_DEPTH 512
_Thread_local static int eval_depth = 0;

/* Thread-local nfo for eval context — tracks source locations during evaluation */
static _Thread_local ray_t* g_eval_nfo = NULL;

/* Thread-local error trace — list of [span_i64, filename, fn_name, source] frames */
static _Thread_local ray_t* g_error_trace = NULL;

/* Interrupt flag — set by REPL signal handler, checked by eval/VM loops */
static volatile sig_atomic_t g_eval_interrupted = 0;

void ray_eval_request_interrupt(void) { g_eval_interrupted = 1; }
void ray_eval_clear_interrupt(void)   { g_eval_interrupted = 0; }
int  ray_eval_is_interrupted(void)    { return g_eval_interrupted != 0; }

ray_t* ray_eval_get_nfo(void) { return g_eval_nfo; }
void   ray_eval_set_nfo(ray_t* nfo) { g_eval_nfo = nfo; }

ray_t* ray_get_error_trace(void) { return g_error_trace; }
void   ray_clear_error_trace(void) {
    if (g_error_trace) { ray_release(g_error_trace); g_error_trace = NULL; }
}

/* ══════════════════════════════════════════
 * Arithmetic builtins
 * ══════════════════════════════════════════ */

static ray_t* make_i64(int64_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_I64;
    obj->i64 = v;
    return obj;
}

static ray_t* make_f64(double v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_F64;
    obj->f64 = v;
    return obj;
}

static ray_t* make_i16(int16_t v) {
    return ray_i16(v);
}

static ray_t* make_i32(int32_t v) {
    return ray_i32(v);
}

static ray_t* make_u8(uint8_t v) {
    return ray_u8(v);
}

static ray_t* make_bool(uint8_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_BOOL;
    obj->b8 = v;
    return obj;
}

/* Helpers to extract numeric value as double */
static int is_numeric(ray_t* x) {
    return x->type == -RAY_I64 || x->type == -RAY_F64 ||
           x->type == -RAY_I16 || x->type == -RAY_I32 ||
           x->type == -RAY_U8  || x->type == -RAY_BOOL;
}

/* Check if an atom is a temporal type */
static int is_temporal(ray_t* x) {
    return x->type == -RAY_DATE || x->type == -RAY_TIME || x->type == -RAY_TIMESTAMP;
}

/* Convert temporal atom to nanoseconds for cross-temporal comparison.
 * DATE = days since epoch → ns, TIME = ms since midnight → ns, TIMESTAMP = ns */
static int64_t temporal_as_ns(ray_t* x) {
    if (x->type == -RAY_TIMESTAMP) return x->i64;
    if (x->type == -RAY_DATE)      return (int64_t)x->i32 * 86400000000000LL;
    if (x->type == -RAY_TIME)      return (int64_t)x->i32 * 1000000LL;
    return 0;
}

/* Extract integer value from any integer atom as int64_t */
static int64_t as_i64(ray_t* x) {
    if (x->type == -RAY_I64)  return x->i64;
    if (x->type == -RAY_I32)  return (int64_t)x->i32;
    if (x->type == -RAY_I16)  return (int64_t)x->i16;
    if (x->type == -RAY_U8)   return (int64_t)x->u8;
    return x->i64; /* fallback */
}

static double as_f64(ray_t* x) {
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

static int is_float_op(ray_t* a, ray_t* b) {
    return a->type == -RAY_F64 || b->type == -RAY_F64;
}

/* Null sentinel checks */
static int is_null_atom(ray_t* x) {
    if (x->type == -RAY_I64)  return x->i64 == INT64_MIN;
    if (x->type == -RAY_I32)  return x->i32 == INT32_MIN;
    if (x->type == -RAY_I16)  return x->i16 == INT16_MIN;
    if (x->type == -RAY_F64)  return isnan(x->f64);
    return 0;
}

/* Return the null value for the promoted result type of two operands */
static ray_t* null_for_promoted(ray_t* a, ray_t* b) {
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

/* Determine the promoted integer result type for two numeric operands.
 * Returns atom type code (negative). */
static int8_t promote_int_type(ray_t* a, ray_t* b) {
    if (a->type == -RAY_I64 || b->type == -RAY_I64) return -RAY_I64;
    if (a->type == -RAY_I32 || b->type == -RAY_I32) return -RAY_I32;
    if (a->type == -RAY_U8 || b->type == -RAY_U8) {
        /* u8 op u8 → u8, but u8 op i16 → i16 etc */
        if (a->type == -RAY_U8 && b->type == -RAY_U8) return -RAY_U8;
        return (a->type == -RAY_I16 || b->type == -RAY_I16) ? -RAY_I16 : -RAY_I64;
    }
    if (a->type == -RAY_I16 || b->type == -RAY_I16) return -RAY_I16;
    return -RAY_I64;
}

/* Promote integer type following right-operand's type (K/q semantics for sub) */
static int8_t promote_int_type_right(ray_t* a, ray_t* b) {
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
static ray_t* make_typed_int(int8_t atom_type, int64_t val) {
    switch (atom_type) {
    case -RAY_I16: return make_i16((int16_t)val);
    case -RAY_I32: return make_i32((int32_t)val);
    case -RAY_U8:  return make_u8((uint8_t)val);
    default:       return make_i64(val);
    }
}

/* Binary arithmetic */
ray_t* ray_add_fn(ray_t* a, ray_t* b) {
    /* Temporal + integer arithmetic (only int types, not float) */
    if (is_temporal(a) && is_numeric(b) && b->type != -RAY_F64) {
        if (is_null_atom(b)) {
            /* null int + temporal → null of temporal type */
            if (a->type == -RAY_DATE)      return ray_date(INT32_MIN);
            if (a->type == -RAY_TIME)      return ray_time(INT32_MIN);
            if (a->type == -RAY_TIMESTAMP) return ray_timestamp(INT64_MIN);
        }
        int64_t v = as_i64(b);
        if (a->type == -RAY_DATE)      return ray_date(a->i64 + v);
        if (a->type == -RAY_TIME)      return ray_time(a->i64 + v);
        if (a->type == -RAY_TIMESTAMP) return ray_timestamp(a->i64 + v);
    }
    if (is_numeric(a) && a->type != -RAY_F64 && is_temporal(b)) {
        if (is_null_atom(a)) {
            if (b->type == -RAY_DATE)      return ray_date(INT32_MIN);
            if (b->type == -RAY_TIME)      return ray_time(INT32_MIN);
            if (b->type == -RAY_TIMESTAMP) return ray_timestamp(INT64_MIN);
        }
        int64_t v = as_i64(a);
        if (b->type == -RAY_DATE)      return ray_date(b->i64 + v);
        if (b->type == -RAY_TIME)      return ray_time(b->i64 + v);
        if (b->type == -RAY_TIMESTAMP) return ray_timestamp(b->i64 + v);
    }
    /* Reject float + temporal */
    if ((a->type == -RAY_F64 && is_temporal(b)) || (is_temporal(a) && b->type == -RAY_F64))
        return ray_error("type", NULL);
    /* Reject null_numeric + temporal (for null floats etc) */
    if (is_numeric(a) && is_null_atom(a) && is_temporal(b))
        return ray_error("type", NULL);
    if (is_temporal(a) && is_numeric(b) && is_null_atom(b))
        return ray_error("type", NULL);
    /* DATE + TIME → TIMESTAMP */
    if (a->type == -RAY_DATE && b->type == -RAY_TIME)
        return ray_timestamp(a->i64 * 86400000000000LL + b->i64 * 1000000LL);
    if (a->type == -RAY_TIME && b->type == -RAY_DATE)
        return ray_timestamp(b->i64 * 86400000000000LL + a->i64 * 1000000LL);
    /* TIME + TIME → TIME */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME)
        return ray_time(a->i64 + b->i64);
    /* TIME + TIMESTAMP → TIMESTAMP (add ms as ns) */
    if (a->type == -RAY_TIME && b->type == -RAY_TIMESTAMP)
        return ray_timestamp(b->i64 + a->i64 * 1000000LL);
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIME)
        return ray_timestamp(a->i64 + b->i64 * 1000000LL);

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot add %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    /* Null propagation */
    if (is_null_atom(a) || is_null_atom(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) return make_f64(as_f64(a) + as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, as_i64(a) + as_i64(b));
}

ray_t* ray_sub_fn(ray_t* a, ray_t* b) {
    /* DATE - int → DATE */
    if (a->type == -RAY_DATE && is_numeric(b)) {
        if (is_null_atom(b)) return ray_date(INT32_MIN);
        return ray_date(a->i64 - as_i64(b));
    }
    /* DATE - DATE → i32 (days difference) */
    if (a->type == -RAY_DATE && b->type == -RAY_DATE)
        return ray_i32((int32_t)(a->i64 - b->i64));
    /* DATE - TIME → TIMESTAMP */
    if (a->type == -RAY_DATE && b->type == -RAY_TIME)
        return ray_timestamp(a->i64 * 86400000000000LL - b->i64 * 1000000LL);
    /* TIME - int → TIME */
    if (a->type == -RAY_TIME && is_numeric(b)) {
        if (is_null_atom(b)) return ray_time(INT32_MIN);
        return ray_time(a->i64 - as_i64(b));
    }
    /* int - TIME → TIME (negative) */
    if (is_numeric(a) && b->type == -RAY_TIME) {
        if (is_null_atom(a)) return ray_time(INT32_MIN);
        return ray_time(as_i64(a) - b->i64);
    }
    /* TIME - TIME → TIME */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME)
        return ray_time(a->i64 - b->i64);
    /* TIMESTAMP - int → TIMESTAMP */
    if (a->type == -RAY_TIMESTAMP && is_numeric(b)) {
        if (is_null_atom(b)) return ray_timestamp(INT64_MIN);
        return ray_timestamp(a->i64 - as_i64(b));
    }
    /* TIMESTAMP - TIME → TIMESTAMP */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIME)
        return ray_timestamp(a->i64 - b->i64 * 1000000LL);
    /* TIMESTAMP - TIMESTAMP → int (nanos difference) */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIMESTAMP)
        return make_i64(a->i64 - b->i64);
    /* TIMESTAMP - DATE → error */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_DATE)
        return ray_error("type", NULL);

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot subtract %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    /* Null propagation */
    if (is_null_atom(a) || is_null_atom(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) {
        double r = as_f64(a) - as_f64(b);
        if (r == 0.0) r = 0.0; /* normalize -0.0 to +0.0 */
        return make_f64(r);
    }
    int8_t rt = promote_int_type_right(a, b);
    return make_typed_int(rt, as_i64(a) - as_i64(b));
}

ray_t* ray_mul_fn(ray_t* a, ray_t* b) {
    /* int * TIME → TIME, TIME * int → TIME */
    if (is_numeric(a) && b->type == -RAY_TIME) {
        if (is_null_atom(a)) return ray_time(INT32_MIN);
        return ray_time(as_i64(a) * b->i64);
    }
    if (a->type == -RAY_TIME && is_numeric(b)) {
        if (is_null_atom(b)) return ray_time(INT32_MIN);
        return ray_time(a->i64 * as_i64(b));
    }
    /* TIME * TIME → error */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME)
        return ray_error("type", NULL);

    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot multiply %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    /* Null propagation */
    if (is_null_atom(a) || is_null_atom(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) return make_f64(as_f64(a) * as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, as_i64(a) * as_i64(b));
}

ray_t* ray_div_fn(ray_t* a, ray_t* b) {
    /* Temporal / numeric → temporal (same type as left operand) */
    if (is_temporal(a) && is_numeric(b)) {
        if (is_null_atom(b)) {
            if (a->type == -RAY_TIME)      return ray_time(INT32_MIN);
            if (a->type == -RAY_DATE)      return ray_date(INT32_MIN);
            return ray_timestamp(INT64_MIN);
        }
        int64_t av = a->i64;
        if (av == INT32_MIN && (a->type == -RAY_TIME || a->type == -RAY_DATE))
            return (a->type == -RAY_TIME) ? ray_time(INT32_MIN) : ray_date(INT32_MIN);
        if (a->type == -RAY_TIMESTAMP && av == INT64_MIN) return ray_timestamp(INT64_MIN);
        if (is_float_op(a, b)) {
            double bv = as_f64(b);
            if (bv == 0.0 || isnan(bv)) {
                if (a->type == -RAY_TIME)      return ray_time(INT32_MIN);
                if (a->type == -RAY_DATE)      return ray_date(INT32_MIN);
                return ray_timestamp(INT64_MIN);
            }
            int64_t result = (int64_t)floor((double)av / bv);
            if (a->type == -RAY_TIME)      return ray_time(result);
            if (a->type == -RAY_DATE)      return ray_date(result);
            return ray_timestamp(result);
        }
        int64_t bv = as_i64(b);
        if (bv == 0) {
            if (a->type == -RAY_TIME)      return ray_time(INT32_MIN);
            if (a->type == -RAY_DATE)      return ray_date(INT32_MIN);
            return ray_timestamp(INT64_MIN);
        }
        int64_t q = av / bv;
        if ((av ^ bv) < 0 && q * bv != av) q--;
        if (a->type == -RAY_TIME)      return ray_time(q);
        if (a->type == -RAY_DATE)      return ray_date(q);
        return ray_timestamp(q);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot divide %s by %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    /* u8: unsigned byte division, no null sentinel — div by 0 returns 0 */
    if (a->type == -RAY_U8) {
        uint8_t bv = (uint8_t)as_i64(b);
        if (bv == 0 || is_null_atom(b)) return make_u8(0);
        if (is_null_atom(a)) return make_u8(0);
        return make_u8((uint8_t)((uint8_t)as_i64(a) / bv));
    }
    /* Null propagation — null operand → null matching left operand type */
    if (is_null_atom(a) || is_null_atom(b)) {
        /* Division null result type = left operand's null type */
        if (a->type == -RAY_F64) return make_f64(__builtin_nan(""));
        if (a->type == -RAY_I32 || a->type == -RAY_DATE || a->type == -RAY_TIME)
            return ray_i32(INT32_MIN);
        if (a->type == -RAY_I16) return ray_i16(INT16_MIN);
        return make_i64(INT64_MIN); /* default: 0Nl */
    }
    /* Integer (floor) division — always returns integer.
     * Float operands are converted to i64 via floor(a/b). */
    if (is_float_op(a, b)) {
        double bv = as_f64(b);
        if (bv == 0.0 || isnan(bv)) {
            /* Zero divisor: return null matching left operand's type */
            if (a->type == -RAY_F64) return make_f64(__builtin_nan(""));
            if (a->type == -RAY_I32) return ray_i32(INT32_MIN);
            if (a->type == -RAY_I16) return ray_i16(INT16_MIN);
            return make_i64(INT64_MIN);
        }
        double result = floor(as_f64(a) / bv);
        /* Return type matches LEFT operand */
        if (a->type == -RAY_F64) return make_f64(result);
        if (a->type == -RAY_I16) return make_i16((int16_t)(int64_t)result);
        if (a->type == -RAY_I32) return make_i32((int32_t)(int64_t)result);
        if (result >= (double)INT64_MIN && result <= (double)INT64_MAX)
            return make_i64((int64_t)result);
        return make_i64(INT64_MIN);
    }
    int64_t bv = as_i64(b);
    if (bv == 0) {
        /* Zero divisor: return null matching left operand's type */
        if (a->type == -RAY_I32) return ray_i32(INT32_MIN);
        if (a->type == -RAY_I16) return ray_i16(INT16_MIN);
        return make_i64(INT64_MIN);
    }
    int64_t av = as_i64(a);
    /* Floor division (toward -inf) */
    int64_t q = av / bv;
    if ((av ^ bv) < 0 && q * bv != av) q--;
    /* Return type matches LEFT operand for i16/i32 */
    if (a->type == -RAY_I16) return make_i16((int16_t)q);
    if (a->type == -RAY_I32) return make_i32((int32_t)q);
    return make_i64(q);
}

ray_t* ray_mod_fn(ray_t* a, ray_t* b) {
    /* Temporal % numeric → temporal (same type as left operand) */
    if (is_temporal(a) && is_numeric(b)) {
        int64_t av = a->i64;
        int is_null_a = (a->type == -RAY_TIME || a->type == -RAY_DATE) ? (av == INT32_MIN) : (av == INT64_MIN);
        if (is_null_a || is_null_atom(b)) {
            if (a->type == -RAY_TIME)      return ray_time(INT32_MIN);
            if (a->type == -RAY_DATE)      return ray_date(INT32_MIN);
            return ray_timestamp(INT64_MIN);
        }
        int64_t bv;
        if (b->type == -RAY_F64) {
            double bvf = b->f64;
            if (bvf == 0.0 || isnan(bvf)) {
                if (a->type == -RAY_TIME) return ray_time(INT32_MIN);
                if (a->type == -RAY_DATE) return ray_date(INT32_MIN);
                return ray_timestamp(INT64_MIN);
            }
            bv = (int64_t)bvf;
        } else {
            bv = as_i64(b);
        }
        if (bv == 0) {
            if (a->type == -RAY_TIME) return ray_time(INT32_MIN);
            if (a->type == -RAY_DATE) return ray_date(INT32_MIN);
            return ray_timestamp(INT64_MIN);
        }
        int64_t q = av / bv;
        if ((av ^ bv) < 0 && q * bv != av) q--;
        int64_t result = av - bv * q;
        if (a->type == -RAY_TIME)      return ray_time(result);
        if (a->type == -RAY_DATE)      return ray_date(result);
        return ray_timestamp(result);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot mod %s by %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));

    /* u8: unsigned byte modulo, no null sentinel — mod by 0 returns 0 */
    if (b->type == -RAY_U8) {
        uint8_t bv = b->u8;
        if (bv == 0) return make_u8(0);
        return make_u8((uint8_t)((uint8_t)as_i64(a) % bv));
    }
    if (a->type == -RAY_U8) {
        /* a is u8 but b is not u8 — treat as integer, result follows b's type */
    }

    /* Null propagation and division by zero: null type follows RIGHT operand */
    if (is_null_atom(a) || is_null_atom(b)) {
        if (b->type == -RAY_F64 || a->type == -RAY_F64) return make_f64(NAN);
        if (b->type == -RAY_I32) return ray_i32(INT32_MIN);
        if (b->type == -RAY_I16) return ray_i16(INT16_MIN);
        if (b->type == -RAY_I64) return make_i64(INT64_MIN);
        return make_i64(INT64_MIN);
    }

    /* Float modulo: result = a - b * floor(a/b), type follows RIGHT or f64 */
    if (is_float_op(a, b)) {
        double av = as_f64(a), bv = as_f64(b);
        if (bv == 0.0 || isnan(bv)) {
            if (b->type == -RAY_F64 || a->type == -RAY_F64) return make_f64(NAN);
            if (b->type == -RAY_I32) return ray_i32(INT32_MIN);
            if (b->type == -RAY_I16) return ray_i16(INT16_MIN);
            return make_i64(INT64_MIN);
        }
        double result = av - bv * floor(av / bv);
        /* Snap tiny residual to 0 */
        if (fabs(result) < 1e-12 || fabs(result - fabs(bv)) < 1e-12) result = bv > 0 ? 0.0 : -0.0;
        if (b->type == -RAY_F64 || a->type == -RAY_F64) return make_f64(result);
        if (b->type == -RAY_I32) return make_i32((int32_t)(int64_t)result);
        if (b->type == -RAY_I16) return make_i16((int16_t)(int64_t)result);
        return make_i64((int64_t)result);
    }

    /* Integer modulo: result = a - b * floor(a/b), sign follows b (divisor) */
    int64_t av = as_i64(a), bv = as_i64(b);
    if (bv == 0) {
        if (b->type == -RAY_I32) return ray_i32(INT32_MIN);
        if (b->type == -RAY_I16) return ray_i16(INT16_MIN);
        return make_i64(INT64_MIN);
    }
    int64_t q = av / bv;
    if ((av ^ bv) < 0 && q * bv != av) q--;  /* floor division */
    int64_t result = av - bv * q;
    /* Result type follows RIGHT operand */
    if (b->type == -RAY_I32) return make_i32((int32_t)result);
    if (b->type == -RAY_I16) return make_i16((int16_t)result);
    if (b->type == -RAY_U8) return make_u8((uint8_t)result);
    return make_i64(result);
}

/* Helper: compare char atom vs string atom.
 * Returns: -1 if no char/string pair, else memcmp-like result via *out. */
static int char_str_cmp(ray_t* a, ray_t* b, int *out) {
    const char *ap, *bp;
    size_t al, bl;
    int a_cs = (a->type == -RAY_STR);
    int b_cs = (b->type == -RAY_STR);
    if (!a_cs || !b_cs) return -1;
    ap = ray_str_ptr(a); al = ray_str_len(a);
    bp = ray_str_ptr(b); bl = ray_str_len(b);
    size_t mn = al < bl ? al : bl;
    int c = memcmp(ap, bp, mn);
    if (c != 0) { *out = c; return 0; }
    *out = (al > bl) ? 1 : (al < bl) ? -1 : 0;
    return 0;
}

/* Comparison */
ray_t* ray_gt_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c > 0 ? 1 : 0); }
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) > 0 ? 1 : 0);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b)) {
        if (is_null_atom(a) || is_null_atom(b))
            return make_bool(is_null_atom(b) && !is_null_atom(a) ? 1 : 0);
        return make_bool(temporal_as_ns(a) > temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = is_null_atom(a), nb = is_null_atom(b);
    if (na && nb) return make_bool(0);       /* null == null → not > */
    if (na) return make_bool(0);             /* null > X → false */
    if (nb) return make_bool(1);             /* X > null → true */
    return make_bool(as_f64(a) > as_f64(b) ? 1 : 0);
}

ray_t* ray_lt_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c < 0 ? 1 : 0); }
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) < 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (is_null_atom(a) || is_null_atom(b))
            return make_bool(is_null_atom(a) && !is_null_atom(b) ? 1 : 0);
        return make_bool(temporal_as_ns(a) < temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = is_null_atom(a), nb = is_null_atom(b);
    if (na && nb) return make_bool(0);       /* null == null → not < */
    if (na) return make_bool(1);             /* null < X → true */
    if (nb) return make_bool(0);             /* X < null → false */
    return make_bool(as_f64(a) < as_f64(b) ? 1 : 0);
}

ray_t* ray_gte(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c >= 0 ? 1 : 0); }
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) >= 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (is_null_atom(a) && is_null_atom(b)) return make_bool(1);
        if (is_null_atom(a)) return make_bool(0);
        if (is_null_atom(b)) return make_bool(1);
        return make_bool(temporal_as_ns(a) >= temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = is_null_atom(a), nb = is_null_atom(b);
    if (na && nb) return make_bool(1);       /* null == null → >= true */
    if (na) return make_bool(0);             /* null >= X → false */
    if (nb) return make_bool(1);             /* X >= null → true */
    return make_bool(as_f64(a) >= as_f64(b) ? 1 : 0);
}

ray_t* ray_lte(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c <= 0 ? 1 : 0); }
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) <= 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (is_null_atom(a) && is_null_atom(b)) return make_bool(1);
        if (is_null_atom(a)) return make_bool(1);
        if (is_null_atom(b)) return make_bool(0);
        return make_bool(temporal_as_ns(a) <= temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = is_null_atom(a), nb = is_null_atom(b);
    if (na && nb) return make_bool(1);       /* null == null → <= true */
    if (na) return make_bool(1);             /* null <= X → true */
    if (nb) return make_bool(0);             /* X <= null → false */
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

/* Check if comparable (numeric or temporal) */
static int is_comparable(ray_t* x) {
    return is_numeric(x) || is_temporal(x);
}

ray_t* ray_eq_fn(ray_t* a, ray_t* b) {
    /* Handle C NULL (null keyword) */
    if (!a && !b) return make_bool(1);
    if (!a || !b) return make_bool(0);
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c == 0 ? 1 : 0); }
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 == b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 == b->i64 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0 ? 1 : 0);
    /* null == null → true */
    if (is_comparable(a) && is_comparable(b) && is_null_atom(a) && is_null_atom(b))
        return make_bool(1);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b))
        return make_bool(temporal_as_ns(a) == temporal_as_ns(b) ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    if (is_float_op(a, b))
        return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) == as_i64(b) ? 1 : 0);
}

ray_t* ray_neq(ray_t* a, ray_t* b) {
    /* Handle C NULL (null keyword) */
    if (!a && !b) return make_bool(0);
    if (!a || !b) return make_bool(1);
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c != 0 ? 1 : 0); }
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 != b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 != b->i64 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) != 0 ? 1 : 0);
    /* null != null → false (all nulls are equal) */
    if (is_comparable(a) && is_comparable(b) && is_null_atom(a) && is_null_atom(b))
        return make_bool(0);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b))
        return make_bool(temporal_as_ns(a) != temporal_as_ns(b) ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    if (is_float_op(a, b))
        return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) != as_i64(b) ? 1 : 0);
}

/* Logical — coerce to truthiness (0/nil/false = falsy, else truthy) */
static inline int is_truthy(ray_t* x) {
    if (x->type == -RAY_BOOL) return x->b8;
    if (x->type == -RAY_I64)  return x->i64 != 0;
    if (x->type == -RAY_F64)  return x->f64 != 0.0;
    return 1; /* non-null objects are truthy */
}

ray_t* ray_and_fn(ray_t* a, ray_t* b) {
    /* Element-wise for bool vectors */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_vec(b) && b->type == RAY_BOOL) {
        int64_t n = a->len < b->len ? a->len : b->len;
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* da = (bool*)ray_data(a);
        bool* db = (bool*)ray_data(b);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = da[i] && db[i];
        r->len = n;
        return r;
    }
    /* Scalar broadcast: vec and scalar */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_atom(b)) {
        int64_t n = a->len;
        bool bv = is_truthy(b);
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* da = (bool*)ray_data(a);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = da[i] && bv;
        r->len = n;
        return r;
    }
    if (ray_is_atom(a) && ray_is_vec(b) && b->type == RAY_BOOL) {
        int64_t n = b->len;
        bool av = is_truthy(a);
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* db = (bool*)ray_data(b);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = av && db[i];
        r->len = n;
        return r;
    }
    return make_bool((is_truthy(a) && is_truthy(b)) ? 1 : 0);
}

ray_t* ray_or_fn(ray_t* a, ray_t* b) {
    /* Element-wise for bool vectors */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_vec(b) && b->type == RAY_BOOL) {
        int64_t n = a->len < b->len ? a->len : b->len;
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* da = (bool*)ray_data(a);
        bool* db = (bool*)ray_data(b);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = da[i] || db[i];
        r->len = n;
        return r;
    }
    /* Scalar broadcast */
    if (ray_is_vec(a) && a->type == RAY_BOOL && ray_is_atom(b)) {
        int64_t n = a->len;
        bool bv = is_truthy(b);
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* da = (bool*)ray_data(a);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = da[i] || bv;
        r->len = n;
        return r;
    }
    if (ray_is_atom(a) && ray_is_vec(b) && b->type == RAY_BOOL) {
        int64_t n = b->len;
        bool av = is_truthy(a);
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* db = (bool*)ray_data(b);
        bool* dr = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dr[i] = av || db[i];
        r->len = n;
        return r;
    }
    return make_bool((is_truthy(a) || is_truthy(b)) ? 1 : 0);
}

/* Unary */
ray_t* ray_not_fn(ray_t* x) {
    /* Element-wise for bool vectors */
    if (ray_is_vec(x) && x->type == RAY_BOOL) {
        int64_t n = x->len;
        ray_t* r = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(r)) return r;
        bool* src = (bool*)ray_data(x);
        bool* dst = (bool*)ray_data(r);
        for (int64_t i = 0; i < n; i++) dst[i] = !src[i];
        r->len = n;
        return r;
    }
    return make_bool(is_truthy(x) ? 0 : 1);
}

ray_t* ray_neg_fn(ray_t* x) {
    if (x->type == -RAY_I64) return make_i64(-x->i64);
    if (x->type == -RAY_F64) return make_f64(-x->f64);
    return ray_error("type", NULL);
}

/* round: round to nearest integer (ties go away from zero), returns f64 */
static ray_t* ray_round_fn(ray_t* x) {
    if (x->type == -RAY_F64) {
        if (isnan(x->f64)) return make_f64(NAN);
        return make_f64(round(x->f64));
    }
    if (is_numeric(x)) {
        if (is_null_atom(x)) return make_f64(NAN);
        return make_f64(round(as_f64(x)));
    }
    return ray_error("type", NULL);
}

/* floor: round toward -inf, returns f64 for f64, identity for int */
static ray_t* ray_floor_fn(ray_t* x) {
    if (x->type == -RAY_F64) {
        if (isnan(x->f64)) return make_f64(NAN);
        return make_f64(floor(x->f64));
    }
    if (is_numeric(x)) {
        if (is_null_atom(x)) { ray_retain(x); return x; }
        ray_retain(x); return x; /* integer floor is identity */
    }
    return ray_error("type", NULL);
}

/* ceil: round toward +inf, returns f64 for f64, identity for int */
static ray_t* ray_ceil_fn(ray_t* x) {
    if (x->type == -RAY_F64) {
        if (isnan(x->f64)) return make_f64(NAN);
        return make_f64(ceil(x->f64));
    }
    if (is_numeric(x)) {
        if (is_null_atom(x)) { ray_retain(x); return x; }
        ray_retain(x); return x; /* integer ceil is identity */
    }
    return ray_error("type", NULL);
}

/* ══════════════════════════════════════════
 * Error handling: try / raise
 * ══════════════════════════════════════════ */

static _Thread_local ray_t *__raise_val = NULL;

/* (raise value) — raise an error with the given value */
ray_t* ray_raise(ray_t* val) {
    if (__raise_val) ray_release(__raise_val);
    ray_retain(val);
    __raise_val = val;
    return ray_error("domain", NULL);
}

/* Forward declaration for call_lambda (used by ray_try) */
static ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc);

/* (try expr handler) — evaluate expr, if error call handler with error value.
 * Special form: receives unevaluated args. */
ray_t* ray_try(ray_t* expr, ray_t* handler_expr) {
    ray_t* result = ray_eval(expr);
    if (!RAY_IS_ERR(result)) return result;

    /* Get error value (set by raise, or default for runtime errors) */
    ray_t* err_val = __raise_val;
    __raise_val = NULL;
    if (!err_val) err_val = make_i64(0);

    /* Evaluate handler expression */
    ray_t* handler = ray_eval(handler_expr);
    if (RAY_IS_ERR(handler)) {
        ray_release(err_val);
        return handler;
    }

    /* Call handler with error value */
    ray_t* handler_result;
    if (handler->type == RAY_LAMBDA) {
        ray_t* args[1] = { err_val };
        handler_result = call_lambda(handler, args, 1);
    } else if (handler->type == RAY_UNARY) {
        ray_unary_fn fn = (ray_unary_fn)(uintptr_t)handler->i64;
        handler_result = fn(err_val);
    } else {
        handler_result = ray_error("type", NULL);
    }

    ray_release(err_val);
    ray_release(handler);
    return handler_result;
}

/* ══════════════════════════════════════════
 * FN_ATOMIC auto-mapping helpers
 * ══════════════════════════════════════════ */

static int is_list(ray_t* x) {
    return x && !RAY_IS_ERR(x) && x->type == RAY_LIST;
}

/* Check if x is a collection: boxed list OR typed vector */
static int is_collection(ray_t* x) {
    return x && !RAY_IS_ERR(x) && (x->type == RAY_LIST || ray_is_vec(x));
}

/* Extract the i-th element of a collection as a ray_t* atom.
 * For boxed lists, returns the stored pointer (no alloc).
 * For typed vectors, allocates a new atom.  Caller must release
 * atoms obtained from typed vectors (allocated == 1). */
static ray_t* collection_elem(ray_t* coll, int64_t i, int *allocated) {
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
        /* RAY_CHAR removed — char vectors no longer exist */
        case RAY_STR: {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(coll, i, &slen);
            return ray_str(sp ? sp : "", sp ? slen : 0);
        }
        default:            *allocated = 0; return ray_error("type", NULL);
    }
}

/* Convert a typed vector to a boxed list.  If already a list, retains
 * and returns it directly.  Caller owns the returned object. */
static ray_t* to_boxed_list(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (x->type == RAY_LIST) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", NULL);

    int64_t len = ray_len(x);
    ray_t* list = ray_alloc(len * sizeof(ray_t*));
    if (!list) return ray_error("oom", NULL);
    list->type = RAY_LIST;
    list->len = len;
    ray_t** dst = (ray_t**)ray_data(list);

    for (int64_t i = 0; i < len; i++) {
        int alloc = 0;
        dst[i] = collection_elem(x, i, &alloc);
        if (RAY_IS_ERR(dst[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(dst[j]);
            ray_release(list);
            return dst[i];
        }
        /* collection_elem always allocates for typed vecs, so ownership transfers */
    }
    return list;
}

/* Unbox a typed vector argument to a boxed list for use in builtins.
 * Sets *_bx to the allocated boxed list (caller must release) or NULL.
 * Returns the (possibly converted) argument, or an error. */
static ray_t* unbox_vec_arg(ray_t* x, ray_t** _bx) {
    *_bx = NULL;
    if (x && !RAY_IS_ERR(x) && ray_is_vec(x)) {
        *_bx = to_boxed_list(x);
        return *_bx;
    }
    return x;
}

/* Store a scalar result into a typed vector at position i.
 * Returns 0 on success, -1 if the element type doesn't match. */
/* Extract a value from an atom for storage, handling cross-type casting.
 * Returns the value as int64_t (for integer/temporal types). */
static int64_t elem_as_i64(ray_t* elem) {
    if (elem->type == -RAY_I64 || elem->type == -RAY_TIMESTAMP ||
        elem->type == -RAY_DATE || elem->type == -RAY_TIME ||
        elem->type == -RAY_SYM) return elem->i64;
    if (elem->type == -RAY_I32)  return (int64_t)elem->i32;
    if (elem->type == -RAY_I16)  return (int64_t)elem->i16;
    if (elem->type == -RAY_U8)   return (int64_t)elem->u8;
    if (elem->type == -RAY_F64)  return (int64_t)elem->f64;
    return elem->i64;
}

static int store_typed_elem(ray_t* vec, int64_t i, ray_t* elem) {
    switch (vec->type) {
        case RAY_I64:       ((int64_t*)ray_data(vec))[i]  = elem_as_i64(elem); return 0;
        case RAY_F64:       ((double*)ray_data(vec))[i]    = (elem->type == -RAY_F64) ? elem->f64 : (double)elem_as_i64(elem); return 0;
        case RAY_I32:       ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_I16:       ((int16_t*)ray_data(vec))[i]   = (int16_t)elem_as_i64(elem); return 0;
        case RAY_BOOL:      ((bool*)ray_data(vec))[i]      = elem->b8;  return 0;
        case RAY_U8:        ((uint8_t*)ray_data(vec))[i]   = (uint8_t)elem_as_i64(elem); return 0;
        /* RAY_CHAR removed — char vectors no longer exist */
        case RAY_DATE:      ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_TIME:      ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_TIMESTAMP: ((int64_t*)ray_data(vec))[i]   = elem_as_i64(elem); return 0;
        case RAY_SYM:       ((int64_t*)ray_data(vec))[i]   = elem->i64; return 0;
        case RAY_GUID:      if (elem->obj) memcpy(((uint8_t*)ray_data(vec)) + i * 16, ray_data(elem->obj), 16); return 0;
        default: return -1;
    }
}

/* Map a binary function element-wise over collections.
 * Both args can be collections (zip-map) or one scalar (broadcast).
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise. */
static ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right);

static ray_t* atomic_map_binary(ray_binary_fn fn, ray_t* left, ray_t* right) {
    return atomic_map_binary_op(fn, 0, left, right);
}

static ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right) {
    int left_coll = is_collection(left);
    int right_coll = is_collection(right);

    if (!left_coll && !right_coll) return fn(left, right);

    int64_t len;
    if (left_coll && right_coll) {
        len = ray_len(left) < ray_len(right) ? ray_len(left) : ray_len(right);
    } else {
        len = left_coll ? ray_len(left) : ray_len(right);
    }

    if (len == 0) {
        /* Return empty I64 vector for empty input */
        return ray_vec_new(RAY_I64, 0);
    }

    /* Probe first element to determine output type */
    int la0 = 0, ra0 = 0;
    ray_t* a0 = left_coll  ? collection_elem(left, 0, &la0)  : left;
    ray_t* b0 = right_coll ? collection_elem(right, 0, &ra0) : right;
    ray_t* e0;
    if (RAY_IS_ERR(a0) || RAY_IS_ERR(b0)) {
        e0 = ray_error("type", NULL);
    } else if (is_collection(a0) || is_collection(b0)) {
        e0 = atomic_map_binary(fn, a0, b0);
    } else {
        e0 = fn(a0, b0);
    }
    if (la0) ray_release(a0);
    if (ra0) ray_release(b0);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = -(e0->type);  /* atom type (-RAY_I64) → vector type (RAY_I64) */

    /* If either input is a boxed list (mixed types), always use boxed list output
     * to preserve type heterogeneity */
    int force_boxed = (left_coll && left->type == RAY_LIST) ||
                      (right_coll && right->type == RAY_LIST);

    /* When the probed result is a null atom, the fn already chose the correct
     * result type (e.g., division returns left-operand-typed null).  Skip the
     * wider-wins promotion so the null sentinel lands in the right vector type. */
    int e0_null = is_null_atom(e0);

    /* When the probed result is a boolean (from comparison ops like ==, <, etc.),
     * preserve the bool output type — do not promote to wider integer type. */
    int e0_bool = (e0->type == -RAY_BOOL);

    /* When LEFT is scalar broadcast to RIGHT vector, the output type follows
     * the RIGHT vector's element type (q/kdb+ semantics) for integer types,
     * unless float or temporal promotion is involved. */
    if (!e0_null && !e0_bool && !left_coll && right_coll && ray_is_vec(right) && out_type != RAY_F64) {
        int8_t vec_type = right->type;
        /* Only override for integer family: if probed type is wider int, downcast */
        int out_is_int = (out_type == RAY_I64 || out_type == RAY_I32 || out_type == RAY_I16 || out_type == RAY_U8);
        int vec_is_int = (vec_type == RAY_I64 || vec_type == RAY_I32 || vec_type == RAY_I16 || vec_type == RAY_U8);
        if (out_is_int && vec_is_int)
            out_type = vec_type;
        /* For temporal: only override if both are same temporal family */
        if ((vec_type == RAY_DATE || vec_type == RAY_TIME || vec_type == RAY_TIMESTAMP) &&
            out_type == vec_type)
            out_type = vec_type; /* no-op, just keep it */
    }
    /* When LEFT is vector and RIGHT is scalar, output follows WIDER integer
     * type between left vector and right scalar (q/kdb+ semantics) */
    if (!e0_null && !e0_bool && left_coll && !right_coll && ray_is_vec(left) && out_type != RAY_F64 &&
        ray_is_atom(right)) {
        int8_t vt = left->type, st = -(right->type);
        int vt_int = (vt == RAY_I64 || vt == RAY_I32 || vt == RAY_I16 || vt == RAY_U8);
        int st_int = (st == RAY_I64 || st == RAY_I32 || st == RAY_I16 || st == RAY_U8);
        int out_is_int = (out_type == RAY_I64 || out_type == RAY_I32 || out_type == RAY_I16 || out_type == RAY_U8);
        if (out_is_int && vt_int && st_int)
            out_type = (vt >= st) ? vt : st; /* wider wins */
    }
    /* When both are vectors, output type follows wider integer type */
    if (!e0_null && !e0_bool && left_coll && right_coll && ray_is_vec(left) && ray_is_vec(right) && out_type != RAY_F64) {
        int8_t lt = left->type, rt = right->type;
        int lt_int = (lt == RAY_I64 || lt == RAY_I32 || lt == RAY_I16 || lt == RAY_U8);
        int rt_int = (rt == RAY_I64 || rt == RAY_I32 || rt == RAY_I16 || rt == RAY_U8);
        if (lt_int && rt_int) {
            /* Pick wider: I64 > I32 > I16 > U8 (using type tag ordering) */
            out_type = (lt >= rt) ? lt : rt;
        }
    }

    /* When LEFT is a vector collection, override i32 output to match the
     * left vector type or i64 (e.g., [DATE]-DATE → i64, [i64]-i32 → i64).
     * Keeps i32 only when left vector is actually i32. */
    if (!e0_null && !e0_bool && out_type == RAY_I32 && left_coll && ray_is_vec(left) && left->type != RAY_I32) {
        out_type = RAY_I64;
    }

    /* ══════════════════════════════════════════════════════════════
     * FAST PATH: opcode-driven vectorized execution.
     * I64 ops use direct array loops (lowest overhead).
     * F64/comparison ops route through DAG executor.
     * ══════════════════════════════════════════════════════════════ */

    /* Direct array loops — all integer types (I64, TIMESTAMP=i64, I32, DATE, TIME=i32, I16, U8) */
    if (!force_boxed && dag_opcode > 0 && dag_opcode <= OP_MOD) {
        int8_t ltype = left_coll ? left->type : -(left->type);
        int8_t rtype = right_coll ? right->type : -(right->type);
        int esz_l = (ltype == RAY_I64 || ltype == RAY_TIMESTAMP) ? 8 :
                    (ltype == RAY_I32 || ltype == RAY_DATE || ltype == RAY_TIME) ? 4 :
                    (ltype == RAY_I16) ? 2 : (ltype == RAY_U8) ? 1 : 0;
        int esz_r = (rtype == RAY_I64 || rtype == RAY_TIMESTAMP) ? 8 :
                    (rtype == RAY_I32 || rtype == RAY_DATE || rtype == RAY_TIME) ? 4 :
                    (rtype == RAY_I16) ? 2 : (rtype == RAY_U8) ? 1 : 0;
        int lv = left_coll && ray_is_vec(left) && esz_l > 0;
        int rv = right_coll && ray_is_vec(right) && esz_r > 0;
        int ls = !left_coll && esz_l > 0;
        int rs = !right_coll && esz_r > 0;

        /* Cross-type temporal arithmetic (DATE+TIME→TIMESTAMP) needs eval-level
         * conversion — only use fast path when types are compatible for raw arithmetic */
        int8_t ltype2 = lv ? left->type : -(left->type);
        int8_t rtype2 = rv ? right->type : -(right->type);
        int same_class = (esz_l == esz_r) || /* same storage width */
                         (ltype2 == RAY_I64 && rtype2 == RAY_I64) || /* both i64 */
                         (ltype2 == RAY_TIMESTAMP && rtype2 == RAY_TIMESTAMP) ||
                         /* scalar int + any integer vec is fine (just adds raw values) */
                         (ls && (rtype2 == ltype2 || ltype2 == RAY_I64)) ||
                         (rs && (ltype2 == rtype2 || rtype2 == RAY_I64));
        /* Reject cross-temporal: DATE+TIME, TIMESTAMP+DATE, etc. */
        int l_temporal = (ltype2==RAY_DATE||ltype2==RAY_TIME||ltype2==RAY_TIMESTAMP);
        int r_temporal = (rtype2==RAY_DATE||rtype2==RAY_TIME||rtype2==RAY_TIMESTAMP);
        if (l_temporal && r_temporal && ltype2 != rtype2) same_class = 0;

        if (same_class && ((ls && rv) || (lv && rs) || (lv && rv))) {
            /* Read elements as i64 regardless of storage width */
            #define READ_INT(ptr, esz, i) \
                ((esz)==8 ? ((int64_t*)(ptr))[(i)] : \
                 (esz)==4 ? (int64_t)((int32_t*)(ptr))[(i)] : \
                 (esz)==2 ? (int64_t)((int16_t*)(ptr))[(i)] : \
                            (int64_t)((uint8_t*)(ptr))[(i)])
            #define SCALAR_INT(obj) \
                (((obj)->type==-RAY_I64||(obj)->type==-RAY_TIMESTAMP) ? (obj)->i64 : \
                 ((obj)->type==-RAY_I32||(obj)->type==-RAY_DATE||(obj)->type==-RAY_TIME) ? (int64_t)(obj)->i32 : \
                 ((obj)->type==-RAY_I16) ? (int64_t)(obj)->i16 : (int64_t)(obj)->u8)

            /* Output type = probed result type (from e0) */
            ray_t* vec = ray_vec_new(out_type, len);
            if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
            vec->len = len;

            void* ldata = lv ? ray_data(left) : NULL;
            void* rdata = rv ? ray_data(right) : NULL;
            int64_t lsv = ls ? SCALAR_INT(left) : 0;
            int64_t rsv = rs ? SCALAR_INT(right) : 0;
            /* Null sentinel for the input width */
            int64_t lnull = (esz_l==8) ? INT64_MIN : (esz_l==4) ? (int64_t)INT32_MIN :
                            (esz_l==2) ? (int64_t)INT16_MIN : 0;
            int64_t rnull = (esz_r==8) ? INT64_MIN : (esz_r==4) ? (int64_t)INT32_MIN :
                            (esz_r==2) ? (int64_t)INT16_MIN : 0;
            /* Output null sentinel */
            int out_esz = ray_elem_size(out_type);

            #define LA(i) (ldata ? READ_INT(ldata, esz_l, i) : lsv)
            #define RA(i) (rdata ? READ_INT(rdata, esz_r, i) : rsv)
            #define ISNULL_L(v) (ls ? (lsv==lnull) : (v)==lnull)
            #define ISNULL_R(v) (rs ? (rsv==rnull) : (v)==rnull)

            /* Compute into i64 temp, then store at output width */
            for (int64_t i = 0; i < len; i++) {
                int64_t a = LA(i), b = RA(i);
                int64_t r;
                int null = ISNULL_L(a) || ISNULL_R(b);
                if (null) {
                store_null:
                    /* Store type-appropriate null */
                    if (out_esz == 8)      ((int64_t*)ray_data(vec))[i] = INT64_MIN;
                    else if (out_esz == 4)  ((int32_t*)ray_data(vec))[i] = INT32_MIN;
                    else if (out_esz == 2)  ((int16_t*)ray_data(vec))[i] = INT16_MIN;
                    else                    ((uint8_t*)ray_data(vec))[i] = 0;
                    continue;
                }
                switch (dag_opcode) {
                case OP_ADD: r = (int64_t)((uint64_t)a + (uint64_t)b); break;
                case OP_SUB: r = (int64_t)((uint64_t)a - (uint64_t)b); break;
                case OP_MUL: r = (int64_t)((uint64_t)a * (uint64_t)b); break;
                case OP_DIV: if (b==0) goto store_null; else { r=a/b; if ((a^b)<0 && r*b!=a) r--; } break;
                case OP_MOD: if (b==0) goto store_null; else { r=a%b; if (r && (r^b)<0) r+=b; } break;
                default: r = 0; break;
                }
                if (out_esz == 8)      ((int64_t*)ray_data(vec))[i] = r;
                else if (out_esz == 4)  ((int32_t*)ray_data(vec))[i] = (int32_t)r;
                else if (out_esz == 2)  ((int16_t*)ray_data(vec))[i] = (int16_t)r;
                else                    ((uint8_t*)ray_data(vec))[i] = (uint8_t)r;
            }
            #undef LA
            #undef RA
            #undef ISNULL_L
            #undef ISNULL_R
            #undef READ_INT
            #undef SCALAR_INT
            ray_release(e0);
            return vec;
        }
    }

    /* DAG executor — for F64 and comparisons */
    if (!force_boxed && dag_opcode > 0) {
        int is_idiv = (dag_opcode == OP_DIV || dag_opcode == OP_MOD);
        int is_cmp  = (dag_opcode >= OP_EQ && dag_opcode <= OP_GE);

        /* Classify operands: numeric/temporal vectors or scalars */
        int8_t lt = left_coll ? left->type : -(left->type);
        int8_t rt = right_coll ? right->type : -(right->type);
        #define IS_NUM_TYPE(t) ((t)==RAY_I64||(t)==RAY_F64||(t)==RAY_I32||(t)==RAY_I16|| \
                                (t)==RAY_U8||(t)==RAY_DATE||(t)==RAY_TIME||(t)==RAY_TIMESTAMP)
        int l_num_vec = left_coll && ray_is_vec(left) && IS_NUM_TYPE(lt);
        int r_num_vec = right_coll && ray_is_vec(right) && IS_NUM_TYPE(rt);
        int l_num_scalar = !left_coll && IS_NUM_TYPE(lt);
        int r_num_scalar = !right_coll && IS_NUM_TYPE(rt);
        #undef IS_NUM_TYPE

        int can_dag = (l_num_vec || r_num_vec) &&
                      (l_num_vec || l_num_scalar) && (r_num_vec || r_num_scalar);

        /* Div/mod: only I64×I64 (executor has floor-div semantics for I64) */
        if (is_idiv && !(lt == RAY_I64 && rt == RAY_I64)) can_dag = 0;
        /* Comparisons: same-type only (cross-type null sentinels differ) */
        if (is_cmp && lt != rt) can_dag = 0;
        /* Cross-type temporal: DAG promote() loses type tag (int+TIMESTAMP→I64 not TIMESTAMP) */
        {   int lt_temp = (lt==RAY_DATE||lt==RAY_TIME||lt==RAY_TIMESTAMP);
            int rt_temp = (rt==RAY_DATE||rt==RAY_TIME||rt==RAY_TIMESTAMP);
            if ((lt_temp || rt_temp) && lt != rt) can_dag = 0;
        }

        if (can_dag) {
                ray_graph_t* g = ray_graph_new(NULL);
                if (g) {
                    /* Build left operand node */
                    ray_op_t* lop = NULL;
                    if (l_num_scalar) {
                        if (left->type == -RAY_F64)
                            lop = ray_const_f64(g, left->f64);
                        else {
                            int64_t sv = as_i64(left);
                            lop = ray_const_i64(g, sv);
                        }
                    } else {
                        lop = ray_const_vec(g, left);
                    }
                    /* Build right operand node */
                    ray_op_t* rop = NULL;
                    if (r_num_scalar) {
                        if (right->type == -RAY_F64)
                            rop = ray_const_f64(g, right->f64);
                        else {
                            int64_t sv = as_i64(right);
                            rop = ray_const_i64(g, sv);
                        }
                    } else {
                        rop = ray_const_vec(g, right);
                    }
                    if (lop && rop) {
                        ray_op_t* root = ray_binop(g, dag_opcode, lop, rop);
                        if (root) {
                            /* For integer floor-division: ray_binop sets F64 output
                             * for OP_DIV; override to I64 for floor-div with null prop */
                            if (is_idiv) root->out_type = RAY_I64;
                            ray_t* result = ray_execute(g, root);
                            ray_graph_free(g);
                            if (result && !RAY_IS_ERR(result)) {
                                ray_release(e0);
                                return result;
                            }
                        } else { ray_graph_free(g); }
                    } else { ray_graph_free(g); }
                }
            }
        }
    /* SLOW PATH: per-element scalar loop (fallback for mixed types, temporal, etc.) */
    if (!force_boxed &&
        (out_type == RAY_I64 || out_type == RAY_F64 || out_type == RAY_I32 ||
         out_type == RAY_I16 || out_type == RAY_BOOL || out_type == RAY_U8 ||
         out_type == RAY_DATE || out_type == RAY_TIME || out_type == RAY_TIMESTAMP)) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_typed_elem(vec, 0, e0);
        ray_release(e0);

        for (int64_t i = 1; i < len; i++) {
            int la = 0, ra = 0;
            ray_t* a = left_coll  ? collection_elem(left, i, &la)  : left;
            ray_t* b = right_coll ? collection_elem(right, i, &ra) : right;
            ray_t* elem = (RAY_IS_ERR(a) || RAY_IS_ERR(b))
                         ? ray_error("type", NULL) : fn(a, b);
            if (la) ray_release(a);
            if (ra) ray_release(b);
            if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
            store_typed_elem(vec, i, elem);
            ray_release(elem);
        }
        return vec;
    }

    /* Determine scalar int type for list+scalar coercion.
     * When a boxed list is combined with a scalar, integer results
     * are coerced to the scalar's integer type (K/q semantics). */
    int8_t scalar_int_type = 0;
    if (force_boxed) {
        ray_t* scalar = (!left_coll) ? left : (!right_coll ? right : NULL);
        if (scalar && ray_is_atom(scalar)) {
            int8_t st = scalar->type;
            if (st == -RAY_I16 || st == -RAY_I32 || st == -RAY_I64 || st == -RAY_U8)
                scalar_int_type = st;
        }
    }

    /* Coerce an integer atom to the scalar's integer type */
    #define COERCE_TO_SCALAR(elem) do { \
        if (scalar_int_type && ray_is_atom(elem) && elem->type != scalar_int_type && \
            elem->type != -RAY_F64 && is_numeric(elem)) { \
            int64_t _v = as_i64(elem); \
            ray_t* _coerced = make_typed_int(scalar_int_type, _v); \
            ray_release(elem); \
            elem = _coerced; \
        } \
    } while(0)

    /* Fallback: boxed list for non-numeric output or mixed-type input */
    COERCE_TO_SCALAR(e0);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { ray_release(e0); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    out[0] = e0;  /* first element already computed */

    for (int64_t i = 1; i < len; i++) {
        int la = 0, ra = 0;
        ray_t* a = left_coll  ? collection_elem(left, i, &la)  : left;
        ray_t* b = right_coll ? collection_elem(right, i, &ra) : right;
        ray_t* elem;
        if (RAY_IS_ERR(a) || RAY_IS_ERR(b)) {
            elem = ray_error("type", NULL);
        } else if (is_collection(a) || is_collection(b)) {
            /* Recursive auto-map when list element is itself a collection */
            elem = atomic_map_binary(fn, a, b);
        } else {
            elem = fn(a, b);
        }
        if (la) ray_release(a);
        if (ra) ray_release(b);
        if (RAY_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return elem;
        }
        COERCE_TO_SCALAR(elem);
        out[i] = elem;
    }
    #undef COERCE_TO_SCALAR
    return result;
}

/* Map a unary function element-wise over a collection.
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise. */
static ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg) {
    if (!is_collection(arg)) return fn(arg);

    int64_t len = ray_len(arg);

    if (len == 0) {
        return ray_vec_new(RAY_I64, 0);
    }

    /* Probe first element to determine output type */
    int alloc0 = 0;
    ray_t* e0_in = collection_elem(arg, 0, &alloc0);
    ray_t* e0 = RAY_IS_ERR(e0_in) ? e0_in : fn(e0_in);
    if (alloc0) ray_release(e0_in);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = -(e0->type);

    /* Try typed vector path for numeric/bool/temporal output */
    if (out_type == RAY_I64 || out_type == RAY_F64 || out_type == RAY_I32 ||
        out_type == RAY_I16 || out_type == RAY_BOOL || out_type == RAY_U8 ||
        out_type == RAY_DATE || out_type == RAY_TIME || out_type == RAY_TIMESTAMP) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_typed_elem(vec, 0, e0);
        ray_release(e0);

        for (int64_t i = 1; i < len; i++) {
            int alloc = 0;
            ray_t* e = collection_elem(arg, i, &alloc);
            ray_t* elem = RAY_IS_ERR(e) ? e : fn(e);
            if (alloc) ray_release(e);
            if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
            store_typed_elem(vec, i, elem);
            ray_release(elem);
        }
        return vec;
    }

    /* Fallback: boxed list for non-numeric output */
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { ray_release(e0); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    out[0] = e0;

    for (int64_t i = 1; i < len; i++) {
        int alloc = 0;
        ray_t* e = collection_elem(arg, i, &alloc);
        ray_t* elem = RAY_IS_ERR(e) ? e : fn(e);
        if (alloc) ray_release(e);
        if (RAY_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return elem;
        }
        out[i] = elem;
    }
    return result;
}

/* ══════════════════════════════════════════
 * Aggregation builtins
 * ══════════════════════════════════════════ */

ray_t* ray_sum_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_SUM);
    if (ray_is_atom(x)) {
        /* u8/i16 scalar sum promotes to i64 */
        if (x->type == -RAY_U8)  return make_i64((int64_t)x->u8);
        if (x->type == -RAY_I16) return make_i64((int64_t)x->i16);
        ray_retain(x); return x;
    }
    if (ray_is_vec(x)) {
        /* Direct reduction for small-type and null-sentinel vectors */
        if (x->type == RAY_I32) {
            int64_t n = x->len;
            int32_t* d = (int32_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT32_MIN) sum += d[i];
            return make_i32((int32_t)sum);
        }
        if (x->type == RAY_I16 || x->type == RAY_U8) {
            /* i16/u8 sum promotes to i64 to avoid overflow */
            int64_t n = x->len, sum = 0;
            if (x->type == RAY_I16) {
                int16_t* d = (int16_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (d[i] != INT16_MIN) sum += d[i];
            } else {
                uint8_t* d = (uint8_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++) sum += d[i];
            }
            return make_i64(sum);
        }
        if (x->type == RAY_I64) {
            int64_t n = x->len;
            int64_t* d = (int64_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT64_MIN) sum += d[i];
            return make_i64(sum);
        }
        if (x->type == RAY_F64) {
            int64_t n = x->len;
            double* d = (double*)ray_data(x);
            double sum = 0.0;
            for (int64_t i = 0; i < n; i++)
                if (!isnan(d[i])) sum += d[i];
            return make_f64(sum);
        }
        if (x->type == RAY_TIME) {
            int64_t n = x->len;
            int32_t* d = (int32_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT32_MIN) sum += d[i];
            return ray_time(sum);
        }
        if (x->type == RAY_TIMESTAMP) {
            int64_t n = x->len;
            int64_t* d = (int64_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT64_MIN) sum += d[i];
            return ray_timestamp(sum);
        }
        if (x->type == RAY_DATE) return ray_error("type", NULL);
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_sum(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return make_i64(0);
    ray_t** elems = (ray_t**)ray_data(x);
    int has_float = 0;
    double fsum = 0.0;
    int64_t isum = 0;
    for (int64_t i = 0; i < len; i++) {
        if (elems[i]->type == -RAY_F64) { has_float = 1; fsum += elems[i]->f64; }
        else if (elems[i]->type == -RAY_I64) { isum += elems[i]->i64; fsum += (double)elems[i]->i64; }
        else return ray_error("type", NULL);
    }
    return has_float ? make_f64(fsum) : make_i64(isum);
}

ray_t* ray_count_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_COUNT);
    if (x->type == RAY_TABLE) return make_i64(ray_table_nrows(x));
    /* String atom: count = string length */
    if (ray_is_atom(x) && (-x->type) == RAY_STR)
        return make_i64((int64_t)ray_str_len(x));
    if (ray_is_vec(x)) {
        /* GUID/STR vectors: return length directly (DAG count doesn't handle these) */
        if (x->type == RAY_GUID || x->type == RAY_STR) return make_i64(x->len);
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_count(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) {
        /* Scalar atom → count 1 */
        if (ray_is_atom(x)) return make_i64(1);
        return ray_error("type", NULL);
    }
    /* Dict: count = number of key-value pairs */
    if (x->attrs & RAY_ATTR_DICT)
        return make_i64(ray_len(x) / 2);
    return make_i64(ray_len(x));
}

ray_t* ray_avg_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_AVG);
    if (ray_is_atom(x)) {
        if (is_null_atom(x)) return make_f64(NAN);
        if (is_numeric(x)) return make_f64(as_f64(x));
        ray_retain(x); return x;
    }
    if (ray_is_vec(x)) {
        /* Direct avg with null skipping */
        int64_t n = x->len;
        if (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16 || x->type == RAY_U8) {
            double sum = 0.0;
            int64_t cnt = 0;
            if (x->type == RAY_I64) {
                int64_t* d = (int64_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (d[i] != INT64_MIN) { sum += (double)d[i]; cnt++; }
            } else if (x->type == RAY_I32) {
                int32_t* d = (int32_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (d[i] != INT32_MIN) { sum += (double)d[i]; cnt++; }
            } else if (x->type == RAY_I16) {
                int16_t* d = (int16_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (d[i] != INT16_MIN) { sum += (double)d[i]; cnt++; }
            } else {
                uint8_t* d = (uint8_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++) { sum += (double)d[i]; cnt++; }
            }
            if (cnt == 0) return make_f64(NAN);
            return make_f64(sum / (double)cnt);
        }
        if (x->type == RAY_F64) {
            double* d = (double*)ray_data(x);
            double sum = 0.0;
            int64_t cnt = 0;
            for (int64_t i = 0; i < n; i++)
                if (!isnan(d[i])) { sum += d[i]; cnt++; }
            if (cnt == 0) return make_f64(NAN);
            return make_f64(sum / (double)cnt);
        }
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_avg(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_error("domain", NULL);
    ray_t** elems = (ray_t**)ray_data(x);
    double sum = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        sum += as_f64(elems[i]);
    }
    return make_f64(sum / (double)len);
}

ray_t* ray_min(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MIN);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        int64_t n = x->len;
        if (n == 0) return make_i64(INT64_MIN); /* empty → null */
        if (x->type == RAY_I32 || x->type == RAY_DATE || x->type == RAY_TIME) {
            int32_t* d = (int32_t*)ray_data(x);
            int32_t m = INT32_MAX; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT32_MIN) { if (!found || d[i] < m) m = d[i]; found = 1; }
            if (!found) {
                if (x->type == RAY_TIME) return ray_time(INT32_MIN);
                if (x->type == RAY_DATE) return ray_date(INT32_MIN);
                return make_i32(INT32_MIN);
            }
            if (x->type == RAY_TIME) return ray_time(m);
            if (x->type == RAY_DATE) return ray_date(m);
            return make_i32(m);
        }
        if (x->type == RAY_I64 || x->type == RAY_TIMESTAMP) {
            int64_t* d = (int64_t*)ray_data(x);
            int64_t m = INT64_MAX; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT64_MIN) { if (!found || d[i] < m) m = d[i]; found = 1; }
            if (!found) return (x->type == RAY_TIMESTAMP) ? ray_timestamp(INT64_MIN) : make_i64(INT64_MIN);
            return (x->type == RAY_TIMESTAMP) ? ray_timestamp(m) : make_i64(m);
        }
        if (x->type == RAY_F64) {
            double* d = (double*)ray_data(x);
            double m = 0; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!isnan(d[i])) { if (!found || d[i] < m) m = d[i]; found = 1; }
            return found ? make_f64(m) : make_f64(NAN);
        }
        if (x->type == RAY_I16) {
            int16_t* d = (int16_t*)ray_data(x);
            int16_t m = INT16_MAX; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT16_MIN) { if (!found || d[i] < m) m = d[i]; found = 1; }
            return found ? make_i16(m) : make_i16(INT16_MIN);
        }
        if (x->type == RAY_U8) {
            uint8_t* d = (uint8_t*)ray_data(x);
            uint8_t m = 255;
            for (int64_t i = 0; i < n; i++)
                if (d[i] < m) m = d[i];
            return make_u8(m);
        }
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_min_op(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_error("domain", NULL);
    ray_t** elems = (ray_t**)ray_data(x);
    if (!is_numeric(elems[0])) return ray_error("type", NULL);
    int has_float = elems[0]->type == -RAY_F64;
    double fmin = as_f64(elems[0]);
    int64_t imin = elems[0]->type == -RAY_I64 ? elems[0]->i64 : 0;
    for (int64_t i = 1; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (elems[i]->type == -RAY_F64) has_float = 1;
        double v = as_f64(elems[i]);
        if (v < fmin) { fmin = v; imin = elems[i]->type == -RAY_I64 ? elems[i]->i64 : 0; }
    }
    return has_float ? make_f64(fmin) : make_i64(imin);
}

ray_t* ray_max(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MAX);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        int64_t n = x->len;
        if (n == 0) return make_i64(INT64_MIN);
        if (x->type == RAY_I32 || x->type == RAY_DATE || x->type == RAY_TIME) {
            int32_t* d = (int32_t*)ray_data(x);
            int32_t m = INT32_MIN; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT32_MIN) { if (!found || d[i] > m) m = d[i]; found = 1; }
            if (!found) {
                if (x->type == RAY_TIME) return ray_time(INT32_MIN);
                if (x->type == RAY_DATE) return ray_date(INT32_MIN);
                return make_i32(INT32_MIN);
            }
            if (x->type == RAY_TIME) return ray_time(m);
            if (x->type == RAY_DATE) return ray_date(m);
            return make_i32(m);
        }
        if (x->type == RAY_I64 || x->type == RAY_TIMESTAMP) {
            int64_t* d = (int64_t*)ray_data(x);
            int64_t m = INT64_MIN; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT64_MIN) { if (!found || d[i] > m) m = d[i]; found = 1; }
            if (!found) return (x->type == RAY_TIMESTAMP) ? ray_timestamp(INT64_MIN) : make_i64(INT64_MIN);
            return (x->type == RAY_TIMESTAMP) ? ray_timestamp(m) : make_i64(m);
        }
        if (x->type == RAY_F64) {
            double* d = (double*)ray_data(x);
            double m = 0; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!isnan(d[i])) { if (!found || d[i] > m) m = d[i]; found = 1; }
            return found ? make_f64(m) : make_f64(NAN);
        }
        if (x->type == RAY_I16) {
            int16_t* d = (int16_t*)ray_data(x);
            int16_t m = INT16_MIN; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != INT16_MIN) { if (!found || d[i] > m) m = d[i]; found = 1; }
            return found ? make_i16(m) : make_i16(INT16_MIN);
        }
        if (x->type == RAY_U8) {
            uint8_t* d = (uint8_t*)ray_data(x);
            uint8_t m = 0;
            for (int64_t i = 0; i < n; i++)
                if (d[i] > m) m = d[i];
            return make_u8(m);
        }
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_max_op(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_error("domain", NULL);
    ray_t** elems = (ray_t**)ray_data(x);
    if (!is_numeric(elems[0])) return ray_error("type", NULL);
    int has_float = elems[0]->type == -RAY_F64;
    double fmax = as_f64(elems[0]);
    int64_t imax = elems[0]->type == -RAY_I64 ? elems[0]->i64 : 0;
    for (int64_t i = 1; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (elems[i]->type == -RAY_F64) has_float = 1;
        double v = as_f64(elems[i]);
        if (v > fmax) { fmax = v; imax = elems[i]->type == -RAY_I64 ? elems[i]->i64 : 0; }
    }
    return has_float ? make_f64(fmax) : make_i64(imax);
}

ray_t* ray_first_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_FIRST);
    /* String first: return first char */
    if (ray_is_atom(x) && (-x->type) == RAY_STR) {
        size_t slen = ray_str_len(x);
        if (slen == 0) return ray_error("domain", NULL);
        const char* p = ray_str_ptr(x);
        return ray_str(p, 1);
    }
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    /* Table first: return first row as dict */
    if (x->type == RAY_TABLE) {
        if (ray_table_nrows(x) == 0) return ray_error("domain", NULL);
        ray_t* idx = make_i64(0);
        ray_t* result = ray_at(x, idx);
        ray_release(idx);
        return result;
    }
    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return make_i64(INT64_MIN); /* 0Nl for empty vec */
        /* For SYM, GUID, STR and other non-numeric types, use collection_elem directly */
        if (x->type == RAY_SYM || x->type == RAY_I32 || x->type == RAY_I16 ||
            x->type == RAY_GUID || x->type == RAY_STR) {
            int alloc = 0;
            return collection_elem(x, 0, &alloc);
        }
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_first(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return ray_error("type", NULL);
    if (ray_len(x) == 0) return make_i64(INT64_MIN); /* 0Nl for empty list */
    ray_t* elem = ((ray_t**)ray_data(x))[0];
    ray_retain(elem);
    return elem;
}

ray_t* ray_last_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_LAST);
    /* String last: return last char */
    if (ray_is_atom(x) && (-x->type) == RAY_STR) {
        size_t slen = ray_str_len(x);
        if (slen == 0) return ray_error("domain", NULL);
        const char* p = ray_str_ptr(x);
        return ray_str(p + slen - 1, 1);
    }
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    /* Table last: return last row as dict */
    if (x->type == RAY_TABLE) {
        int64_t nrows = ray_table_nrows(x);
        if (nrows == 0) return ray_error("domain", NULL);
        ray_t* idx = make_i64(nrows - 1);
        ray_t* result = ray_at(x, idx);
        ray_release(idx);
        return result;
    }
    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return make_i64(INT64_MIN); /* 0Nl for empty vec */
        if (x->type == RAY_SYM || x->type == RAY_I32 || x->type == RAY_I16 ||
            x->type == RAY_GUID || x->type == RAY_STR) {
            int alloc = 0;
            return collection_elem(x, ray_len(x) - 1, &alloc);
        }
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_last(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return make_i64(INT64_MIN); /* 0Nl for empty list */
    ray_t* elem = ((ray_t**)ray_data(x))[len - 1];
    ray_retain(elem);
    return elem;
}

/* Helper: copy typed vec elements to double scratch buffer.
 * Returns scratch ray_t* (caller must ray_release), or error. */
static ray_t* vec_to_f64_scratch(ray_t* x, double** out_vals) {
    int64_t len = ray_len(x);
    ray_t* scratch = ray_alloc(len * sizeof(double));
    if (!scratch) return ray_error("oom", NULL);
    scratch->type = RAY_F64;
    scratch->len = len;
    double* vals = (double*)ray_data(scratch);
    if (x->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) vals[i] = (double)d[i];
    } else if (x->type == RAY_F64) {
        memcpy(vals, ray_data(x), (size_t)len * sizeof(double));
    } else if (x->type == RAY_I32) {
        int32_t* d = (int32_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) vals[i] = (double)d[i];
    } else if (x->type == RAY_I16) {
        int16_t* d = (int16_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) vals[i] = (double)d[i];
    } else if (x->type == RAY_U8) {
        uint8_t* d = (uint8_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) vals[i] = (double)d[i];
    } else {
        ray_release(scratch);
        return ray_error("type", NULL);
    }
    *out_vals = vals;
    return scratch;
}

ray_t* ray_med(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (RAY_IS_ERR(x)) return x;
    /* Scalar: median of single value → f64 */
    if (ray_is_atom(x)) {
        if (is_null_atom(x)) return make_f64(NAN);
        if (is_numeric(x)) return make_f64(as_f64(x));
        return ray_error("type", NULL);
    }
    int64_t len;
    ray_t* scratch = NULL;
    double* vals = NULL;

    if (ray_is_vec(x)) {
        len = ray_len(x);
        if (len == 0) return make_f64(NAN);
        scratch = vec_to_f64_scratch(x, &vals);
        if (RAY_IS_ERR(scratch)) return scratch;
    } else if (is_list(x)) {
        len = ray_len(x);
        if (len == 0) return make_f64(NAN);
        ray_t** elems = (ray_t**)ray_data(x);
        scratch = ray_alloc(len * sizeof(double));
        if (!scratch) return ray_error("oom", NULL);
        scratch->type = RAY_F64;
        scratch->len = len;
        vals = (double*)ray_data(scratch);
        for (int64_t i = 0; i < len; i++) {
            if (!is_numeric(elems[i])) { ray_release(scratch); return ray_error("type", NULL); }
            vals[i] = as_f64(elems[i]);
        }
    } else {
        return ray_error("type", NULL);
    }

    /* Insertion sort */
    for (int64_t i = 1; i < len; i++) {
        double key = vals[i];
        int64_t j = i - 1;
        while (j >= 0 && vals[j] > key) { vals[j + 1] = vals[j]; j--; }
        vals[j + 1] = key;
    }
    double median;
    if (len % 2 == 1) median = vals[len / 2];
    else median = (vals[len / 2 - 1] + vals[len / 2]) / 2.0;
    ray_release(scratch);
    return make_f64(median);
}

/* Helper: compute stddev from array of f64 values, skipping NaN (null) */
static ray_t* dev_from_f64(double* vals, int64_t len) {
    double sum = 0.0;
    int64_t cnt = 0;
    for (int64_t i = 0; i < len; i++)
        if (!isnan(vals[i])) { sum += vals[i]; cnt++; }
    if (cnt == 0) return make_f64(NAN);
    double mean = sum / (double)cnt;
    double var = 0.0;
    for (int64_t i = 0; i < len; i++)
        if (!isnan(vals[i])) { double d = vals[i] - mean; var += d * d; }
    return make_f64(sqrt(var / (double)cnt));
}

ray_t* ray_dev(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (RAY_IS_ERR(x)) return x;
    if (ray_is_atom(x)) {
        if (is_null_atom(x)) return make_f64(NAN);
        if (is_numeric(x)) return make_f64(0.0);
        return ray_error("type", NULL);
    }
    if (ray_is_vec(x)) {
        int64_t len = ray_len(x);
        if (len == 0) return make_f64(NAN);
        /* Build f64 scratch with null sentinels → NaN */
        ray_t* scratch = ray_alloc(len * sizeof(double));
        if (!scratch) return ray_error("oom", NULL);
        scratch->type = RAY_F64; scratch->len = len;
        double* vals = (double*)ray_data(scratch);
        if (x->type == RAY_I64) {
            int64_t* d = (int64_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = (d[i] == INT64_MIN) ? NAN : (double)d[i];
        } else if (x->type == RAY_I32) {
            int32_t* d = (int32_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = (d[i] == INT32_MIN) ? NAN : (double)d[i];
        } else if (x->type == RAY_I16) {
            int16_t* d = (int16_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = (d[i] == INT16_MIN) ? NAN : (double)d[i];
        } else if (x->type == RAY_U8) {
            uint8_t* d = (uint8_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = (double)d[i];
        } else if (x->type == RAY_F64) {
            memcpy(vals, ray_data(x), (size_t)len * sizeof(double));
        } else {
            ray_release(scratch);
            return ray_error("type", NULL);
        }
        ray_t* result = dev_from_f64(vals, len);
        ray_release(scratch);
        return result;
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return make_f64(NAN);
    ray_t** elems = (ray_t**)ray_data(x);
    double sum = 0.0;
    int64_t cnt = 0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (!is_null_atom(elems[i])) { sum += as_f64(elems[i]); cnt++; }
    }
    if (cnt == 0) return make_f64(NAN);
    double mean = sum / (double)cnt;
    double var = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_null_atom(elems[i])) { double d = as_f64(elems[i]) - mean; var += d * d; }
    }
    return make_f64(sqrt(var / (double)cnt));
}

/* ══════════════════════════════════════════
 * Higher-order functions: map, pmap, fold, scan, filter, apply
 * ══════════════════════════════════════════ */

/* Helper: call a function object with 1 arg, returning result.
 * Handles UNARY, BINARY, LAMBDA types. Does not release fn or arg. */
static ray_t* call_fn1(ray_t* fn, ray_t* arg) {
    if (fn->type == RAY_UNARY) {
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        return f(arg);
    }
    if (fn->type == RAY_LAMBDA) {
        ray_t* args[1] = { arg };
        return call_lambda(fn, args, 1);
    }
    return ray_error("type", NULL);
}

/* Helper: call a function object with 2 args. Does not release fn or args. */
static ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b) {
    if (fn->type == RAY_BINARY) {
        ray_binary_fn f = (ray_binary_fn)(uintptr_t)fn->i64;
        if ((fn->attrs & RAY_FN_ATOMIC) && (is_collection(a) || is_collection(b)))
            return atomic_map_binary(f, a, b);
        return f(a, b);
    }
    if (fn->type == RAY_LAMBDA) {
        ray_t* args[2] = { a, b };
        return call_lambda(fn, args, 2);
    }
    if (fn->type == RAY_UNARY) {
        /* Partial application not supported, just call with first arg */
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        return f(a);
    }
    return ray_error("type", NULL);
}

/* (map fn val vec) — apply binary fn(val, elem) to each element of vec.
 * Also supports (map fn vec) for unary mapping. */
ray_t* ray_map(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;

    if (n == 2) {
        /* Unary map: (map fn vec) */
        ray_t* vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
        int64_t len = ray_len(vec);
        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
        result->type = RAY_LIST;
        result->len = len;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** elems = (ray_t**)ray_data(vec);
        for (int64_t i = 0; i < len; i++) {
            out[i] = call_fn1(fn, elems[i]);
            if (RAY_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) ray_release(out[j]);
                ray_release(result); if (_bx) ray_release(_bx);
                return out[i];
            }
        }
        if (_bx) ray_release(_bx);
        return result;
    }

    /* Binary map: (map fn val vec) — apply fn(val, elem) */
    ray_t* val = args[1];
    ray_t* vec = unbox_vec_arg(args[2], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    /* If vec is scalar, just call fn(val, vec) once */
    if (!is_list(vec)) {
        if (_bx) ray_release(_bx);
        return call_fn2(fn, val, args[2]);
    }
    int64_t len = ray_len(vec);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, val, elems[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (pmap fn val vec) — same as map, parallel not implemented yet (sequential fallback) */
ray_t* ray_pmap(ray_t** args, int64_t n) {
    return ray_map(args, n);
}

/* (fold fn vec) or (fold fn init vec) — reduce with binary fn */
ray_t* ray_fold(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* vec;
    ray_t* acc;
    ray_t* _bx = NULL;
    if (n == 2) {
        /* (fold fn vec) — use first element as initial value */
        vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
        int64_t len = ray_len(vec);
        if (len == 0) { if (_bx) ray_release(_bx); return ray_error("domain", NULL); }
        ray_t** elems = (ray_t**)ray_data(vec);
        ray_retain(elems[0]);
        acc = elems[0];
        for (int64_t i = 1; i < len; i++) {
            ray_t* next = call_fn2(fn, acc, elems[i]);
            ray_release(acc);
            if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
            acc = next;
        }
        if (_bx) ray_release(_bx);
        return acc;
    }

    /* (fold fn init vec) */
    ray_retain(args[1]);
    acc = args[1];
    vec = unbox_vec_arg(args[2], &_bx);
    if (RAY_IS_ERR(vec)) { ray_release(acc); return vec; }
    if (!is_list(vec)) { ray_release(acc); if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        ray_t* next = call_fn2(fn, acc, elems[i]);
        ray_release(acc);
        if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
        acc = next;
    }
    if (_bx) ray_release(_bx);
    return acc;
}

/* (scan fn vec) — running fold, returns vector of partial results */
ray_t* ray_scan_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    if (len == 0) {
        if (_bx) ray_release(_bx);
        ray_t* result = ray_alloc(0);
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 0;
        return result;
    }

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);

    ray_retain(elems[0]);
    out[0] = elems[0];
    for (int64_t i = 1; i < len; i++) {
        out[i] = call_fn2(fn, out[i - 1], elems[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (filter vec mask) — filter vector by boolean mask */
ray_t* ray_filter_fn(ray_t* vec, ray_t* mask) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (ray_is_lazy(mask)) mask = ray_lazy_materialize(mask);

    /* Table filter: apply mask to each column */
    if (vec->type == RAY_TABLE && ray_is_vec(mask) && mask->type == RAY_BOOL) {
        int64_t ncols = vec->len;
        int64_t nrows = ray_table_nrows(vec);
        if (nrows != mask->len) return ray_error("domain", NULL);
        ray_t* schema = ray_table_schema(vec);
        ray_t* result = ray_table_new((int32_t)ncols);
        if (RAY_IS_ERR(result)) return result;
        if (schema) { ray_retain(schema); *(ray_t**)ray_data(result) = schema; }
        ray_t** src_cols = (ray_t**)((char*)ray_data(vec) + sizeof(ray_t*));
        ray_t** dst_cols = (ray_t**)((char*)ray_data(result) + sizeof(ray_t*));
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* filtered = ray_filter_fn(src_cols[c], mask);
            if (RAY_IS_ERR(filtered)) { ray_release(result); return filtered; }
            dst_cols[c] = filtered;
        }
        result->len = ncols;
        return result;
    }

    /* String filter: STR atom + bool mask → filter characters */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR && ray_is_vec(mask) && mask->type == RAY_BOOL) {
        const char* sp = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        int64_t mlen = mask->len;
        if ((int64_t)slen != mlen) return ray_error("domain", NULL);
        bool* mb = (bool*)ray_data(mask);
        int64_t count = 0;
        for (int64_t i = 0; i < mlen; i++) if (mb[i]) count++;
        char buf[8192];
        if ((size_t)count > sizeof(buf)) return ray_error("limit", NULL);
        int64_t j = 0;
        for (int64_t i = 0; i < mlen; i++) {
            if (mb[i]) buf[j++] = sp[i];
        }
        return ray_str(buf, (size_t)count);
    }

    /* Fast path: typed vector + typed bool mask */
    if (ray_is_vec(vec) && ray_is_vec(mask) && mask->type == RAY_BOOL) {
        int64_t len = vec->len;
        int64_t mlen = mask->len;
        if (len != mlen) return ray_error("domain", NULL);
        bool* mb = (bool*)ray_data(mask);

        /* Count true values */
        int64_t count = 0;
        for (int64_t i = 0; i < len; i++) if (mb[i]) count++;

        int8_t vtype = vec->type;
        int esz = ray_elem_size(vtype);
        ray_t* result = ray_vec_new(vtype, count);
        if (RAY_IS_ERR(result)) return result;
        char* src = (char*)ray_data(vec);
        char* dst = (char*)ray_data(result);
        int64_t j = 0;
        for (int64_t i = 0; i < len; i++) {
            if (mb[i]) {
                memcpy(dst + j * esz, src + i * esz, esz);
                j++;
            }
        }
        result->len = count;
        return result;
    }

    /* Fallback: boxed list path */
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec = unbox_vec_arg(vec, &_bx1);
    if (RAY_IS_ERR(vec)) return vec;
    mask = unbox_vec_arg(mask, &_bx2);
    if (RAY_IS_ERR(mask)) { if (_bx1) ray_release(_bx1); return mask; }
    if (!is_list(vec) || !is_list(mask)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    int64_t mlen = ray_len(mask);
    if (len != mlen) return ray_error("domain", NULL);

    ray_t** velems = (ray_t**)ray_data(vec);
    ray_t** melems = (ray_t**)ray_data(mask);

    /* Validate mask is all booleans */
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type != -RAY_BOOL) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    }
    /* Count true values */
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->b8) count++;
    }

    ray_t* result = ray_alloc(count * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = count;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t j = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->b8) {
            ray_retain(velems[i]);
            out[j++] = velems[i];
        }
    }
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    return result;
}

/* (apply fn vec1 vec2) — zip-apply fn element-wise over two vectors */
ray_t* ray_apply(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];

    /* If both args are scalars, just call fn(a, b) once */
    if (ray_is_atom(args[1]) && ray_is_atom(args[2]))
        return call_fn2(fn, args[1], args[2]);

    ray_t *_bx1 = NULL, *_bx2 = NULL;
    ray_t* vec1 = unbox_vec_arg(args[1], &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    ray_t* vec2 = unbox_vec_arg(args[2], &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    int64_t len = len1 < len2 ? len1 : len2;

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, e1[i], e2[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result); if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2);
            return out[i];
        }
    }
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    return result;
}

/* ══════════════════════════════════════════
 * Collection operations
 * ══════════════════════════════════════════ */

/* Helper: compare two atoms for equality (value-based) */
static int atom_eq(ray_t* a, ray_t* b) {
    if (a->type != b->type) {
        if (is_numeric(a) && is_numeric(b)) {
            int a_null = is_null_atom(a);
            int b_null = is_null_atom(b);
            if (a_null && b_null) return 1;
            if (a_null || b_null) return 0;
            return as_f64(a) == as_f64(b);
        }
        return 0;
    }
    switch (a->type) {
    case -RAY_I64:  return a->i64 == b->i64;
    case -RAY_I32:  return a->i32 == b->i32;
    case -RAY_I16:  return a->i16 == b->i16;
    case -RAY_U8:   return a->u8 == b->u8;
    case -RAY_F64:  return a->f64 == b->f64;
    case -RAY_BOOL: return a->b8 == b->b8;
    case -RAY_SYM:  return a->i64 == b->i64;
    case -RAY_DATE: case -RAY_TIME:
        return a->i32 == b->i32;
    case -RAY_TIMESTAMP:
        return a->i64 == b->i64;
    case -RAY_GUID: {
        const uint8_t* ga = a->obj ? (const uint8_t*)ray_data(a->obj) : (const uint8_t*)ray_data((ray_t*)a);
        const uint8_t* gb = b->obj ? (const uint8_t*)ray_data(b->obj) : (const uint8_t*)ray_data((ray_t*)b);
        return memcmp(ga, gb, 16) == 0;
    }
    case -RAY_STR:
        return ray_str_len(a) == ray_str_len(b) &&
               memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
    default:
        /* Vector equality: same type and length, element-wise comparison */
        if (a->type > 0 && a->type == b->type && a->len == b->len) {
            int esz = ray_elem_size(a->type);
            return memcmp(ray_data(a), ray_data(b), (size_t)(a->len * esz)) == 0;
        }
        return 0;
    }
}

/* Forward declaration */
static ray_t* list_to_typed_vec(ray_t* list, int8_t orig_vec_type);

/* (distinct vec) — remove duplicates, preserving first occurrence */
ray_t* ray_distinct_fn(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);

    /* String distinct: unique chars, sorted */
    if (ray_is_atom(x) && (-x->type) == RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t slen = ray_str_len(x);
        if (slen == 0) { ray_retain(x); return x; }
        char uniq[256];
        int nu = 0;
        for (size_t i = 0; i < slen; i++) {
            int dup = 0;
            for (int j = 0; j < nu; j++) { if (uniq[j] == sp[i]) { dup = 1; break; } }
            if (!dup && nu < 256) uniq[nu++] = sp[i];
        }
        /* Sort */
        for (int i = 0; i < nu - 1; i++)
            for (int j = i + 1; j < nu; j++)
                if ((unsigned char)uniq[i] > (unsigned char)uniq[j]) { char t = uniq[i]; uniq[i] = uniq[j]; uniq[j] = t; }
        return ray_str(uniq, (size_t)nu);
    }

    /* Typed vector path: convert to boxed list, deduplicate, convert back */
    if (ray_is_vec(x)) {
        int64_t len = ray_len(x);
        if (len == 0) { ray_retain(x); return x; }
        int8_t vtype = x->type;

        /* Convert to boxed list for dedup */
        ray_t* blist = to_boxed_list(x);
        if (RAY_IS_ERR(blist)) return blist;
        ray_t** elems = (ray_t**)ray_data(blist);

        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) { ray_release(blist); return ray_error("oom", NULL); }
        result->type = RAY_LIST;
        ray_t** out = (ray_t**)ray_data(result);
        int64_t count = 0;

        for (int64_t i = 0; i < len; i++) {
            /* Skip null values */
            if (is_null_atom(elems[i])) continue;
            int dup = 0;
            for (int64_t j = 0; j < count; j++) {
                if (atom_eq(out[j], elems[i])) { dup = 1; break; }
            }
            if (!dup) {
                ray_retain(elems[i]);
                out[count++] = elems[i];
            }
        }
        result->len = count;

        /* Sort the deduped elements for numeric/char types (bubble sort, small n) */
        if (vtype != RAY_SYM && vtype != RAY_GUID && vtype != RAY_STR) {
            for (int64_t i = 0; i < count - 1; i++) {
                for (int64_t j = i + 1; j < count; j++) {
                    double a_v = as_f64(out[i]);
                    double b_v = as_f64(out[j]);
                    if (a_v > b_v) {
                        ray_t* tmp = out[i]; out[i] = out[j]; out[j] = tmp;
                    }
                }
            }
        }

        /* Convert back to typed vector (promote i16 to i64 for set operations) */
        int8_t out_type = (vtype == RAY_I16) ? RAY_I64 : vtype;
        ray_t* tvec = list_to_typed_vec(result, out_type);
        ray_release(blist);
        return tvec;
    }

    ray_t* _bx = NULL;
    x = unbox_vec_arg(x, &_bx);
    if (RAY_IS_ERR(x)) return x;
    if (!is_list(x)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(x);
    if (len == 0) { if (_bx) ray_release(_bx); ray_retain(x); return x; }
    ray_t** elems = (ray_t**)ray_data(x);

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len; i++) {
        int dup = 0;
        for (int64_t j = 0; j < count; j++) {
            if (atom_eq(out[j], elems[i])) { dup = 1; break; }
        }
        if (!dup) {
            ray_retain(elems[i]);
            out[count++] = elems[i];
        }
    }
    result->len = count;
    /* Sort: atoms before vectors (scalars have negative type) */
    for (int64_t i = 0; i < count - 1; i++) {
        for (int64_t j = i + 1; j < count; j++) {
            int ai = ray_is_atom(out[i]);
            int aj = ray_is_atom(out[j]);
            if (!ai && aj) {
                ray_t* tmp = out[i]; out[i] = out[j]; out[j] = tmp;
            }
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (in val vec) — check membership */
ray_t* ray_in(ray_t* val, ray_t* vec) {
    if (ray_is_lazy(val)) val = ray_lazy_materialize(val);
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* STR in STR: for each char of val, check membership in vec string */
    if (ray_is_atom(val) && (-val->type) == RAY_STR && ray_is_atom(vec) && (-vec->type) == RAY_STR) {
        const char* vp = ray_str_ptr(val);
        size_t vlen = ray_str_len(val);
        const char* sp = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        ray_t* result = ray_vec_new(RAY_BOOL, (int64_t)vlen);
        if (RAY_IS_ERR(result)) return result;
        result->len = (int64_t)vlen;
        bool* out = (bool*)ray_data(result);
        for (size_t i = 0; i < vlen; i++) {
            out[i] = false;
            for (size_t j = 0; j < slen; j++) {
                if (vp[i] == sp[j]) { out[i] = true; break; }
            }
        }
        return result;
    }
    /* Scalar in scalar: equality check */
    if (ray_is_atom(val) && ray_is_atom(vec))
        return make_bool(atom_eq(val, vec) ? 1 : 0);
    /* STR in LIST: for each char of val, check membership in list elements */
    if (ray_is_atom(val) && (-val->type) == RAY_STR && (vec->type == RAY_LIST)) {
        const char* vp = ray_str_ptr(val);
        size_t vlen = ray_str_len(val);
        ray_t* result = ray_vec_new(RAY_BOOL, (int64_t)vlen);
        if (RAY_IS_ERR(result)) return result;
        result->len = (int64_t)vlen;
        bool* out_b = (bool*)ray_data(result);
        ray_t** list_elems = (ray_t**)ray_data(vec);
        int64_t list_len = ray_len(vec);
        for (size_t i = 0; i < vlen; i++) {
            out_b[i] = false;
            ray_t* ch = ray_str(&vp[i], 1);
            for (int64_t j = 0; j < list_len; j++) {
                if (atom_eq(ch, list_elems[j])) { out_b[i] = true; break; }
            }
            ray_release(ch);
        }
        return result;
    }
    /* Vector val: map in over each element */
    if (is_collection(val) && !ray_is_atom(val)) {
        int64_t vlen = ray_len(val);
        if (vlen == 0) {
            /* Empty collection → return empty list */
            return ray_list_new(0);
        }
        /* Probe first element to check if result is scalar or vector */
        int alloc0 = 0;
        ray_t* e0 = collection_elem(val, 0, &alloc0);
        if (RAY_IS_ERR(e0)) return e0;
        ray_t* r0 = ray_in(e0, vec);
        if (alloc0) ray_release(e0);
        if (RAY_IS_ERR(r0)) return r0;
        if (ray_is_atom(r0) && r0->type == -RAY_BOOL) {
            /* All results are scalar bools — use typed bool vector */
            ray_t* result = ray_vec_new(RAY_BOOL, vlen);
            if (RAY_IS_ERR(result)) { ray_release(r0); return result; }
            result->len = vlen;
            bool* out = (bool*)ray_data(result);
            out[0] = r0->b8;
            ray_release(r0);
            for (int64_t i = 1; i < vlen; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(result); return elem; }
                ray_t* r = ray_in(elem, vec);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(r)) { ray_release(result); return r; }
                out[i] = r->b8;
                ray_release(r);
            }
            return result;
        } else {
            /* Results are non-scalar — collect as list */
            ray_t* result = ray_list_new(vlen);
            if (RAY_IS_ERR(result)) { ray_release(r0); return result; }
            result = ray_list_append(result, r0);
            ray_release(r0);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 1; i < vlen; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(result); return elem; }
                ray_t* r = ray_in(elem, vec);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(r)) { ray_release(result); return r; }
                result = ray_list_append(result, r);
                ray_release(r);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
    }
    ray_t* _bx = NULL;
    vec = unbox_vec_arg(vec, &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(val, elems[i])) { if (_bx) ray_release(_bx); return make_bool(1); }
    }
    if (_bx) ray_release(_bx);
    return make_bool(0);
}

/* Helper: convert a boxed list result back to a typed vector if the original was typed */
static ray_t* list_to_typed_vec(ray_t* list, int8_t orig_vec_type) {
    if (!list || RAY_IS_ERR(list) || list->type != RAY_LIST) return list;
    int64_t count = list->len;
    /* For SYM and STR types, only convert when empty (to get [] instead of ()) */
    if (orig_vec_type == RAY_SYM || orig_vec_type == RAY_STR) {
        if (count == 0) {
            ray_release(list);
            return ray_vec_new(orig_vec_type, 0);
        }
        return list; /* Keep as boxed list for non-empty SYM/STR */
    }
    ray_t* vec = ray_vec_new(orig_vec_type, count);
    if (RAY_IS_ERR(vec)) return vec;
    vec->len = count;
    ray_t** elems = (ray_t**)ray_data(list);
    for (int64_t i = 0; i < count; i++)
        store_typed_elem(vec, i, elems[i]);
    /* Release the list (ray_release_owned_refs handles child elements) */
    ray_release(list);
    return vec;
}

/* (except vec1 vec2) — elements in vec1 not in vec2 */
ray_t* ray_except(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);
    int8_t orig_type = ray_is_vec(vec1) ? vec1->type : -1;
    /* Handle scalar vec2: treat as single-element filter */
    if (ray_is_atom(vec2) && !ray_is_atom(vec1)) {
        ray_t *_bx1 = NULL;
        ray_t* v1 = unbox_vec_arg(vec1, &_bx1);
        if (RAY_IS_ERR(v1)) return v1;
        if (!is_list(v1)) { if (_bx1) ray_release(_bx1); return ray_error("type", NULL); }
        int64_t len = ray_len(v1);
        ray_t** e1 = (ray_t**)ray_data(v1);
        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) { if (_bx1) ray_release(_bx1); return ray_error("oom", NULL); }
        result->type = RAY_LIST;
        ray_t** out = (ray_t**)ray_data(result);
        int64_t count = 0;
        for (int64_t i = 0; i < len; i++) {
            if (!atom_eq(e1[i], vec2)) {
                ray_retain(e1[i]);
                out[count++] = e1[i];
            }
        }
        result->len = count;
        if (_bx1) ray_release(_bx1);
        if (orig_type >= 0 && orig_type != RAY_SYM && orig_type != RAY_STR) {
            ray_t* vec = ray_vec_new(orig_type, count);
            if (!RAY_IS_ERR(vec)) {
                vec->len = count;
                ray_t** elems = (ray_t**)ray_data(result);
                for (int64_t i = 0; i < count; i++) {
                    store_typed_elem(vec, i, elems[i]);
                    ray_release(elems[i]);
                }
                result->len = 0;
                ray_release(result);
                return vec;
            }
        }
        return result;
    }
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec1 = unbox_vec_arg(vec1, &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    vec2 = unbox_vec_arg(vec2, &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc(len1 * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len1; i++) {
        int found = 0;
        for (int64_t j = 0; j < len2; j++) {
            if (atom_eq(e1[i], e2[j])) { found = 1; break; }
        }
        if (!found) {
            ray_retain(e1[i]);
            out[count++] = e1[i];
        }
    }
    result->len = count;
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    /* Convert back to typed vector if original was typed */
    if (orig_type >= 0 && orig_type != RAY_SYM && orig_type != RAY_STR) {
        ray_t* vec = ray_vec_new(orig_type, count);
        if (!RAY_IS_ERR(vec)) {
            vec->len = count;
            ray_t** elems = (ray_t**)ray_data(result);
            for (int64_t i = 0; i < count; i++) {
                store_typed_elem(vec, i, elems[i]);
                ray_release(elems[i]);
            }
            result->len = 0; /* prevent double-free of elements */
            ray_release(result);
            return vec;
        }
    }
    if (orig_type >= 0 && count == 0) {
        ray_release(result);
        return ray_vec_new(orig_type, 0);
    }
    return result;
}

/* (union vec1 vec2) — elements in vec1 + elements in vec2 not already in vec1 */
ray_t* ray_union(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);
    int8_t orig_type = ray_is_vec(vec1) ? vec1->type : -1;
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec1 = unbox_vec_arg(vec1, &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    vec2 = unbox_vec_arg(vec2, &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc((len1 + len2) * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len1; i++) {
        ray_retain(e1[i]);
        out[count++] = e1[i];
    }
    for (int64_t i = 0; i < len2; i++) {
        int found = 0;
        for (int64_t j = 0; j < count; j++) {
            if (atom_eq(out[j], e2[i])) { found = 1; break; }
        }
        if (!found) {
            ray_retain(e2[i]);
            out[count++] = e2[i];
        }
    }
    result->len = count;
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    if (orig_type >= 0 && orig_type != RAY_SYM && orig_type != RAY_STR) {
        ray_t* vec = ray_vec_new(orig_type, count);
        if (!RAY_IS_ERR(vec)) {
            vec->len = count;
            ray_t** elems = (ray_t**)ray_data(result);
            for (int64_t i = 0; i < count; i++) {
                store_typed_elem(vec, i, elems[i]);
                ray_release(elems[i]);
            }
            result->len = 0;
            ray_release(result);
            return vec;
        }
    }
    if (orig_type >= 0 && count == 0) {
        ray_release(result);
        return ray_vec_new(orig_type, 0);
    }
    return result;
}

/* (sect vec1 vec2) — intersection: elements in both */
ray_t* ray_sect(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);
    int8_t orig_type = ray_is_vec(vec1) ? vec1->type : -1;
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec1 = unbox_vec_arg(vec1, &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    vec2 = unbox_vec_arg(vec2, &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc(len1 * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len1; i++) {
        for (int64_t j = 0; j < len2; j++) {
            if (atom_eq(e1[i], e2[j])) {
                ray_retain(e1[i]);
                out[count++] = e1[i];
                break;
            }
        }
    }
    result->len = count;
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    if (orig_type >= 0 && orig_type != RAY_SYM && orig_type != RAY_STR) {
        ray_t* vec = ray_vec_new(orig_type, count);
        if (!RAY_IS_ERR(vec)) {
            vec->len = count;
            ray_t** elems = (ray_t**)ray_data(result);
            for (int64_t i = 0; i < count; i++) {
                store_typed_elem(vec, i, elems[i]);
                ray_release(elems[i]);
            }
            result->len = 0;
            ray_release(result);
            return vec;
        }
    }
    if (orig_type >= 0 && count == 0) {
        ray_release(result);
        return ray_vec_new(orig_type, 0);
    }
    return result;
}

/* (take vec n) — first n elements (positive) or last |n| elements (negative) */
ray_t* ray_take(ray_t* vec, ray_t* n_obj) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* Range take: (take collection [start amount]) — slice from start for amount elements */
    if (ray_is_vec(n_obj) && n_obj->type == RAY_I64 && ray_len(n_obj) == 2) {
        int64_t* idx = (int64_t*)ray_data(n_obj);
        int64_t start = idx[0];
        int64_t amount = idx[1];
        if (amount < 0) return ray_error("length", NULL);

        /* Table range take */
        if (vec->type == RAY_TABLE) {
            int64_t ncols = ray_table_ncols(vec);
            ray_t* result = ray_table_new(ncols);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 0; i < ncols; i++) {
                ray_t* col = ray_table_get_col_idx(vec, i);
                int64_t name_id = ray_table_col_name(vec, i);
                ray_t* taken = ray_take(col, n_obj);
                if (RAY_IS_ERR(taken)) { ray_release(result); return taken; }
                result = ray_table_add_col(result, name_id, taken);
                if (RAY_IS_ERR(result)) { ray_release(taken); return result; }
            }
            return result;
        }

        /* String range take */
        if (ray_is_atom(vec) && (-vec->type) == RAY_STR) {
            const char* s = ray_str_ptr(vec);
            int64_t slen = (int64_t)ray_str_len(vec);
            if (start < 0) start = slen + start;
            if (start < 0) start = 0;
            if (start >= slen) return ray_str("", 0);
            int64_t end = start + amount;
            if (end > slen) end = slen;
            return ray_str(s + start, (size_t)(end - start));
        }

        /* Typed vector range take */
        if (ray_is_vec(vec)) {
            int64_t len = ray_len(vec);
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
            if (start >= len) return ray_vec_new(vec->type, 0);
            int64_t end = start + amount;
            if (end > len) end = len;
            int64_t count = end - start;
            int8_t vtype = vec->type;
            int esz = ray_elem_size(vtype);
            ray_t* result = ray_vec_new(vtype, count);
            if (RAY_IS_ERR(result)) return result;
            result->len = count;
            memcpy(ray_data(result), (char*)ray_data(vec) + start * esz, (size_t)(count * esz));
            return result;
        }

        /* Dict range take */
        if (vec->type == RAY_LIST && (vec->attrs & RAY_ATTR_DICT)) {
            int64_t len = ray_len(vec) / 2; /* dict has key-value pairs */
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
            if (start >= len) {
                ray_t* result = ray_alloc(0);
                result->type = RAY_LIST;
                result->attrs = RAY_ATTR_DICT;
                result->len = 0;
                return result;
            }
            int64_t end = start + amount;
            if (end > len) end = len;
            int64_t count = end - start;
            ray_t** elems = (ray_t**)ray_data(vec);
            ray_t* result = ray_alloc(count * 2 * sizeof(ray_t*));
            if (!result) return ray_error("oom", NULL);
            result->type = RAY_LIST;
            result->attrs = RAY_ATTR_DICT;
            result->len = count * 2;
            ray_t** out = (ray_t**)ray_data(result);
            for (int64_t i = 0; i < count; i++) {
                ray_retain(elems[(start + i) * 2]);
                ray_retain(elems[(start + i) * 2 + 1]);
                out[i * 2] = elems[(start + i) * 2];
                out[i * 2 + 1] = elems[(start + i) * 2 + 1];
            }
            return result;
        }

        /* Boxed list range take */
        if (vec->type == RAY_LIST) {
            int64_t len = ray_len(vec);
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
            if (start >= len) {
                ray_t* result = ray_alloc(0);
                result->type = RAY_LIST;
                result->len = 0;
                return result;
            }
            int64_t end = start + amount;
            if (end > len) end = len;
            int64_t count = end - start;
            ray_t** elems = (ray_t**)ray_data(vec);
            ray_t* result = ray_alloc(count * sizeof(ray_t*));
            if (!result) return ray_error("oom", NULL);
            result->type = RAY_LIST;
            result->len = count;
            ray_t** out = (ray_t**)ray_data(result);
            for (int64_t i = 0; i < count; i++) {
                ray_retain(elems[start + i]);
                out[i] = elems[start + i];
            }
            return result;
        }

        return ray_error("type", NULL);
    }
    /* Char take: (take 'a' n) → string of n copies of char */
    if (ray_is_atom(vec) && vec->type == -RAY_STR && ray_str_len(vec) == 1 && ray_is_atom(n_obj) && is_numeric(n_obj)) {
        int64_t n = as_i64(n_obj);
        int64_t count = n < 0 ? -n : n;
        char buf[8192];
        if (count > (int64_t)sizeof(buf)) return ray_error("limit", NULL);
        for (int64_t i = 0; i < count; i++) buf[i] = vec->sdata[0];
        return ray_str(buf, (size_t)count);
    }
    /* Scalar take: (take value n) → repeat value n times */
    if (ray_is_atom(vec) && (-vec->type) != RAY_STR && ray_is_atom(n_obj) && is_numeric(n_obj)) {
        int64_t n = as_i64(n_obj);
        int64_t count = n < 0 ? -n : n;
        int8_t vtype = -(vec->type);
        ray_t* result = ray_vec_new(vtype, count);
        if (RAY_IS_ERR(result)) return result;
        result->len = count;
        for (int64_t i = 0; i < count; i++)
            store_typed_elem(result, i, vec);
        return result;
    }
    /* String take: (take "hello" 3) → "hel", with wrapping extension */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR && ray_is_atom(n_obj) && is_numeric(n_obj)) {
        const char* s = ray_str_ptr(vec);
        int64_t slen = (int64_t)ray_str_len(vec);
        int64_t n = as_i64(n_obj);
        int64_t abs_n = n < 0 ? -n : n;
        char buf[8192];
        if (abs_n > (int64_t)sizeof(buf)) return ray_error("limit", NULL);
        if (slen == 0) {
            return ray_str("", 0);
        }
        if (n >= 0) {
            for (int64_t i = 0; i < abs_n; i++) buf[i] = s[i % slen];
        } else {
            for (int64_t i = 0; i < abs_n; i++) {
                int64_t si = slen - (abs_n - i) % slen;
                if (si == slen) si = 0;
                buf[i] = s[si];
            }
        }
        return ray_str(buf, (size_t)abs_n);
    }
    /* Table take: apply take to each column */
    if (vec->type == RAY_TABLE && is_numeric(n_obj)) {
        int64_t ncols = ray_table_ncols(vec);
        ray_t* result = ray_table_new(ncols);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < ncols; i++) {
            ray_t* col = ray_table_get_col_idx(vec, i);
            int64_t name_id = ray_table_col_name(vec, i);
            ray_t* taken = ray_take(col, n_obj);
            if (RAY_IS_ERR(taken)) { ray_release(result); return taken; }
            result = ray_table_add_col(result, name_id, taken);
            if (RAY_IS_ERR(result)) { ray_release(taken); return result; }
        }
        return result;
    }
    /* Typed vector take with extension */
    if (ray_is_vec(vec) && is_numeric(n_obj)) {
        int64_t len = ray_len(vec);
        int64_t n = as_i64(n_obj);
        int64_t abs_n = n < 0 ? -n : n;
        int8_t vtype = vec->type;
        int esz = ray_elem_size(vtype);
        ray_t* result = ray_vec_new(vtype, abs_n);
        if (RAY_IS_ERR(result)) return result;
        result->len = abs_n;
        char* src = (char*)ray_data(vec);
        char* dst = (char*)ray_data(result);
        if (len == 0) {
            memset(dst, 0, (size_t)(abs_n * esz));
        } else if (n >= 0) {
            for (int64_t i = 0; i < abs_n; i++)
                memcpy(dst + i * esz, src + (i % len) * esz, esz);
        } else {
            /* Negative: take from end with wrap */
            for (int64_t i = 0; i < abs_n; i++) {
                int64_t si = len - (abs_n - i) % len;
                if (si == len) si = 0;
                memcpy(dst + i * esz, src + si * esz, esz);
            }
        }
        return result;
    }
    ray_t* _bx = NULL;
    vec = unbox_vec_arg(vec, &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec) || !is_numeric(n_obj))
        { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    int64_t n = as_i64(n_obj);
    ray_t** elems = (ray_t**)ray_data(vec);

    int64_t abs_n = n < 0 ? -n : n;
    /* For dicts, n counts key-value pairs (each pair = 2 elements) */
    int is_dict = (vec->attrs & RAY_ATTR_DICT) ? 1 : 0;
    int64_t elem_count = is_dict ? abs_n * 2 : abs_n;
    (void)is_dict; /* len is already in elements for dict storage */
    ray_t* result = ray_alloc(elem_count * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    if (is_dict) result->attrs |= RAY_ATTR_DICT;
    result->len = elem_count;
    ray_t** out = (ray_t**)ray_data(result);
    if (len == 0) {
        result->len = 0;
    } else if (n >= 0) {
        for (int64_t i = 0; i < elem_count; i++) {
            ray_retain(elems[i % len]);
            out[i] = elems[i % len];
        }
    } else {
        for (int64_t i = 0; i < elem_count; i++) {
            int64_t si = len - (elem_count - i) % len;
            if (si == len) si = 0;
            ray_retain(elems[si]);
            out[i] = elems[si];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (at vec idx) or (at table 'col) — index into vector or table */
ray_t* ray_at(ray_t* vec, ray_t* idx) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* Table column access by symbol key */
    if (vec->type == RAY_TABLE && idx->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(vec, idx->i64);
        if (!col) return ray_error("domain", NULL);
        int8_t ctype = col->type;
        /* Return typed vectors directly for temporal/GUID types that need
         * to work with arithmetic and other vector operations.
         * Other types are converted to boxed lists for backward compat. */
        if (ctype == RAY_TIME || ctype == RAY_DATE || ctype == RAY_TIMESTAMP ||
            ctype == RAY_GUID) {
            ray_retain(col);
            return col;
        }
        /* Convert typed column vector to a Rayfall list */
        int64_t nrows = col->len;
        ray_t* result = ray_alloc(nrows * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = nrows;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < nrows; i++) {
            if (ctype == RAY_I64) {
                out[i] = make_i64(((int64_t*)ray_data(col))[i]);
            } else if (ctype == RAY_F64) {
                out[i] = make_f64(((double*)ray_data(col))[i]);
            } else if (ctype == RAY_BOOL) {
                out[i] = make_bool(((uint8_t*)ray_data(col))[i]);
            } else if (ctype == RAY_SYM) {
                ray_t* s = ray_alloc(0);
                if (!s) { ray_release(result); return ray_error("oom", NULL); }
                s->type = -RAY_SYM;
                s->i64 = ((int64_t*)ray_data(col))[i];
                out[i] = s;
            } else if (ctype == RAY_STR) {
                size_t slen;
                const char *sptr = ray_str_vec_get(col, i, &slen);
                out[i] = ray_str(sptr ? sptr : "", sptr ? slen : 0);
            } else {
                /* Fallback: use collection_elem for any other typed vector */
                int alloc = 0;
                out[i] = collection_elem(col, i, &alloc);
            }
            if (RAY_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) ray_release(out[j]);
                ray_release(result);
                return out[i];
            }
        }
        return result;
    }

    /* Table row access by integer index: (at table 0) → {col1: val1, col2: val2} */
    if (vec->type == RAY_TABLE && ray_is_atom(idx) &&
        (idx->type == -RAY_I64 || idx->type == -RAY_I32 ||
         idx->type == -RAY_I16 || idx->type == -RAY_U8)) {
        int64_t row = as_i64(idx);
        int64_t nrows = ray_table_nrows(vec);
        if (row < 0 || row >= nrows) return ray_error("domain", NULL);
        int64_t ncols = vec->len;
        ray_t* schema = ray_table_schema(vec);
        /* Columns are stored after the schema pointer in data region */
        ray_t** cols = (ray_t**)((char*)ray_data(vec) + sizeof(ray_t*));
        /* Build a dict: alternating key, value pairs */
        ray_t* dict = ray_list_new(0);
        if (RAY_IS_ERR(dict)) return dict;
        dict->attrs |= RAY_ATTR_DICT;
        for (int64_t c = 0; c < ncols; c++) {
            /* Add key */
            int64_t key_id = schema ? ((int64_t*)ray_data(schema))[c] : c;
            ray_t* k = ray_sym(key_id);
            if (RAY_IS_ERR(k)) { ray_release(dict); return k; }
            dict = ray_list_append(dict, k);
            ray_release(k);
            if (RAY_IS_ERR(dict)) return dict;
            /* Add value */
            int alloc = 0;
            ray_t* val = collection_elem(cols[c], row, &alloc);
            if (RAY_IS_ERR(val)) { ray_release(dict); return val; }
            dict = ray_list_append(dict, val);
            if (alloc) ray_release(val);
            if (RAY_IS_ERR(dict)) return dict;
        }
        return dict;
    }

    /* Dict key access: (at dict key) → value or 0Nl if missing */
    if (vec->type == RAY_LIST && (vec->attrs & RAY_ATTR_DICT) && idx->type == -RAY_SYM) {
        ray_t** items = (ray_t**)ray_data(vec);
        int64_t n2 = vec->len;
        for (int64_t i = 0; i < n2; i += 2) {
            if (items[i]->type == -RAY_SYM && items[i]->i64 == idx->i64) {
                ray_retain(items[i + 1]);
                return items[i + 1];
            }
        }
        return make_i64(INT64_MIN); /* 0Nl for missing key */
    }

    /* String indexing: (at "hello" 1) → 'e', (at "hello" [0 4]) → "ho" */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR) {
        const char* s = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        if (is_collection(idx)) {
            /* Multiple indices → build string from chars */
            int64_t idxlen = ray_len(idx);
            char buf[8192];
            if ((size_t)idxlen > sizeof(buf)) return ray_error("limit", NULL);
            for (int64_t j = 0; j < idxlen; j++) {
                int alloc = 0;
                ray_t* ie = collection_elem(idx, j, &alloc);
                int64_t k = as_i64(ie);
                if (alloc) ray_release(ie);
                if (k < 0 || (size_t)k >= slen) return ray_error("domain", NULL);
                buf[j] = s[k];
            }
            return ray_str(buf, (size_t)idxlen);
        }
        int64_t i = as_i64(idx);
        if (i < 0 || (size_t)i >= slen) return ray_error("domain", NULL);
        /* Return 1-char string atom */
        return ray_str(&s[i], 1);
    }

    /* Vector index: (at vec [i j k]) → vector of values */
    if (is_collection(idx) && idx->type != -RAY_SYM) {
        int64_t idxlen = ray_len(idx);
        ray_t* result = ray_alloc(idxlen * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = idxlen;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t j = 0; j < idxlen; j++) {
            int alloc = 0;
            ray_t* idx_elem = collection_elem(idx, j, &alloc);
            if (RAY_IS_ERR(idx_elem)) {
                for (int64_t k = 0; k < j; k++) ray_release(out[k]);
                ray_release(result);
                return idx_elem;
            }
            ray_t* sub_idx = idx_elem;
            ray_t* val = ray_at(vec, sub_idx);
            if (alloc) ray_release(idx_elem);
            if (RAY_IS_ERR(val)) {
                for (int64_t k = 0; k < j; k++) ray_release(out[k]);
                ray_release(result);
                return val;
            }
            out[j] = val;
        }
        return result;
    }

    if (idx->type != -RAY_I64 && idx->type != -RAY_I32 &&
        idx->type != -RAY_I16 && idx->type != -RAY_U8)
        return ray_error("type", NULL);
    int64_t i = as_i64(idx);

    /* Typed vector: extract element directly */
    if (ray_is_vec(vec)) {
        int64_t len = ray_len(vec);
        if (i < 0 || i >= len) return make_i64(INT64_MIN); /* out of bounds → 0Nl */
        int alloc = 0;
        ray_t* elem = collection_elem(vec, i, &alloc);
        /* collection_elem always allocates for typed vecs, so elem is owned */
        return elem;
    }

    if (!is_list(vec)) return ray_error("type", NULL);
    int64_t len = ray_len(vec);
    if (i < 0 || i >= len) return make_i64(INT64_MIN); /* out of bounds → 0Nl */
    ray_t* elem = ((ray_t**)ray_data(vec))[i];
    ray_retain(elem);
    return elem;
}

/* (find vec val) — index of first occurrence, or -1 */
ray_t* ray_find(ray_t* vec, ray_t* val) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (ray_is_lazy(val)) val = ray_lazy_materialize(val);
    /* String find: (find "hello" 'l') → index of char in string */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR && ray_is_atom(val) && val->type == -RAY_STR && ray_str_len(val) == 1) {
        const char* s = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        char c = ray_str_ptr(val)[0];
        for (size_t i = 0; i < slen; i++) {
            if (s[i] == c) return make_i64((int64_t)i);
        }
        return make_i64(INT64_MIN);
    }
    /* Vector val: (find vec [v1 v2]) → [idx1 idx2] */
    if (is_collection(val)) {
        /* If vec is empty, return empty vector */
        if (is_collection(vec) && ray_len(vec) == 0)
            return ray_vec_new(RAY_I64, 0);
        int64_t vlen = ray_len(val);
        ray_t* result = ray_alloc(vlen * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = vlen;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t j = 0; j < vlen; j++) {
            int alloc = 0;
            ray_t* ve = collection_elem(val, j, &alloc);
            out[j] = ray_find(vec, ve);
            if (alloc) ray_release(ve);
            if (RAY_IS_ERR(out[j])) {
                for (int64_t k = 0; k < j; k++) ray_release(out[k]);
                ray_release(result);
                return out[j];
            }
        }
        return result;
    }
    ray_t* _bx = NULL;
    vec = unbox_vec_arg(vec, &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(elems[i], val)) { if (_bx) ray_release(_bx); return make_i64(i); }
    }
    if (_bx) ray_release(_bx);
    return make_i64(INT64_MIN); /* 0Nl = not found */
}

/* (til n) — generate integer sequence [0, 1, ..., n-1] */
static void til_fill(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    int64_t* out = (int64_t*)ctx;
    for (int64_t i = start; i < end; i++)
        out[i] = i;
}

ray_t* ray_til(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_I64) return ray_error("type", NULL);
    int64_t n = x->i64;
    if (n < 0) return ray_error("domain", NULL);
    if (n == 0) return ray_vec_new(RAY_I64, 0);

    ray_t* vec = ray_vec_new(RAY_I64, n);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    vec->len = n;
    int64_t* out = (int64_t*)ray_data(vec);
    ray_pool_dispatch(ray_pool_get(), til_fill, out, n);
    return vec;
}

/* (reverse vec) — reverse a vector */
ray_t* ray_reverse(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    ray_t* _bx = NULL;
    x = unbox_vec_arg(x, &_bx);
    if (RAY_IS_ERR(x)) return x;
    if (!is_list(x)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(x);
    ray_t** elems = (ray_t**)ray_data(x);

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < len; i++) {
        ray_retain(elems[len - 1 - i]);
        out[i] = elems[len - 1 - i];
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* ══════════════════════════════════════════
 * Sorting builtins
 * ══════════════════════════════════════════ */

/* Reorder vector elements by an index array */
static ray_t* gather_by_idx(ray_t* vec, int64_t* idx, int64_t n) {
    int8_t type = vec->type;

    if (type == RAY_STR) {
        ray_t* result = ray_vec_new(type, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        for (int64_t i = 0; i < n; i++) {
            size_t slen;
            const char* s = ray_str_vec_get(vec, idx[i], &slen);
            result = ray_str_vec_set(result, i, s ? s : "", s ? slen : 0);
        }
        return result;
    }

    /* RAY_SYM: use adaptive width, create with matching width */
    if (type == RAY_SYM) {
        uint8_t w = vec->attrs & RAY_SYM_W_MASK;
        ray_t* result = ray_sym_vec_new(w, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t esz = (uint8_t)RAY_SYM_ELEM(w);
        char* src = (char*)ray_data(vec);
        char* dst = (char*)ray_data(result);
        switch (esz) {
        case 8: for (int64_t i = 0; i < n; i++) memcpy(dst + i*8, src + idx[i]*8, 8); break;
        case 4: for (int64_t i = 0; i < n; i++) memcpy(dst + i*4, src + idx[i]*4, 4); break;
        case 2: for (int64_t i = 0; i < n; i++) memcpy(dst + i*2, src + idx[i]*2, 2); break;
        case 1: for (int64_t i = 0; i < n; i++) dst[i] = src[idx[i]]; break;
        default: for (int64_t i = 0; i < n; i++) memcpy(dst + i*esz, src + idx[i]*esz, esz); break;
        }
        if (vec->sym_dict) {
            ray_retain(vec->sym_dict);
            result->sym_dict = vec->sym_dict;
        }
        return result;
    }

    ray_t* result = ray_vec_new(type, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    uint8_t esz = ray_type_sizes[type];
    char* src = (char*)ray_data(vec);
    char* dst = (char*)ray_data(result);
    /* Typed gather — compiler constant esz enables vectorization, alias-safe */
    switch (esz) {
    case 8: for (int64_t i = 0; i < n; i++) memcpy(dst + i*8, src + idx[i]*8, 8); break;
    case 4: for (int64_t i = 0; i < n; i++) memcpy(dst + i*4, src + idx[i]*4, 4); break;
    case 2: for (int64_t i = 0; i < n; i++) memcpy(dst + i*2, src + idx[i]*2, 2); break;
    case 1: for (int64_t i = 0; i < n; i++) dst[i] = src[idx[i]]; break;
    default: for (int64_t i = 0; i < n; i++) memcpy(dst + i*esz, src + idx[i]*esz, esz); break;
    case 16: for (int64_t i = 0; i < n; i++) memcpy(dst + i*16, src + idx[i]*16, 16); break;
    }

    return result;
}

/* (asc v) — sort vector ascending */
static ray_t* ray_asc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", "asc expects a vector");

    int64_t n = ray_len(x);
    if (n <= 1) { ray_retain(x); return x; }

    uint8_t desc = 0;
    ray_t* idx = ray_sort_indices(&x, &desc, NULL, 1, n);
    if (RAY_IS_ERR(idx)) return idx;

    ray_t* result = gather_by_idx(x, (int64_t*)ray_data(idx), n);
    ray_release(idx);
    return result;
}

/* (desc v) — sort vector descending */
static ray_t* ray_desc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", "desc expects a vector");

    int64_t n = ray_len(x);
    if (n <= 1) { ray_retain(x); return x; }

    uint8_t desc = 1;
    ray_t* idx = ray_sort_indices(&x, &desc, NULL, 1, n);
    if (RAY_IS_ERR(idx)) return idx;

    ray_t* result = gather_by_idx(x, (int64_t*)ray_data(idx), n);
    ray_release(idx);
    return result;
}

/* (iasc v) — ascending sort indices */
static ray_t* ray_iasc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "iasc expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 0;
    return ray_sort_indices(&x, &desc, NULL, 1, n);
}

/* (idesc v) — descending sort indices */
static ray_t* ray_idesc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "idesc expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 1;
    return ray_sort_indices(&x, &desc, NULL, 1, n);
}

/* (rank v) — rank positions (inverse permutation of iasc) */
static ray_t* ray_rank_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "rank expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 0;
    ray_t* idx = ray_sort_indices(&x, &desc, NULL, 1, n);
    if (RAY_IS_ERR(idx)) return idx;

    ray_t* result = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(result)) { ray_release(idx); return result; }
    result->len = n;

    int64_t* idx_data = (int64_t*)ray_data(idx);
    int64_t* rank_data = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < n; i++)
        rank_data[idx_data[i]] = i;

    ray_release(idx);
    return result;
}

/* Helper: resolve key symbols to table columns for xasc/xdesc */
static ray_t* sort_table_by_keys(ray_t* tbl, ray_t* keys, uint8_t descending) {
    if (!tbl || tbl->type != RAY_TABLE)
        return ray_error("type", "xasc/xdesc expects a table as first argument");

    /* keys can be a SYM atom, a SYM vector, or a list of SYM atoms */
    int64_t n_keys = 0;
    int64_t key_ids[16];

    if (keys->type == -RAY_SYM) {
        /* Single symbol atom */
        key_ids[0] = keys->i64;
        n_keys = 1;
    } else if (keys->type == RAY_SYM) {
        /* SYM vector */
        int64_t* syms = (int64_t*)ray_data(keys);
        n_keys = ray_len(keys);
        if (n_keys > 16) return ray_error("limit", "xasc/xdesc: max 16 key columns");
        for (int64_t i = 0; i < n_keys; i++) key_ids[i] = syms[i];
    } else if (is_list(keys)) {
        /* List of symbol atoms */
        ray_t** elems = (ray_t**)ray_data(keys);
        n_keys = ray_len(keys);
        if (n_keys > 16) return ray_error("limit", "xasc/xdesc: max 16 key columns");
        for (int64_t i = 0; i < n_keys; i++) {
            if (elems[i]->type != -RAY_SYM)
                return ray_error("type", "xasc/xdesc key must be a symbol");
            key_ids[i] = elems[i]->i64;
        }
    } else {
        return ray_error("type", "xasc/xdesc key must be a symbol or list of symbols");
    }

    if (n_keys == 0) { ray_retain(tbl); return tbl; }

    int64_t nrows = ray_table_nrows(tbl);
    if (nrows <= 1) { ray_retain(tbl); return tbl; }

    /* Resolve key columns */
    ray_t* key_cols[16];
    for (int64_t i = 0; i < n_keys; i++) {
        key_cols[i] = ray_table_get_col(tbl, key_ids[i]);
        if (!key_cols[i])
            return ray_error("domain", "xasc/xdesc: key column not found in table");
    }

    /* Build descs array */
    uint8_t descs[16];
    for (int64_t i = 0; i < n_keys; i++) descs[i] = descending;

    ray_t* idx = ray_sort_indices(key_cols, descs, NULL, (uint8_t)n_keys, nrows);
    if (RAY_IS_ERR(idx)) return idx;

    int64_t* idx_data = (int64_t*)ray_data(idx);
    int64_t ncols = ray_table_ncols(tbl);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(idx); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        int64_t name_id = ray_table_col_name(tbl, c);
        ray_t* gathered = gather_by_idx(col, idx_data, nrows);
        if (RAY_IS_ERR(gathered)) {
            ray_release(idx);
            ray_release(result);
            return gathered;
        }
        result = ray_table_add_col(result, name_id, gathered);
        ray_release(gathered);
        if (RAY_IS_ERR(result)) { ray_release(idx); return result; }
    }

    ray_release(idx);
    return result;
}

/* (xasc tbl keys) — sort table ascending by key columns */
static ray_t* ray_xasc_fn(ray_t* tbl, ray_t* keys) {
    return sort_table_by_keys(tbl, keys, 0);
}

/* (xdesc tbl keys) — sort table descending by key columns */
static ray_t* ray_xdesc_fn(ray_t* tbl, ray_t* keys) {
    return sort_table_by_keys(tbl, keys, 1);
}

/* ══════════════════════════════════════════
 * Table construction and access
 * ══════════════════════════════════════════ */

/* (list v1 v2 ...) — package args into a list */
ray_t* ray_list(ray_t** args, int64_t n) {
    ray_t* result = ray_alloc(n * sizeof(ray_t*));
    if (!result) return ray_error("oom", NULL);
    result->type = RAY_LIST;
    result->len = n;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < n; i++) {
        ray_retain(args[i]);
        out[i] = args[i];
    }
    return result;
}

/* (table [col_names] (list col1 col2 ...)) — build a RAY_TABLE */
ray_t* ray_table(ray_t* names, ray_t* cols) {
    ray_t *_bxn = NULL, *_bxc = NULL;
    names = unbox_vec_arg(names, &_bxn);
    if (RAY_IS_ERR(names)) return names;
    cols = unbox_vec_arg(cols, &_bxc);
    if (RAY_IS_ERR(cols)) { if (_bxn) ray_release(_bxn); return cols; }
    if (!is_list(names) || !is_list(cols)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", NULL); }
    int64_t ncols = ray_len(names);
    if (ray_len(cols) != ncols) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", NULL); }

    ray_t** name_elems = (ray_t**)ray_data(names);
    ray_t** col_elems = (ray_t**)ray_data(cols);
    int64_t expected_rows = -1;

    ray_t* tbl = ray_table_new(ncols);
    if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }

    for (int64_t i = 0; i < ncols; i++) {
        if (name_elems[i]->type != -RAY_SYM)
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", NULL); }
        int64_t name_id = name_elems[i]->i64;

        /* Convert Rayfall list (or typed vec) to typed column vector */
        ray_t* col_src = col_elems[i];

        /* Single atom → wrap in a 1-element vector */
        ray_t* atom_wrap = NULL;
        if (ray_is_atom(col_src) && col_src->type != -RAY_SYM) {
            int8_t atype = -col_src->type;
            if (atype == RAY_GUID) {
                atom_wrap = ray_vec_new(RAY_GUID, 1);
                if (!RAY_IS_ERR(atom_wrap) && col_src->obj)
                    memcpy(ray_data(atom_wrap), ray_data(col_src->obj), 16);
                if (!RAY_IS_ERR(atom_wrap)) atom_wrap->len = 1;
            } else if (atype == RAY_TIMESTAMP || atype == RAY_I64 || atype == RAY_SYM) {
                atom_wrap = ray_vec_new(atype, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((int64_t*)ray_data(atom_wrap))[0] = col_src->i64; atom_wrap->len = 1; }
            } else if (atype == RAY_F64) {
                atom_wrap = ray_vec_new(RAY_F64, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((double*)ray_data(atom_wrap))[0] = col_src->f64; atom_wrap->len = 1; }
            } else if (atype == RAY_DATE || atype == RAY_TIME || atype == RAY_I32) {
                atom_wrap = ray_vec_new(atype, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((int32_t*)ray_data(atom_wrap))[0] = col_src->i32; atom_wrap->len = 1; }
            } else if (atype == RAY_BOOL) {
                atom_wrap = ray_vec_new(RAY_BOOL, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((uint8_t*)ray_data(atom_wrap))[0] = col_src->b8; atom_wrap->len = 1; }
            }
            if (atom_wrap && !RAY_IS_ERR(atom_wrap)) col_src = atom_wrap;
        }

        /* If the column is already a typed vector, use it directly */
        if (ray_is_vec(col_src)) {
            int64_t nrows = ray_len(col_src);
            if (expected_rows < 0) expected_rows = nrows;
            else if (nrows != expected_rows)
                { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", NULL); }
            ray_retain(col_src);
            tbl = ray_table_add_col(tbl, name_id, col_src);
            ray_release(col_src);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }

        if (!is_list(col_src))
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", NULL); }
        int64_t nrows = ray_len(col_src);

        /* Validate all columns have consistent row count */
        if (expected_rows < 0) expected_rows = nrows;
        else if (nrows != expected_rows)
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", NULL); }

        ray_t** row_elems = (ray_t**)ray_data(col_src);

        /* Determine column type from elements (scan for mixed I64/F64 → F64) */
        int8_t col_type = RAY_I64;
        if (nrows > 0) {
            if (row_elems[0]->type == -RAY_F64) col_type = RAY_F64;
            else if (row_elems[0]->type == -RAY_BOOL) col_type = RAY_BOOL;
            else if (row_elems[0]->type == -RAY_SYM) col_type = RAY_SYM;
            else if (row_elems[0]->type == -RAY_STR) col_type = RAY_STR;
            else if (row_elems[0]->type == -RAY_GUID) col_type = RAY_GUID;
            else if (row_elems[0]->type == -RAY_TIMESTAMP) col_type = RAY_TIMESTAMP;
            else if (row_elems[0]->type == -RAY_DATE) col_type = RAY_DATE;
            else if (row_elems[0]->type == -RAY_TIME) col_type = RAY_TIME;
            /* RAY_CHAR removed — char atoms are now -RAY_STR */
        }
        /* Promote I64 → F64 if any element is F64 */
        if (col_type == RAY_I64) {
            for (int64_t j = 0; j < nrows; j++) {
                if (row_elems[j]->type == -RAY_F64) { col_type = RAY_F64; break; }
            }
        }

        ray_t* col_vec = ray_vec_new(col_type, nrows);
        if (RAY_IS_ERR(col_vec))
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return col_vec; }

        for (int64_t j = 0; j < nrows; j++) {
            if (col_type == RAY_STR) {
                if (row_elems[j]->type != -RAY_STR) {
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", NULL);
                }
                const char *sptr = ray_str_ptr(row_elems[j]);
                size_t slen = ray_str_len(row_elems[j]);
                col_vec = ray_str_vec_append(col_vec, sptr, slen);
            } else if (col_type == RAY_GUID) {
                if (row_elems[j]->type != -RAY_GUID || !row_elems[j]->obj) {
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", NULL);
                }
                col_vec = ray_vec_append(col_vec, ray_data(row_elems[j]->obj));
            } else {
                /* Validate each element matches the column type (allow I64→F64 promotion) */
                int type_ok = (row_elems[j]->type == -col_type);
                if (!type_ok && col_type == RAY_F64 && row_elems[j]->type == -RAY_I64) type_ok = 1;
                if (!type_ok) {
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", NULL);
                }
                void* val_ptr;
                double promoted;
                if (col_type == RAY_F64 && row_elems[j]->type == -RAY_I64) {
                    promoted = (double)row_elems[j]->i64;
                    val_ptr = &promoted;
                } else if (col_type == RAY_I64) val_ptr = &row_elems[j]->i64;
                else if (col_type == RAY_F64) val_ptr = &row_elems[j]->f64;
                else if (col_type == RAY_BOOL) val_ptr = &row_elems[j]->b8;
                else val_ptr = &row_elems[j]->i64; /* SYM/TIMESTAMP/DATE/TIME stored as i64 */
                col_vec = ray_vec_append(col_vec, val_ptr);
            }
            if (RAY_IS_ERR(col_vec))
                { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return col_vec; }
        }

        tbl = ray_table_add_col(tbl, name_id, col_vec);
        ray_release(col_vec);
        if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
    }

    if (_bxn) ray_release(_bxn);
    if (_bxc) ray_release(_bxc);
    return tbl;
}

/* (key table) — return column names as a list of symbols */
ray_t* ray_key(ray_t* x) {
    /* Dict: extract keys as SYM vector */
    if (x->type == RAY_LIST && (x->attrs & RAY_ATTR_DICT)) {
        int64_t n2 = x->len / 2;
        ray_t* vec = ray_vec_new(RAY_SYM, n2);
        if (RAY_IS_ERR(vec)) return vec;
        vec->len = n2;
        int64_t* out = (int64_t*)ray_data(vec);
        ray_t** items = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n2; i++)
            out[i] = items[i * 2]->i64;
        return vec;
    }
    if (x->type != RAY_TABLE) return ray_error("type", NULL);
    int64_t ncols = ray_table_ncols(x);
    ray_t* result = ray_alloc(ncols * sizeof(ray_t*));
    if (!result) return ray_error("oom", NULL);
    result->type = RAY_LIST;
    result->len = ncols;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < ncols; i++) {
        int64_t name_id = ray_table_col_name(x, i);
        ray_t* sym = ray_alloc(0);
        if (!sym) { ray_release(result); return ray_error("oom", NULL); }
        sym->type = -RAY_SYM;
        sym->i64 = name_id;
        out[i] = sym;
    }
    return result;
}

/* (value dict/table) — extract values */
ray_t* ray_value(ray_t* x) {
    /* Table: return list of column vectors */
    if (x->type == RAY_TABLE) {
        int64_t ncols = x->len;
        ray_t* result = ray_alloc(ncols * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = ncols;
        /* Columns start after the schema pointer */
        ray_t** cols = (ray_t**)((char*)ray_data(x) + sizeof(ray_t*));
        ray_t** dst = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < ncols; i++) {
            ray_retain(cols[i]);
            dst[i] = cols[i];
        }
        return result;
    }
    if (x->type != RAY_LIST || !(x->attrs & RAY_ATTR_DICT))
        return ray_error("type", NULL);
    int64_t n = ray_len(x);
    int64_t nvals = n / 2;
    ray_t* result = ray_alloc(nvals * sizeof(ray_t*));
    if (!result) return ray_error("oom", NULL);
    result->type = RAY_LIST;
    result->len = nvals;
    ray_t** src = (ray_t**)ray_data(x);
    ray_t** dst = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < nvals; i++) {
        dst[i] = src[i * 2 + 1];
        ray_retain(dst[i]);
    }
    return result;
}

/* ══════════════════════════════════════════
 * Select query — DAG bridge
 * ══════════════════════════════════════════ */

/* Helper: look up a key in a dict (RAY_LIST with ATTR_DICT).
 * Returns the value expression (unevaluated), or NULL if not found. */
static ray_t* dict_get(ray_t* dict, const char* key) {
    if (!dict || dict->type != RAY_LIST) return NULL;
    int64_t n = ray_len(dict);
    ray_t** elems = (ray_t**)ray_data(dict);
    int64_t key_id = ray_sym_intern(key, strlen(key));
    for (int64_t i = 0; i + 1 < n; i += 2) {
        if (elems[i]->type == -RAY_SYM && elems[i]->i64 == key_id)
            return elems[i + 1];
    }
    return NULL;
}

/* Map a Rayfall builtin name to a DAG binary op constructor */
typedef ray_op_t* (*dag_binary_ctor)(ray_graph_t*, ray_op_t*, ray_op_t*);
typedef ray_op_t* (*dag_unary_ctor)(ray_graph_t*, ray_op_t*);

static dag_binary_ctor resolve_binary_dag(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return NULL;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 1) {
        switch (name[0]) {
            case '+': return ray_add;
            case '-': return ray_sub;
            case '*': return ray_mul;
            case '/': return ray_div;
            case '%': return ray_mod;
            case '>': return ray_gt;
            case '<': return ray_lt;
        }
    } else if (len == 2) {
        if (name[0] == '>' && name[1] == '=') return ray_ge;
        if (name[0] == '<' && name[1] == '=') return ray_le;
        if (name[0] == '=' && name[1] == '=') return ray_eq;
        if (name[0] == '!' && name[1] == '=') return ray_ne;
        if (name[0] == 'o' && name[1] == 'r') return ray_or;
    } else if (len == 3 && name[0] == 'a' && name[1] == 'n' && name[2] == 'd') {
        return ray_and;
    }
    return NULL;
}

/* Map Rayfall aggregation name to DAG opcode */
static uint16_t resolve_agg_opcode(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return 0;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 3 && memcmp(name, "sum", 3) == 0) return OP_SUM;
    if (len == 3 && memcmp(name, "avg", 3) == 0) return OP_AVG;
    if (len == 3 && memcmp(name, "min", 3) == 0) return OP_MIN;
    if (len == 3 && memcmp(name, "max", 3) == 0) return OP_MAX;
    if (len == 5 && memcmp(name, "count", 5) == 0) return OP_COUNT;
    if (len == 5 && memcmp(name, "first", 5) == 0) return OP_FIRST;
    if (len == 4 && memcmp(name, "last", 4) == 0) return OP_LAST;
    return 0;
}

/* Compile a Rayfall AST expression into a DAG node */
static ray_op_t* compile_expr_dag(ray_graph_t* g, ray_t* expr) {
    if (!expr) return NULL;

    /* Atom literal → const node */
    if (expr->type == -RAY_I64)
        return ray_const_i64(g, expr->i64);
    if (expr->type == -RAY_F64)
        return ray_const_f64(g, expr->f64);
    if (expr->type == -RAY_BOOL)
        return ray_const_bool(g, expr->b8);
    if (expr->type == -RAY_STR) {
        const char *ptr = ray_str_ptr(expr);
        size_t len = ray_str_len(expr);
        return ray_const_str(g, ptr, len);
    }

    /* Symbol literal → cannot compile to DAG (no const SYM node type).
     * Return NULL to trigger eval-level fallback. */
    if (expr->type == -RAY_SYM && !(expr->attrs & RAY_ATTR_NAME))
        return NULL;

    /* Name reference → column scan */
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        ray_t* s = ray_sym_str(expr->i64);
        if (!s) return NULL;
        return ray_scan(g, ray_str_ptr(s));
    }

    /* List → function call: (fn arg1 arg2 ...) */
    if (expr->type == RAY_LIST && !(expr->attrs & (RAY_ATTR_DICT))) {
        int64_t n = ray_len(expr);
        if (n == 0) return NULL;
        ray_t** elems = (ray_t**)ray_data(expr);
        ray_t* head = elems[0];

        /* Head must be a name referencing a builtin */
        if (head->type != -RAY_SYM) return NULL;
        int64_t fn_sym = head->i64;

        /* Check for xbar */
        ray_t* fn_name_str = ray_sym_str(fn_sym);
        if (fn_name_str && ray_str_len(fn_name_str) == 4
            && memcmp(ray_str_ptr(fn_name_str), "xbar", 4) == 0) {
            if (n != 3) return NULL;
            ray_op_t* col = compile_expr_dag(g, elems[1]);
            ray_op_t* bucket = compile_expr_dag(g, elems[2]);
            if (!col || !bucket) return NULL;
            /* xbar(x, b) = x - (x % b)  (stays in integer domain) */
            return ray_sub(g, col, ray_mod(g, col, bucket));
        }

        /* Binary op? */
        if (n == 3) {
            dag_binary_ctor ctor = resolve_binary_dag(fn_sym);
            if (ctor) {
                ray_op_t* left = compile_expr_dag(g, elems[1]);
                ray_op_t* right = compile_expr_dag(g, elems[2]);
                if (!left || !right) return NULL;
                return ctor(g, left, right);
            }
        }

        /* Unary aggregation? */
        if (n == 2) {
            /* Check if it's a unary DAG op like neg, not, etc. */
            if (fn_name_str && ray_str_len(fn_name_str) == 3
                && memcmp(ray_str_ptr(fn_name_str), "not", 3) == 0) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                return arg ? ray_not(g, arg) : NULL;
            }
            if (fn_name_str && ray_str_len(fn_name_str) == 3
                && memcmp(ray_str_ptr(fn_name_str), "neg", 3) == 0) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                return arg ? ray_neg(g, arg) : NULL;
            }
            /* Aggregation functions return DAG agg nodes */
            uint16_t agg_op = resolve_agg_opcode(fn_sym);
            if (agg_op) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                if (!arg) return NULL;
                switch (agg_op) {
                    case OP_SUM:   return ray_sum(g, arg);
                    case OP_AVG:   return ray_avg(g, arg);
                    case OP_MIN:   return ray_min_op(g, arg);
                    case OP_MAX:   return ray_max_op(g, arg);
                    case OP_COUNT: return ray_count(g, arg);
                    case OP_FIRST: return ray_first(g, arg);
                    case OP_LAST:  return ray_last(g, arg);
                    default: return NULL;
                }
            }
        }
    }

    return NULL;
}

/* Check if an expression is an aggregation call (head is an agg function) */
static int is_agg_expr(ray_t* expr) {
    if (!expr || expr->type != RAY_LIST) return 0;
    if (expr->attrs & (RAY_ATTR_DICT)) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (elems[0]->type != -RAY_SYM) return 0;
    return resolve_agg_opcode(elems[0]->i64) != 0;
}

/* Forward declarations for eval-level groupby fallback */
static ray_t* ray_group_fn(ray_t* x);

/* (select {from: t [where: pred] [by: key] [col: expr ...]})
 * Special form — receives unevaluated dict arg. */
ray_t* ray_select_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return ray_error("type", NULL);

    /* Evaluate 'from:' to get the source table */
    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return ray_error("domain", NULL);
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", NULL); }

    ray_t* where_expr = dict_get(dict, "where");
    ray_t* by_expr = dict_get(dict, "by");

    /* Collect output columns (keys that are not from/where/by) */
    int64_t dict_n = ray_len(dict);
    ray_t** dict_elems = (ray_t**)ray_data(dict);
    int64_t from_id  = ray_sym_intern("from",  4);
    int64_t where_id = ray_sym_intern("where", 5);
    int64_t by_id    = ray_sym_intern("by",    2);

    /* Count output columns */
    int n_out = 0;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid != from_id && kid != where_id && kid != by_id)
            n_out++;
    }

    /* Simple case: no output cols, no where, no by → return table as-is */
    if (n_out == 0 && !where_expr && !by_expr)
        return tbl;

    /* Build DAG */
    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) { ray_release(tbl); return ray_error("oom", NULL); }

    ray_op_t* root = ray_const_table(g, tbl);

    /* Apply WHERE filter */
    if (where_expr) {
        ray_op_t* pred = compile_expr_dag(g, where_expr);
        if (!pred) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
        root = ray_filter(g, root, pred);
    }

    /* GROUP BY */
    if (by_expr) {
        /* Check if group key is a LIST column (e.g., string list) —
         * the DAG executor doesn't support LIST group keys, so fall back
         * to eval-level grouping. */
        int use_eval_group = 0;
        if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME)) {
            ray_t* key_col = ray_table_get_col(tbl, by_expr->i64);
            if (key_col && (key_col->type == RAY_LIST || key_col->type == RAY_STR || key_col->type == RAY_GUID))
                use_eval_group = 1;
        }
        if (use_eval_group) {
            ray_graph_free(g);
            /* Eval-level groupby for LIST columns */
            ray_t* key_col = ray_table_get_col(tbl, by_expr->i64);
            ray_t* groups = ray_group_fn(key_col);
            if (RAY_IS_ERR(groups)) { ray_release(tbl); return groups; }

            /* groups is a dict: {key_val: [indices ...], ...} */
            int64_t gn = ray_len(groups);
            int64_t n_groups = gn / 2;

            /* Collect aggregation results */
            int n_agg_out = 0;
            int64_t agg_names[16];
            ray_t* agg_results[16];
            for (int64_t i = 0; i + 1 < dict_n && n_agg_out < 16; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id) continue;
                ray_t* val_expr_item = dict_elems[i + 1];
                if (!is_agg_expr(val_expr_item)) continue;

                ray_t** agg_elems = (ray_t**)ray_data(val_expr_item);
                ray_t* agg_fn_name = agg_elems[0];
                ray_t* agg_col_expr = agg_elems[1];

                /* Resolve source column — it's a column name reference, not an env variable */
                ray_t* src_col_val = NULL;
                if (agg_col_expr->type == -RAY_SYM && (agg_col_expr->attrs & RAY_ATTR_NAME)) {
                    src_col_val = ray_table_get_col(tbl, agg_col_expr->i64);
                    if (src_col_val) ray_retain(src_col_val);
                }
                if (!src_col_val) {
                    src_col_val = ray_eval(agg_col_expr);
                    if (RAY_IS_ERR(src_col_val)) { ray_release(groups); ray_release(tbl); return src_col_val; }
                }

                /* For each group, compute aggregation */
                ray_t* agg_vec = NULL;
                ray_t** grp_items = (ray_t**)ray_data(groups);
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    ray_t* idx_list = grp_items[gi * 2 + 1];
                    /* Gather values at indices */
                    ray_t* subset = ray_at(src_col_val, idx_list);
                    if (RAY_IS_ERR(subset)) continue;
                    /* Apply aggregation */
                    ray_t* agg_val = NULL;
                    ray_t* fn_obj = ray_env_get(agg_fn_name->i64);
                    if (fn_obj && fn_obj->type == RAY_UNARY) {
                        ray_unary_fn uf = (ray_unary_fn)(uintptr_t)fn_obj->i64;
                        agg_val = uf(subset);
                    }
                    ray_release(subset);
                    if (!agg_val || RAY_IS_ERR(agg_val)) continue;

                    if (!agg_vec) {
                        int8_t vt = -(agg_val->type);
                        agg_vec = ray_vec_new(vt, n_groups);
                        if (RAY_IS_ERR(agg_vec)) { ray_release(agg_val); break; }
                        agg_vec->len = n_groups;
                    }
                    store_typed_elem(agg_vec, gi, agg_val);
                    ray_release(agg_val);
                }
                ray_release(src_col_val);
                agg_names[n_agg_out] = kid;
                agg_results[n_agg_out] = agg_vec;
                n_agg_out++;
            }

            /* Build result table: key column + aggregation columns */
            ray_t* result = ray_table_new(1 + n_agg_out);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }

            /* Key column: unique keys from groups */
            ray_t** grp_items = (ray_t**)ray_data(groups);
            ray_t* key_col_src = ray_table_get_col(tbl, by_expr->i64);
            if (key_col_src && key_col_src->type == RAY_STR) {
                /* Build STR vector for string group keys */
                ray_t* key_vec = ray_vec_new(RAY_STR, n_groups);
                for (int64_t gi = 0; gi < n_groups && !RAY_IS_ERR(key_vec); gi++) {
                    ray_t* k = grp_items[gi * 2]; /* -RAY_STR atom */
                    const char* sp = ray_str_ptr(k);
                    size_t slen = ray_str_len(k);
                    key_vec = ray_str_vec_append(key_vec, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(key_vec)) { ray_release(groups); ray_release(tbl); return key_vec; }
                result = ray_table_add_col(result, by_expr->i64, key_vec);
                ray_release(key_vec);
            } else {
                ray_t* key_list = ray_alloc(n_groups * sizeof(ray_t*));
                key_list->type = RAY_LIST;
                key_list->len = n_groups;
                ray_t** key_out = (ray_t**)ray_data(key_list);
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    ray_retain(grp_items[gi * 2]);
                    key_out[gi] = grp_items[gi * 2];
                }
                result = ray_table_add_col(result, by_expr->i64, key_list);
                ray_release(key_list);
            }

            for (int i = 0; i < n_agg_out; i++) {
                if (agg_results[i])
                    result = ray_table_add_col(result, agg_names[i], agg_results[i]);
                if (agg_results[i]) ray_release(agg_results[i]);
            }

            ray_release(groups);
            ray_release(tbl);
            return result;
        }

        /* Compile group key(s) */
        ray_op_t* key_ops[16];
        uint8_t n_keys = 0;

        if (by_expr->type == RAY_SYM) {
            /* Multiple keys as SYM vector: [col1 col2 ...] */
            int64_t nk = ray_len(by_expr);
            int64_t* sym_ids = (int64_t*)ray_data(by_expr);
            for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                ray_t* name_str = ray_sym_str(sym_ids[i]);
                if (!name_str) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                key_ops[n_keys] = ray_scan(g, ray_str_ptr(name_str));
                if (!key_ops[n_keys]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                n_keys++;
            }
        } else {
            /* Single key expression */
            key_ops[0] = compile_expr_dag(g, by_expr);
            if (!key_ops[0]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
            n_keys = 1;
        }

        /* Collect aggregation expressions from output columns */
        uint16_t agg_ops[16];
        ray_op_t* agg_ins[16];
        uint8_t n_aggs = 0;

        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id) continue;

            ray_t* val_expr = dict_elems[i + 1];
            if (is_agg_expr(val_expr) && n_aggs < 16) {
                ray_t** agg_elems = (ray_t**)ray_data(val_expr);
                agg_ops[n_aggs] = resolve_agg_opcode(agg_elems[0]->i64);
                /* Compile the aggregation input (the column reference) */
                agg_ins[n_aggs] = compile_expr_dag(g, agg_elems[1]);
                if (!agg_ins[n_aggs]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                n_aggs++;
            }
        }

        if (n_aggs > 0) {
            root = ray_group(g, key_ops, n_keys, agg_ops, agg_ins, n_aggs);
        } else {
            /* No explicit aggregations — apply WHERE filter first (if any),
             * then use DAG GROUP+COUNT for fast hash-parallel group boundaries,
             * then gather first-of-group from the filtered table. */
            ray_t* filtered_tbl = tbl;
            if (where_expr) {
                root = ray_optimize(g, root);
                ray_t* fres = ray_execute(g, root);
                ray_graph_free(g); g = NULL;
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                if (ray_is_lazy(fres)) fres = ray_lazy_materialize(fres);
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                filtered_tbl = fres;
                /* Rebuild graph on filtered table for GROUP+COUNT */
                g = ray_graph_new(filtered_tbl);
                if (!g) { if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                n_keys = 0;
                if (by_expr->type == RAY_SYM) {
                    int64_t nk = ray_len(by_expr);
                    int64_t* sym_ids = (int64_t*)ray_data(by_expr);
                    for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                        ray_t* ns = ray_sym_str(sym_ids[i]);
                        if (ns) key_ops[n_keys++] = ray_scan(g, ray_str_ptr(ns));
                    }
                } else {
                    key_ops[0] = compile_expr_dag(g, by_expr);
                    if (key_ops[0]) n_keys = 1;
                }
            }

            uint16_t cnt_op = OP_COUNT;
            ray_op_t* cnt_in = key_ops[0];
            root = ray_group(g, key_ops, n_keys, &cnt_op, &cnt_in, 1);
            root = ray_optimize(g, root);
            ray_t* grouped = ray_execute(g, root);
            ray_graph_free(g); g = NULL;
            if (!grouped || RAY_IS_ERR(grouped)) { if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return grouped; }
            if (ray_is_lazy(grouped)) grouped = ray_lazy_materialize(grouped);

            int64_t n_groups = ray_table_nrows(grouped);

            /* Resolve key column sym early — needed for empty result schema */
            int64_t key_sym = -1;
            if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME))
                key_sym = by_expr->i64;
            else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1)
                key_sym = ((int64_t*)ray_data(by_expr))[0];

            if (n_groups == 0) {
                ray_release(grouped);
                int64_t nc0 = ray_table_ncols(filtered_tbl);
                ray_t* empty = ray_table_new(nc0);
                if (!RAY_IS_ERR(empty)) {
                    /* Key column first */
                    { ray_t* sc = ray_table_get_col(filtered_tbl, key_sym);
                      if (sc) {
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) : ray_vec_new(sc->type, 0);
                        if (!RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, key_sym, ev); ray_release(ev); }
                      }
                    }
                    for (int64_t c = 0; c < nc0; c++) {
                        int64_t cn = ray_table_col_name(filtered_tbl, c);
                        if (cn == key_sym) continue;
                        ray_t* sc = ray_table_get_col_idx(filtered_tbl, c);
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) :
                                    (sc->type == RAY_LIST) ? ray_list_new(0) :
                                    ray_vec_new(sc->type, 0);
                        if (!RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, cn, ev); ray_release(ev); }
                    }
                }
                if (filtered_tbl != tbl) ray_release(filtered_tbl);
                ray_release(tbl);
                return empty;
            }

            /* Build first_idx: scan filtered key column once, record first
             * occurrence of each group key value. */
            if (key_sym < 0) {
                /* Computed group key (e.g., xbar) — fall back to eval-level groupby */
                ray_release(grouped);
                int64_t tbl_ncols = ray_table_ncols(filtered_tbl);
                ray_env_push_scope();
                for (int64_t c = 0; c < tbl_ncols; c++) {
                    int64_t cn = ray_table_col_name(filtered_tbl, c);
                    ray_t* cv = ray_table_get_col_idx(filtered_tbl, c);
                    ray_env_set_local(cn, cv);
                }
                ray_t* computed_key = ray_eval(by_expr);
                ray_env_pop_scope();
                if (!computed_key || RAY_IS_ERR(computed_key)) {
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return computed_key ? computed_key : ray_error("domain", NULL);
                }
                ray_t* groups2 = ray_group_fn(computed_key);
                if (!groups2 || RAY_IS_ERR(groups2)) {
                    ray_release(computed_key);
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return groups2 ? groups2 : ray_error("domain", NULL);
                }
                int64_t ng2 = ray_len(groups2) / 2;
                if (ng2 == 0) { ray_release(groups2); ray_release(computed_key); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_table_new(0); }
                ray_t** gi2 = (ray_t**)ray_data(groups2);
                int64_t fi2[256];
                for (int64_t g2 = 0; g2 < ng2 && g2 < 256; g2++) {
                    int alloc2 = 0;
                    ray_t* i02 = collection_elem(gi2[g2 * 2 + 1], 0, &alloc2);
                    fi2[g2] = as_i64(i02);
                    if (alloc2) ray_release(i02);
                }
                int64_t ckey_name = ray_sym_intern("+", 1);
                if (by_expr->type == RAY_LIST && by_expr->len >= 2) {
                    ray_t** be = (ray_t**)ray_data(by_expr);
                    if (be[1]->type == -RAY_SYM && (be[1]->attrs & RAY_ATTR_NAME))
                        ckey_name = be[1]->i64;
                }
                ray_t* res2 = ray_table_new(tbl_ncols);
                /* Key column first */
                { ray_t* okc = ray_table_get_col(filtered_tbl, ckey_name);
                  if (okc) {
                    ray_t* kv = ray_vec_new(okc->type, ng2);
                    for (int64_t g2 = 0; g2 < ng2; g2++) { int a2 = 0; ray_t* v2 = collection_elem(okc, fi2[g2], &a2); store_typed_elem(kv, g2, v2); if (a2) ray_release(v2); }
                    kv->len = ng2;
                    res2 = ray_table_add_col(res2, ckey_name, kv); ray_release(kv);
                  }
                }
                for (int64_t c = 0; c < tbl_ncols; c++) {
                    int64_t cn = ray_table_col_name(filtered_tbl, c);
                    if (cn == ckey_name) continue;
                    ray_t* sc = ray_table_get_col_idx(filtered_tbl, c);
                    ray_t* dc = ray_vec_new(sc->type, ng2);
                    for (int64_t g2 = 0; g2 < ng2; g2++) { int a2 = 0; ray_t* v2 = collection_elem(sc, fi2[g2], &a2); store_typed_elem(dc, g2, v2); if (a2) ray_release(v2); }
                    dc->len = ng2;
                    res2 = ray_table_add_col(res2, cn, dc); ray_release(dc);
                }
                ray_release(groups2); ray_release(computed_key);
                if (filtered_tbl != tbl) ray_release(filtered_tbl);
                ray_release(tbl);
                return res2;
            }

            ray_t* orig_key_col = ray_table_get_col(filtered_tbl, key_sym);
            int64_t nrows_orig = orig_key_col ? orig_key_col->len : 0;

            /* Read group key values from grouped table BEFORE releasing it.
             * grp_key_col points into grouped — must not access after release. */
            ray_t* grp_key_col = ray_table_get_col(grouped, key_sym);
            int8_t kt = orig_key_col ? orig_key_col->type : 0;

            /* Heap-allocate gk_vals when n_groups > 256 */
            int64_t gk_stack[256];
            ray_t* gk_heap_hdr = NULL;
            int64_t* gk_vals = gk_stack;
            if (n_groups > 256) {
                gk_heap_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!gk_heap_hdr) { ray_release(grouped); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                gk_vals = (int64_t*)ray_data(gk_heap_hdr);
            }

            /* Copy group key values while grouped is still alive.
             * GUID: 16 bytes per key → separate buffer.
             * STR/LIST/GUID: handled by use_eval_group, never reach here. */
            /* GUID/STR/LIST keys are routed through use_eval_group above,
             * so only integer-like types reach here. */
            if (grp_key_col) {
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    if (kt == RAY_F64)
                        memcpy(&gk_vals[gi], &((double*)ray_data(grp_key_col))[gi], 8);
                    else
                        gk_vals[gi] = ray_read_sym(ray_data(grp_key_col), gi, kt, grp_key_col->attrs);
                }
            }
            ray_release(grouped); /* grp_key_col is now invalid */

            /* Allocate first_idx */
            int64_t first_idx_stack[256];
            ray_t* fi_heap_hdr = NULL;
            int64_t* first_idx = first_idx_stack;
            if (n_groups > 256) {
                fi_heap_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!fi_heap_hdr) { if (gk_heap_hdr) ray_free(gk_heap_hdr); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                first_idx = (int64_t*)ray_data(fi_heap_hdr);
            }

            /* Single scan: mark first occurrence of each group key */
            for (int64_t gi = 0; gi < n_groups; gi++) first_idx[gi] = -1;
            int64_t found = 0;
            for (int64_t r = 0; r < nrows_orig && found < n_groups; r++) {
                int64_t ov;
                if (kt == RAY_F64) memcpy(&ov, &((double*)ray_data(orig_key_col))[r], 8);
                else ov = ray_read_sym(ray_data(orig_key_col), r, kt, orig_key_col->attrs);
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    if (first_idx[gi] >= 0) continue;
                    if (ov == gk_vals[gi]) { first_idx[gi] = r; found++; break; }
                }
            }
            if (gk_heap_hdr) ray_free(gk_heap_hdr);

            /* Now build the result table using first_idx gathered above.
             * key_sym and n_groups are already set. */

            /* Build result table: key column first, then others */
            int64_t ncols = ray_table_ncols(filtered_tbl);
            ray_t* result = ray_table_new(ncols);
            if (RAY_IS_ERR(result)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); return result; }

            /* Add key column first */
            ray_t* key_vec_src = ray_table_get_col(filtered_tbl, key_sym);
            if (key_vec_src->type == RAY_STR) {
                ray_t* key_vec_dst = ray_vec_new(RAY_STR, n_groups);
                if (!key_vec_dst || RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst ? key_vec_dst : ray_error("oom", NULL); }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(key_vec_src, first_idx[gi], &slen);
                    key_vec_dst = ray_str_vec_append(key_vec_dst, sp ? sp : "", sp ? slen : 0);
                    if (RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst; }
                }
                result = ray_table_add_col(result, key_sym, key_vec_dst);
                ray_release(key_vec_dst);
            } else {
                ray_t* key_vec_dst = ray_vec_new(key_vec_src->type, n_groups);
                if (RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst; }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    int alloc = 0;
                    ray_t* val = collection_elem(key_vec_src, first_idx[gi], &alloc);
                    store_typed_elem(key_vec_dst, gi, val);
                    if (alloc) ray_release(val);
                }
                key_vec_dst->len = n_groups;
                result = ray_table_add_col(result, key_sym, key_vec_dst);
                ray_release(key_vec_dst);
            }

            /* Add non-key columns */
            for (int64_t c = 0; c < ncols; c++) {
                int64_t col_name = ray_table_col_name(filtered_tbl, c);
                if (col_name == key_sym) continue;
                ray_t* src_col = ray_table_get_col_idx(filtered_tbl, c);
                int8_t ct = src_col->type;

                if (ct == RAY_STR) {
                    /* String column: build STR vector */
                    ray_t* dst = ray_vec_new(RAY_STR, n_groups);
                    if (!dst || RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst ? dst : ray_error("oom", NULL); }
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        size_t slen = 0;
                        const char* sp = ray_str_vec_get(src_col, first_idx[gi], &slen);
                        dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                        if (RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst; }
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                } else if (ct == RAY_LIST) {
                    /* List column: pick items */
                    ray_t* dst = ray_alloc(n_groups * sizeof(ray_t*));
                    if (!dst) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return ray_error("oom", NULL); }
                    dst->type = RAY_LIST;
                    dst->len = n_groups;
                    ray_t** dout = (ray_t**)ray_data(dst);
                    ray_t** src_items = (ray_t**)ray_data(src_col);
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        dout[gi] = src_items[first_idx[gi]];
                        ray_retain(dout[gi]);
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                } else {
                    /* Typed vector: copy elements at first indices */
                    ray_t* dst = ray_vec_new(ct, n_groups);
                    if (RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst; }
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        int alloc = 0;
                        ray_t* val = collection_elem(src_col, first_idx[gi], &alloc);
                        store_typed_elem(dst, gi, val);
                        if (alloc) ray_release(val);
                    }
                    dst->len = n_groups;
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                }
                if (RAY_IS_ERR(result)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); return result; }
            }

            if (fi_heap_hdr) ray_free(fi_heap_hdr);
            if (filtered_tbl != tbl) ray_release(filtered_tbl);
            ray_release(tbl);
            return result;
        }
    } else if (n_out > 0) {
        /* Projection only (no group by) — select specific columns */
        ray_op_t* col_ops[16];
        uint8_t nc = 0;
        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id) continue;
            if (nc < 16) {
                col_ops[nc] = compile_expr_dag(g, dict_elems[i + 1]);
                if (!col_ops[nc]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                nc++;
            }
        }
        root = ray_select(g, root, col_ops, nc);
    }

    /* Optimize and execute */
    root = ray_optimize(g, root);
    ray_t* result = ray_execute(g, root);

    ray_graph_free(g);

    /* Post-process: reorder GROUP BY BOOL results to match first-occurrence
     * order in the original table (exec.c radix sort puts false before true) */
    if (by_expr && result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
        if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
            ray_t* key_col = ray_table_get_col_idx(result, 0);
            if (key_col && key_col->type == RAY_BOOL && key_col->len >= 2) {
                /* Find first-occurrence order of bool values in original table */
                int64_t by_sym = -1;
                if (by_expr->type == -RAY_SYM) by_sym = by_expr->i64;
                ray_t* orig_key = (by_sym >= 0) ? ray_table_get_col(tbl, by_sym) : NULL;
                if (orig_key && orig_key->type == RAY_BOOL && orig_key->len > 0) {
                    bool first_val = ((bool*)ray_data(orig_key))[0];
                    bool result_first = ((bool*)ray_data(key_col))[0];
                    if (first_val != result_first) {
                        /* Swap rows: reverse row order in all columns */
                        int64_t nrows_r = ray_table_nrows(result);
                        int64_t ncols_r = ray_table_ncols(result);
                        ray_t* reordered = ray_table_new((int32_t)ncols_r);
                        if (reordered && !RAY_IS_ERR(reordered)) {
                            int ok = 1;
                            for (int64_t c = 0; c < ncols_r && ok; c++) {
                                int64_t cn = ray_table_col_name(result, c);
                                ray_t* col = ray_table_get_col_idx(result, c);
                                int esz = ray_elem_size(col->type);
                                ray_t* new_col = ray_vec_new(col->type, nrows_r);
                                if (RAY_IS_ERR(new_col)) { ok = 0; break; }
                                new_col->len = nrows_r;
                                char* src = (char*)ray_data(col);
                                char* dst = (char*)ray_data(new_col);
                                for (int64_t r = 0; r < nrows_r; r++)
                                    memcpy(dst + r * esz, src + (nrows_r - 1 - r) * esz, esz);
                                reordered = ray_table_add_col(reordered, cn, new_col);
                                ray_release(new_col);
                                if (RAY_IS_ERR(reordered)) { ok = 0; break; }
                            }
                            if (ok) {
                                ray_release(result);
                                result = reordered;
                            } else if (reordered && !RAY_IS_ERR(reordered)) {
                                ray_release(reordered);
                            }
                        }
                    }
                }
            }
        }
    }

    ray_release(tbl);

    /* Rename output columns if user specified names */
    if (result && !RAY_IS_ERR(result) && n_out > 0) {
        /* Materialize lazy results if needed */
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
    }
    if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE && n_out > 0) {
        ray_t* schema = ray_table_schema(result);
        if (schema && !RAY_IS_ERR(schema) && schema->type > 0 && schema->type < RAY_TYPE_COUNT) {
            int64_t ncols = schema->len;
            /* Count key columns in by clause */
            int n_key_cols = 0;
            if (by_expr) {
                if (ray_is_vec(by_expr) && by_expr->type == RAY_SYM) n_key_cols = (int)ray_len(by_expr);
                else n_key_cols = 1;
            }
            /* User-defined output column names */
            int64_t user_names[16];
            int n_user = 0;
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid != from_id && kid != where_id && kid != by_id && n_user < 16)
                    user_names[n_user++] = kid;
            }
            /* Rename agg columns (after key columns) using table API */
            for (int j = 0; j < n_user && n_key_cols + j < ncols; j++)
                ray_table_set_col_name(result, n_key_cols + j, user_names[j]);
        }
    }

    return result;
}

/* (xbar col bucket) — time/value bucketing: floor(col/bucket)*bucket */
ray_t* ray_xbar(ray_t* col, ray_t* bucket) {
    /* Recursive unwrap for nested collections (list of vectors) */
    if (is_collection(col) || is_collection(bucket))
        return atomic_map_binary(ray_xbar, col, bucket);
    /* Both are integer types (i64, i32, i16) → integer xbar */
    if (is_numeric(col) && is_numeric(bucket) && !is_float_op(col, bucket)) {
        int64_t a = as_i64(col), b = as_i64(bucket);
        if (b == 0 || is_null_atom(col) || is_null_atom(bucket))
            return ray_error("domain", NULL);
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        /* Result type follows the wider of the two operands */
        if (col->type == -RAY_I32 && bucket->type == -RAY_I32) return make_i32((int32_t)result);
        if (col->type == -RAY_I16 && bucket->type == -RAY_I16) return make_i16((int16_t)result);
        return make_i64(result);
    }
    /* Float path: either operand is f64 */
    if (is_numeric(col) && is_numeric(bucket)) {
        double c = as_f64(col), b = as_f64(bucket);
        if (b == 0.0 || isnan(c) || isnan(b)) return ray_error("domain", NULL);
        double fq = floor(c / b);
        return make_f64(fq * b);
    }
    /* Temporal xbar: col is temporal, bucket is integer or temporal (not float) */
    if (is_temporal(col) && (is_temporal(bucket) ||
        (is_numeric(bucket) && bucket->type != -RAY_F64))) {
        int64_t a = col->i64, b;
        if (is_temporal(bucket)) {
            b = bucket->i64;
            /* Cross-temporal conversion: TIME(ms) bucket on TIMESTAMP(ns) col */
            if (col->type == -RAY_TIMESTAMP && bucket->type == -RAY_TIME)
                b *= 1000000LL;
        } else {
            b = as_i64(bucket);
        }
        if (b == 0 || is_null_atom(bucket)) return ray_error("domain", NULL);
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        if (col->type == -RAY_TIME) return ray_time(result);
        if (col->type == -RAY_DATE) return ray_date(result);
        return ray_timestamp(result);
    }
    return ray_error("type", NULL);
}

/* ══════════════════════════════════════════
 * Update, Insert, Upsert
 * ══════════════════════════════════════════ */

/* Helper: convert a Rayfall list of atoms into a typed column vector by
 * appending to an existing column (for insert/upsert). */
static ray_t* append_atom_to_col(ray_t* col_vec, ray_t* atom) {
    int8_t ct = col_vec->type;
    if (ct == RAY_I64) {
        if (atom->type != -RAY_I64)
            return ray_error("type", NULL);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_SYM) {
        if (atom->type != -RAY_SYM)
            return ray_error("type", NULL);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_F64) {
        if (atom->type != -RAY_F64 && atom->type != -RAY_I64)
            return ray_error("type", NULL);
        double v = (atom->type == -RAY_F64) ? atom->f64 : (double)atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_BOOL) {
        if (atom->type != -RAY_BOOL)
            return ray_error("type", NULL);
        uint8_t v = atom->b8;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_STR && atom->type == -RAY_STR) {
        const char *sptr = ray_str_ptr(atom);
        size_t slen = ray_str_len(atom);
        return ray_str_vec_append(col_vec, sptr, slen);
    }
    return ray_error("type", NULL);
}

/* (update {col: expr ... from: t [where: pred]})
 * Special form — receives unevaluated dict arg.
 * For rows matching where (or all if no where), evaluate column expressions
 * and replace those column values. Returns a new table. */
/* Forward declarations */
static ray_t* ray_group_fn(ray_t* x);
static ray_t* ray_concat_fn(ray_t* a, ray_t* b);

ray_t* ray_update(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return ray_error("type", NULL);

    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return ray_error("domain", NULL);
    /* Detect in-place update: from: 't means quoted symbol */
    int64_t inplace_sym = -1;
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type == -RAY_SYM) {
        /* from: 't — resolve symbol to table variable */
        inplace_sym = tbl->i64;
        ray_release(tbl);
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    }
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", NULL); }

    ray_t* where_expr = dict_get(dict, "where");
    ray_t* by_expr = dict_get(dict, "by");

    /* UPDATE WITH BY: group, compute aggregate, broadcast back */
    if (by_expr && !where_expr) {
        int64_t dict_n = ray_len(dict);
        ray_t** dict_elems = (ray_t**)ray_data(dict);
        int64_t from_id  = ray_sym_intern("from",  4);
        int64_t where_id = ray_sym_intern("where", 5);
        int64_t by_id    = ray_sym_intern("by",    2);

        /* Resolve group key column name.
         * by_expr is a name reference (not evaluated) — extract sym_id directly */
        int64_t by_col_name = -1;
        if (by_expr->type == -RAY_SYM) {
            by_col_name = by_expr->i64;
        }
        if (by_col_name < 0) { ray_release(tbl); return ray_error("type", NULL); }

        /* Find group column in table */
        ray_t* grp_col = ray_table_get_col(tbl, by_col_name);
        if (!grp_col) { ray_release(tbl); return ray_error("domain", NULL); }
        int64_t nrows2 = ray_table_nrows(tbl);

        /* Use ray_group_fn to get group indices: {key: [indices]} */
        ray_t* groups = NULL;
        {
            groups = ray_group_fn(grp_col);
            if (!groups || RAY_IS_ERR(groups)) { ray_release(tbl); return groups ? groups : ray_error("oom", NULL); }
        }

        /* Start with a copy of the original table */
        int64_t ncols = ray_table_ncols(tbl);
        ray_t* result = ray_table_new((int32_t)ncols);
        if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            ray_t* col = ray_table_get_col_idx(tbl, c);
            ray_retain(col);
            result = ray_table_add_col(result, cn, col);
            ray_release(col);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        }

        /* For each aggregate expression, compute per group and broadcast */
        for (int64_t d = 0; d + 1 < dict_n; d += 2) {
            int64_t kid = dict_elems[d]->i64;
            if (kid == from_id || kid == where_id || kid == by_id) continue;
            ray_t* agg_expr = dict_elems[d + 1];

            /* Evaluate the aggregate for each group and broadcast */
            ray_t* grp_items = (ray_t**)ray_data(groups) ? groups : NULL;
            if (!grp_items) { ray_release(result); ray_release(groups); ray_release(tbl); return ray_error("oom", NULL); }
            int64_t ngroups = groups->len / 2;
            ray_t** gdata = (ray_t**)ray_data(groups);

            /* We need to evaluate the aggregate per group.
             * Build the result column by evaluating the expression on each group's subset. */
            ray_t* out_col = ray_vec_new(RAY_I64, nrows2); /* will be resized to correct type */
            if (RAY_IS_ERR(out_col)) { ray_release(result); ray_release(groups); ray_release(tbl); return out_col; }

            int8_t out_type = RAY_I64;
            int first_group = 1;

            for (int64_t gi = 0; gi < ngroups; gi++) {
                ray_t* idx_vec = gdata[gi * 2 + 1]; /* index vector for this group */
                int64_t gsize = ray_len(idx_vec);

                /* Build a sub-table for this group */
                ray_t* sub_tbl = ray_table_new((int32_t)ncols);
                if (RAY_IS_ERR(sub_tbl)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_tbl; }
                for (int64_t c = 0; c < ncols; c++) {
                    int64_t cn = ray_table_col_name(tbl, c);
                    ray_t* full_col = ray_table_get_col_idx(tbl, c);
                    int8_t ct = full_col->type;
                    ray_t* sub_col = ray_vec_new(ct, gsize);
                    if (RAY_IS_ERR(sub_col)) { ray_release(sub_tbl); ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_col; }
                    sub_col->len = gsize;
                    int esz = ray_elem_size(ct);
                    char* src = (char*)ray_data(full_col);
                    char* dst = (char*)ray_data(sub_col);
                    int64_t* idxs = (int64_t*)ray_data(idx_vec);
                    for (int64_t r = 0; r < gsize; r++)
                        memcpy(dst + r * esz, src + idxs[r] * esz, esz);
                    sub_tbl = ray_table_add_col(sub_tbl, cn, sub_col);
                    ray_release(sub_col);
                    if (RAY_IS_ERR(sub_tbl)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_tbl; }
                }

                /* Evaluate expression on sub-table via DAG */
                ray_graph_t* ug = ray_graph_new(sub_tbl);
                ray_op_t* expr_op = compile_expr_dag(ug, agg_expr);
                if (!expr_op) { ray_graph_free(ug); ray_release(sub_tbl); ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return ray_error("domain", NULL); }
                expr_op = ray_optimize(ug, expr_op);
                ray_t* agg_result = ray_execute(ug, expr_op);
                ray_graph_free(ug);
                ray_release(sub_tbl);

                if (RAY_IS_ERR(agg_result)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return agg_result; }

                /* Determine output type from first group */
                if (first_group) {
                    if (ray_is_atom(agg_result)) out_type = -agg_result->type;
                    else if (ray_is_vec(agg_result)) out_type = agg_result->type;
                    ray_release(out_col);
                    out_col = ray_vec_new(out_type, nrows2);
                    if (RAY_IS_ERR(out_col)) { ray_release(agg_result); ray_release(result); ray_release(groups); ray_release(tbl); return out_col; }
                    out_col->len = nrows2;
                    first_group = 0;
                }

                /* Broadcast aggregate value to all rows in this group */
                int64_t* idxs = (int64_t*)ray_data(idx_vec);
                if (ray_is_atom(agg_result)) {
                    for (int64_t r = 0; r < gsize; r++)
                        store_typed_elem(out_col, idxs[r], agg_result);
                }
                ray_release(agg_result);
            }

            /* Add the new column to the result table */
            result = ray_table_add_col(result, kid, out_col);
            ray_release(out_col);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        }

        ray_release(groups);
        /* Store in-place if needed */
        if (inplace_sym >= 0) {
            ray_env_set(inplace_sym, result);
        }
        ray_release(tbl);
        return result;
    }

    /* Evaluate WHERE using the DAG to get a boolean mask */
    int64_t nrows = ray_table_nrows(tbl);
    uint8_t* mask = NULL;

    if (where_expr) {
        /* Try DAG compilation first, fall back to eval-level */
        ray_t* mask_vec = NULL;
        ray_graph_t* g = ray_graph_new(tbl);
        if (g) {
            ray_op_t* pred = compile_expr_dag(g, where_expr);
            if (pred) {
                pred = ray_optimize(g, pred);
                mask_vec = ray_execute(g, pred);
            }
            ray_graph_free(g);
        }
        /* Fallback: eval-level predicate evaluation */
        if (!mask_vec || RAY_IS_ERR(mask_vec)) {
            /* Bind column names to column vectors in env, then eval */
            int64_t ncols2 = ray_table_ncols(tbl);
            ray_env_push_scope();
            for (int64_t c = 0; c < ncols2; c++) {
                int64_t cn = ray_table_col_name(tbl, c);
                ray_t* col = ray_table_get_col_idx(tbl, c);
                ray_env_set(cn, col);
            }
            mask_vec = ray_eval(where_expr);
            ray_env_pop_scope();
        }
        if (!mask_vec || RAY_IS_ERR(mask_vec)) { ray_release(tbl); return mask_vec ? mask_vec : ray_error("type", NULL); }
        if (mask_vec->type != RAY_BOOL || mask_vec->len != nrows) {
            ray_release(mask_vec);
            ray_release(tbl);
            return ray_error("type", NULL);
        }
        mask = (uint8_t*)ray_data(mask_vec);
        /* Keep mask_vec alive until we're done */

        /* Build a new table with updated columns */
        int64_t ncols = ray_table_ncols(tbl);
        int64_t dict_n = ray_len(dict);
        ray_t** dict_elems = (ray_t**)ray_data(dict);
        int64_t from_id = ray_sym_intern("from", 4);
        int64_t where_id = ray_sym_intern("where", 5);

        ray_t* result = ray_table_new(ncols);
        if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }

        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            ray_t* orig_col = ray_table_get_col_idx(tbl, c);

            /* Check if this column has an update expression */
            ray_t* update_expr = NULL;
            for (int64_t d = 0; d + 1 < dict_n; d += 2) {
                int64_t kid = dict_elems[d]->i64;
                if (kid == from_id || kid == where_id) continue;
                if (kid == col_name) { update_expr = dict_elems[d + 1]; break; }
            }

            if (!update_expr) {
                /* No update for this column — copy as-is */
                ray_retain(orig_col);
                result = ray_table_add_col(result, col_name, orig_col);
                ray_release(orig_col);
            } else {
                /* Evaluate the expression for each row and apply to matching rows */
                int8_t ct = orig_col->type;
                ray_t* new_col = ray_vec_new(ct, nrows);
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }

                /* Evaluate expression via DAG, fallback to eval-level */
                ray_t* expr_vec = NULL;
                {
                    ray_graph_t* ug = ray_graph_new(tbl);
                    if (ug) {
                        ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                        if (expr_op) {
                            expr_op = ray_optimize(ug, expr_op);
                            expr_vec = ray_execute(ug, expr_op);
                        }
                        ray_graph_free(ug);
                    }
                }
                if (!expr_vec || RAY_IS_ERR(expr_vec)) {
                    /* Fallback: eval with column bindings */
                    int64_t ncols_e = ray_table_ncols(tbl);
                    ray_env_push_scope();
                    for (int64_t c2 = 0; c2 < ncols_e; c2++) {
                        int64_t cn = ray_table_col_name(tbl, c2);
                        ray_t* col2 = ray_table_get_col_idx(tbl, c2);
                        ray_env_set(cn, col2);
                    }
                    expr_vec = ray_eval(update_expr);
                    ray_env_pop_scope();
                }
                if (!expr_vec || RAY_IS_ERR(expr_vec)) { ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return expr_vec ? expr_vec : ray_error("type", NULL); }

                /* WHERE update: expression result replaces ONLY masked rows.
                 * When type differs (e.g., I64 col, F64 expr from (* col 1.1)),
                 * keep original column type and cast expr results.
                 * Only numeric promotions are allowed — STR↔numeric is a type error. */
                int8_t expr_type = (expr_vec->type < 0) ? -expr_vec->type : expr_vec->type;
                if (expr_type != ct && expr_type > 0 && ray_is_vec(expr_vec)) {
                    /* Only allow numeric promotions (I64↔F64, I32↔F64) */
                    int is_numeric_promo = (ct == RAY_I64 || ct == RAY_I32 || ct == RAY_F64) &&
                                           (expr_type == RAY_I64 || expr_type == RAY_I32 || expr_type == RAY_F64);
                    if (!is_numeric_promo) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return ray_error("type", NULL);
                    }
                    /* Copy original column values first */
                    int esz = ray_elem_size(ct);
                    memcpy(ray_data(new_col), ray_data(orig_col), (size_t)(nrows * esz));
                    new_col->len = nrows;
                    /* Overlay masked rows with type conversion */
                    for (int64_t r = 0; r < nrows; r++) {
                        if (!mask[r]) continue;
                        if (ct == RAY_I64 && expr_type == RAY_F64)
                            ((int64_t*)ray_data(new_col))[r] = (int64_t)((double*)ray_data(expr_vec))[r];
                        else if (ct == RAY_I32 && expr_type == RAY_F64)
                            ((int32_t*)ray_data(new_col))[r] = (int32_t)((double*)ray_data(expr_vec))[r];
                        else if (ct == RAY_F64 && expr_type == RAY_I64)
                            ((double*)ray_data(new_col))[r] = (double)((int64_t*)ray_data(expr_vec))[r];
                    }
                    ray_release(expr_vec);
                    result = ray_table_add_col(result, col_name, new_col);
                    ray_release(new_col);
                    if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
                    continue;
                }

                /* Broadcast scalar atom to full column vector if needed */
                if (expr_vec->type < 0) {
                    /* Type check atom against column type BEFORE broadcast */
                    int ok = (expr_vec->type == -ct);
                    if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                    if (!ok && ct == RAY_LIST && expr_vec->type == -RAY_SYM) ok = 1;
                    if (!ok && ct == RAY_SYM && expr_vec->type == -RAY_SYM) ok = 1;
                    if (!ok) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return ray_error("type", NULL);
                    }
                    /* SYM atom to LIST column: build boxed list, merge with mask */
                    if (ct == RAY_LIST && expr_vec->type == -RAY_SYM) {
                        ray_free(new_col);
                        ray_t* new_list = ray_list_new((int32_t)nrows);
                        if (RAY_IS_ERR(new_list)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_list; }
                        ray_t** orig_elems = (ray_t**)ray_data(orig_col);
                        for (int64_t r = 0; r < nrows; r++) {
                            ray_t* elem = mask[r] ? expr_vec : orig_elems[r];
                            ray_retain(elem);
                            new_list = ray_list_append(new_list, elem);
                            ray_release(elem);
                            if (RAY_IS_ERR(new_list)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_list; }
                        }
                        ray_release(expr_vec);
                        result = ray_table_add_col(result, col_name, new_list);
                        ray_release(new_list);
                        if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
                        continue;
                    }
                    ray_t* bcast = ray_vec_new(ct, nrows);
                    if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                    if (ct == RAY_STR && expr_vec->type == -RAY_STR) {
                        const char* sp = ray_str_ptr(expr_vec);
                        size_t sl = ray_str_len(expr_vec);
                        for (int64_t r = 0; r < nrows; r++) {
                            bcast = ray_str_vec_append(bcast, sp, sl);
                            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                        }
                    } else {
                        size_t esz = (ct == RAY_BOOL) ? 1 : 8;
                        uint8_t elem[8] = {0};
                        if (ct == RAY_F64 && expr_vec->type == -RAY_I64) {
                            double promoted = (double)expr_vec->i64;
                            memcpy(elem, &promoted, 8);
                        } else {
                            memcpy(elem, &expr_vec->i64, esz);
                        }
                        for (int64_t r = 0; r < nrows; r++) {
                            bcast = ray_vec_append(bcast, elem);
                            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                        }
                    }
                    ray_release(expr_vec);
                    expr_vec = bcast;
                }

                /* Promote I64 vector to F64 if column is F64 */
                if (expr_vec->type == RAY_I64 && ct == RAY_F64) {
                    int64_t nr = ray_len(expr_vec);
                    ray_t* promoted = ray_vec_new(RAY_F64, nr);
                    if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return promoted; }
                    int64_t* src_data = (int64_t*)ray_data(expr_vec);
                    for (int64_t r = 0; r < nr; r++) {
                        double v = (double)src_data[r];
                        promoted = ray_vec_append(promoted, &v);
                        if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return promoted; }
                    }
                    ray_release(expr_vec);
                    expr_vec = promoted;
                }

                /* Type check: expr_vec must match original column type */
                if (expr_vec->type != ct) {
                    ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                    return ray_error("type", NULL);
                }

                /* Merge: use expr_vec for matching rows, orig_col for non-matching */
                if (ct == RAY_STR) {
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        size_t slen = 0;
                        const char* sp = ray_str_vec_get(src_vec, r, &slen);
                        new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                    }
                } else if (ct == RAY_SYM) {
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        int64_t sym_val = ray_read_sym(ray_data(src_vec), r, src_vec->type, src_vec->attrs);
                        new_col = ray_vec_append(new_col, &sym_val);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                    }
                } else {
                    size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
                    uint8_t* orig_data = (uint8_t*)ray_data(orig_col);
                    uint8_t* expr_data = (uint8_t*)ray_data(expr_vec);
                    for (int64_t r = 0; r < nrows; r++) {
                        void* src = mask[r] ? (expr_data + r * elem_sz) : (orig_data + r * elem_sz);
                        new_col = ray_vec_append(new_col, src);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                    }
                }
                result = ray_table_add_col(result, col_name, new_col);
                ray_release(new_col);
                ray_release(expr_vec);
            }
            if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
        }

        ray_release(mask_vec);
        if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
        }
        ray_release(tbl);
        return result;
    }

    /* No WHERE — update all rows */
    int64_t ncols = ray_table_ncols(tbl);
    int64_t dict_n = ray_len(dict);
    ray_t** dict_elems = (ray_t**)ray_data(dict);
    int64_t from_id = ray_sym_intern("from", 4);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);

        ray_t* update_expr = NULL;
        for (int64_t d = 0; d + 1 < dict_n; d += 2) {
            int64_t kid = dict_elems[d]->i64;
            if (kid == from_id) continue;
            if (kid == col_name) { update_expr = dict_elems[d + 1]; break; }
        }

        if (!update_expr) {
            ray_retain(orig_col);
            result = ray_table_add_col(result, col_name, orig_col);
            ray_release(orig_col);
        } else {
            ray_t* expr_vec = NULL;
            {
                ray_graph_t* ug = ray_graph_new(tbl);
                if (ug) {
                    ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                    if (expr_op) {
                        expr_op = ray_optimize(ug, expr_op);
                        expr_vec = ray_execute(ug, expr_op);
                    }
                    ray_graph_free(ug);
                }
            }
            if (!expr_vec || RAY_IS_ERR(expr_vec)) {
                /* Fallback: eval with column bindings */
                int64_t ncols_f = ray_table_ncols(tbl);
                ray_env_push_scope();
                for (int64_t cf = 0; cf < ncols_f; cf++) {
                    int64_t cn = ray_table_col_name(tbl, cf);
                    ray_t* colf = ray_table_get_col_idx(tbl, cf);
                    ray_env_set(cn, colf);
                }
                expr_vec = ray_eval(update_expr);
                ray_env_pop_scope();
            }
            if (!expr_vec || RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec ? expr_vec : ray_error("type", NULL); }

            /* Broadcast scalar atom to full column vector if needed */
            if (expr_vec->type < 0) {
                int64_t nrows = ray_table_nrows(tbl);
                int8_t ct = orig_col->type;
                /* Type check atom against column type BEFORE broadcast */
                int ok = (expr_vec->type == -ct);
                if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                /* SYM atom → LIST column (LIST of SYM atoms) */
                if (!ok && ct == RAY_LIST && expr_vec->type == -RAY_SYM) ok = 1;
                if (!ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return ray_error("type", NULL);
                }
                /* SYM atom to LIST column: broadcast as boxed list */
                if (ct == RAY_LIST && expr_vec->type == -RAY_SYM) {
                    ray_t* bcast = ray_list_new((int32_t)nrows);
                    if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_retain(expr_vec);
                        bcast = ray_list_append(bcast, expr_vec);
                        ray_release(expr_vec);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                    ray_release(expr_vec);
                    expr_vec = bcast;
                    goto no_where_add_col;
                }
                ray_t* bcast = ray_vec_new(ct, nrows);
                if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                if (ct == RAY_STR && expr_vec->type == -RAY_STR) {
                    const char* sp = ray_str_ptr(expr_vec);
                    size_t sl = ray_str_len(expr_vec);
                    for (int64_t r = 0; r < nrows; r++) {
                        bcast = ray_str_vec_append(bcast, sp, sl);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                } else {
                    size_t esz = (ct == RAY_BOOL) ? 1 : 8;
                    uint8_t elem[8] = {0};
                    if (ct == RAY_F64 && expr_vec->type == -RAY_I64) {
                        double promoted = (double)expr_vec->i64;
                        memcpy(elem, &promoted, 8);
                    } else {
                        memcpy(elem, &expr_vec->i64, esz);
                    }
                    for (int64_t r = 0; r < nrows; r++) {
                        bcast = ray_vec_append(bcast, elem);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                }
                ray_release(expr_vec);
                expr_vec = bcast;
            }

            /* Promote I64 vector to F64 if column is F64 */
            if (expr_vec->type == RAY_I64 && orig_col->type == RAY_F64) {
                int64_t nr = ray_len(expr_vec);
                ray_t* promoted = ray_vec_new(RAY_F64, nr);
                if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return promoted; }
                int64_t* src_data = (int64_t*)ray_data(expr_vec);
                for (int64_t r = 0; r < nr; r++) {
                    double v = (double)src_data[r];
                    promoted = ray_vec_append(promoted, &v);
                    if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return promoted; }
                }
                ray_release(expr_vec);
                expr_vec = promoted;
            }

            /* No-WHERE update: allow type change for same-category types.
             * Atoms (type<0) will be broadcast later, check after broadcast.
             * For vectors, check now: only numeric promotions or same type.
             * Also allow SYM/LIST interop (columns may be stored as LIST). */
            if (expr_vec->type > 0 && expr_vec->type != orig_col->type) {
                int is_ok = 0;
                /* Numeric promotions */
                if ((orig_col->type == RAY_I64 || orig_col->type == RAY_I32 || orig_col->type == RAY_F64) &&
                    (expr_vec->type == RAY_I64 || expr_vec->type == RAY_I32 || expr_vec->type == RAY_F64))
                    is_ok = 1;
                /* SYM/LIST interop */
                if ((orig_col->type == RAY_SYM || orig_col->type == RAY_LIST) &&
                    (expr_vec->type == RAY_SYM || expr_vec->type == RAY_LIST))
                    is_ok = 1;
                if (!is_ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return ray_error("type", NULL);
                }
            }

no_where_add_col:
            result = ray_table_add_col(result, col_name, expr_vec);
            ray_release(expr_vec);
        }
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    /* Add NEW columns from dict (columns not already in the table) */
    for (int64_t d = 0; d + 1 < dict_n; d += 2) {
        int64_t kid = dict_elems[d]->i64;
        if (kid == from_id) continue;
        /* Check if this column already exists */
        int exists = 0;
        for (int64_t c = 0; c < ncols; c++) {
            if (ray_table_col_name(tbl, c) == kid) { exists = 1; break; }
        }
        if (exists) continue;

        /* New column: evaluate expression and add */
        ray_t* update_expr = dict_elems[d + 1];
        ray_graph_t* ug = ray_graph_new(tbl);
        ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
        if (!expr_op) { ray_release(result); ray_release(tbl); ray_graph_free(ug); return ray_error("domain", NULL); }
        expr_op = ray_optimize(ug, expr_op);
        ray_t* expr_vec = ray_execute(ug, expr_op);
        ray_graph_free(ug);
        if (RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec; }

        /* Broadcast scalar to column */
        if (expr_vec->type < 0) {
            int64_t nrows = ray_table_nrows(tbl);
            int8_t ct = -expr_vec->type;
            ray_t* bcast = ray_vec_new(ct, nrows);
            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
            size_t esz = ray_elem_size(ct);
            uint8_t elem[8] = {0};
            memcpy(elem, &expr_vec->i64, esz > 8 ? 8 : esz);
            for (int64_t r = 0; r < nrows; r++) {
                bcast = ray_vec_append(bcast, elem);
                if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
            }
            ray_release(expr_vec);
            expr_vec = bcast;
        }

        result = ray_table_add_col(result, kid, expr_vec);
        ray_release(expr_vec);
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    /* Store in-place if from: 't */
    if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
    }
    ray_release(tbl);
    return result;
}

/* (insert table (list val1 val2 ...)) — append a row to a table */
ray_t* ray_insert(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);

    /* Special form: detect 'sym (quoted symbol for in-place insert) */
    int64_t inplace_sym = -1;
    ray_t* tbl_raw = args[0];
    ray_t* tbl;

    /* Detect calling convention: already-evaluated args (from upsert) vs raw parse tree */
    int already_eval = (tbl_raw && tbl_raw->type == RAY_TABLE);

    if (!already_eval && tbl_raw && tbl_raw->type == -RAY_SYM && !(tbl_raw->attrs & RAY_ATTR_NAME)) {
        /* Quoted symbol 'sym (no ATTR_NAME) — in-place insert */
        inplace_sym = tbl_raw->i64;
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    } else if (already_eval) {
        tbl = tbl_raw;
        ray_retain(tbl);
    } else {
        tbl = ray_eval(tbl_raw);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", NULL);
    }

    /* Evaluate the row argument (skip if already evaluated) */
    ray_t* row = already_eval ? (ray_retain(args[1]), args[1]) : ray_eval(args[1]);
    if (!row || RAY_IS_ERR(row)) { ray_release(tbl); return row ? row : ray_error("type", NULL); }
    if (tbl->type != RAY_TABLE) { ray_release(tbl); ray_release(row); return ray_error("type", NULL); }

    int64_t ncols = ray_table_ncols(tbl);
    ray_t* row_orig = row; /* keep original eval result for cleanup */

    if (!is_list(row) && row->type != RAY_TABLE) { ray_release(tbl); ray_release(row); return ray_error("type", NULL); }

    /* Table row: convert to list of column vectors */
    ray_t* tbl_row_list = NULL;
    if (row->type == RAY_TABLE) {
        int64_t src_ncols = ray_table_ncols(row);
        if (src_ncols != ncols) { ray_release(tbl); ray_release(row); return ray_error("domain", NULL); }
        tbl_row_list = ray_alloc(ncols * sizeof(ray_t*));
        if (!tbl_row_list) { ray_release(tbl); ray_release(row_orig); return ray_error("oom", NULL); }
        tbl_row_list->type = RAY_LIST;
        tbl_row_list->len = ncols;
        ray_t** trl = (ray_t**)ray_data(tbl_row_list);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            ray_t* src_col = ray_table_get_col(row, col_name);
            if (!src_col) src_col = ray_table_get_col_idx(row, c);
            if (!src_col) {
                tbl_row_list->len = 0;
                ray_free(tbl_row_list);
                ray_release(tbl); ray_release(row_orig);
                return ray_error("domain", NULL);
            }
            trl[c] = src_col;
            ray_retain(src_col);
        }
        row = tbl_row_list;
    }

    /* Dict row: extract values in table column order */
    ray_t* dict_vals = NULL;
    if (row->attrs & RAY_ATTR_DICT) {
        dict_vals = ray_alloc(ncols * sizeof(ray_t*));
        if (!dict_vals) { ray_release(tbl); ray_release(row_orig); return ray_error("oom", NULL); }
        dict_vals->type = RAY_LIST;
        dict_vals->len = ncols;
        ray_t** dv = (ray_t**)ray_data(dict_vals);
        ray_t** dict_items = (ray_t**)ray_data(row);
        int64_t dict_len = ray_len(row);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            dv[c] = NULL;
            for (int64_t d = 0; d + 1 < dict_len; d += 2) {
                if (dict_items[d]->type == -RAY_SYM && dict_items[d]->i64 == col_name) {
                    dv[c] = dict_items[d + 1];
                    ray_retain(dv[c]);
                    break;
                }
            }
            /* dv[c] may be NULL for missing keys — will insert null
             * (but only if ALL dict keys exist as table columns) */
        }
        /* Verify all dict keys exist as table columns */
        for (int64_t d = 0; d + 1 < dict_len; d += 2) {
            if (dict_items[d]->type != -RAY_SYM) continue;
            int64_t dk = dict_items[d]->i64;
            int found_in_tbl = 0;
            for (int64_t c = 0; c < ncols; c++) {
                if (ray_table_col_name(tbl, c) == dk) { found_in_tbl = 1; break; }
            }
            if (!found_in_tbl) {
                for (int64_t c = 0; c < ncols; c++) if (dv[c]) ray_release(dv[c]);
                dict_vals->len = 0;
                ray_free(dict_vals);
                ray_release(tbl); ray_release(row_orig);
                return ray_error("domain", NULL);
            }
        }
        row = dict_vals;
    }

    if (ray_len(row) != ncols) {
        if (dict_vals) {
            for (int64_t c = 0; c < ncols; c++) ray_release(((ray_t**)ray_data(dict_vals))[c]);
            dict_vals->len = 0;
            ray_free(dict_vals);
        }
        ray_release(tbl); ray_release(row_orig);
        return ray_error("domain", NULL);
    }

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows + 1);
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        /* Copy existing data */
        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                size_t slen = 0;
                const char* sp = ray_str_vec_get(orig_col, r, &slen);
                new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                new_col = ray_vec_append(new_col, &sym_val);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                new_col = ray_vec_append(new_col, src + r * elem_sz);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        }

        /* Append new row value(s) — atom for single row, vector for multi-row */
        if (!row_elems[c]) {
            /* NULL = null value for this column type */
            ray_t* null_atom = NULL;
            if (ct == RAY_I64) null_atom = ray_i64(INT64_MIN);
            else if (ct == RAY_F64) { double nan_val = NAN; null_atom = ray_f64(nan_val); }
            else if (ct == RAY_SYM) null_atom = ray_sym(INT64_MIN);
            else null_atom = ray_i64(0);
            new_col = append_atom_to_col(new_col, null_atom);
            ray_release(null_atom);
        } else if (ray_is_atom(row_elems[c])) {
            new_col = append_atom_to_col(new_col, row_elems[c]);
        } else if (ray_is_vec(row_elems[c]) || row_elems[c]->type == RAY_LIST) {
            ray_t* merged = ray_concat_fn(new_col, row_elems[c]);
            ray_release(new_col);
            new_col = merged;
        } else {
            new_col = append_atom_to_col(new_col, row_elems[c]);
        }
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    /* Cleanup dict_vals, tbl_row_list, and original row */
    if (dict_vals) {
        ray_t** dv = (ray_t**)ray_data(dict_vals);
        for (int64_t c = 0; c < ncols; c++) if (dv[c]) ray_release(dv[c]);
        dict_vals->len = 0; /* prevent ray_free from double-releasing children */
        ray_free(dict_vals);
    }
    if (tbl_row_list) {
        ray_t** trl = (ray_t**)ray_data(tbl_row_list);
        for (int64_t c = 0; c < ncols; c++) if (trl[c]) ray_release(trl[c]);
        tbl_row_list->len = 0;
        ray_free(tbl_row_list);
    }
    ray_release(tbl);
    ray_release(row_orig);

    /* In-place: update the variable in the env */
    if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
        ray_retain(result);
        return result;
    }
    return result;
}

/* (upsert table key_col (list val1 val2 ...)) — update row if key matches, else insert.
 * Special form: first arg may be 'sym for in-place, other args are evaluated. */
ray_t* ray_upsert(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);

    /* Detect calling convention: already-evaluated args (from recursive call) vs raw parse tree */
    int64_t inplace_sym = -1;
    ray_t* tbl_raw = args[0];
    int already_eval = (tbl_raw && tbl_raw->type == RAY_TABLE);
    ray_t* tbl;

    if (!already_eval && tbl_raw && tbl_raw->type == -RAY_SYM && !(tbl_raw->attrs & RAY_ATTR_NAME)) {
        inplace_sym = tbl_raw->i64;
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    } else if (already_eval) {
        tbl = tbl_raw;
        ray_retain(tbl);
    } else {
        tbl = ray_eval(tbl_raw);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", NULL);
    }

    ray_t* key_sym = already_eval ? (ray_retain(args[1]), args[1]) : ray_eval(args[1]);
    if (!key_sym || RAY_IS_ERR(key_sym)) { ray_release(tbl); return key_sym ? key_sym : ray_error("type", NULL); }

    ray_t* row = already_eval ? (ray_retain(args[2]), args[2]) : ray_eval(args[2]);
    if (!row || RAY_IS_ERR(row)) { ray_release(tbl); ray_release(key_sym); return row ? row : ray_error("type", NULL); }

    if (tbl->type != RAY_TABLE) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("type", NULL); }
    if (!is_list(row) && row->type != RAY_TABLE) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("type", NULL); }

    int64_t ncols = ray_table_ncols(tbl);

    /* Table row: iterate row-by-row for proper upsert semantics */
    if (row->type == RAY_TABLE) {
        int64_t src_nrows = ray_table_nrows(row);
        /* Get source columns by matching target column names; missing cols → NULL */
        ray_t* src_cols[64];
        for (int64_t c = 0; c < ncols && c < 64; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            src_cols[c] = ray_table_get_col(row, cn);
        }
        ray_t* cur_tbl = tbl;
        ray_retain(cur_tbl);
        for (int64_t r = 0; r < src_nrows; r++) {
            ray_t* single = ray_alloc(ncols * sizeof(ray_t*));
            if (!single) { ray_release(cur_tbl); ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
            single->type = RAY_LIST;
            single->len = ncols;
            ray_t** sr = (ray_t**)ray_data(single);
            for (int64_t c = 0; c < ncols; c++) {
                int alloc = 0;
                sr[c] = src_cols[c] ? collection_elem(src_cols[c], r, &alloc) : NULL;
                if (!alloc && sr[c]) ray_retain(sr[c]);
            }
            ray_t* upsert_args[3] = { cur_tbl, key_sym, single };
            ray_t* new_tbl = ray_upsert(upsert_args, 3);
            for (int64_t c = 0; c < ncols; c++) if (sr[c]) ray_release(sr[c]);
            single->len = 0;
            ray_free(single);
            ray_release(cur_tbl);
            if (RAY_IS_ERR(new_tbl)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return new_tbl; }
            cur_tbl = new_tbl;
        }
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(cur_tbl)) {
            ray_env_set(inplace_sym, cur_tbl);
            ray_retain(cur_tbl);
        }
        return cur_tbl;
    }

    /* Dict row: extract values in column order to create a plain list */
    ray_t* dict_row_list = NULL;
    if (row->attrs & RAY_ATTR_DICT) {
        dict_row_list = ray_alloc(ncols * sizeof(ray_t*));
        if (!dict_row_list) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
        dict_row_list->type = RAY_LIST;
        dict_row_list->len = ncols;
        ray_t** drl = (ray_t**)ray_data(dict_row_list);
        ray_t** dict_items = (ray_t**)ray_data(row);
        int64_t dict_len = ray_len(row);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            drl[c] = NULL;
            for (int64_t d = 0; d + 1 < dict_len; d += 2) {
                if (dict_items[d]->type == -RAY_SYM && dict_items[d]->i64 == col_name) {
                    drl[c] = dict_items[d + 1];
                    ray_retain(drl[c]);
                    break;
                }
            }
            if (!drl[c]) {
                for (int64_t j = 0; j < c; j++) if (drl[j]) ray_release(drl[j]);
                dict_row_list->len = 0;
                ray_free(dict_row_list);
                ray_release(tbl); ray_release(key_sym); ray_release(row);
                return ray_error("domain", NULL);
            }
        }
        ray_release(row);
        row = dict_row_list;
    }

    if (ray_len(row) != ncols) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    /* Determine key columns — integer N means "first N columns are keys" */
    int64_t n_key_cols = 1;
    int64_t key_col_indices[16];
    if (key_sym->type == -RAY_SYM) {
        key_col_indices[0] = -1;
        for (int64_t c = 0; c < ncols; c++) {
            if (ray_table_col_name(tbl, c) == key_sym->i64) {
                key_col_indices[0] = c;
                break;
            }
        }
        if (key_col_indices[0] < 0) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }
    } else if (key_sym->type == -RAY_I64) {
        n_key_cols = key_sym->i64;
        if (n_key_cols <= 0 || n_key_cols > ncols || n_key_cols > 16) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }
        for (int64_t k = 0; k < n_key_cols; k++) key_col_indices[k] = k;
    } else {
        ray_release(tbl); ray_release(key_sym); ray_release(row);
        return ray_error("type", NULL);
    }

    /* Multi-row upsert: if row values are vectors, iterate row-by-row */
    ray_t* key_elem = row_elems[key_col_indices[0]];
    if (ray_is_vec(key_elem) || key_elem->type == RAY_LIST) {
        int64_t new_nrows = ray_len(key_elem);
        ray_t* cur_tbl = tbl;
        ray_retain(cur_tbl);
        for (int64_t r = 0; r < new_nrows; r++) {
            /* Build single-row list from multi-row columns */
            ray_t* single_row = ray_alloc(ncols * sizeof(ray_t*));
            if (!single_row) { ray_release(cur_tbl); ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
            single_row->type = RAY_LIST;
            single_row->len = ncols;
            ray_t** sr = (ray_t**)ray_data(single_row);
            for (int64_t c = 0; c < ncols; c++) {
                int alloc = 0;
                sr[c] = collection_elem(row_elems[c], r, &alloc);
                if (!alloc && sr[c]) ray_retain(sr[c]);
            }
            /* Upsert single row into current table */
            ray_t* upsert_args[3] = { cur_tbl, key_sym, single_row };
            ray_t* new_tbl = ray_upsert(upsert_args, 3);
            /* Clean up single_row */
            for (int64_t c = 0; c < ncols; c++) if (sr[c]) ray_release(sr[c]);
            single_row->len = 0;
            ray_free(single_row);
            ray_release(cur_tbl);
            if (RAY_IS_ERR(new_tbl)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return new_tbl; }
            cur_tbl = new_tbl;
        }
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(cur_tbl)) {
            ray_env_set(inplace_sym, cur_tbl);
            ray_retain(cur_tbl);
        }
        return cur_tbl;
    }

    /* Type-check key columns before searching */
    for (int64_t k = 0; k < n_key_cols; k++) {
        int64_t kci = key_col_indices[k];
        ray_t* key_col = ray_table_get_col_idx(tbl, kci);
        ray_t* key_atom = row_elems[kci];
        int8_t kt = key_col->type;
        if (kt == RAY_STR && key_atom->type != -RAY_STR) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
        if (kt == RAY_SYM && key_atom->type != -RAY_SYM) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
    }

    /* Find the row to update by composite key match */
    int64_t match_row = -1;
    for (int64_t r = 0; r < nrows; r++) {
        int match = 1;
        for (int64_t k = 0; k < n_key_cols && match; k++) {
            int64_t kci = key_col_indices[k];
            ray_t* key_col = ray_table_get_col_idx(tbl, kci);
            ray_t* key_atom = row_elems[kci];
            int8_t kt = key_col->type;
            if (kt == RAY_F64) {
                double needle = (key_atom->type == -RAY_F64) ? key_atom->f64 : (double)key_atom->i64;
                if (((double*)ray_data(key_col))[r] != needle) match = 0;
            } else if (kt == RAY_SYM) {
                if (ray_read_sym(ray_data(key_col), r, key_col->type, key_col->attrs) != key_atom->i64) match = 0;
            } else if (kt == RAY_STR) {
                const char* ns = ray_str_ptr(key_atom);
                size_t nl = ray_str_len(key_atom);
                size_t rl = 0;
                const char* rs = ray_str_vec_get(key_col, r, &rl);
                if (rl != nl || (nl > 0 && (!rs || !ns || memcmp(rs, ns, nl) != 0))) match = 0;
            } else {
                int64_t needle = elem_as_i64(key_atom);
                int64_t existing = (kt == RAY_I64 || kt == RAY_TIMESTAMP) ?
                    ((int64_t*)ray_data(key_col))[r] :
                    (kt == RAY_I32 || kt == RAY_DATE || kt == RAY_TIME) ?
                    (int64_t)((int32_t*)ray_data(key_col))[r] :
                    (kt == RAY_BOOL) ? (int64_t)((uint8_t*)ray_data(key_col))[r] :
                    ((int64_t*)ray_data(key_col))[r];
                if (existing != needle) match = 0;
            }
        }
        if (match) { match_row = r; break; }
    }

    if (match_row < 0) {
        /* Key not found — insert: pass pre-evaluated args */
        ray_t* insert_args[2] = { tbl, row };
        ray_t* result = ray_insert(insert_args, 2);
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
            ray_retain(result);
        }
        return result;
    }

    /* Key found — update that row */
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows);
        if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }

        /* If row_elems[c] is NULL (missing column), keep original values */
        int has_new_val = (row_elems[c] != NULL);

        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(orig_col, r, &slen);
                    new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                    new_col = ray_vec_append(new_col, &sym_val);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    new_col = ray_vec_append(new_col, src + r * elem_sz);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return result; }
    }

    ray_release(tbl);
    ray_release(key_sym);
    ray_release(row);

    if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
        ray_retain(result);
    }
    return result;
}

/* ══════════════════════════════════════════
 * Join operations
 * ══════════════════════════════════════════ */

/* Shared implementation for left-join (join_type=1) and inner-join (join_type=0).
 * (left-join t1 t2 [key ...]) / (inner-join t1 t2 [key ...]) */
static ray_t* join_impl(ray_t** args, int64_t n, uint8_t join_type) {
    if (n < 3) return ray_error("domain", NULL);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* keys      = args[2];

    /* Detect alternative calling convention: (join [keys] t1 t2) */
    if (left_tbl->type != RAY_TABLE && args[1]->type == RAY_TABLE && args[2]->type == RAY_TABLE) {
        keys      = args[0];
        left_tbl  = args[1];
        right_tbl = args[2];
    }

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    ray_t* _bxk = NULL;
    keys = unbox_vec_arg(keys, &_bxk);
    if (RAY_IS_ERR(keys)) return keys;
    if (!is_list(keys))
        { if (_bxk) ray_release(_bxk); return ray_error("type", NULL); }

    int64_t nk = ray_len(keys);
    if (nk == 0 || nk > 16) { if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_t** key_elems = (ray_t**)ray_data(keys);

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_op_t* lk[16], *rk[16];
    for (int64_t i = 0; i < nk; i++) {
        if (key_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* name_str = ray_sym_str(key_elems[i]->i64);
        if (!name_str) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        lk[i] = ray_scan(g, ray_str_ptr(name_str));
        rk[i] = ray_scan(g, ray_str_ptr(name_str));
        if (!lk[i] || !rk[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_join(g, left_node, lk, right_node, rk,
                           (uint8_t)nk, join_type);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

ray_t* ray_left_join(ray_t** args, int64_t n)  { return join_impl(args, n, 1); }
ray_t* ray_inner_join(ray_t** args, int64_t n) { return join_impl(args, n, 0); }

/* (window-join t1 t2 [eq-keys] time-col)
 * ASOF join: for each left row, find closest right row with time <= left.time
 * within the same equality partition. */
ray_t* ray_window_join(ray_t** args, int64_t n) {
    if (n < 4) return ray_error("domain", NULL);

    /* Special form: evaluate first 4 args, keep agg dict (args[4]) unevaluated */
    ray_t* eargs[5];
    for (int i = 0; i < 4 && i < (int)n; i++) {
        eargs[i] = ray_eval(args[i]);
        if (!eargs[i] || RAY_IS_ERR(eargs[i])) {
            for (int j = 0; j < i; j++) ray_release(eargs[j]);
            return eargs[i] ? eargs[i] : ray_error("type", NULL);
        }
    }
    eargs[4] = (n >= 5) ? args[4] : NULL; /* agg dict stays unevaluated */

    /* Detect rayforce calling convention:
     * (window-join [eq+time keys] intervals left right {agg})
     * vs teide convention:
     * (window-join left right [eq-keys] time-sym) */
    if (n >= 5 && ray_is_vec(eargs[0]) && eargs[0]->type == RAY_SYM &&
        eargs[2]->type == RAY_TABLE && eargs[3]->type == RAY_TABLE) {
        /* Rayforce convention: implement at eval level */
        ray_t* keys_vec = eargs[0];      /* [Sym Time] — equality + time keys */
        ray_t* intervals = eargs[1];     /* list of [lo hi] time windows */
        ray_t* left_tbl = eargs[2];      /* trades */
        ray_t* right_tbl = eargs[3];     /* quotes */
        ray_t* agg_dict = eargs[4];      /* unevaluated dict */

        int64_t nkeys = ray_len(keys_vec);
        if (nkeys < 2) return ray_error("domain", NULL);
        int64_t* key_ids = (int64_t*)ray_data(keys_vec);

        /* Last key is the time key, rest are equality keys */
        int64_t time_key = key_ids[nkeys - 1];
        int64_t n_eq = nkeys - 1;

        int64_t left_nrows = ray_table_nrows(left_tbl);
        int64_t right_nrows = ray_table_nrows(right_tbl);

        /* Get left time column */
        ray_t* left_time = ray_table_get_col(left_tbl, time_key);
        ray_t* right_time = ray_table_get_col(right_tbl, time_key);
        if (!left_time || !right_time) return ray_error("domain", NULL);

        /* Get equality columns */
        ray_t* left_eq[16], *right_eq[16];
        for (int64_t e = 0; e < n_eq && e < 16; e++) {
            left_eq[e] = ray_table_get_col(left_tbl, key_ids[e]);
            right_eq[e] = ray_table_get_col(right_tbl, key_ids[e]);
            if (!left_eq[e] || !right_eq[e]) return ray_error("domain", NULL);
        }

        /* Get aggregation info from dict */
        int64_t agg_result_name = -1;
        uint16_t agg_op = OP_MIN;
        int64_t agg_src_col = -1;
        if (agg_dict && agg_dict->type == RAY_LIST && (agg_dict->attrs & RAY_ATTR_DICT)) {
            ray_t** ad = (ray_t**)ray_data(agg_dict);
            int64_t adn = ray_len(agg_dict);
            if (adn >= 2) {
                agg_result_name = ad[0]->i64; /* minBid */
                ray_t* agg_expr = ad[1]; /* (min Bid) */
                if (agg_expr->type == RAY_LIST && agg_expr->len >= 2) {
                    ray_t** ae = (ray_t**)ray_data(agg_expr);
                    if (ae[0]->type == -RAY_SYM && (ae[0]->attrs & RAY_ATTR_NAME))
                        agg_op = resolve_agg_opcode(ae[0]->i64);
                    if (ae[1]->type == -RAY_SYM && (ae[1]->attrs & RAY_ATTR_NAME))
                        agg_src_col = ae[1]->i64;
                }
            }
        }

        ray_t* right_agg_col = (agg_src_col >= 0) ? ray_table_get_col(right_tbl, agg_src_col) : NULL;
        if (agg_src_col >= 0 && !right_agg_col) return ray_error("domain", NULL);

        /* For each left row, find matching right rows within the time window */
        /* intervals is a list of [lo, hi] pairs, one per left row */
        ray_t* result_agg = ray_vec_new(RAY_I64, left_nrows);
        if (RAY_IS_ERR(result_agg)) return result_agg;

        for (int64_t lr = 0; lr < left_nrows; lr++) {
            /* Get interval for this left row */
            int alloc_iv = 0;
            ray_t* iv = collection_elem(intervals, lr, &alloc_iv);
            if (!iv || RAY_IS_ERR(iv) || ray_len(iv) < 2) {
                if (alloc_iv && iv) ray_release(iv);
                ray_release(result_agg);
                return ray_error("domain", NULL);
            }
            int alloc_lo = 0, alloc_hi = 0;
            ray_t* lo_atom = collection_elem(iv, 0, &alloc_lo);
            ray_t* hi_atom = collection_elem(iv, 1, &alloc_hi);
            int64_t lo = as_i64(lo_atom);
            int64_t hi = as_i64(hi_atom);
            if (alloc_lo) ray_release(lo_atom);
            if (alloc_hi) ray_release(hi_atom);
            if (alloc_iv) ray_release(iv);

            /* Find right rows matching equality keys AND time in [lo, hi] */
            int64_t best_val_i = INT64_MAX;
            double best_val_f = 1e300;
            int found = 0;
            int is_f64 = (right_agg_col && right_agg_col->type == RAY_F64);

            for (int64_t rr = 0; rr < right_nrows; rr++) {
                /* Check equality keys */
                int eq_match = 1;
                for (int64_t e = 0; e < n_eq && eq_match; e++) {
                    int64_t lv = (left_eq[e]->type == RAY_SYM) ?
                        ((int64_t*)ray_data(left_eq[e]))[lr] :
                        ((int64_t*)ray_data(left_eq[e]))[lr];
                    int64_t rv = (right_eq[e]->type == RAY_SYM) ?
                        ((int64_t*)ray_data(right_eq[e]))[rr] :
                        ((int64_t*)ray_data(right_eq[e]))[rr];
                    if (lv != rv) eq_match = 0;
                }
                if (!eq_match) continue;

                /* Check time window — TIME is i32, TIMESTAMP is i64 */
                int64_t rt;
                if (right_time->type == RAY_TIME || right_time->type == RAY_I32 || right_time->type == RAY_DATE)
                    rt = (int64_t)((int32_t*)ray_data(right_time))[rr];
                else
                    rt = ((int64_t*)ray_data(right_time))[rr];
                if (rt < lo || rt > hi) continue;

                /* Apply aggregation */
                if (right_agg_col) {
                    if (is_f64) {
                        double v = ((double*)ray_data(right_agg_col))[rr];
                        if (!found || (agg_op == OP_MIN && v < best_val_f) ||
                            (agg_op == OP_MAX && v > best_val_f))
                            best_val_f = v;
                    } else {
                        int64_t v = ((int64_t*)ray_data(right_agg_col))[rr];
                        if (!found || (agg_op == OP_MIN && v < best_val_i) ||
                            (agg_op == OP_MAX && v > best_val_i))
                            best_val_i = v;
                    }
                    found = 1;
                }
            }

            /* Store result */
            if (is_f64) {
                double v = found ? best_val_f : NAN;
                result_agg = ray_vec_append(result_agg, &v);
            } else {
                int64_t v = found ? best_val_i : INT64_MIN;
                result_agg = ray_vec_append(result_agg, &v);
            }
            if (RAY_IS_ERR(result_agg)) return result_agg;
        }

        /* Build result table: left table + aggregation column */
        int64_t ncols = ray_table_ncols(left_tbl);
        ray_t* result = ray_table_new(ncols + 1);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(left_tbl, c);
            ray_t* cv = ray_table_get_col_idx(left_tbl, c);
            ray_retain(cv);
            result = ray_table_add_col(result, cn, cv);
            ray_release(cv);
        }
        if (agg_result_name >= 0) {
            result = ray_table_add_col(result, agg_result_name, result_agg);
        }
        ray_release(result_agg);
        for (int i = 0; i < 4; i++) ray_release(eargs[i]);
        return result;
    }

    ray_t* left_tbl  = eargs[0];
    ray_t* right_tbl = eargs[1];
    ray_t* eq_keys   = eargs[2];
    ray_t* time_sym  = eargs[3];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    if (time_sym->type != -RAY_SYM)
        return ray_error("type", NULL);

    uint8_t n_eq = 0;
    ray_t** eq_elems = NULL;
    ray_t* _bxeq = NULL;
    eq_keys = unbox_vec_arg(eq_keys, &_bxeq);
    if (is_list(eq_keys)) {
        n_eq = (uint8_t)ray_len(eq_keys);
        eq_elems = (ray_t**)ray_data(eq_keys);
    }

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) return ray_error("oom", NULL);

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); return ray_error("domain", NULL); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); return ray_error("domain", NULL); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g);
            return ray_error("type", NULL);
        }
        ray_t* nm = ray_sym_str(eq_elems[i]->i64);
        if (!nm) { ray_graph_free(g); return ray_error("domain", NULL); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); return ray_error("domain", NULL); }
    }

    if (_bxeq) ray_release(_bxeq);

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

/* (asof-join [key1 key2 ... timeKey] leftTable rightTable)
 * Last key is the time/asof column, rest are equality keys. */
ray_t* ray_asof_join_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);
    ray_t* keys_vec   = args[0];
    ray_t* left_tbl   = args[1];
    ray_t* right_tbl  = args[2];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);

    /* Keys vector must be a SYM vector with at least 2 elements (eq + time) */
    ray_t* _bxk = NULL;
    keys_vec = unbox_vec_arg(keys_vec, &_bxk);
    if (!is_list(keys_vec) || ray_len(keys_vec) < 2) {
        if (_bxk) ray_release(_bxk);
        return ray_error("domain", NULL);
    }
    ray_t** kelems = (ray_t**)ray_data(keys_vec);
    int64_t nkeys = ray_len(keys_vec);

    /* Last key is the time column */
    ray_t* time_sym = kelems[nkeys - 1];
    if (time_sym->type != -RAY_SYM) {
        if (_bxk) ray_release(_bxk);
        return ray_error("type", NULL);
    }

    /* Remaining keys are equality keys */
    uint8_t n_eq = (uint8_t)(nkeys - 1);
    ray_t** eq_syms = kelems; /* first n_eq elements */

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_syms[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* nm = ray_sym_str(eq_syms[i]->i64);
        if (!nm) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

/* ══════════════════════════════════════════
 * I/O builtins: println, read-csv, write-csv, as, type
 * ══════════════════════════════════════════ */

/* Helper: print a ray_t value to a file handle */
void ray_lang_print(FILE* fp, ray_t* val) {
    if (!val || RAY_IS_ERR(val)) { fprintf(fp, "error"); return; }
    /* Materialize lazy handles before printing */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (!val || RAY_IS_ERR(val)) { fprintf(fp, "error"); return; }
    switch (val->type) {
    case -RAY_I64:  fprintf(fp, "%ld", (long)val->i64); break;
    case -RAY_F64:  fprintf(fp, "%g", val->f64); break;
    case -RAY_BOOL: fprintf(fp, "%s", val->b8 ? "true" : "false"); break;
    case -RAY_SYM: {
        ray_t* s = ray_sym_str(val->i64);
        if (s) fprintf(fp, "'%.*s", (int)ray_str_len(s), ray_str_ptr(s));
        else fprintf(fp, "'?");
        break;
    }
    case -RAY_STR: {
        const char* s = ray_str_ptr(val);
        size_t slen = ray_str_len(val);
        fprintf(fp, "%.*s", (int)slen, s);
        break;
    }
    case RAY_LIST: {
        fprintf(fp, "[");
        int64_t len = ray_len(val);
        ray_t** elems = (ray_t**)ray_data(val);
        for (int64_t i = 0; i < len; i++) {
            if (i > 0) fprintf(fp, " ");
            ray_lang_print(fp, elems[i]);
        }
        fprintf(fp, "]");
        break;
    }
    case RAY_TABLE:
        fprintf(fp, "<table %ldx%ld>",
                (long)ray_table_nrows(val), (long)ray_table_ncols(val));
        break;
    default:
        fprintf(fp, "<type:%d>", val->type);
        break;
    }
}

/* Helper: format string with % placeholders, substituting args.
 * Returns a heap-allocated char* (caller must ray_sys_free) and sets *out_len.
 * If fmt has no %, returns NULL (caller falls back to plain print). */
static char* fmt_interpolate(const char* fmt, size_t flen, ray_t** args, int64_t nargs, int64_t arg_start, size_t* out_len) {
    /* Quick scan: any % in fmt? */
    int has_pct = 0;
    for (size_t i = 0; i < flen; i++) if (fmt[i] == '%') { has_pct = 1; break; }
    if (!has_pct) return NULL;

    /* Build result in a dynamic buffer */
    size_t cap = flen + 256;
    char* buf = ray_sys_alloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    int64_t ai = arg_start;

    for (size_t i = 0; i < flen; i++) {
        if (fmt[i] == '%' && ai < nargs) {
            /* Format the arg into a temp buffer */
            char tmp[256];
            ray_t* a = args[ai++];
            if (ray_is_lazy(a)) a = ray_lazy_materialize(a);
            int tlen = 0;
            if (!a || RAY_IS_ERR(a)) {
                tlen = snprintf(tmp, sizeof(tmp), "error");
            } else if (a->type == -RAY_I64) {
                tlen = snprintf(tmp, sizeof(tmp), "%ld", (long)a->i64);
            } else if (a->type == -RAY_F64) {
                tlen = snprintf(tmp, sizeof(tmp), "%g", a->f64);
            } else if (a->type == -RAY_BOOL) {
                tlen = snprintf(tmp, sizeof(tmp), "%s", a->b8 ? "true" : "false");
            } else if (a->type == -RAY_STR) {
                const char* sp = ray_str_ptr(a);
                size_t sl = ray_str_len(a);
                while (pos + sl + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
                memcpy(buf + pos, sp, sl);
                pos += sl;
                continue;
            } else if (a->type == -RAY_SYM) {
                ray_t* ss = ray_sym_str(a->i64);
                if (ss) {
                    const char* sp = ray_str_ptr(ss);
                    size_t sl = ray_str_len(ss);
                    while (pos + sl + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
                    memcpy(buf + pos, sp, sl);
                    pos += sl;
                    ray_release(ss);
                    continue;
                }
                tlen = snprintf(tmp, sizeof(tmp), "'?");
            } else {
                /* Fall back to ray_fmt */
                ray_t* formatted = ray_fmt(a, 0);
                if (formatted && !RAY_IS_ERR(formatted)) {
                    const char* sp = ray_str_ptr(formatted);
                    size_t sl = ray_str_len(formatted);
                    while (pos + sl + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
                    memcpy(buf + pos, sp, sl);
                    pos += sl;
                    ray_release(formatted);
                    continue;
                }
                if (formatted) ray_release(formatted);
                tlen = snprintf(tmp, sizeof(tmp), "<type:%d>", a->type);
            }
            while (pos + (size_t)tlen + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
            memcpy(buf + pos, tmp, (size_t)tlen);
            pos += (size_t)tlen;
        } else {
            if (pos + 2 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
            buf[pos++] = fmt[i];
        }
    }
    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* (println val1 val2 ...) — print values to stdout, newline at end.
 * If first arg is a string with % placeholders, substitutes remaining args. */
ray_t* ray_println(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    /* Format string mode: first arg is a string with % placeholders */
    if (n >= 2 && args[0] && args[0]->type == -RAY_STR) {
        const char* fmt = ray_str_ptr(args[0]);
        size_t flen = ray_str_len(args[0]);
        size_t out_len = 0;
        char* result = fmt_interpolate(fmt, flen, args, n, 1, &out_len);
        if (result) {
            fwrite(result, 1, out_len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            ray_sys_free(result);
            return make_i64(0);
        }
    }

    for (int64_t i = 0; i < n; i++) {
        if (i > 0) fputc(' ', stdout);
        ray_lang_print(stdout, args[i]);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return make_i64(0);
}

/* (show val1 val2 ...) — print values to stdout using ray_fmt, newline at end */
static ray_t* ray_show(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);
        if (!args[i] || RAY_IS_ERR(args[i])) { fprintf(stdout, "error"); continue; }
        ray_t* formatted = ray_fmt(args[i], 1);
        if (formatted && !RAY_IS_ERR(formatted)) {
            const char* sp = ray_str_ptr(formatted);
            size_t sl = ray_str_len(formatted);
            fwrite(sp, 1, sl, stdout);
            ray_release(formatted);
        } else {
            if (formatted) ray_release(formatted);
            ray_lang_print(stdout, args[i]);
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
    return make_i64(0);
}

/* (format "hello % world %" a b) — string formatting with % placeholders */
static ray_t* ray_format_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);
    if (!args[0] || args[0]->type != -RAY_STR) return ray_error("type", NULL);
    const char* fmt = ray_str_ptr(args[0]);
    size_t flen = ray_str_len(args[0]);
    size_t out_len = 0;
    char* result = fmt_interpolate(fmt, flen, args, n, 1, &out_len);
    if (result) {
        ray_t* s = ray_str(result, out_len);
        ray_sys_free(result);
        return s;
    }
    /* No placeholders: return fmt as-is */
    ray_retain(args[0]);
    return args[0];
}

/* (resolve 'name) — check if name exists in env, return value or null.
 * SPECIAL_FORM: does not evaluate args. */
static ray_t* ray_resolve_fn(ray_t** args, int64_t n) {
    if (n < 1) return NULL;
    ray_t* name = ray_eval(args[0]);
    if (!name || RAY_IS_ERR(name)) return NULL;
    if (name->type != -RAY_SYM) { ray_release(name); return NULL; }
    ray_t* val = ray_env_get(name->i64);
    ray_release(name);
    if (!val) return NULL;
    ray_retain(val);
    return val;
}

/* (timeit expr) — evaluate expression and return time in ms as F64.
 * SPECIAL_FORM: does not pre-evaluate args. */
static ray_t* ray_timeit_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    clock_t t0 = clock();
    ray_t* result = ray_eval(args[0]);
    clock_t t1 = clock();
    if (result && !RAY_IS_ERR(result)) ray_release(result);
    double ms = (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1000.0;
    return make_f64(ms);
}

/* (exit code) — exit the process */
static ray_t* ray_exit_fn(ray_t* arg) {
    int code = 0;
    if (arg && is_numeric(arg)) code = (int)as_i64(arg);
    exit(code);
    return NULL; /* unreachable */
}

/* (read-csv path) — read CSV file, return RAY_TABLE */
/* Helper: resolve a type name symbol to a ray type code */
static int8_t resolve_type_name(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return -1;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    int8_t result = -1;
    if (len == 3 && memcmp(name, "I64", 3) == 0) result = RAY_I64;
    else if (len == 3 && memcmp(name, "I32", 3) == 0) result = RAY_I32;
    else if (len == 3 && memcmp(name, "I16", 3) == 0) result = RAY_I16;
    else if (len == 3 && memcmp(name, "F64", 3) == 0) result = RAY_F64;
    else if (len == 2 && memcmp(name, "B8", 2) == 0) result = RAY_BOOL;
    else if (len == 2 && memcmp(name, "U8", 2) == 0) result = RAY_U8;
    else if (len == 6 && memcmp(name, "SYMBOL", 6) == 0) result = RAY_SYM;
    else if (len == 3 && memcmp(name, "STR", 3) == 0) result = RAY_STR;
    else if (len == 3 && memcmp(name, "F32", 3) == 0) result = RAY_F32;
    else if (len == 4 && memcmp(name, "DATE", 4) == 0) result = RAY_DATE;
    else if (len == 4 && memcmp(name, "TIME", 4) == 0) result = RAY_TIME;
    else if (len == 9 && memcmp(name, "TIMESTAMP", 9) == 0) result = RAY_TIMESTAMP;
    else if (len == 4 && memcmp(name, "GUID", 4) == 0) result = RAY_GUID;
    ray_release(s);
    return result;
}

ray_t* ray_read_csv_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);

    /* (read-csv [types] "path") or (read-csv "path") */
    ray_t* path_obj = NULL;
    ray_t* schema = NULL;
    if (n >= 2 && ray_is_vec(args[0]) && args[0]->type == RAY_SYM) {
        schema = args[0];
        path_obj = args[1];
    } else {
        path_obj = args[0];
    }

    const char* path = NULL;
    if (path_obj->type == -RAY_STR)
        path = ray_str_ptr(path_obj);
    else
        return ray_error("type", NULL);
    if (!path) return ray_error("domain", NULL);

    if (schema) {
        int64_t ncols = schema->len;
        int8_t col_types[256];
        if (ncols > 256) return ray_error("limit", NULL);
        int64_t* sym_ids = (int64_t*)ray_data(schema);
        for (int64_t i = 0; i < ncols; i++) {
            col_types[i] = resolve_type_name(sym_ids[i]);
            if (col_types[i] < 0) return ray_error("type", NULL);
        }
        ray_t* tbl = ray_read_csv_opts(path, 0, true, col_types, (int32_t)ncols);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("io", NULL);
        return tbl;
    }

    ray_t* tbl = ray_read_csv(path);
    if (!tbl || RAY_IS_ERR(tbl)) return ray_error("io", NULL);
    return tbl;
}

/* (write-csv table path) — write table to CSV file */
ray_t* ray_write_csv_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    ray_t* tbl = args[0];
    ray_t* path_obj = args[1];
    if (tbl->type != RAY_TABLE) return ray_error("type", NULL);
    const char* path = NULL;
    if (path_obj->type == -RAY_STR)
        path = ray_str_ptr(path_obj);
    else
        return ray_error("type", NULL);
    if (!path) return ray_error("domain", NULL);
    ray_err_t err = ray_write_csv(tbl, path);
    if (err != RAY_OK) return ray_error(ray_err_code_str(err), NULL);
    return make_i64(0);
}

/* (as 'TypeName value) — type cast */
/* Case-insensitive type name match helper */
static int cast_match(const char* tname, size_t tlen, const char* target) {
    size_t tgt_len = strlen(target);
    if (tlen != tgt_len) return 0;
    for (size_t i = 0; i < tlen; i++) {
        char a = tname[i], b = target[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

ray_t* ray_cast_fn(ray_t* type_sym, ray_t* val) {
    if (type_sym->type != -RAY_SYM) return ray_error("type", NULL);
    ray_t* s = ray_sym_str(type_sym->i64);
    if (!s) return ray_error("domain", NULL);
    const char* tname = ray_str_ptr(s);
    size_t tlen = ray_str_len(s);

    /* Cast to I64 / i64 */
    if (cast_match(tname, tlen, "I64") || cast_match(tname, tlen, "i64")) {
        ray_release(s);
        if (val->type == -RAY_I64) { ray_retain(val); return val; }
        if (val->type == -RAY_F64) return make_i64((int64_t)val->f64);
        if (val->type == -RAY_BOOL) return make_i64(val->b8 ? 1 : 0);
        if (val->type == -RAY_I32 || val->type == -RAY_DATE || val->type == -RAY_TIME)
            return make_i64(val->i32);
        if (val->type == -RAY_TIMESTAMP) return make_i64(val->i64);
        if (val->type == -RAY_I16) return make_i64(val->i16);
        if (val->type == -RAY_U8) return make_i64(val->u8);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            if (!sp) return ray_error("domain", NULL);
            char* end;
            int64_t v = strtoll(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return make_i64(v);
        }
        /* Vector/list cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_I64, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int64_t* out = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i64;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to I32 / i32 */
    if (cast_match(tname, tlen, "I32") || cast_match(tname, tlen, "i32")) {
        ray_release(s);
        if (val->type == -RAY_I32) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_i32(val->b8 ? 1 : 0);
        if (val->type == -RAY_U8)  return ray_i32((int32_t)val->u8);
        if (val->type == -RAY_I16) return ray_i32(val->i16);
        if (val->type == -RAY_I64) return ray_i32((int32_t)val->i64);
        if (val->type == -RAY_F64) return ray_i32((int32_t)val->f64);
        if (val->type == -RAY_DATE || val->type == -RAY_TIME) return ray_i32(val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_i32((int32_t)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val); char* end;
            long v = strtol(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return ray_i32((int32_t)v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_I32, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int32_t* out = (int32_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i32;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to I16 / i16 */
    if (cast_match(tname, tlen, "I16") || cast_match(tname, tlen, "i16")) {
        ray_release(s);
        if (val->type == -RAY_I16) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_i16(val->b8 ? 1 : 0);
        if (val->type == -RAY_U8)  return ray_i16((int16_t)val->u8);
        if (val->type == -RAY_I32) return ray_i16((int16_t)val->i32);
        if (val->type == -RAY_I64) return ray_i16((int16_t)val->i64);
        if (val->type == -RAY_F64) return ray_i16((int16_t)val->f64);
        if (val->type == -RAY_DATE || val->type == -RAY_TIME) return ray_i16((int16_t)val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_i16((int16_t)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val); char* end;
            long v = strtol(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return ray_i16((int16_t)v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_I16, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int16_t* out = (int16_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i16;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to F64 / f64 */
    if (cast_match(tname, tlen, "F64") || cast_match(tname, tlen, "f64")) {
        ray_release(s);
        if (val->type == -RAY_F64) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return make_f64(val->b8 ? 1.0 : 0.0);
        if (val->type == -RAY_I64) return make_f64((double)val->i64);
        if (val->type == -RAY_I32) return make_f64((double)val->i32);
        if (val->type == -RAY_I16) return make_f64((double)val->i16);
        if (val->type == -RAY_U8)  return make_f64((double)val->u8);
        if (val->type == -RAY_DATE || val->type == -RAY_TIME) return make_f64((double)val->i32);
        if (val->type == -RAY_TIMESTAMP) return make_f64((double)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            if (!sp) return ray_error("domain", NULL);
            char* end;
            double v = strtod(sp, &end);
            if (end == sp) return ray_error("domain", NULL);
            return make_f64(v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_F64, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            double* out = (double*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->f64;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to B8/BOOL/b8 */
    if (cast_match(tname, tlen, "BOOL") || cast_match(tname, tlen, "B8") || cast_match(tname, tlen, "b8")) {
        ray_release(s);
        if (val->type == -RAY_BOOL) { ray_retain(val); return val; }
        if (val->type == -RAY_I64) return make_bool(val->i64 != 0 ? 1 : 0);
        if (val->type == -RAY_I32) return make_bool(val->i32 != 0 ? 1 : 0);
        if (val->type == -RAY_I16) return make_bool(val->i16 != 0 ? 1 : 0);
        if (val->type == -RAY_U8) return make_bool(val->u8 != 0 ? 1 : 0);
        if (val->type == -RAY_F64) return make_bool(val->f64 != 0.0 ? 1 : 0);
        if (val->type == -RAY_DATE) return make_bool(val->i32 != 0 ? 1 : 0);
        if (val->type == -RAY_TIME) return make_bool(val->i32 != 0 ? 1 : 0);
        if (val->type == -RAY_TIMESTAMP) return make_bool(val->i64 != 0 ? 1 : 0);
        if (val->type == -RAY_STR) return make_bool(ray_str_len(val) > 0 ? 1 : 0);
        /* Vector cast: b8/B8 */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_BOOL, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            bool* out = (bool*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->b8;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to STR/str */
    if (cast_match(tname, tlen, "STR") || cast_match(tname, tlen, "str")) {
        ray_release(s);
        if (val->type == -RAY_STR) { ray_retain(val); return val; }
        if (val->type == -RAY_SYM) {
            ray_t* sym_str = ray_sym_str(val->i64);
            return sym_str ? sym_str : ray_str("", 0);
        }
        if (val->type == -RAY_I64) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%lld", (long long)val->i64);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_I32) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i32);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_I16) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i16);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_F64) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%g", val->f64);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_BOOL) {
            return val->b8 ? ray_str("true", 4) : ray_str("false", 5);
        }
        /* Fallback: use ray_fmt for any other atom type */
        if (ray_is_atom(val)) {
            ray_t* formatted = ray_fmt(val, 0);
            if (formatted && !RAY_IS_ERR(formatted)) return formatted;
            if (formatted) ray_release(formatted);
        }
        /* Vector/list → STR vector: cast each element to string */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_STR, n2);
            if (!vec || RAY_IS_ERR(vec)) return vec ? vec : ray_error("oom", NULL);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                const char* sp = ray_str_ptr(cast);
                size_t slen = ray_str_len(cast);
                vec = ray_str_vec_append(vec, sp ? sp : "", sp ? slen : 0);
                ray_release(cast);
                if (RAY_IS_ERR(vec)) return vec;
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to SYMBOL/sym */
    if (cast_match(tname, tlen, "SYMBOL") || cast_match(tname, tlen, "sym") || cast_match(tname, tlen, "symbol")) {
        ray_release(s);
        if (val->type == -RAY_SYM) { ray_retain(val); return val; }
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            size_t slen = ray_str_len(val);
            int64_t id = ray_sym_intern(sp, slen);
            return ray_sym(id);
        }
        /* Integer/bool atom → symbol: convert to plain number string */
        if (ray_is_atom(val) && (is_numeric(val) || val->type == -RAY_BOOL)) {
            char buf[64]; int n2;
            if (val->type == -RAY_BOOL)     n2 = snprintf(buf, sizeof(buf), "%d", (int)val->b8);
            else if (val->type == -RAY_U8)  n2 = snprintf(buf, sizeof(buf), "%u", (unsigned)val->u8);
            else if (val->type == -RAY_I16) n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i16);
            else if (val->type == -RAY_I32) n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i32);
            else if (val->type == -RAY_F64) {
                /* Format float: remove trailing zeros but keep at least one decimal */
                n2 = snprintf(buf, sizeof(buf), "%.17g", val->f64);
            }
            else n2 = snprintf(buf, sizeof(buf), "%lld", (long long)as_i64(val));
            if (n2 > 0) {
                int64_t id = ray_sym_intern(buf, (size_t)n2);
                return ray_sym(id);
            }
        }
        /* Temporal/guid atom → symbol: use ray_fmt for formatting */
        if (ray_is_atom(val) && (is_temporal(val) || val->type == -RAY_GUID)) {
            ray_t* formatted = ray_fmt(val, 0);
            if (formatted && !RAY_IS_ERR(formatted)) {
                const char* sp = ray_str_ptr(formatted);
                size_t slen = ray_str_len(formatted);
                int64_t id = ray_sym_intern(sp, slen);
                ray_release(formatted);
                return ray_sym(id);
            }
            if (formatted) ray_release(formatted);
        }
        /* Vector cast: SYMBOL vec from other vecs */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_SYM, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int64_t* out = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i64;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to DATE/date */
    if (cast_match(tname, tlen, "DATE") || cast_match(tname, tlen, "date")) {
        ray_release(s);
        if (val->type == -RAY_DATE) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_date((int64_t)val->b8);
        if (val->type == -RAY_U8)  return ray_date((int64_t)val->u8);
        if (val->type == -RAY_I16) return ray_date((int64_t)val->i16);
        if (val->type == -RAY_I32) return ray_date((int64_t)val->i32);
        if (val->type == -RAY_I64) return ray_date(val->i64);
        if (val->type == -RAY_F64) return ray_date((int64_t)val->f64);
        if (val->type == -RAY_TIME) return ray_date((int64_t)val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_date((int64_t)(val->i64 / 86400000000000LL));
        if (val->type == -RAY_STR) {
            /* Parse "YYYY.MM.DD" format */
            const char* sp = ray_str_ptr(val);
            int y, m, d2;
            if (sscanf(sp, "%d.%d.%d", &y, &m, &d2) != 3) return ray_error("domain", NULL);
            int64_t days = 0;
            { int ty;
              for (ty = 2000; ty < y; ty++) days += (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
              for (ty = y; ty < 2000; ty++) days -= (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
            }
            { static const int md2[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
              int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
              for (int mi = 1; mi < m; mi++) days += md2[mi] + (mi == 2 && leap ? 1 : 0);
              days += d2 - 1;
            }
            return ray_date(days);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_DATE, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int32_t* out = (int32_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i32;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to TIME/time */
    if (cast_match(tname, tlen, "TIME") || cast_match(tname, tlen, "time")) {
        ray_release(s);
        if (val->type == -RAY_TIME) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_time((int64_t)val->b8);
        if (val->type == -RAY_U8)  return ray_time((int64_t)val->u8);
        if (val->type == -RAY_I16) return ray_time((int64_t)val->i16);
        if (val->type == -RAY_I32) return ray_time((int64_t)val->i32);
        if (val->type == -RAY_I64) return ray_time(val->i64);
        if (val->type == -RAY_F64) return ray_time((int64_t)val->f64);
        if (val->type == -RAY_DATE) return ray_time((int64_t)val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_time((int64_t)(val->i64 % 86400000000000LL));
        if (val->type == -RAY_STR) {
            /* Parse "HH:MM:SS[.mmm]" */
            const char* sp = ray_str_ptr(val);
            int th = 0, tm = 0, ts = 0, tms = 0;
            int nr = sscanf(sp, "%d:%d:%d", &th, &tm, &ts);
            if (nr < 2) return ray_error("domain", NULL);
            const char* dot = strchr(sp, '.');
            if (dot) {
                dot++;
                char mbuf[4] = "000";
                int mi = 0;
                while (*dot >= '0' && *dot <= '9' && mi < 3) mbuf[mi++] = *dot++;
                tms = (int)strtol(mbuf, NULL, 10);
            }
            int32_t ms = (int32_t)th * 3600000 + (int32_t)tm * 60000 + (int32_t)ts * 1000 + tms;
            return ray_time((int64_t)ms);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_TIME, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int32_t* out = (int32_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i32;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to TIMESTAMP/timestamp */
    if (cast_match(tname, tlen, "TIMESTAMP") || cast_match(tname, tlen, "timestamp")) {
        ray_release(s);
        if (val->type == -RAY_TIMESTAMP) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_timestamp((int64_t)val->b8);
        if (val->type == -RAY_U8)  return ray_timestamp((int64_t)val->u8);
        if (val->type == -RAY_I16) return ray_timestamp((int64_t)val->i16);
        if (val->type == -RAY_I32) return ray_timestamp((int64_t)val->i32);
        if (val->type == -RAY_I64) return ray_timestamp(val->i64);
        if (val->type == -RAY_F64) return ray_timestamp((int64_t)val->f64);
        if (val->type == -RAY_TIME) return ray_timestamp((int64_t)val->i32);
        if (val->type == -RAY_DATE) {
            int64_t days = val->i32;
            return ray_timestamp(days * 24LL * 60 * 60 * 1000000000LL);
        }
        /* ISO string → timestamp: "YYYY-MM-DD[T ]HH:MM:SS[.nnn...]" or "YYYY.MM.DDDHH:MM:SS.nnn..." */
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            size_t sl = ray_str_len(val);
            if (sl < 10) return ray_error("domain", NULL);
            int y, m, d, hh = 0, mm = 0, ss = 0;
            long long frac = 0;
            /* Try both formats: YYYY-MM-DD and YYYY.MM.DD */
            int parsed = sscanf(sp, "%d-%d-%d", &y, &m, &d);
            /* parse date: try YYYY-MM-DD then YYYY.MM.DD */
            if (parsed != 3) {
                parsed = sscanf(sp, "%d.%d.%d", &y, &m, &d);
                /* YYYY.MM.DD format */
            }
            if (parsed != 3) return ray_error("domain", NULL);
            /* Parse optional time part */
            if (sl > 10 && (sp[10] == 'T' || sp[10] == ' ' || sp[10] == 'D')) {
                sscanf(sp + 11, "%d:%d:%d", &hh, &mm, &ss);
                /* Parse fractional seconds */
                const char* dot = memchr(sp + 11, '.', sl - 11);
                if (dot) {
                    dot++;
                    char fbuf[10] = "000000000";
                    int fi = 0;
                    while (*dot >= '0' && *dot <= '9' && fi < 9) fbuf[fi++] = *dot++;
                    frac = strtoll(fbuf, NULL, 10);
                }
            }
            /* Convert to days since 2000-01-01 */
            int64_t days = 0;
            { int ty;
              for (ty = 2000; ty < y; ty++) days += (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
              for (ty = y; ty < 2000; ty++) days -= (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
            }
            { static const int md[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
              int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
              for (int mi = 1; mi < m; mi++) days += md[mi] + (mi == 2 && leap ? 1 : 0);
              days += d - 1;
            }
            int64_t ns = days * 86400000000000LL + (int64_t)hh * 3600000000000LL +
                         (int64_t)mm * 60000000000LL + (int64_t)ss * 1000000000LL + frac;
            /* Handle timezone offset: Z, +HH:MM, -HH:MM, +HHMM, -HHMM */
            if (sl > 19) {
                const char* tz = sp + 19; /* after YYYY-MM-DDTHH:MM:SS */
                /* Skip fractional seconds */
                if (tz < sp + sl && *tz == '.') {
                    tz++;
                    while (tz < sp + sl && *tz >= '0' && *tz <= '9') tz++;
                }
                if (tz < sp + sl) {
                    if (*tz == 'Z') {
                        /* UTC, no adjustment */
                    } else if (*tz == '+' || *tz == '-') {
                        int tz_sign = (*tz == '+') ? 1 : -1;
                        int tz_hh = 0, tz_mm = 0;
                        tz++;
                        /* Parse HH:MM or HHMM */
                        if (tz + 4 < sp + sl && tz[2] == ':') {
                            sscanf(tz, "%2d:%2d", &tz_hh, &tz_mm);
                        } else {
                            sscanf(tz, "%2d%2d", &tz_hh, &tz_mm);
                        }
                        int64_t tz_ns = ((int64_t)tz_hh * 3600 + (int64_t)tz_mm * 60) * 1000000000LL;
                        ns -= tz_sign * tz_ns;
                    }
                }
            }
            return ray_timestamp(ns);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_TIMESTAMP, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            int64_t* out = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->i64;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to GUID/guid */
    if (cast_match(tname, tlen, "GUID") || cast_match(tname, tlen, "guid")) {
        ray_release(s);
        if (val->type == -RAY_GUID) { ray_retain(val); return val; }
        if (val->type == -RAY_STR) {
            /* Parse UUID string: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
            const char* sp = ray_str_ptr(val);
            size_t sl = ray_str_len(val);
            if (sl < 36) return ray_error("domain", NULL);
            uint8_t bytes[16];
            const char* p = sp;
            for (int bi = 0; bi < 16; bi++) {
                if (*p == '-') p++;
                char hi = *p++;
                char lo = *p++;
                int h = (hi >= 'a') ? hi - 'a' + 10 : (hi >= 'A') ? hi - 'A' + 10 : hi - '0';
                int l = (lo >= 'a') ? lo - 'a' + 10 : (lo >= 'A') ? lo - 'A' + 10 : lo - '0';
                bytes[bi] = (uint8_t)((h << 4) | l);
            }
            return ray_guid(bytes);
        }
        /* Vector of GUIDs: empty vector cast */
        if (ray_is_vec(val) && val->len == 0)
            return ray_vec_new(RAY_GUID, 0);
        /* List of strings → GUID vector */
        if (val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_GUID, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            uint8_t* data = (uint8_t*)ray_data(vec);
            ray_t** items = (ray_t**)ray_data(val);
            for (int64_t i = 0; i < n2; i++) {
                ray_t* cast = ray_cast_fn(type_sym, items[i]);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                if (cast->obj) memcpy(data + i * 16, ray_data(cast->obj), 16);
                else memcpy(data + i * 16, ray_data(cast), 16);
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to U8/u8 */
    if (cast_match(tname, tlen, "U8") || cast_match(tname, tlen, "u8")) {
        ray_release(s);
        if (val->type == -RAY_U8) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_u8(val->b8 ? 1 : 0);
        if (val->type == -RAY_I16) return ray_u8((uint8_t)val->i16);
        if (val->type == -RAY_I32) return ray_u8((uint8_t)val->i32);
        if (val->type == -RAY_I64) return ray_u8((uint8_t)val->i64);
        if (val->type == -RAY_F64) return ray_u8((uint8_t)val->f64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            char* end; long v = strtol(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return ray_u8((uint8_t)v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_U8, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            uint8_t* out = (uint8_t*)ray_data(vec);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                out[i] = cast->u8;
                ray_release(cast);
            }
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to DICT */
    if (cast_match(tname, tlen, "DICT")) {
        ray_release(s);
        if (val->type == RAY_LIST && (val->attrs & RAY_ATTR_DICT)) { ray_retain(val); return val; }
        /* Table → Dict */
        if (val->type == RAY_TABLE) {
            int64_t ncols = ray_table_ncols(val);
            ray_t* dict = ray_list_new(ncols * 2);
            if (RAY_IS_ERR(dict)) return dict;
            dict->attrs |= RAY_ATTR_DICT;
            for (int64_t c = 0; c < ncols; c++) {
                int64_t col_name = ray_table_col_name(val, c);
                ray_t* col_val = ray_table_get_col_idx(val, c);
                ray_t* key = ray_sym(col_name);
                if (RAY_IS_ERR(key)) { ray_release(dict); return key; }
                dict = ray_list_append(dict, key);
                ray_release(key);
                if (RAY_IS_ERR(dict)) return dict;
                ray_retain(col_val);
                dict = ray_list_append(dict, col_val);
                ray_release(col_val);
                if (RAY_IS_ERR(dict)) return dict;
            }
            return dict;
        }
        return ray_error("type", NULL);
    }
    /* Cast to TABLE */
    if (cast_match(tname, tlen, "TABLE")) {
        ray_release(s);
        if (val->type == RAY_TABLE) { ray_retain(val); return val; }
        /* Dict → Table */
        if (val->type == RAY_LIST && (val->attrs & RAY_ATTR_DICT)) {
            int64_t dict_n = ray_len(val);
            int32_t ncols = (int32_t)(dict_n / 2);
            ray_t** items = (ray_t**)ray_data(val);
            ray_t* tbl = ray_table_new(ncols);
            if (RAY_IS_ERR(tbl)) return tbl;
            for (int32_t c = 0; c < ncols; c++) {
                int64_t col_name = items[c * 2]->i64;
                ray_t* col_val = items[c * 2 + 1];
                ray_retain(col_val);
                tbl = ray_table_add_col(tbl, col_name, col_val);
                ray_release(col_val);
                if (RAY_IS_ERR(tbl)) return tbl;
            }
            return tbl;
        }
        return ray_error("type", NULL);
    }
    ray_release(s);
    return ray_error("domain", NULL);
}

/* (type val) — return the type code of a value */
/* Type name mapping: atoms use negative types, vectors use positive */
static const char* type_sym_name(int8_t type) {
    switch (type < 0 ? -type : type) {
    case RAY_BOOL:      return type < 0 ? "b8" : "B8";
    case RAY_U8:        return type < 0 ? "u8" : "U8";
    case RAY_I16:       return type < 0 ? "i16" : "I16";
    case RAY_I32:       return type < 0 ? "i32" : "I32";
    case RAY_I64:       return type < 0 ? "i64" : "I64";
    case RAY_F32:       return type < 0 ? "f32" : "F32";
    case RAY_F64:       return type < 0 ? "f64" : "F64";
    case RAY_DATE:      return type < 0 ? "date" : "DATE";
    case RAY_TIME:      return type < 0 ? "time" : "TIME";
    case RAY_TIMESTAMP: return type < 0 ? "timestamp" : "TIMESTAMP";
    case RAY_SYM:       return type < 0 ? "symbol" : "SYMBOL";
    case RAY_STR:       return type < 0 ? "str" : "STR";
    case RAY_GUID:      return type < 0 ? "guid" : "GUID";
    case RAY_TABLE:     return "TABLE";
    case RAY_DICT:      return "DICT";
    case RAY_LIST:      return "LIST";
    default:            return "?";
    }
}

ray_t* ray_type_fn(ray_t* val) {
    if (!val) return ray_sym(ray_sym_intern("null", 4));
    /* Dict is a LIST with ATTR_DICT flag */
    if (val->type == RAY_LIST && (val->attrs & RAY_ATTR_DICT)) {
        int64_t id = ray_sym_intern("DICT", 4);
        return ray_sym(id);
    }
    const char* name = type_sym_name(val->type);
    int64_t id = ray_sym_intern(name, strlen(name));
    return ray_sym(id);
}

/* (read path) — read a file's contents as a string */
ray_t* ray_read_file(ray_t* path_obj) {
    if (path_obj->type != -RAY_STR) return ray_error("type", NULL);
    const char* path = ray_str_ptr(path_obj);
    if (!path) return ray_error("domain", NULL);
    FILE* fp = fopen(path, "rb");
    if (!fp) return ray_error("io", NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return ray_error("io", NULL); }
    /* Use ray_alloc for the buffer */
    ray_t* buf = ray_alloc((size_t)sz + 1);
    if (!buf || RAY_IS_ERR(buf)) { fclose(fp); return ray_error("oom", NULL); }
    char* data = (char*)ray_data(buf);
    size_t rd = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    data[rd] = '\0';
    ray_t* result = ray_str(data, rd);
    ray_release(buf);
    return result;
}

/* (load path) — read and evaluate a Rayfall script file via mmap */
ray_t* ray_load_file(ray_t* path_obj) {
    if (path_obj->type != -RAY_STR) return ray_error("type", NULL);
    const char* path = ray_str_ptr(path_obj);
    if (!path) return ray_error("domain", NULL);
    size_t path_len = ray_str_len(path_obj);

#if defined(_WIN32)
    /* Windows: fall back to fread */
    FILE* fp = fopen(path, "r");
    if (!fp) return ray_error("io", NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return ray_error("io", NULL); }
    if (sz == 0) { fclose(fp); return ray_i64(0); }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return ray_error("oom", NULL); }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    ray_t* nfo = ray_nfo_create(path, path_len, buf, rd);
    ray_t* parsed = ray_parse_with_nfo(buf, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); free(buf); return parsed; }

    ray_t* prev_nfo = g_eval_nfo;
    g_eval_nfo = nfo;
    ray_t* result = ray_eval(parsed);
    g_eval_nfo = prev_nfo;

    ray_release(parsed);
    ray_release(nfo);
    free(buf);
    return result;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ray_error("io", NULL);
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0) { close(fd); return ray_error("io", NULL); }
    size_t sz = (size_t)st.st_size;
    if (sz == 0) { close(fd); return ray_i64(0); }
    char* map = (char*)mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return ray_error("io", NULL);
    /* Copy to NUL-terminated buffer — mmap region may not have a trailing NUL */
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { munmap(map, sz); return ray_error("oom", NULL); }
    memcpy(buf, map, sz);
    buf[sz] = '\0';
    munmap(map, sz);

    ray_t* nfo = ray_nfo_create(path, path_len, buf, sz);
    ray_t* parsed = ray_parse_with_nfo(buf, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); free(buf); return parsed; }

    ray_t* prev_nfo = g_eval_nfo;
    g_eval_nfo = nfo;
    ray_t* result = ray_eval(parsed);
    g_eval_nfo = prev_nfo;

    ray_release(parsed);
    ray_release(nfo);
    free(buf);
    return result;
#endif
}

/* (write path content) — write string to a file */
ray_t* ray_write_file(ray_t* path_obj, ray_t* content) {
    if (path_obj->type != -RAY_STR) return ray_error("type", NULL);
    if (content->type != -RAY_STR) return ray_error("type", NULL);
    const char* path = ray_str_ptr(path_obj);
    const char* data = ray_str_ptr(content);
    size_t len = ray_str_len(content);
    if (!path || !data) return ray_error("domain", NULL);
    FILE* fp = fopen(path, "wb");
    if (!fp) return ray_error("io", NULL);
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len) return ray_error("io", NULL);
    return make_i64(0);
}

/* ══════════════════════════════════════════
 * Special forms: set, let, if, do
 * ══════════════════════════════════════════ */

/* (set name value) — bind in global env. Receives unevaluated args. */
ray_t* ray_set(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return ray_error("type", NULL);
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    if (ray_env_set(name_obj->i64, val) != RAY_OK) {
        ray_release(val);
        return ray_error("oom", NULL);
    }
    return val;  /* set returns the value */
}

/* (let name value) — bind in local scope. Receives unevaluated args. */
ray_t* ray_let(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return ray_error("type", NULL);
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    ray_err_t err = ray_env_set_local(name_obj->i64, val);
    if (err != RAY_OK) { ray_release(val); return ray_error(ray_err_code_str(err), NULL); }
    return val;
}

/* (if cond then else?) — conditional. Receives unevaluated args. */
ray_t* ray_cond(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    ray_t* cond = ray_eval(args[0]);
    if (RAY_IS_ERR(cond)) return cond;
    /* Materialize lazy handles before testing truthiness */
    if (ray_is_lazy(cond))
        cond = ray_lazy_materialize(cond);
    if (RAY_IS_ERR(cond)) return cond;
    int truthy = 0;
    if (cond->type == -RAY_BOOL) truthy = cond->b8;
    else if (cond->type == -RAY_I64) truthy = cond->i64 != 0;
    else truthy = 1;  /* non-null is truthy */
    ray_release(cond);
    if (truthy) return ray_eval(args[1]);
    if (n >= 3) return ray_eval(args[2]);
    /* No else branch: return 0 */
    return make_i64(0);
}

/* (do expr1 expr2 ...) — evaluate in sequence, return last. Pushes local scope. */
ray_t* ray_do(ray_t** args, int64_t n) {
    if (n == 0) return make_i64(0);
    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);
    ray_t* result = NULL;
    for (int64_t i = 0; i < n; i++) {
        if (result) ray_release(result);
        result = ray_eval(args[i]);
        if (RAY_IS_ERR(result)) {
            ray_env_pop_scope();
            return result;
        }
    }
    ray_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Lambda functions
 * ══════════════════════════════════════════ */

/* (fn [params...] body...) — create a lambda object.
 * Stores params list and body expressions in data area. */
ray_t* ray_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    /* args[0] = param vector (list of name symbols), args[1..n-1] = body exprs */
    ray_t* params_list = args[0];

    /* Create lambda object with space for 7 slots:
     * [0] params, [1] body, [2] bytecode, [3] constants, [4] n_locals,
     * [5] nfo (source location), [6] dbg (debug metadata) */
    ray_t* lambda = ray_alloc(7 * sizeof(ray_t*));
    if (!lambda) return ray_error("oom", NULL);
    lambda->type = RAY_LAMBDA;
    lambda->attrs = 0;
    lambda->len = 0;

    /* Store params list */
    ray_retain(params_list);
    LAMBDA_PARAMS(lambda) = params_list;

    /* Build body list: wrap body expressions in a RAY_LIST */
    int64_t body_count = n - 1;
    ray_t* body = ray_alloc(body_count * sizeof(ray_t*));
    if (!body) {
        ray_release(params_list);
        ray_release(lambda);
        return ray_error("oom", NULL);
    }
    body->type = RAY_LIST;
    body->len = body_count;
    ray_t** body_elems = (ray_t**)ray_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        ray_retain(args[i + 1]);
        body_elems[i] = args[i + 1];
    }
    LAMBDA_BODY(lambda) = body;

    /* Clear compiled slots */
    LAMBDA_BC(lambda) = NULL;
    LAMBDA_CONSTS(lambda) = NULL;
    LAMBDA_NLOCALS(lambda) = 0;

    /* Attach source location info from current eval context */
    if (g_eval_nfo) {
        LAMBDA_NFO(lambda) = g_eval_nfo;
        ray_retain(g_eval_nfo);
    } else {
        LAMBDA_NFO(lambda) = NULL;
    }
    LAMBDA_DBG(lambda) = NULL;

    return lambda;
}

/* Build a single error trace frame from a lambda's debug/nfo info at the given
 * bytecode IP.  Appends [span_i64, filename, fn_name, source] to g_error_trace. */
static void add_error_frame(ray_t* fn, int32_t ip) {
    if (!fn || fn->type != RAY_LAMBDA) return;
    ray_t* dbg = LAMBDA_DBG(fn);
    ray_t* nfo = LAMBDA_NFO(fn);
    if (!dbg && !nfo) return;

    ray_span_t span = {0};
    if (dbg) span = ray_bc_dbg_get(dbg, ip);
    if (span.id == 0) return;

    /* Build frame: alloc 4-slot list, set elements directly */
    ray_t* frame = ray_alloc(4 * sizeof(ray_t*));
    if (!frame || RAY_IS_ERR(frame)) return;
    frame->type = RAY_LIST;
    frame->len = 4;
    ray_t** fe = (ray_t**)ray_data(frame);

    /* [0] span as i64 atom */
    fe[0] = ray_i64(span.id);

    /* [1] filename from nfo */
    if (nfo && NFO_FILENAME(nfo)) {
        fe[1] = NFO_FILENAME(nfo);
        ray_retain(fe[1]);
    } else {
        fe[1] = ray_str("<unknown>", 9);
    }

    /* [2] function name — NULL for anonymous lambdas */
    fe[2] = NULL;

    /* [3] source from nfo */
    if (nfo && NFO_SOURCE(nfo)) {
        fe[3] = NFO_SOURCE(nfo);
        ray_retain(fe[3]);
    } else {
        fe[3] = ray_str("", 0);
    }

    /* Append frame to trace list */
    if (!g_error_trace) {
        g_error_trace = ray_alloc(sizeof(ray_t*));
        if (!g_error_trace) { ray_release(frame); return; }
        g_error_trace->type = RAY_LIST;
        g_error_trace->len = 1;
        ((ray_t**)ray_data(g_error_trace))[0] = frame;
    } else {
        g_error_trace = ray_list_append(g_error_trace, frame);
        ray_release(frame);
    }
}

/* Add error frame from eval context (nfo + AST node) for call-site errors. */
static void add_eval_error_frame(ray_t* nfo, ray_t* node) {
    if (!nfo || !node) return;
    ray_span_t span = ray_nfo_get(nfo, node);
    if (span.id == 0) return;

    ray_t* frame = ray_alloc(4 * sizeof(ray_t*));
    if (!frame || RAY_IS_ERR(frame)) return;
    frame->type = RAY_LIST;
    frame->len = 4;
    ray_t** fe = (ray_t**)ray_data(frame);

    fe[0] = ray_i64(span.id);
    fe[1] = NFO_FILENAME(nfo) ? (ray_retain(NFO_FILENAME(nfo)), NFO_FILENAME(nfo))
                              : ray_str("<unknown>", 9);
    fe[2] = NULL;
    fe[3] = NFO_SOURCE(nfo) ? (ray_retain(NFO_SOURCE(nfo)), NFO_SOURCE(nfo))
                            : ray_str("", 0);

    if (!g_error_trace) {
        g_error_trace = ray_alloc(sizeof(ray_t*));
        if (!g_error_trace) { ray_release(frame); return; }
        g_error_trace->type = RAY_LIST;
        g_error_trace->len = 1;
        ((ray_t**)ray_data(g_error_trace))[0] = frame;
    } else {
        g_error_trace = ray_list_append(g_error_trace, frame);
        ray_release(frame);
    }
}

/* Execute compiled bytecode for a lambda. */
static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc);

/* Call a lambda: compile on first call, then execute bytecode. */
static ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc) {
    /* Lazy compilation on first call */
    if (!LAMBDA_IS_COMPILED(lambda)) {
        ray_compile(lambda);
    }

    /* If compilation succeeded, run bytecode; otherwise fall back to tree-walk */
    if (LAMBDA_IS_COMPILED(lambda)) {
        return vm_exec(lambda, call_args, argc);
    }

    /* Fallback: tree-walking interpreter */
    ray_t* params_list = LAMBDA_PARAMS(lambda);
    ray_t* body = LAMBDA_BODY(lambda);

    int64_t param_count = ray_len(params_list);

    if (argc != param_count)
        return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);

    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);

    /* Bind 'self' to the current lambda for recursion */
    {
        static int64_t self_sym_id = -1;
        if (self_sym_id < 0) self_sym_id = ray_sym_intern("self", 4);
        ray_env_set_local(self_sym_id, lambda);
    }

    int64_t* param_ids = (int64_t*)ray_data(params_list);
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        (void)ray_env_set_local(param_ids[i], call_args[i]);
    }

    int64_t body_count = ray_len(body);
    ray_t** body_exprs = (ray_t**)ray_data(body);
    ray_t* result = NULL;
    for (int64_t i = 0; i < body_count; i++) {
        if (result) ray_release(result);
        result = ray_eval(body_exprs[i]);
        if (RAY_IS_ERR(result)) {
            ray_env_pop_scope();
            return result;
        }
    }

    ray_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Stack-based VM executor (computed goto, frame-based)
 * ══════════════════════════════════════════ */

static _Thread_local ray_vm_t *__VM = NULL;

static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc) {
    /* Computed goto dispatch table */
    static void *dispatch[OP__COUNT] = {
        [OP_RET]        = &&op_ret,
        [OP_JMP]        = &&op_jmp,
        [OP_JMPF]       = &&op_jmpf,
        [OP_LOADCONST]  = &&op_loadconst,
        [OP_LOADENV]    = &&op_loadenv,
        [OP_STOREENV]   = &&op_storeenv,
        [OP_POP]        = &&op_pop,
        [OP_RESOLVE]    = &&op_resolve,
        [OP_CALL1]      = &&op_call1,
        [OP_CALL2]      = &&op_call2,
        [OP_CALLN]      = &&op_calln,
        [OP_CALLF]      = &&op_callf,
        [OP_CALLS]      = &&op_calls,
        [OP_CALLD]      = &&op_calld,
        [OP_DUP]        = &&op_dup,
        [OP_LOADCONST_W] = &&op_loadconst_w,
        [OP_RESOLVE_W]  = &&op_resolve_w,
        [OP_TRAP]       = &&op_trap,
        [OP_TRAP_END]   = &&op_trap_end,
    };

    /* Arity check before allocating VM state */
    {
        int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
        if (argc != param_count)
            return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);
    }

    ray_t *vm_block = ray_alloc(sizeof(ray_vm_t));
    if (!vm_block || RAY_IS_ERR(vm_block)) return ray_error("oom", NULL);
    ray_vm_t *vmp = (ray_vm_t *)ray_data(vm_block);
    memset(vmp, 0, sizeof(ray_vm_t));
    __VM = vmp;

#define vm (*vmp)

    /* Set up initial frame */
    vm.fn = lambda;
    ray_retain(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    vm.fp = 0;
    vm.sp = n_locals;

    /* Bind parameters into local slots */
    int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        ray_retain(call_args[i]);
        vm.ps[i] = call_args[i];
    }

    uint8_t *code = (uint8_t *)ray_data(LAMBDA_BC(lambda));
    ray_t **cpool = (ray_t **)ray_data(LAMBDA_CONSTS(lambda));
    int32_t ip = 0;
    ray_t *vm_err_obj = NULL;

#define DISPATCH() goto *dispatch[code[ip++]]
#define PUSH(v)    (vm.ps[vm.sp++] = (v))
#define POP()      (vm.ps[--vm.sp])
#define PEEK()     (vm.ps[vm.sp - 1])
#define LOCAL(s)   (vm.ps[vm.fp + (s)])

    DISPATCH();

op_loadconst: {
    uint8_t idx = code[ip++];
    ray_t *val = cpool[idx];
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadconst_w: {
    uint16_t idx = (uint16_t)(code[ip] << 8) | code[ip + 1];
    ip += 2;
    ray_t *val = cpool[idx];
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadenv: {
    uint8_t slot = code[ip++];
    ray_t *val = LOCAL(slot);
    if (val) ray_retain(val);
    else val = make_i64(0);
    PUSH(val);
    DISPATCH();
}

op_storeenv: {
    uint8_t slot = code[ip++];
    ray_t *val = POP();
    if (LOCAL(slot)) ray_release(LOCAL(slot));
    LOCAL(slot) = val;
    DISPATCH();
}

op_pop: {
    if (vm.sp > vm.fp + n_locals) {
        ray_t *val = POP();
        if (val) ray_release(val);
    }
    DISPATCH();
}

op_dup: {
    ray_t *val = PEEK();
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_resolve: {
    uint8_t idx = code[ip++];
    ray_t *name_obj = cpool[idx];
    ray_t *val = ray_env_get(name_obj->i64);
    if (!val) goto vm_error_name;
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_resolve_w: {
    uint16_t idx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *name_obj = cpool[idx];
    ray_t *val = ray_env_get(name_obj->i64);
    if (!val) goto vm_error_name;
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_jmp: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ip += offset;
    if (offset < 0 && g_eval_interrupted) goto vm_error_limit;
    DISPATCH();
}

op_jmpf: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *cond = POP();
    int truthy = 0;
    if (cond->type == -RAY_BOOL) truthy = cond->b8;
    else if (cond->type == -RAY_I64) truthy = cond->i64 != 0;
    else truthy = 1;
    ray_release(cond);
    if (!truthy) ip += offset;
    DISPATCH();
}

op_call1: {
    ray_t *arg = POP();
    ray_t *fn_obj = POP();
    ray_unary_fn fn = (ray_unary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result;
    if ((fn_obj->attrs & RAY_FN_ATOMIC) && arg->type >= 0)
        result = atomic_map_unary(fn, arg);
    else
        result = fn(arg);
    ray_release(arg);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_call2: {
    ray_t *right = POP();
    ray_t *left = POP();
    ray_t *fn_obj = POP();
    ray_binary_fn fn = (ray_binary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result;
    /* Fast path: atoms have negative type — skip collection check entirely.
     * Only call is_collection when at least one arg has type >= 0 (vector/list). */
    if ((fn_obj->attrs & RAY_FN_ATOMIC) && (left->type >= 0 || right->type >= 0))
        result = atomic_map_binary_op(fn, RAY_FN_OPCODE(fn_obj), left, right);
    else
        result = fn(left, right);
    ray_release(left);
    ray_release(right);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_calln: {
    uint8_t n = code[ip++];
    if (n > 64) goto vm_error;
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();
    ray_vary_fn fn = (ray_vary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result = fn(fn_args, n);
    for (int32_t i = 0; i < n; i++)
        ray_release(fn_args[i]);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_callf: {
    uint8_t n = code[ip++];
    if (n > 64) goto vm_error;
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();

    /* Compiled lambda: push frame, switch to callee bytecode */
    if (fn_obj->type == RAY_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            ray_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* Push return frame */
            if (vm.rp >= VM_STACK_SIZE) goto vm_error_limit;
            vm.rs[vm.rp++] = (vm_ctx_t){ .fn = vm.fn, .fp = vm.fp, .ip = ip };

            /* Set up new frame */
            vm.fn = fn_obj;  /* takes ownership of stack ref */
            vm.fp = vm.sp;
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            if (vm.sp + callee_locals >= VM_STACK_SIZE) goto vm_error_limit;
            vm.sp += callee_locals;
            n_locals = callee_locals;

            /* Bind parameters */
            int64_t pcnt = ray_len(LAMBDA_PARAMS(fn_obj));
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];  /* transfer ownership from args */
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                ray_release(fn_args[i]);  /* excess args */

            /* Check for Ctrl-C interrupt on each compiled call */
            if (g_eval_interrupted) goto vm_error_limit;

            /* Switch to callee bytecode */
            code = (uint8_t *)ray_data(LAMBDA_BC(fn_obj));
            cpool = (ray_t **)ray_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Non-lambda or uncompiled: dispatch by type */
    {
        ray_t *result;
        switch (fn_obj->type) {
        case RAY_UNARY:
            result = ((ray_unary_fn)(uintptr_t)fn_obj->i64)(fn_args[0]);
            ray_release(fn_args[0]);
            for (int32_t i = 1; i < n; i++) ray_release(fn_args[i]);
            break;
        case RAY_BINARY:
            result = ((ray_binary_fn)(uintptr_t)fn_obj->i64)(fn_args[0], fn_args[1]);
            ray_release(fn_args[0]);
            ray_release(fn_args[1]);
            for (int32_t i = 2; i < n; i++) ray_release(fn_args[i]);
            break;
        case RAY_VARY:
            result = ((ray_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        case RAY_LAMBDA:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        default:
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            result = ray_error("type", NULL);
            break;
        }
        ray_release(fn_obj);
        if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
        PUSH(result);
        DISPATCH();
    }
}

op_calls: {
    /* Self-recursive call — lean path matching rayforce 1.
     * No fn object on stack. Args are already at sp-argc..sp.
     * Push return frame, set fp so args become locals, extend for extra locals. */
    uint8_t argc = code[ip++];

    /* Stack overflow guard */
    if (RAY_UNLIKELY(vm.rp >= VM_STACK_SIZE)) goto vm_error_limit;
    if (RAY_UNLIKELY(vm.sp + n_locals >= VM_STACK_SIZE)) goto vm_error_limit;

    /* Push return frame (fn=NULL signals self-call to OP_RET) */
    vm.rs[vm.rp++] = (vm_ctx_t){ .fn = NULL, .fp = vm.fp, .ip = ip };

    /* Args on stack become the new frame's first locals.
     * Compiler guarantees argc == param count, so argc <= n_locals. */
    vm.fp = vm.sp - argc;

    /* Extend stack for extra locals beyond params (let bindings etc.) */
    for (int32_t i = argc; i < n_locals; i++)
        vm.ps[vm.sp++] = NULL;

    ip = 0;
    DISPATCH();
}

op_calld: {
    /* Dynamic dispatch: evaluate AST directly via ray_eval */
    uint8_t n = code[ip++];
    if (n == 0) {
        /* n=0: the AST itself is on the stack, eval it directly */
        ray_t *ast = POP();
        ray_t *result = ray_eval(ast);
        ray_release(ast);
        if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
        PUSH(result);
        DISPATCH();
    }
    /* n>0: build call list and eval */
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();

    ray_t *call_list = ray_alloc((n + 1) * sizeof(ray_t *));
    if (!call_list || RAY_IS_ERR(call_list)) {
        for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
        ray_release(fn_obj);
        goto vm_error;
    }
    call_list->type = RAY_LIST;
    call_list->len = n + 1;
    ray_t **elems = (ray_t **)ray_data(call_list);
    elems[0] = fn_obj;
    for (int32_t i = 0; i < n; i++)
        elems[i + 1] = fn_args[i];

    ray_t *result = ray_eval(call_list);
    ray_release(call_list);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_ret: {
    ray_t *result;
    bool from_stack = (vm.sp > vm.fp + n_locals);
    if (from_stack) {
        result = POP();
        ray_retain(result);  /* prevent free during cleanup if aliased in locals */
    } else {
        result = make_i64(0);
    }

    /* Clean up current frame — release all locals and leftover stack slots */
    while (vm.sp > vm.fp) {
        ray_t *v = vm.ps[--vm.sp];
        if (v) ray_release(v);
    }

    /* Undo protective retain — POP's reference is the caller's ownership */
    if (from_stack) ray_release(result);

    if (vm.rp == 0) {
        /* Top-level return */
        ray_release(vm.fn);
        __VM = NULL;
#undef vm
        ray_free(vm_block);
        return result;  /* caller owns the POP'd reference */
#define vm (*vmp)
    }

    /* Pop return frame */
    vm.rp--;
    vm.fp = vm.rs[vm.rp].fp;
    ip = vm.rs[vm.rp].ip;
    if (vm.rs[vm.rp].fn) {
        /* Normal call: restore caller's function */
        ray_release(vm.fn);
        vm.fn = vm.rs[vm.rp].fn;
        code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
        cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
        n_locals = LAMBDA_NLOCALS(vm.fn);
    }
    /* Self-call (fn==NULL): vm.fn/code/cpool/n_locals are already correct */
    PUSH(result);
    DISPATCH();
}

op_trap: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    if (vm.tp >= VM_TRAP_SIZE) goto vm_error_limit;
    vm.ts[vm.tp++] = (vm_trap_t){
        .rp = vm.rp, .sp = vm.sp, .handler_ip = ip + offset,
        .fn = vm.fn, .fp = vm.fp, .n_locals = n_locals
    };
    ray_retain(vm.fn);
    DISPATCH();
}

op_trap_end: {
    if (vm.tp > 0) {
        vm.tp--;
        ray_release(vm.ts[vm.tp].fn);
    }
    DISPATCH();
}

    const char *vm_err_str = "domain";
    const char *vm_err_detail = NULL;
    goto vm_error_cleanup;

vm_error_limit:
    vm_err_str = "limit";
    vm_err_detail = "stack overflow";
    goto vm_error_cleanup;

vm_error_name:
    vm_err_str = "name";
    vm_err_detail = NULL;
    goto vm_error_cleanup;

vm_error:
    vm_err_str = "domain";
    vm_err_detail = NULL;

vm_error_cleanup: {
    /* Check for trap frame */
    if (vm.tp > 0) {
        vm.tp--;
        vm_trap_t trap = vm.ts[vm.tp];

        /* Clean up return frames above trap point */
        while (vm.rp > trap.rp) {
            vm.rp--;
            if (vm.rs[vm.rp].fn) ray_release(vm.rs[vm.rp].fn);
        }

        /* Clean up stack above trap point */
        while (vm.sp > trap.sp) {
            ray_t *v = vm.ps[--vm.sp];
            if (v) ray_release(v);
        }

        /* Get error value */
        ray_t *err_val = __raise_val;
        __raise_val = NULL;
        if (!err_val) err_val = make_i64(0);

        /* Restore context and push error value */
        ray_release(vm.fn);
        vm.fn = trap.fn;  /* takes ownership from trap frame */
        vm.fp = trap.fp;
        n_locals = trap.n_locals;
        code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
        cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
        ip = trap.handler_ip;
        PUSH(err_val);
        DISPATCH();
    }

    /* No trap frame — regular error cleanup */

    /* Build error trace: current frame + callers from return stack */
    add_error_frame(vm.fn, ip > 0 ? ip - 1 : 0);
    for (int32_t i = vm.rp - 1; i >= 0; i--) {
        if (vm.rs[i].fn)
            add_error_frame(vm.rs[i].fn, vm.rs[i].ip > 0 ? vm.rs[i].ip - 1 : 0);
    }

    for (int32_t i = 0; i < vm.sp; i++)
        if (vm.ps[i]) ray_release(vm.ps[i]);
    ray_release(vm.fn);
    for (int32_t i = 0; i < vm.rp; i++)
        if (vm.rs[i].fn) ray_release(vm.rs[i].fn);
    for (int32_t i = 0; i < vm.tp; i++)
        ray_release(vm.ts[i].fn);
    __VM = NULL;
#undef vm
    ray_free(vm_block);
    if (vm_err_obj)
        return vm_err_obj;
    if (vm_err_detail)
        return ray_error(vm_err_str, "%s", vm_err_detail);
    return ray_error(vm_err_str, NULL);
}

#undef DISPATCH
#undef PUSH
#undef POP
#undef PEEK
#undef LOCAL
#undef vm
}

/* ══════════════════════════════════════════
 * Additional builtins (ported from rayforce)
 * ══════════════════════════════════════════ */

/* (enlist a b c ...) → typed vector from atoms */
static ray_t* ray_enlist(ray_t** args, int64_t n) {
    if (n == 0) return ray_vec_new(RAY_I64, 0);
    /* Determine type from first arg */
    int8_t atype = args[0]->type;
    bool homogeneous = true;
    bool has_float = (atype == -RAY_F64);
    bool has_int = (atype == -RAY_I64);
    for (int64_t i = 1; i < n; i++) {
        if (args[i]->type != atype) homogeneous = false;
        if (args[i]->type == -RAY_F64) has_float = true;
        if (args[i]->type == -RAY_I64) has_int = true;
    }
    /* Mixed int/float → promote to f64 */
    if (!homogeneous && has_float && has_int) {
        ray_t* vec = ray_vec_new(RAY_F64, n);
        if (RAY_IS_ERR(vec)) return vec;
        double* d = (double*)ray_data(vec);
        for (int64_t i = 0; i < n; i++)
            d[i] = (args[i]->type == -RAY_F64) ? args[i]->f64 : (double)args[i]->i64;
        vec->len = n;
        return vec;
    }
    if (homogeneous && atype < 0) {
        int8_t vtype = -atype;
        ray_t* vec = ray_vec_new(vtype, n);
        if (RAY_IS_ERR(vec)) return vec;
        switch (vtype) {
        case RAY_I64: case RAY_TIMESTAMP: {
            int64_t* d = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i64;
            break;
        }
        case RAY_F64: {
            double* d = (double*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->f64;
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            int32_t* d = (int32_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i32;
            break;
        }
        case RAY_I16: {
            int16_t* d = (int16_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i16;
            break;
        }
        case RAY_BOOL: {
            bool* d = (bool*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->b8;
            break;
        }
        case RAY_SYM: {
            int64_t* d = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i64;
            break;
        }
        case RAY_U8: {
            uint8_t* d = (uint8_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->u8;
            break;
        }
        case RAY_STR: {
            ray_t* svec = ray_vec_new(RAY_STR, n);
            if (RAY_IS_ERR(svec)) { ray_free(vec); return svec; }
            for (int64_t i = 0; i < n; i++) {
                svec = ray_str_vec_append(svec, ray_str_ptr(args[i]), ray_str_len(args[i]));
                if (RAY_IS_ERR(svec)) return svec;
            }
            ray_free(vec);
            return svec;
        }
        case RAY_GUID: {
            uint8_t* d = (uint8_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) {
                const uint8_t* gd = args[i]->obj ? (const uint8_t*)ray_data(args[i]->obj) : (const uint8_t*)ray_data(args[i]);
                memcpy(d + i * 16, gd, 16);
            }
            break;
        }
        default: goto as_list;
        }
        vec->len = n;
        return vec;
    }
as_list:;
    /* Heterogeneous → list */
    ray_t* lst = ray_list_new((int32_t)n);
    if (RAY_IS_ERR(lst)) return lst;
    for (int64_t i = 0; i < n; i++) {
        ray_retain(args[i]);
        lst = ray_list_append(lst, args[i]);
        ray_release(args[i]);
        if (RAY_IS_ERR(lst)) return lst;
    }
    return lst;
}

/* (dict keys vals) → dict */
static ray_t* ray_dict_fn(ray_t* keys, ray_t* vals) {
    if (!ray_is_vec(keys) || (keys->type != RAY_SYM && keys->len > 0))
        return ray_error("type", NULL);
    int64_t n = keys->len;
    ray_t* dict = ray_list_new((int32_t)(n * 2));
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    int64_t* syms = (int64_t*)ray_data(keys);
    for (int64_t i = 0; i < n; i++) {
        ray_t* k = ray_sym(syms[i]);
        if (RAY_IS_ERR(k)) { ray_release(dict); return k; }
        dict = ray_list_append(dict, k);
        ray_release(k);
        if (RAY_IS_ERR(dict)) return dict;

        /* Get value: from list or vector */
        ray_t* v;
        int alloc = 0;
        if (vals->type == RAY_LIST) {
            v = (i < vals->len) ? ((ray_t**)ray_data(vals))[i] : NULL;
        } else if (ray_is_vec(vals)) {
            v = collection_elem(vals, i, &alloc);
        } else {
            v = vals;
        }
        if (v && !RAY_IS_ERR(v)) {
            dict = ray_list_append(dict, v);
            if (alloc) ray_release(v);
        } else {
            ray_t* null_val = ray_i64(INT64_MIN);
            dict = ray_list_append(dict, null_val);
            ray_release(null_val);
        }
        if (RAY_IS_ERR(dict)) return dict;
    }
    return dict;
}

/* (nil? x) → true if x is null */
static ray_t* ray_nil_fn(ray_t* x) {
    if (!x) return ray_bool(true);
    if (ray_is_atom(x)) {
        switch (-x->type) {
        case RAY_I16:  return ray_bool(x->i16 == INT16_MIN);
        case RAY_I32:  case RAY_DATE: case RAY_TIME:
            return ray_bool(x->i32 == INT32_MIN);
        case RAY_I64:  case RAY_TIMESTAMP: case RAY_SYM:
            return ray_bool(x->i64 == INT64_MIN);
        case RAY_F64:  return ray_bool(isnan(x->f64));
        }
    }
    return ray_bool(false);
}

/* (where bool-vec) → indices of true values */
static ray_t* ray_where_fn(ray_t* x) {
    if (!ray_is_vec(x) || x->type != RAY_BOOL)
        return ray_error("type", NULL);
    bool* data = (bool*)ray_data(x);
    int64_t n = x->len;
    /* Count trues */
    int64_t cnt = 0;
    for (int64_t i = 0; i < n; i++) if (data[i]) cnt++;
    ray_t* result = ray_vec_new(RAY_I64, cnt);
    if (RAY_IS_ERR(result)) return result;
    int64_t* out = (int64_t*)ray_data(result);
    int64_t j = 0;
    for (int64_t i = 0; i < n; i++) if (data[i]) out[j++] = i;
    result->len = cnt;
    return result;
}

/* (group vec) → dict mapping each unique value to its indices */
static ray_t* ray_group_fn(ray_t* x) {
    if (!ray_is_vec(x) && x->type != RAY_LIST)
        return ray_error("type", NULL);
    int64_t n = x->len;
    if (n == 0) {
        ray_t* d = ray_list_new(0);
        if (!RAY_IS_ERR(d)) d->attrs |= RAY_ATTR_DICT;
        return d;
    }

    /* Use a fixed-size approach: collect unique values with ray_alloc blocks */
    /* Max groups = n (all unique). Store in a large ray_alloc block. */
    int64_t max_groups = n < 1024 ? n : 1024;
    ray_t* val_block = ray_alloc((size_t)(max_groups * sizeof(int64_t)));
    if (RAY_IS_ERR(val_block)) return val_block;
    int64_t* gvals = (int64_t*)ray_data(val_block);

    /* For each group, store indices in a separate i64 vector */
    ray_t** idx_vecs = NULL;
    ray_t* ivblock = ray_alloc((size_t)(max_groups * sizeof(ray_t*)));
    if (RAY_IS_ERR(ivblock)) { ray_free(val_block); return ivblock; }
    idx_vecs = (ray_t**)ray_data(ivblock);
    int64_t ngroups = 0;

    /* For LIST type, use atom_eq-based grouping with stored keys */
    if (x->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(x);
        /* Store group keys as ray_t* pointers */
        ray_t* kblock = ray_alloc((size_t)(max_groups * sizeof(ray_t*)));
        if (RAY_IS_ERR(kblock)) { ray_free(val_block); ray_free(ivblock); return kblock; }
        ray_t** gkeys = (ray_t**)ray_data(kblock);

        for (int64_t i = 0; i < n; i++) {
            ray_t* elem = elems[i];
            int64_t gi = -1;
            for (int64_t g = 0; g < ngroups; g++) {
                if (atom_eq(gkeys[g], elem)) { gi = g; break; }
            }
            if (gi < 0) {
                if (ngroups >= max_groups) {
                    for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                    ray_free(val_block); ray_free(ivblock); ray_free(kblock);
                    return ray_error("limit", NULL);
                }
                gi = ngroups++;
                gkeys[gi] = elem;
                idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
            }
            idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
        }
        /* Build dict */
        ray_t* dict = ray_list_new((int32_t)(ngroups * 2));
        if (RAY_IS_ERR(dict)) { ray_free(kblock); goto gfail; }
        dict->attrs |= RAY_ATTR_DICT;
        for (int64_t g = 0; g < ngroups; g++) {
            ray_retain(gkeys[g]);
            dict = ray_list_append(dict, gkeys[g]);
            ray_release(gkeys[g]);
            if (RAY_IS_ERR(dict)) { ray_free(kblock); goto gfail; }
            dict = ray_list_append(dict, idx_vecs[g]);
            ray_release(idx_vecs[g]);
            idx_vecs[g] = NULL;
            if (RAY_IS_ERR(dict)) { ray_free(kblock); goto gfail; }
        }
        ray_free(val_block); ray_free(ivblock); ray_free(kblock);
        return dict;
    }

    /* RAY_STR: string-based grouping using ray_str_vec_get */
    if (x->type == RAY_STR) {
        /* Store group keys as (ptr, len) pairs — use a scratch block for strings */
        ray_t* skblock = ray_alloc((size_t)(max_groups * sizeof(ray_t*)));
        if (RAY_IS_ERR(skblock)) { ray_free(val_block); ray_free(ivblock); return skblock; }
        ray_t** str_keys = (ray_t**)ray_data(skblock);

        for (int64_t i = 0; i < n; i++) {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(x, i, &slen);

            int64_t gi = -1;
            for (int64_t g = 0; g < ngroups; g++) {
                size_t gsl = ray_str_len(str_keys[g]);
                const char* gsp = ray_str_ptr(str_keys[g]);
                if (gsl == slen && (slen == 0 || memcmp(gsp, sp, slen) == 0)) {
                    gi = g; break;
                }
            }
            if (gi < 0) {
                if (ngroups >= max_groups) {
                    for (int64_t g = 0; g < ngroups; g++) {
                        ray_release(str_keys[g]);
                        ray_release(idx_vecs[g]);
                    }
                    ray_free(val_block); ray_free(ivblock); ray_free(skblock);
                    return ray_error("limit", NULL);
                }
                gi = ngroups++;
                str_keys[gi] = ray_str(sp ? sp : "", slen);
                idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
            }
            idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
        }

        /* Build dict */
        ray_t* dict = ray_list_new((int32_t)(ngroups * 2));
        if (RAY_IS_ERR(dict)) {
            for (int64_t g = 0; g < ngroups; g++) {
                ray_release(str_keys[g]);
                ray_release(idx_vecs[g]);
            }
            ray_free(val_block); ray_free(ivblock); ray_free(skblock);
            return ray_error("domain", NULL);
        }
        dict->attrs |= RAY_ATTR_DICT;
        for (int64_t g = 0; g < ngroups; g++) {
            dict = ray_list_append(dict, str_keys[g]);
            ray_release(str_keys[g]);
            if (RAY_IS_ERR(dict)) { ray_free(skblock); goto gfail; }
            dict = ray_list_append(dict, idx_vecs[g]);
            ray_release(idx_vecs[g]);
            idx_vecs[g] = NULL;
            if (RAY_IS_ERR(dict)) { ray_free(skblock); goto gfail; }
        }
        ray_free(val_block); ray_free(ivblock); ray_free(skblock);
        return dict;
    }

    for (int64_t i = 0; i < n; i++) {
        int64_t v;
        if (x->type == RAY_SYM || x->type == RAY_I64 || x->type == RAY_TIMESTAMP)
            v = ((int64_t*)ray_data(x))[i];
        else if (x->type == RAY_I32 || x->type == RAY_DATE || x->type == RAY_TIME)
            v = ((int32_t*)ray_data(x))[i];
        else if (x->type == RAY_BOOL)
            v = ((bool*)ray_data(x))[i] ? 1 : 0;
        else
            v = i;

        int64_t gi = -1;
        for (int64_t g = 0; g < ngroups; g++) {
            if (gvals[g] == v) { gi = g; break; }
        }
        if (gi < 0) {
            if (ngroups >= max_groups) {
                for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                ray_free(val_block); ray_free(ivblock);
                return ray_error("limit", NULL);
            }
            gi = ngroups++;
            gvals[gi] = v;
            idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
        }
        idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
    }

    /* Build dict */
    ray_t* dict = ray_list_new((int32_t)(ngroups * 2));
    if (RAY_IS_ERR(dict)) goto gfail;
    dict->attrs |= RAY_ATTR_DICT;

    for (int64_t g = 0; g < ngroups; g++) {
        ray_t* k;
        if (x->type == RAY_SYM) k = ray_sym(gvals[g]);
        else if (x->type == RAY_BOOL) k = ray_bool(gvals[g] != 0);
        else k = ray_i64(gvals[g]);
        if (RAY_IS_ERR(k)) { ray_release(dict); goto gfail; }
        dict = ray_list_append(dict, k);
        ray_release(k);
        if (RAY_IS_ERR(dict)) goto gfail;
        dict = ray_list_append(dict, idx_vecs[g]);
        ray_release(idx_vecs[g]);
        idx_vecs[g] = NULL;
        if (RAY_IS_ERR(dict)) goto gfail;
    }
    ray_free(val_block);
    ray_free(ivblock);
    return dict;

gfail:
    for (int64_t g = 0; g < ngroups; g++)
        if (idx_vecs[g]) ray_release(idx_vecs[g]);
    ray_free(val_block);
    ray_free(ivblock);
    return ray_error("domain", NULL);
}

/* (concat a b) → concatenate vectors/strings/dicts/tables */
static ray_t* ray_concat_fn(ray_t* a, ray_t* b) {
    /* Helper: get string content from atom (STR or CHAR), stripping trailing nulls */
    {
        int a_is_str = ray_is_atom(a) && ((-a->type) == RAY_STR);
        int b_is_str = ray_is_atom(b) && ((-b->type) == RAY_STR);
        if (a_is_str && b_is_str) {
            const char *ap, *bp;
            size_t la, lb;
            ap = ray_str_ptr(a); la = ray_str_len(a);
            bp = ray_str_ptr(b); lb = ray_str_len(b);
            /* Strip trailing null bytes */
            while (la > 0 && ap[la - 1] == '\0') la--;
            while (lb > 0 && bp[lb - 1] == '\0') lb--;
            char buf[8192];
            if (la + lb > sizeof(buf)) return ray_error("limit", NULL);
            memcpy(buf, ap, la);
            memcpy(buf + la, bp, lb);
            return ray_str(buf, la + lb);
        }
    }
    /* Vector concat: same type */
    if (ray_is_vec(a) && ray_is_vec(b) && a->type == b->type) {
        int64_t na = a->len, nb = b->len;
        if (a->type == RAY_STR) {
            ray_t* result = ray_vec_new(RAY_STR, na + nb);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 0; i < na; i++) {
                size_t slen;
                const char* sp = ray_str_vec_get(a, i, &slen);
                result = ray_str_vec_append(result, sp, slen);
                if (RAY_IS_ERR(result)) return result;
            }
            for (int64_t i = 0; i < nb; i++) {
                size_t slen;
                const char* sp = ray_str_vec_get(b, i, &slen);
                result = ray_str_vec_append(result, sp, slen);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
        int esz = ray_elem_size(a->type);
        ray_t* result = ray_vec_new(a->type, na + nb);
        if (RAY_IS_ERR(result)) return result;
        memcpy(ray_data(result), ray_data(a), (size_t)(na * esz));
        memcpy((char*)ray_data(result) + na * esz, ray_data(b), (size_t)(nb * esz));
        result->len = na + nb;
        /* Copy sym_dict / sym stuff for SYM vectors */
        if (a->type == RAY_SYM && a->sym_dict)
            result->sym_dict = (ray_retain(a->sym_dict), a->sym_dict);
        return result;
    }
    /* Concat typed vec + boxed list or boxed list + typed vec → boxed list */
    if ((ray_is_vec(a) && b->type == RAY_LIST) || (a->type == RAY_LIST && ray_is_vec(b))) {
        ray_t* la = (a->type == RAY_LIST) ? a : NULL;
        ray_t* lb = (b->type == RAY_LIST) ? b : NULL;
        ray_t* va = ray_is_vec(a) ? a : NULL;
        ray_t* vb = ray_is_vec(b) ? b : NULL;
        int64_t na = a->len, nb = b->len;
        ray_t* result = ray_alloc((na + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + nb;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < na; i++) {
            if (va) {
                int alloc = 0;
                out[i] = collection_elem(va, i, &alloc);
            } else {
                out[i] = ((ray_t**)ray_data(la))[i];
                ray_retain(out[i]);
            }
        }
        for (int64_t i = 0; i < nb; i++) {
            if (vb) {
                int alloc = 0;
                out[na + i] = collection_elem(vb, i, &alloc);
            } else {
                out[na + i] = ((ray_t**)ray_data(lb))[i];
                ray_retain(out[na + i]);
            }
        }
        return result;
    }
    /* Boxed list concat */
    if (a->type == RAY_LIST && b->type == RAY_LIST && !(a->attrs & RAY_ATTR_DICT) && !(b->attrs & RAY_ATTR_DICT)) {
        int64_t na = a->len, nb = b->len;
        ray_t* result = ray_alloc((na + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + nb;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** ae = (ray_t**)ray_data(a);
        ray_t** be = (ray_t**)ray_data(b);
        for (int64_t i = 0; i < na; i++) { ray_retain(ae[i]); out[i] = ae[i]; }
        for (int64_t i = 0; i < nb; i++) { ray_retain(be[i]); out[na + i] = be[i]; }
        return result;
    }
    /* Vector concat: mixed types → boxed list (preserves original element types) */
    if (ray_is_vec(a) && ray_is_vec(b) && a->type != b->type) {
        int64_t na = a->len, nb = b->len;
        ray_t* result = ray_alloc((na + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + nb;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < na; i++) {
            int alloc = 0;
            out[i] = collection_elem(a, i, &alloc);
            /* collection_elem always allocates for typed vecs, so ownership transfers */
        }
        for (int64_t i = 0; i < nb; i++) {
            int alloc = 0;
            out[na + i] = collection_elem(b, i, &alloc);
        }
        return result;
    }
    /* Atom + vector or vector + atom → append */
    if (ray_is_atom(a) && ray_is_vec(b) && (-a->type) == b->type) {
        int64_t nb = b->len;
        int esz = ray_elem_size(b->type);
        ray_t* result = ray_vec_new(b->type, 1 + nb);
        if (RAY_IS_ERR(result)) return result;
        /* Copy atom value as first element */
        switch (b->type) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)ray_data(result))[0] = a->i64; break;
        case RAY_F64:
            ((double*)ray_data(result))[0] = a->f64; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(result))[0] = a->i32; break;
        case RAY_I16:
            ((int16_t*)ray_data(result))[0] = a->i16; break;
        case RAY_BOOL:
            ((bool*)ray_data(result))[0] = a->b8; break;
        case RAY_U8:
            ((uint8_t*)ray_data(result))[0] = a->u8; break;
        case RAY_GUID: {
            const uint8_t* gd = a->obj ? (const uint8_t*)ray_data(a->obj) : (const uint8_t*)ray_data((ray_t*)a);
            memcpy(ray_data(result), gd, 16); break;
        }
        default: ray_free(result); return ray_error("type", NULL);
        }
        memcpy((char*)ray_data(result) + esz, ray_data(b), (size_t)(nb * esz));
        result->len = 1 + nb;
        return result;
    }
    if (ray_is_vec(a) && ray_is_atom(b) && a->type == (-b->type)) {
        int64_t na = a->len;
        int esz = ray_elem_size(a->type);
        ray_t* result = ray_vec_new(a->type, na + 1);
        if (RAY_IS_ERR(result)) return result;
        memcpy(ray_data(result), ray_data(a), (size_t)(na * esz));
        switch (a->type) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)ray_data(result))[na] = b->i64; break;
        case RAY_F64:
            ((double*)ray_data(result))[na] = b->f64; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(result))[na] = b->i32; break;
        case RAY_I16:
            ((int16_t*)ray_data(result))[na] = b->i16; break;
        case RAY_BOOL:
            ((bool*)ray_data(result))[na] = b->b8; break;
        case RAY_U8:
            ((uint8_t*)ray_data(result))[na] = b->u8; break;
        case RAY_GUID: {
            const uint8_t* gd = b->obj ? (const uint8_t*)ray_data(b->obj) : (const uint8_t*)ray_data((ray_t*)b);
            memcpy((uint8_t*)ray_data(result) + na * 16, gd, 16); break;
        }
        default: ray_free(result); return ray_error("type", NULL);
        }
        result->len = na + 1;
        return result;
    }
    /* Atom + atom of same type → 2-element vector */
    if (ray_is_atom(a) && ray_is_atom(b) && a->type == b->type && a->type != -RAY_STR) {
        int8_t vtype = -(a->type);
        ray_t* result = ray_vec_new(vtype, 2);
        if (RAY_IS_ERR(result)) return result;
        result->len = 2;
        switch (vtype) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)ray_data(result))[0] = a->i64;
            ((int64_t*)ray_data(result))[1] = b->i64;
            break;
        case RAY_F64:
            ((double*)ray_data(result))[0] = a->f64;
            ((double*)ray_data(result))[1] = b->f64;
            break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(result))[0] = a->i32;
            ((int32_t*)ray_data(result))[1] = b->i32;
            break;
        case RAY_I16:
            ((int16_t*)ray_data(result))[0] = a->i16;
            ((int16_t*)ray_data(result))[1] = b->i16;
            break;
        case RAY_BOOL:
            ((bool*)ray_data(result))[0] = a->b8;
            ((bool*)ray_data(result))[1] = b->b8;
            break;
        case RAY_U8:
            ((uint8_t*)ray_data(result))[0] = a->u8;
            ((uint8_t*)ray_data(result))[1] = b->u8;
            break;
        case RAY_GUID: {
            const uint8_t* ga = a->obj ? (const uint8_t*)ray_data(a->obj) : (const uint8_t*)ray_data((ray_t*)a);
            const uint8_t* gb = b->obj ? (const uint8_t*)ray_data(b->obj) : (const uint8_t*)ray_data((ray_t*)b);
            memcpy(ray_data(result), ga, 16);
            memcpy((uint8_t*)ray_data(result) + 16, gb, 16);
            break;
        }
        default: ray_free(result); return ray_error("type", NULL);
        }
        return result;
    }
    /* Dict concat: merge */
    if (a->type == RAY_LIST && (a->attrs & RAY_ATTR_DICT) &&
        b->type == RAY_LIST && (b->attrs & RAY_ATTR_DICT)) {
        ray_t* result = ray_list_new(0);
        if (RAY_IS_ERR(result)) return result;
        result->attrs |= RAY_ATTR_DICT;
        /* Copy all from a */
        ray_t** aitems = (ray_t**)ray_data(a);
        for (int64_t i = 0; i < a->len; i++) {
            ray_retain(aitems[i]);
            result = ray_list_append(result, aitems[i]);
            ray_release(aitems[i]);
            if (RAY_IS_ERR(result)) return result;
        }
        /* Merge from b: overwrite existing keys, add new ones */
        ray_t** bitems = (ray_t**)ray_data(b);
        for (int64_t i = 0; i < b->len; i += 2) {
            /* Find key in result */
            ray_t** ritems = (ray_t**)ray_data(result);
            bool found = false;
            for (int64_t j = 0; j < result->len; j += 2) {
                if (ritems[j]->i64 == bitems[i]->i64) {
                    /* Replace value */
                    ray_release(ritems[j + 1]);
                    ray_retain(bitems[i + 1]);
                    ritems[j + 1] = bitems[i + 1];
                    found = true;
                    break;
                }
            }
            if (!found) {
                ray_retain(bitems[i]);
                result = ray_list_append(result, bitems[i]);
                ray_release(bitems[i]);
                if (RAY_IS_ERR(result)) return result;
                ray_retain(bitems[i + 1]);
                result = ray_list_append(result, bitems[i + 1]);
                ray_release(bitems[i + 1]);
                if (RAY_IS_ERR(result)) return result;
            }
        }
        return result;
    }
    /* Table concat: append rows */
    if (a->type == RAY_TABLE && b->type == RAY_TABLE) {
        int64_t ncols_a = a->len;
        int64_t ncols_b = b->len;
        /* Match columns of a in b by name */
        ray_t* result = ray_table_new((int32_t)ncols_a);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t c = 0; c < ncols_a; c++) {
            int64_t col_name_a = ray_table_col_name(a, c);
            ray_t* acol = ray_table_get_col_idx(a, c);
            /* Find matching column in b by name */
            ray_t* bcol = NULL;
            for (int64_t j = 0; j < ncols_b; j++) {
                if (ray_table_col_name(b, j) == col_name_a) {
                    bcol = ray_table_get_col_idx(b, j);
                    break;
                }
            }
            if (!bcol) {
                /* Column not found in b — error if b also has columns not in a */
                if (ncols_b > ncols_a) {
                    ray_release(result);
                    return ray_error("domain", NULL);
                }
                /* Otherwise error */
                ray_release(result);
                return ray_error("domain", NULL);
            }
            /* Type check: columns must have the same type */
            if (acol->type != bcol->type) {
                ray_release(result);
                return ray_error("type", NULL);
            }
            ray_t* col = ray_concat_fn(acol, bcol);
            if (RAY_IS_ERR(col)) { ray_release(result); return col; }
            result = ray_table_add_col(result, col_name_a, col);
            ray_release(col);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Atom + boxed list → prepend atom to list */
    if (ray_is_atom(a) && b->type == RAY_LIST && !(b->attrs & RAY_ATTR_DICT)) {
        int64_t nb = b->len;
        ray_t* result = ray_alloc((1 + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 1 + nb;
        ray_t** out = (ray_t**)ray_data(result);
        ray_retain(a);
        out[0] = a;
        ray_t** be = (ray_t**)ray_data(b);
        for (int64_t i = 0; i < nb; i++) { ray_retain(be[i]); out[1 + i] = be[i]; }
        return result;
    }
    /* Boxed list + atom → append atom to list */
    if (a->type == RAY_LIST && !(a->attrs & RAY_ATTR_DICT) && ray_is_atom(b)) {
        int64_t na = a->len;
        ray_t* result = ray_alloc((na + 1) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + 1;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** ae = (ray_t**)ray_data(a);
        for (int64_t i = 0; i < na; i++) { ray_retain(ae[i]); out[i] = ae[i]; }
        ray_retain(b);
        out[na] = b;
        return result;
    }
    /* Atom + atom of different types → 2-element boxed list */
    if (ray_is_atom(a) && ray_is_atom(b) && a->type != b->type) {
        ray_t* result = ray_alloc(2 * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 2;
        ray_t** out = (ray_t**)ray_data(result);
        ray_retain(a); out[0] = a;
        ray_retain(b); out[1] = b;
        return result;
    }
    return ray_error("type", NULL);
}

/* (raze list-of-vecs) → flattened vector */
static ray_t* ray_raze_fn(ray_t* x) {
    /* Scalar passthrough */
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    /* Typed vector passthrough */
    if (ray_is_vec(x)) { ray_retain(x); return x; }
    if (x->type != RAY_LIST)
        return ray_error("type", NULL);
    int64_t n = x->len;
    if (n == 0) return ray_list_new(0);
    ray_t** items = (ray_t**)ray_data(x);
    /* Try to concat all items */
    ray_t* result = items[0];
    ray_retain(result);
    for (int64_t i = 1; i < n; i++) {
        ray_t* next = ray_concat_fn(result, items[i]);
        ray_release(result);
        if (RAY_IS_ERR(next)) return next;
        result = next;
    }
    return result;
}

/* (within vals [lo hi]) → bool vector, true where lo <= val <= hi */
static ray_t* ray_within_fn(ray_t* vals, ray_t* range) {
    if (!ray_is_vec(vals) || !ray_is_vec(range) || range->len != 2)
        return ray_error("type", NULL);
    int64_t n = vals->len;
    ray_t* result = ray_vec_new(RAY_BOOL, n);
    if (RAY_IS_ERR(result)) return result;
    bool* out = (bool*)ray_data(result);

    if (vals->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(vals);
        int64_t* r = (int64_t*)ray_data(range);
        int64_t lo = r[0], hi = r[1];
        for (int64_t i = 0; i < n; i++) out[i] = (d[i] >= lo && d[i] <= hi);
    } else if (vals->type == RAY_F64) {
        double* d = (double*)ray_data(vals);
        double* r = (double*)ray_data(range);
        double lo = r[0], hi = r[1];
        for (int64_t i = 0; i < n; i++) out[i] = (d[i] >= lo && d[i] <= hi);
    } else if (vals->type == RAY_I32 || vals->type == RAY_DATE || vals->type == RAY_TIME) {
        int32_t* d = (int32_t*)ray_data(vals);
        int32_t* r = (int32_t*)ray_data(range);
        int32_t lo = r[0], hi = r[1];
        for (int64_t i = 0; i < n; i++) out[i] = (d[i] >= lo && d[i] <= hi);
    } else {
        ray_free(result);
        return ray_error("type", NULL);
    }
    result->len = n;
    return result;
}

/* (div a b) → float division (always returns f64) */
static ray_t* ray_fdiv_fn(ray_t* a, ray_t* b) {
    if (!ray_is_atom(a) || !ray_is_atom(b)) return ray_error("type", NULL);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    /* Null propagation */
    if (is_null_atom(a) || is_null_atom(b)) return make_f64(NAN);
    double fa = as_f64(a), fb = as_f64(b);
    if (fb == 0.0) return make_f64(NAN);
    return make_f64(fa / fb);
}

/* (rand n max) → vector of n random i64 in [0, max) */
static ray_t* ray_rand_fn(ray_t* a, ray_t* b) {
    if (!ray_is_atom(a) || !ray_is_atom(b)) return ray_error("type", NULL);
    int64_t n, mx;
    if (a->type == -RAY_I64) n = a->i64;
    else if (a->type == -RAY_I32) n = a->i32;
    else return ray_error("type", NULL);
    if (b->type == -RAY_I64) mx = b->i64;
    else if (b->type == -RAY_I32) mx = b->i32;
    else return ray_error("type", NULL);
    if (n < 0) return ray_error("domain", NULL);
    if (mx <= 0) return ray_error("domain", NULL);
    if (n == 0) return ray_vec_new(RAY_I64, 0);
    ray_t* vec = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(vec)) return vec;
    int64_t* d = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) d[i] = (int64_t)(rand() % mx);
    vec->len = n;
    return vec;
}

/* (bin sorted-vec val) → rightmost index where sorted[i] <= val, -1 if none */
static ray_t* ray_bin_fn(ray_t* sorted, ray_t* val) {
    if (!ray_is_vec(sorted) || sorted->type != RAY_I64)
        return ray_error("type", NULL);
    int64_t* d = (int64_t*)ray_data(sorted);
    int64_t n = sorted->len;

    if (ray_is_atom(val) && (val->type == -RAY_I64 || val->type == -RAY_I32)) {
        int64_t v = val->i64;
        int64_t lo = 0, hi = n - 1, result = -1;
        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            if (d[mid] <= v) { result = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return ray_i64(result);
    }
    if (ray_is_vec(val) && val->type == RAY_I64) {
        int64_t* vals = (int64_t*)ray_data(val);
        int64_t vn = val->len;
        ray_t* rvec = ray_vec_new(RAY_I64, vn);
        if (RAY_IS_ERR(rvec)) return rvec;
        int64_t* out = (int64_t*)ray_data(rvec);
        for (int64_t i = 0; i < vn; i++) {
            int64_t v = vals[i];
            int64_t lo = 0, hi = n - 1, r = -1;
            while (lo <= hi) {
                int64_t mid = lo + (hi - lo) / 2;
                if (d[mid] <= v) { r = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            out[i] = r;
        }
        rvec->len = vn;
        return rvec;
    }
    return ray_error("type", NULL);
}

/* (binr sorted-vec val) → leftmost index where sorted[i] >= val */
static ray_t* ray_binr_fn(ray_t* sorted, ray_t* val) {
    if (!ray_is_vec(sorted) || sorted->type != RAY_I64)
        return ray_error("type", NULL);
    int64_t* d = (int64_t*)ray_data(sorted);
    int64_t n = sorted->len;

    if (ray_is_atom(val) && (val->type == -RAY_I64 || val->type == -RAY_I32)) {
        int64_t v = val->i64;
        int64_t lo = 0, hi = n - 1, result = n;
        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            if (d[mid] >= v) { result = mid; hi = mid - 1; }
            else lo = mid + 1;
        }
        return ray_i64(result >= n ? n - 1 : result);
    }
    if (ray_is_vec(val) && val->type == RAY_I64) {
        int64_t* vals = (int64_t*)ray_data(val);
        int64_t vn = val->len;
        ray_t* rvec = ray_vec_new(RAY_I64, vn);
        if (RAY_IS_ERR(rvec)) return rvec;
        int64_t* out = (int64_t*)ray_data(rvec);
        for (int64_t i = 0; i < vn; i++) {
            int64_t v = vals[i];
            int64_t lo = 0, hi = n - 1, r = n;
            while (lo <= hi) {
                int64_t mid = lo + (hi - lo) / 2;
                if (d[mid] >= v) { r = mid; hi = mid - 1; }
                else lo = mid + 1;
            }
            out[i] = r >= n ? n - 1 : r;
        }
        rvec->len = vn;
        return rvec;
    }
    return ray_error("type", NULL);
}

/* (map-left fn fixed vec) → apply fn(fixed, elem) for each elem in vec */
/* Helper for map-left/map-right: iterate over vec calling fn with two args */
static ray_t* map_iterate(ray_t* fn, ray_t* fixed, ray_t* vec, int fixed_is_left) {
    /* If both are scalars, just call once */
    if (!ray_is_vec(vec) && vec->type != RAY_LIST) {
        if (fixed_is_left)
            return call_fn2(fn, fixed, vec);
        else
            return call_fn2(fn, vec, fixed);
    }

    int64_t vn = vec->len;
    ray_t* stack_results[4096];
    ray_t** results = stack_results;
    if (vn > 4096) {
        results = (ray_t**)ray_sys_alloc((size_t)vn * sizeof(ray_t*));
        if (!results) return ray_error("oom", NULL);
    }

    for (int64_t i = 0; i < vn; i++) {
        int alloc = 0;
        ray_t* elem = collection_elem(vec, i, &alloc);
        if (fixed_is_left)
            results[i] = call_fn2(fn, fixed, elem);
        else
            results[i] = call_fn2(fn, elem, fixed);
        if (alloc) ray_release(elem);
        if (RAY_IS_ERR(results[i])) {
            ray_t* err = results[i];
            for (int64_t j = 0; j < i; j++) ray_release(results[j]);
            if (results != stack_results) ray_sys_free(results);
            return err;
        }
    }
    ray_t* out = ray_enlist(results, vn);
    for (int64_t i = 0; i < vn; i++) ray_release(results[i]);
    if (results != stack_results) ray_sys_free(results);
    return out;
}

/* (map-left fn fixed vec) → apply fn(fixed, elem) for each elem in vec.
 * If vec is scalar but fixed is a vector, auto-swap (iterate over fixed). */
static ray_t* ray_map_left(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("domain", NULL);
    ray_t* fn = args[0];
    ray_t* fixed = args[1];
    ray_t* vec = args[2];

    /* Auto-detect: if vec is scalar but fixed is a vector, swap roles */
    if (!ray_is_vec(vec) && vec->type != RAY_LIST &&
        (ray_is_vec(fixed) || fixed->type == RAY_LIST)) {
        return map_iterate(fn, vec, fixed, 0); /* fn(elem_of_fixed, vec) — but we want fn(fixed=scalar, elem) */
    }

    return map_iterate(fn, fixed, vec, 1); /* fn(fixed, elem) */
}

/* (map-right fn vec fixed) → apply fn(elem, fixed) for each elem in vec.
 * If vec is scalar but fixed is a vector, auto-swap (iterate over fixed). */
static ray_t* ray_map_right(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("domain", NULL);
    ray_t* fn = args[0];
    ray_t* vec = args[1];
    ray_t* fixed = args[2];

    /* Auto-detect: if vec is scalar but fixed is a vector, swap roles */
    if (!ray_is_vec(vec) && vec->type != RAY_LIST &&
        (ray_is_vec(fixed) || fixed->type == RAY_LIST)) {
        return map_iterate(fn, vec, fixed, 1); /* fn(vec_scalar, elem_of_fixed) */
    }

    return map_iterate(fn, fixed, vec, 0); /* fn(elem, fixed) */
}

/* (split str delim) → list of strings */
static ray_t* ray_split_fn(ray_t* str, ray_t* delim) {
    /* List split: (split list indices) → list of sub-lists */
    if (str->type == RAY_LIST &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return NULL; /* null for empty indices */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", NULL);
        for (int64_t ii = 0; ii < nidx; ii++) {
            int alloc = 0;
            ray_t* ie = collection_elem(delim, ii, &alloc);
            idx_buf[ii] = as_i64(ie);
            if (alloc) ray_release(ie);
        }
        int64_t total = str->len;
        ray_t** items = (ray_t**)ray_data(str);
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            /* Try to make a typed vector if all elements are same type */
            if (seglen > 0) {
                int8_t first_type = items[start]->type;
                int all_same = 1;
                for (int64_t j = start + 1; j < start + seglen && j < total; j++) {
                    if (items[j]->type != first_type) { all_same = 0; break; }
                }
                if (all_same && first_type < 0 && first_type != -RAY_STR) {
                    int8_t vtype = -first_type;
                    ray_t* vec = ray_vec_new(vtype, seglen);
                    if (!RAY_IS_ERR(vec)) {
                        vec->len = seglen;
                        for (int64_t j = 0; j < seglen && start + j < total; j++)
                            store_typed_elem(vec, j, items[start + j]);
                        result = ray_list_append(result, vec);
                        ray_release(vec);
                        if (RAY_IS_ERR(result)) return result;
                        continue;
                    }
                }
            }
            /* Heterogeneous or string segment: make a sub-list */
            ray_t* seg = ray_list_new(seglen);
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            for (int64_t j = 0; j < seglen && start + j < total; j++) {
                ray_retain(items[start + j]);
                seg = ray_list_append(seg, items[start + j]);
                ray_release(items[start + j]);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            }
            result = ray_list_append(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Vector/string split: (split vec/str indices) → list of sub-vectors/substrings */
    if ((ray_is_vec(str) || (ray_is_atom(str) && (-str->type) == RAY_STR)) &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return NULL; /* null for empty indices */
        /* Extract indices as i64 */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", NULL);
        for (int64_t ii = 0; ii < nidx; ii++) {
            int alloc = 0;
            ray_t* ie = collection_elem(delim, ii, &alloc);
            idx_buf[ii] = as_i64(ie);
            if (alloc) ray_release(ie);
        }
        /* String split by indices */
        if (ray_is_atom(str) && (-str->type) == RAY_STR) {
            const char* sp2 = ray_str_ptr(str);
            size_t total = ray_str_len(str);
            ray_t* result = ray_list_new(nidx + 1);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 0; i < nidx; i++) {
                int64_t start = idx_buf[i];
                int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : (int64_t)total;
                int64_t seglen = end - start;
                if (seglen < 0) seglen = 0;
                if (start > (int64_t)total) start = (int64_t)total;
                if (start + seglen > (int64_t)total) seglen = (int64_t)total - start;
                ray_t* seg = ray_str(sp2 + start, (size_t)seglen);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
                result = ray_list_append(result, seg);
                ray_release(seg);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
        /* Vector split by indices */
        int64_t total = str->len;
        int esz = ray_elem_size(str->type);
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            ray_t* seg = ray_vec_new(str->type, seglen);
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            seg->len = seglen;
            if (seglen > 0) memcpy(ray_data(seg), (char*)ray_data(str) + start * esz, seglen * esz);
            result = ray_list_append(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Normalize str and delim to string pointers */
    const char *sp, *dp;
    size_t slen, dlen;
    ray_t* sym_str_s = NULL;
    ray_t* sym_str_d = NULL;
    if (str->type == -RAY_STR) { sp = ray_str_ptr(str); slen = ray_str_len(str); }
    else if (str->type == -RAY_SYM) { sym_str_s = ray_sym_str(str->i64); if (!sym_str_s) return ray_error("domain", NULL); sp = ray_str_ptr(sym_str_s); slen = ray_str_len(sym_str_s); }
    /* RAY_CHAR removed — all chars are now -RAY_STR */
    else return ray_error("type", NULL);
    if (delim->type == -RAY_STR) { dp = ray_str_ptr(delim); dlen = ray_str_len(delim); }
    /* RAY_CHAR removed — all chars are now -RAY_STR */
    else { if (sym_str_s) ray_release(sym_str_s); return ray_error("type", NULL); }

    ray_t* result = ray_list_new(8);
    if (RAY_IS_ERR(result)) { if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return result; }

    if (dlen == 0 || slen == 0) {
        ray_t* part = ray_str(sp, slen);
        result = ray_list_append(result, part);
        ray_release(part);
        if (sym_str_s) ray_release(sym_str_s);
        if (sym_str_d) ray_release(sym_str_d);
        return result;
    }

    size_t start = 0;
    for (size_t i = 0; i <= slen - dlen; ) {
        if (memcmp(sp + i, dp, dlen) == 0) {
            ray_t* part = ray_str(sp + start, i - start);
            if (RAY_IS_ERR(part)) { ray_release(result); if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return part; }
            result = ray_list_append(result, part);
            ray_release(part);
            if (RAY_IS_ERR(result)) { if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return result; }
            i += dlen;
            start = i;
        } else {
            i++;
        }
    }
    /* Last part */
    ray_t* part = ray_str(sp + start, slen - start);
    if (RAY_IS_ERR(part)) { ray_release(result); if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return part; }
    result = ray_list_append(result, part);
    ray_release(part);
    if (sym_str_s) ray_release(sym_str_s);
    if (sym_str_d) ray_release(sym_str_d);
    return result;
}

/* (ser val) → serialize to bytes (stub: just returns the value) */
static ray_t* ray_ser_fn(ray_t* val) {
    if (!val) {
        /* null → empty bytes marker */
        ray_t* r = ray_alloc(1);
        if (!r) return ray_error("oom", NULL);
        r->type = -RAY_U8;
        r->u8 = 0;
        r->attrs |= 0x80; /* marker: serialized null */
        return r;
    }
    ray_retain(val);
    return val;
}

/* (de val) → deserialize (stub: returns the value, or null for serialized null) */
static ray_t* ray_de_fn(ray_t* val) {
    if (!val) return NULL;
    if (val->type == -RAY_U8 && (val->attrs & 0x80)) return NULL;
    ray_retain(val);
    return val;
}

/* (guid n) → generate n random GUIDs as GUID vector */
static ray_t* ray_guid_fn(ray_t* n_arg) {
    if (!n_arg || !is_numeric(n_arg)) return ray_error("type", NULL);
    int64_t n = as_i64(n_arg);
    if (n < 0) return ray_error("domain", NULL);
    ray_t* result = ray_vec_new(RAY_GUID, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    uint8_t* data = (uint8_t*)ray_data(result);
    for (int64_t i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++)
            data[i * 16 + j] = (uint8_t)(rand() & 0xFF);
        /* Set version 4 and variant bits */
        data[i * 16 + 6] = (data[i * 16 + 6] & 0x0F) | 0x40;
        data[i * 16 + 8] = (data[i * 16 + 8] & 0x3F) | 0x80;
    }
    return result;
}

/* (alter 'var op args...) → in-place mutation (special form: args unevaluated) */
static ray_t* ray_alter_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);
    /* First arg: evaluate to get the symbol */
    ray_t* name_sym = ray_eval(args[0]);
    if (!name_sym || RAY_IS_ERR(name_sym)) return name_sym ? name_sym : ray_error("type", NULL);
    if (name_sym->type != -RAY_SYM) { ray_release(name_sym); return ray_error("type", NULL); }

    /* Resolve the variable */
    ray_t* var = ray_env_get(name_sym->i64);
    if (!var) { ray_release(name_sym); return ray_error("name", NULL); }

    /* Second arg: operation name (unevaluated, must be a name) */
    ray_t* op = args[1];
    if (!op || op->type != -RAY_SYM) { ray_release(name_sym); return ray_error("type", NULL); }
    ray_t* op_name = ray_sym_str(op->i64);
    if (!op_name) { ray_release(name_sym); return ray_error("domain", NULL); }
    const char* oname = ray_str_ptr(op_name);
    size_t olen = ray_str_len(op_name);

    if (olen == 3 && memcmp(oname, "set", 3) == 0) {
        /* (alter 'v set idx val) — idx can be scalar or vector of indices */
        ray_release(op_name);
        if (n < 4) { ray_release(name_sym); return ray_error("domain", NULL); }
        ray_t* idx = ray_eval(args[2]);
        if (!idx || RAY_IS_ERR(idx)) { ray_release(name_sym); return idx ? idx : ray_error("type", NULL); }
        ray_t* val = ray_eval(args[3]);
        if (!val || RAY_IS_ERR(val)) { ray_release(idx); ray_release(name_sym); return val ? val : ray_error("type", NULL); }
        if (!ray_is_vec(var) && var->type != RAY_LIST) { ray_release(idx); ray_release(val); ray_release(name_sym); return ray_error("type", NULL); }

        /* For LIST types, build a new list with replaced elements */
        if (var->type == RAY_LIST) {
            int64_t vlen = ray_len(var);
            ray_t** elems = (ray_t**)ray_data(var);
            ray_t* new_list = ray_alloc(vlen * sizeof(ray_t*));
            if (!new_list) { ray_release(idx); ray_release(val); ray_release(name_sym); return ray_error("oom", NULL); }
            new_list->type = RAY_LIST;
            new_list->len = vlen;
            if (var->attrs & RAY_ATTR_DICT) new_list->attrs |= RAY_ATTR_DICT;
            ray_t** out = (ray_t**)ray_data(new_list);
            for (int64_t i = 0; i < vlen; i++) { ray_retain(elems[i]); out[i] = elems[i]; }

            if (ray_is_atom(idx) && is_numeric(idx)) {
                int64_t i = as_i64(idx);
                if (i >= 0 && i < vlen) { ray_release(out[i]); ray_retain(val); out[i] = val; }
            } else if (ray_is_vec(idx)) {
                int64_t nidx = idx->len;
                for (int64_t k = 0; k < nidx; k++) {
                    int alloc = 0;
                    ray_t* ie = collection_elem(idx, k, &alloc);
                    int64_t i = as_i64(ie);
                    if (alloc) ray_release(ie);
                    if (i >= 0 && i < vlen) { ray_release(out[i]); ray_retain(val); out[i] = val; }
                }
            }
            ray_release(idx); ray_release(val);
            ray_env_set(name_sym->i64, new_list);
            ray_release(name_sym);
            ray_retain(new_list);
            return new_list;
        }

        /* COW if shared (typed vectors only) */
        var = ray_cow(var);
        if (RAY_IS_ERR(var)) { ray_release(idx); ray_release(val); ray_release(name_sym); return var; }

        if (ray_is_atom(idx) && is_numeric(idx)) {
            /* Single index */
            int64_t i = as_i64(idx);
            ray_release(idx);
            if (i < 0 || i >= var->len) { ray_release(val); ray_release(name_sym); return ray_error("range", NULL); }
            store_typed_elem(var, i, val);
        } else if (ray_is_vec(idx)) {
            /* Vector of indices — set each to val.
             * If val is a vector of same length, set pairwise.
             * If val is scalar or shorter, broadcast. */
            int64_t nidx = idx->len;
            int val_is_vec = ray_is_vec(val) && val->len == nidx;
            for (int64_t k = 0; k < nidx; k++) {
                int alloc = 0;
                ray_t* ie = collection_elem(idx, k, &alloc);
                int64_t i = as_i64(ie);
                if (alloc) ray_release(ie);
                if (i < 0 || i >= var->len) continue;
                if (val_is_vec) {
                    int va = 0;
                    ray_t* ve = collection_elem(val, k, &va);
                    store_typed_elem(var, i, ve);
                    if (va) ray_release(ve);
                } else {
                    store_typed_elem(var, i, val);
                }
            }
            ray_release(idx);
        } else {
            ray_release(idx); ray_release(val); ray_release(name_sym);
            return ray_error("type", NULL);
        }
        ray_release(val);
        ray_env_set(name_sym->i64, var);
        ray_release(name_sym);
        ray_retain(var);
        return var;
    }
    if (olen == 6 && memcmp(oname, "concat", 6) == 0) {
        /* (alter 'v concat val) */
        ray_release(op_name);
        if (n < 3) { ray_release(name_sym); return ray_error("domain", NULL); }
        ray_t* val = ray_eval(args[2]);
        if (!val || RAY_IS_ERR(val)) { ray_release(name_sym); return val ? val : ray_error("type", NULL); }
        ray_t* new_vec = ray_concat_fn(var, val);
        ray_release(val);
        if (RAY_IS_ERR(new_vec)) { ray_release(name_sym); return new_vec; }
        ray_env_set(name_sym->i64, new_vec);
        ray_release(name_sym);
        ray_retain(new_vec);
        return new_vec;
    }
    if (olen == 6 && memcmp(oname, "remove", 6) == 0) {
        /* (alter 'v remove idx) — remove element(s) at index/indices */
        ray_release(op_name);
        if (n < 3) { ray_release(name_sym); return ray_error("domain", NULL); }
        ray_t* idx = ray_eval(args[2]);
        if (!idx || RAY_IS_ERR(idx)) { ray_release(name_sym); return idx ? idx : ray_error("type", NULL); }

        if (!var || var->type != RAY_LIST) {
            ray_release(idx); ray_release(name_sym);
            return ray_error("type", NULL);
        }

        int64_t vlen = ray_len(var);
        ray_t** elems = (ray_t**)ray_data(var);

        /* Build a set of indices to remove */
        int64_t remove_idx[256];
        int64_t nremove = 0;
        if (ray_is_atom(idx) && is_numeric(idx)) {
            remove_idx[0] = as_i64(idx);
            nremove = 1;
        } else if (ray_is_vec(idx)) {
            nremove = idx->len;
            if (nremove > 256) { ray_release(idx); ray_release(name_sym); return ray_error("limit", NULL); }
            for (int64_t i = 0; i < nremove; i++) {
                int alloc = 0;
                ray_t* e = collection_elem(idx, i, &alloc);
                remove_idx[i] = as_i64(e);
                if (alloc) ray_release(e);
            }
        } else {
            ray_release(idx); ray_release(name_sym);
            return ray_error("type", NULL);
        }
        ray_release(idx);

        /* Build new list without the removed indices */
        int64_t new_len = vlen;
        for (int64_t i = 0; i < nremove; i++)
            if (remove_idx[i] >= 0 && remove_idx[i] < vlen) new_len--;

        ray_t* new_list = ray_alloc(new_len * sizeof(ray_t*));
        if (!new_list) { ray_release(name_sym); return ray_error("oom", NULL); }
        new_list->type = RAY_LIST;
        new_list->len = new_len;
        if (var->attrs & RAY_ATTR_DICT) new_list->attrs |= RAY_ATTR_DICT;
        ray_t** out = (ray_t**)ray_data(new_list);
        int64_t j = 0;
        for (int64_t i = 0; i < vlen; i++) {
            int skip = 0;
            for (int64_t k = 0; k < nremove; k++)
                if (remove_idx[k] == i) { skip = 1; break; }
            if (!skip) {
                ray_retain(elems[i]);
                out[j++] = elems[i];
            }
        }
        new_list->len = j;
        ray_env_set(name_sym->i64, new_list);
        ray_release(name_sym);
        ray_retain(new_list);
        return new_list;
    }
    ray_release(op_name);
    ray_release(name_sym);
    return ray_error("domain", NULL);
}

/* ══════════════════════════════════════════
 * Pattern matching (like)
 * ══════════════════════════════════════════ */

/* Glob-style pattern matching: * (any chars), ? (single char), [abc] (char class) */
static bool str_glob(const char* s, const char* p) {
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return true;
            while (*s) { if (str_glob(s, p)) return true; s++; }
            return false;
        }
        if (*p == '?') { if (!*s) return false; s++; p++; continue; }
        if (*p == '[') {
            p++;
            bool neg = (*p == '!'); if (neg) p++;
            bool match = false;
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (*s >= p[0] && *s <= p[2]) match = true;
                    p += 3;
                } else {
                    if (*s == *p) match = true;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (neg ? match : !match) return false;
            s++; continue;
        }
        if (*s != *p) return false;
        s++; p++;
    }
    return !*s;
}

/* (like str pattern) — glob-style pattern matching
 * Supports: * (any chars), ? (single char), [abc] (char class)
 * Returns: bool atom or bool vector */
static ray_t* ray_like_fn(ray_t* x, ray_t* pattern) {
    /* Pattern must be a string atom */
    if (pattern->type != -RAY_STR) return ray_error("type", "like: pattern must be a string");
    const char* pat = ray_str_ptr(pattern);

    /* Atom: single match */
    if (x->type == -RAY_STR || x->type == -RAY_SYM) {
        const char* s;
        if (x->type == -RAY_SYM) {
            ray_t* sym_str = ray_sym_str(x->i64);
            s = sym_str ? ray_str_ptr(sym_str) : "";
        } else {
            s = ray_str_ptr(x);
        }
        return make_bool(str_glob(s, pat) ? 1 : 0);
    }

    /* Vector: map over elements */
    if (ray_is_vec(x) && (x->type == RAY_SYM || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* result = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t* out = (uint8_t*)ray_data(result);

        if (x->type == RAY_SYM) {
            int64_t* sym_ids = (int64_t*)ray_data(x);
            for (int64_t i = 0; i < n; i++) {
                ray_t* sym_str = ray_sym_str(sym_ids[i]);
                const char* s = sym_str ? ray_str_ptr(sym_str) : "";
                out[i] = str_glob(s, pat) ? 1 : 0;
            }
        } else {
            /* RAY_STR vector */
            for (int64_t i = 0; i < n; i++) {
                size_t slen;
                const char* s = ray_str_vec_get(x, i, &slen);
                /* Need null-terminated for glob — str_vec_get may not be */
                char buf[256];
                if (s && slen < sizeof(buf)) {
                    memcpy(buf, s, slen); buf[slen] = '\0';
                    out[i] = str_glob(buf, pat) ? 1 : 0;
                } else {
                    out[i] = 0;
                }
            }
        }
        return result;
    }

    return ray_error("type", "like: expects string or symbol");
}

/* ══════════════════════════════════════════
 * Temporal clocks (date, time, timestamp)
 * ══════════════════════════════════════════ */

/* Helper: is the argument the symbol 'global? */
static bool is_global_arg(ray_t* arg) {
    if (arg && arg->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(arg->i64);
        if (s && ray_str_len(s) == 6 && memcmp(ray_str_ptr(s), "global", 6) == 0)
            return true;
    }
    return false;
}

/* Compute seconds since 2000.01.01 00:00:00 UTC (the rayforce epoch) */
static time_t ray_epoch_offset(void) {
    /* 2000-01-01 00:00:00 UTC = 946684800 seconds after 1970 epoch */
    return (time_t)946684800;
}

/* (date 'local) or (date 'global) — returns current date as DATE atom (days since 2000.01.01) */
static ray_t* ray_date_clock(ray_t* arg) {
    bool local = !is_global_arg(arg);
    time_t now = time(NULL);
    struct tm* t = local ? localtime(&now) : gmtime(&now);
    if (!t) return ray_error("domain", "date: failed to get current time");

    /* Reconstruct midnight of today */
    struct tm day = *t;
    day.tm_hour = 0; day.tm_min = 0; day.tm_sec = 0; day.tm_isdst = -1;
    time_t day_time = mktime(&day);

    /* For UTC (global), mktime interprets as local — adjust via difference */
    if (!local) {
        /* Use a simpler approach: total days from epoch */
        int32_t days = (int32_t)((now - ray_epoch_offset()) / 86400);
        return ray_date((int64_t)days);
    }

    /* Local: days since the rayforce epoch, in local time sense */
    int32_t days = (int32_t)((day_time - ray_epoch_offset()) / 86400);
    return ray_date((int64_t)days);
}

/* (time 'local) or (time 'global) — returns current time as TIME atom (ms since midnight) */
static ray_t* ray_time_clock(ray_t* arg) {
    bool local = !is_global_arg(arg);
    time_t now = time(NULL);
    struct tm* t = local ? localtime(&now) : gmtime(&now);
    if (!t) return ray_error("domain", "time: failed to get current time");

    int32_t ms = t->tm_hour * 3600000 + t->tm_min * 60000 + t->tm_sec * 1000;
    return ray_time((int64_t)ms);
}

/* (timestamp 'local) or (timestamp 'global) — returns current timestamp (ns since 2000.01.01) */
static ray_t* ray_timestamp_clock(ray_t* arg) {
    bool local = !is_global_arg(arg);
    time_t now = time(NULL);
    struct tm* t = local ? localtime(&now) : gmtime(&now);
    if (!t) return ray_error("domain", "timestamp: failed to get current time");

    int64_t secs;
    if (!local) {
        secs = now - ray_epoch_offset();
    } else {
        /* For local, compute offset from rayforce epoch in local terms */
        struct tm lt = *t;
        lt.tm_isdst = -1;
        secs = mktime(&lt) - ray_epoch_offset();
    }

    int64_t nanos = secs * 1000000000LL;
    return ray_timestamp(nanos);
}

/* ══════════════════════════════════════════
 * Eval, parse, print, system, env builtins
 * ══════════════════════════════════════════ */

/* (eval expr) — evaluate a parsed expression */
static ray_t* ray_eval_builtin(ray_t* x) {
    return ray_eval(x);
}

/* (parse str) — parse a string into an AST */
static ray_t* ray_parse_builtin(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "parse expects a string");
    const char* src = ray_str_ptr(x);
    if (!src) return ray_error("domain", NULL);
    ray_t* parsed = ray_parse(src);
    return parsed ? parsed : ray_error("parse", NULL);
}

/* (print val) — print without newline, return the value */
static ray_t* ray_print_fn(ray_t* x) {
    ray_fmt_print(stdout, x, 0);
    fflush(stdout);
    return x;
}

/* (meta x) — return metadata about an object as a dict */
static ray_t* ray_meta_fn(ray_t* x) {
    if (!x) return ray_error("type", NULL);

    const char* tname = type_sym_name(x->type);
    int64_t type_sym = ray_sym_intern("type", 4);
    int64_t type_id  = ray_sym_intern(tname, strlen(tname));

    if (ray_is_atom(x)) {
        /* Atom: return {type: <typename>} */
        ray_t* dict = ray_list_new(2);
        if (RAY_IS_ERR(dict)) return dict;
        dict->attrs |= RAY_ATTR_DICT;
        ray_t* k = ray_sym(type_sym);
        dict = ray_list_append(dict, k); ray_release(k);
        ray_t* tv = ray_sym(type_id);
        dict = ray_list_append(dict, tv); ray_release(tv);
        return dict;
    }

    /* Vector/table/list: return {type: <typename>, len: <n>} */
    ray_t* dict = ray_list_new(4);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    ray_t* k1 = ray_sym(type_sym);
    dict = ray_list_append(dict, k1); ray_release(k1);
    if (x->type == RAY_LIST && (x->attrs & RAY_ATTR_DICT)) {
        int64_t did = ray_sym_intern("DICT", 4);
        ray_t* tv = ray_sym(did);
        dict = ray_list_append(dict, tv); ray_release(tv);
    } else {
        ray_t* tv = ray_sym(type_id);
        dict = ray_list_append(dict, tv); ray_release(tv);
    }

    int64_t len_sym = ray_sym_intern("len", 3);
    ray_t* k2 = ray_sym(len_sym);
    dict = ray_list_append(dict, k2); ray_release(k2);
    ray_t* lv = make_i64(x->len);
    dict = ray_list_append(dict, lv); ray_release(lv);

    return dict;
}

/* (gc) — no-op garbage collection trigger, return 0 */
static ray_t* ray_gc_fn(ray_t* x) { (void)x; return ray_i64(0); }

/* (system cmd) — run shell command, return exit code */
static ray_t* ray_system_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "system expects a string");
    const char* cmd = ray_str_ptr(x);
    if (!cmd) return ray_error("domain", NULL);
    int rc = system(cmd);
    return make_i64(rc);
}

/* (getenv name) — get environment variable */
static ray_t* ray_getenv_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "getenv expects a string");
    const char* name = ray_str_ptr(x);
    if (!name) return ray_error("domain", NULL);
    const char* val = getenv(name);
    return val ? ray_str(val, strlen(val)) : ray_str("", 0);
}

/* (setenv name val) — set environment variable */
#if !defined(_WIN32)
extern int setenv(const char*, const char*, int);
#endif
static ray_t* ray_setenv_fn(ray_t* name, ray_t* val) {
    if (name->type != -RAY_STR || val->type != -RAY_STR)
        return ray_error("type", "setenv expects two strings");
    const char* n = ray_str_ptr(name);
    const char* v = ray_str_ptr(val);
    if (!n || !v) return ray_error("domain", NULL);
#if defined(_WIN32)
    _putenv_s(n, v);
#else
    setenv(n, v, 1);
#endif
    return val;
}

/* ══════════════════════════════════════════
 * New builtins: quote, return, args, rc, diverse, get, remove, row, timer, env,
 * fold-left, fold-right, scan-left, scan-right
 * ══════════════════════════════════════════ */

/* (quote expr) — special form, returns argument unevaluated */
static ray_t* ray_quote_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", "quote expects 1 argument");
    ray_retain(args[0]);
    return args[0];
}

/* (return x) — early return from function (identity in Rayfall) */
static ray_t* ray_return_fn(ray_t* x) {
    ray_retain(x);
    return x;
}

/* (args) — return command-line arguments as a list of strings */
static ray_t* ray_args_fn(ray_t* x) {
    (void)x;
    /* Return empty list — CLI args not wired into eval context */
    ray_t* list = ray_list_new(0);
    if (!list) return ray_error("oom", NULL);
    return list;
}

/* (rc x) — return reference count of object */
static ray_t* ray_rc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return make_i64(0);
    return make_i64((int64_t)x->rc);
}

/* (diverse x) — check if all elements in a collection are unique */
static ray_t* ray_diverse_fn(ray_t* x) {
    if (ray_is_atom(x)) return make_bool(1);
    if (!is_collection(x)) return ray_error("type", "diverse expects a collection");

    int64_t n = ray_len(x);
    if (n <= 1) return make_bool(1);

    ray_t* d = ray_distinct_fn(x);
    if (RAY_IS_ERR(d)) return d;
    int64_t dn = ray_len(d);
    ray_release(d);
    return make_bool(dn == n ? 1 : 0);
}

/* (get dict key) — dictionary/table lookup (alias for at) */
static ray_t* ray_get_fn(ray_t* dict, ray_t* key) {
    return ray_at(dict, key);
}

/* (remove dict key) — remove key from dict, return new dict */
static ray_t* ray_remove_fn(ray_t* dict, ray_t* key) {
    if (dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return ray_error("type", "remove expects a dict");
    if (key->type != -RAY_SYM)
        return ray_error("type", "remove key must be a symbol");

    ray_t** items = (ray_t**)ray_data(dict);
    int64_t n = dict->len;
    ray_t* result = ray_list_new(0);
    if (RAY_IS_ERR(result)) return result;
    result->attrs |= RAY_ATTR_DICT;

    for (int64_t i = 0; i < n; i += 2) {
        if (items[i]->type == -RAY_SYM && items[i]->i64 == key->i64)
            continue; /* skip this key-value pair */
        result = ray_list_append(result, items[i]);
        if (RAY_IS_ERR(result)) return result;
        if (i + 1 < n) {
            result = ray_list_append(result, items[i + 1]);
            if (RAY_IS_ERR(result)) return result;
        }
    }
    return result;
}

/* (row table idx) — extract a single row from a table as a dict */
static ray_t* ray_row_fn(ray_t* tbl, ray_t* idx) {
    if (tbl->type != RAY_TABLE) return ray_error("type", "row expects a table");
    if (!is_numeric(idx)) return ray_error("type", "row index must be integer");
    /* Delegate to at — it already handles table integer indexing */
    return ray_at(tbl, idx);
}

/* (timer) — return high-res timestamp in nanoseconds for benchmarking */
static ray_t* ray_timer_fn(ray_t* x) {
    (void)x;
    clock_t t = clock();
    int64_t nanos = (int64_t)((double)t / (double)CLOCKS_PER_SEC * 1e9);
    return make_i64(nanos);
}

/* (env) — return dict of all global environment bindings */
static ray_t* ray_env_fn(ray_t* x) {
    (void)x;
    int64_t sym_ids[1024];
    ray_t* vals[1024];
    int32_t count = ray_env_list(sym_ids, vals, 1024);

    ray_t* dict = ray_list_new(0);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    for (int32_t i = 0; i < count; i++) {
        ray_t* k = ray_sym(sym_ids[i]);
        if (RAY_IS_ERR(k)) { ray_release(dict); return k; }
        dict = ray_list_append(dict, k);
        ray_release(k);
        if (RAY_IS_ERR(dict)) return dict;
        ray_retain(vals[i]);
        dict = ray_list_append(dict, vals[i]);
        if (RAY_IS_ERR(dict)) { ray_release(vals[i]); return dict; }
    }
    return dict;
}

/* (fold-left fn init coll) — left fold with explicit initial value */
static ray_t* ray_fold_left(ray_t** args, int64_t n) {
    /* Same as (fold fn init coll) — fold already goes left-to-right */
    return ray_fold(args, n);
}

/* (fold-right fn init coll) — right fold */
static ray_t* ray_fold_right(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* acc;
    ray_t* vec;

    if (n == 2) {
        /* (fold-right fn vec) — use last element as initial value */
        vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
        int64_t len = ray_len(vec);
        if (len == 0) { if (_bx) ray_release(_bx); return ray_error("domain", NULL); }
        ray_t** elems = (ray_t**)ray_data(vec);
        ray_retain(elems[len - 1]);
        acc = elems[len - 1];
        for (int64_t i = len - 2; i >= 0; i--) {
            ray_t* next = call_fn2(fn, elems[i], acc);
            ray_release(acc);
            if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
            acc = next;
        }
        if (_bx) ray_release(_bx);
        return acc;
    }

    /* (fold-right fn init coll) */
    ray_retain(args[1]);
    acc = args[1];
    vec = unbox_vec_arg(args[2], &_bx);
    if (RAY_IS_ERR(vec)) { ray_release(acc); return vec; }
    if (!is_list(vec)) { ray_release(acc); if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = len - 1; i >= 0; i--) {
        ray_t* next = call_fn2(fn, elems[i], acc);
        ray_release(acc);
        if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
        acc = next;
    }
    if (_bx) ray_release(_bx);
    return acc;
}

/* (scan-left fn vec) — running left fold (same as scan) */
static ray_t* ray_scan_left(ray_t** args, int64_t n) {
    return ray_scan_fn(args, n);
}

/* (scan-right fn vec) — running right fold, returns vector of partial results */
static ray_t* ray_scan_right(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    if (len == 0) {
        if (_bx) ray_release(_bx);
        ray_t* result = ray_alloc(0);
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 0;
        return result;
    }

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);

    ray_retain(elems[len - 1]);
    out[len - 1] = elems[len - 1];
    for (int64_t i = len - 2; i >= 0; i--) {
        out[i] = call_fn2(fn, elems[i], out[i + 1]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = i + 1; j < len; j++) ray_release(out[j]);
            ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* ══════════════════════════════════════════
 * del, internals, memstat, modify, pivot,
 * sysinfo, unify, xrank builtins
 * ══════════════════════════════════════════ */

/* (del name) — delete variable from environment (special form, unevaluated arg) */
static ray_t* ray_del_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("arity", "del expects 1 argument");
    ray_t* name = args[0];
    if (name->type != -RAY_SYM)
        return ray_error("type", "del expects a symbol");
    ray_env_set(name->i64, NULL);
    return ray_i64(0);
}

/* (internals) — return dict with internal build information */
static ray_t* ray_internals_fn(ray_t* x) {
    (void)x;
    ray_t* dict = ray_list_new(4);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    int64_t ver_sym = ray_sym_intern("version", 7);
    ray_t* k1 = ray_sym(ver_sym);
    dict = ray_list_append(dict, k1); ray_release(k1);
#ifdef RAYFORCE_VERSION
    ray_t* v1 = ray_str(RAYFORCE_VERSION, strlen(RAYFORCE_VERSION));
#else
    ray_t* v1 = ray_str("unknown", 7);
#endif
    dict = ray_list_append(dict, v1); ray_release(v1);

    int64_t date_sym = ray_sym_intern("build-date", 10);
    ray_t* k2 = ray_sym(date_sym);
    dict = ray_list_append(dict, k2); ray_release(k2);
#ifdef RAYFORCE_BUILD_DATE
    ray_t* v2 = ray_str(RAYFORCE_BUILD_DATE, strlen(RAYFORCE_BUILD_DATE));
#else
    ray_t* v2 = ray_str("unknown", 7);
#endif
    dict = ray_list_append(dict, v2); ray_release(v2);

    return dict;
}

/* (memstat) — return dict with memory allocator statistics */
static ray_t* ray_memstat_fn(ray_t* x) {
    (void)x;
    ray_mem_stats_t st;
    ray_mem_stats(&st);

    ray_t* dict = ray_list_new(10);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    /* alloc-count */
    int64_t s1 = ray_sym_intern("alloc-count", 11);
    ray_t* k1 = ray_sym(s1); dict = ray_list_append(dict, k1); ray_release(k1);
    ray_t* v1 = make_i64((int64_t)st.alloc_count);
    dict = ray_list_append(dict, v1); ray_release(v1);

    /* bytes-allocated */
    int64_t s2 = ray_sym_intern("bytes-allocated", 15);
    ray_t* k2 = ray_sym(s2); dict = ray_list_append(dict, k2); ray_release(k2);
    ray_t* v2 = make_i64((int64_t)st.bytes_allocated);
    dict = ray_list_append(dict, v2); ray_release(v2);

    /* peak-bytes */
    int64_t s3 = ray_sym_intern("peak-bytes", 10);
    ray_t* k3 = ray_sym(s3); dict = ray_list_append(dict, k3); ray_release(k3);
    ray_t* v3 = make_i64((int64_t)st.peak_bytes);
    dict = ray_list_append(dict, v3); ray_release(v3);

    /* slab-hits */
    int64_t s4 = ray_sym_intern("slab-hits", 9);
    ray_t* k4 = ray_sym(s4); dict = ray_list_append(dict, k4); ray_release(k4);
    ray_t* v4 = make_i64((int64_t)st.slab_hits);
    dict = ray_list_append(dict, v4); ray_release(v4);

    /* sys-current */
    int64_t s5 = ray_sym_intern("sys-current", 11);
    ray_t* k5 = ray_sym(s5); dict = ray_list_append(dict, k5); ray_release(k5);
    ray_t* v5 = make_i64((int64_t)st.sys_current);
    dict = ray_list_append(dict, v5); ray_release(v5);

    return dict;
}

/* (modify tbl col_name fn) — apply fn to the named column, return new table */
static ray_t* ray_modify_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("arity", "modify expects 3 arguments: table, column, function");
    ray_t* tbl = args[0];
    ray_t* col_name = args[1];
    ray_t* fn = args[2];

    if (tbl->type != RAY_TABLE)
        return ray_error("type", "modify: first arg must be a table");
    if (col_name->type != -RAY_SYM)
        return ray_error("type", "modify: second arg must be a symbol");

    int64_t target_sym = col_name->i64;
    ray_t* col = ray_table_get_col(tbl, target_sym);
    if (!col) return ray_error("domain", "modify: column not found");

    /* Apply fn to the entire column vector (atomic fns will map element-wise) */
    ray_t* new_col = call_fn1(fn, col);
    if (RAY_IS_ERR(new_col)) return new_col;

    /* Build new table: copy all columns, replacing the target */
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(new_col); return result; }

    for (int64_t i = 0; i < ncols; i++) {
        int64_t cname = ray_table_col_name(tbl, i);
        ray_t* cvec = (cname == target_sym) ? new_col : ray_table_get_col_idx(tbl, i);
        result = ray_table_add_col(result, cname, cvec);
        if (RAY_IS_ERR(result)) { ray_release(new_col); return result; }
    }
    ray_release(new_col);
    return result;
}

/* Convert a scalar value to a symbol ID for use as a column name.
 * SYM atoms return their ID directly; others are formatted as strings. */
/* Map a RAY_UNARY agg function pointer to a DAG opcode.
 * Returns 0 if the function is not a known aggregation builtin. */
static uint16_t pivot_fn_to_agg_op(ray_t* fn) {
    if (fn->type != RAY_UNARY) return 0;
    ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
    if (f == ray_sum_fn)   return OP_SUM;
    if (f == ray_avg_fn)   return OP_AVG;
    if (f == ray_min)      return OP_MIN;
    if (f == ray_max)      return OP_MAX;
    if (f == ray_count_fn) return OP_COUNT;
    if (f == ray_first_fn) return OP_FIRST;
    if (f == ray_last_fn)  return OP_LAST;
    return 0;
}

/* (pivot table index_col pivot_col value_col agg_fn) — pivot table */
static ray_t* ray_pivot_fn(ray_t** args, int64_t n) {
    if (n != 5) return ray_error("arity", "pivot expects 5 arguments: table, index, pivot-col, value-col, agg-fn");
    ray_t* tbl            = args[0];
    ray_t* index_arg      = args[1];   /* sym atom or list of syms */
    ray_t* pivot_col_name = args[2];   /* sym atom */
    ray_t* value_col_name = args[3];   /* sym atom */
    ray_t* agg_fn         = args[4];   /* function */

    if (tbl->type != RAY_TABLE)
        return ray_error("type", "pivot: first argument must be a table");
    if (pivot_col_name->type != -RAY_SYM)
        return ray_error("type", "pivot: pivot-col must be a symbol");
    if (value_col_name->type != -RAY_SYM)
        return ray_error("type", "pivot: value-col must be a symbol");
    if (agg_fn->type != RAY_UNARY && agg_fn->type != RAY_LAMBDA &&
        agg_fn->type != RAY_VARY)
        return ray_error("type", "pivot: agg-fn must be a function");

    /* Determine index columns */
    int64_t idx_syms[16];
    int64_t n_idx = 0;
    if (index_arg->type == -RAY_SYM) {
        idx_syms[0] = index_arg->i64;
        n_idx = 1;
    } else if (index_arg->type == RAY_LIST || ray_is_vec(index_arg)) {
        int64_t len = ray_len(index_arg);
        if (len > 16) return ray_error("limit", "pivot: too many index columns");
        for (int64_t i = 0; i < len; i++) {
            int alloc = 0;
            ray_t* elem = collection_elem(index_arg, i, &alloc);
            if (RAY_IS_ERR(elem)) return elem;
            if (elem->type != -RAY_SYM) {
                if (alloc) ray_release(elem);
                return ray_error("type", "pivot: index columns must be symbols");
            }
            idx_syms[i] = elem->i64;
            if (alloc) ray_release(elem);
        }
        n_idx = len;
    } else {
        return ray_error("type", "pivot: index must be a symbol or list of symbols");
    }

    /* Get pivot column, value column */
    ray_t* pcol = ray_table_get_col(tbl, pivot_col_name->i64);
    if (!pcol) return ray_error("domain", "pivot: pivot column not found");
    ray_t* vcol = ray_table_get_col(tbl, value_col_name->i64);
    if (!vcol) return ray_error("domain", "pivot: value column not found");

    /* Get index columns */
    ray_t* icols[16];
    for (int64_t i = 0; i < n_idx; i++) {
        icols[i] = ray_table_get_col(tbl, idx_syms[i]);
        if (!icols[i]) return ray_error("domain", "pivot: index column not found");
    }

    int64_t nrows = ray_table_nrows(tbl);
    if (nrows == 0) return ray_table_new(0);

    /* DAG fast path: known agg builtins on hashable columns → OP_PIVOT */
    uint16_t agg_op = pivot_fn_to_agg_op(agg_fn);
    bool dag_ok = (agg_op != 0 && pcol->type != RAY_STR && vcol->type != RAY_STR);
    for (int64_t i = 0; i < n_idx && dag_ok; i++)
        if (icols[i]->type == RAY_STR) dag_ok = false;

    if (dag_ok) {
        ray_graph_t* g = ray_graph_new(tbl);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* idx_ops[16];
        bool ok = true;
        for (int64_t i = 0; i < n_idx && ok; i++) {
            ray_t* s = ray_sym_str(idx_syms[i]);
            idx_ops[i] = s ? ray_scan(g, ray_str_ptr(s)) : NULL;
            if (!idx_ops[i]) ok = false;
        }
        ray_t* ps = ray_sym_str(pivot_col_name->i64);
        ray_t* vs = ray_sym_str(value_col_name->i64);
        ray_op_t* p_op = (ps && ok) ? ray_scan(g, ray_str_ptr(ps)) : NULL;
        ray_op_t* v_op = (vs && p_op) ? ray_scan(g, ray_str_ptr(vs)) : NULL;
        if (v_op) {
            ray_op_t* root = ray_pivot_op(g, idx_ops, (uint8_t)n_idx, p_op, v_op, agg_op);
            if (root) {
                ray_t* result = ray_execute(g, root);
                ray_graph_free(g);
                return result;
            }
        }
        ray_graph_free(g);
    }

    /* Generic fallback: use OP_GROUP DAG to group by (index_cols, pivot_col),
     * then apply agg_fn per group and unstack.  Single O(n) hash pass. */

    /* Build GROUP BY (idx0, ..., idxN-1, pivot_col) with COUNT agg via DAG */
    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) return ray_error("oom", NULL);

    uint8_t n_keys = (uint8_t)(n_idx + 1);
    ray_op_t* key_ops[16];
    bool ok = true;
    for (int64_t i = 0; i < n_idx && ok; i++) {
        ray_t* s = ray_sym_str(idx_syms[i]);
        key_ops[i] = s ? ray_scan(g, ray_str_ptr(s)) : NULL;
        if (!key_ops[i]) ok = false;
    }
    {
        ray_t* ps = ray_sym_str(pivot_col_name->i64);
        key_ops[n_idx] = (ps && ok) ? ray_scan(g, ray_str_ptr(ps)) : NULL;
        if (!key_ops[n_idx]) ok = false;
    }
    /* Value column scan for COUNT (just need a column ref for group) */
    ray_t* vs = ray_sym_str(value_col_name->i64);
    ray_op_t* val_scan = (vs && ok) ? ray_scan(g, ray_str_ptr(vs)) : NULL;
    if (!val_scan) { ray_graph_free(g); return ray_error("domain", "pivot: failed to build DAG"); }

    uint16_t grp_agg_ops[1] = { OP_COUNT };
    ray_op_t* grp_agg_ins[1] = { val_scan };
    ray_op_t* grp_root = ray_group(g, key_ops, n_keys, grp_agg_ops, grp_agg_ins, 1);
    if (!grp_root) { ray_graph_free(g); return ray_error("oom", NULL); }

    ray_t* grouped = ray_execute(g, grp_root);
    ray_graph_free(g);
    if (!grouped || RAY_IS_ERR(grouped)) return grouped;

    /* `grouped` is a table: (idx0, ..., idxN-1, pivot_col, _count).
     * Each row is one (index, pivot) combination.
     * Now for each group, gather the value column subset and apply agg_fn. */
    int64_t n_grps = ray_table_nrows(grouped);

    /* Get grouped columns */
    ray_t* g_icols[16];
    for (int64_t i = 0; i < n_idx; i++)
        g_icols[i] = ray_table_get_col(grouped, idx_syms[i]);
    ray_t* g_pcol = ray_table_get_col(grouped, pivot_col_name->i64);

    /* Collect distinct pivot values and index keys from grouped table */
    ray_retain(g_pcol);
    ray_t* dvals = ray_distinct_fn(g_pcol);
    ray_release(g_pcol);
    if (RAY_IS_ERR(dvals)) { ray_release(grouped); return dvals; }
    int64_t n_pv = ray_len(dvals);

    /* Distinct index keys: use grouped table's index columns (already unique rows) */
    /* Build index→row mapping via the original table:
     * For each group row, find which original rows belong to it. */

    /* Re-scan original table to assign rows to groups.
     * Build group_id[r] for each original row r. */
    ray_t* gid_vec = ray_vec_new(RAY_I64, nrows);
    if (!gid_vec || RAY_IS_ERR(gid_vec)) { ray_release(dvals); ray_release(grouped); return ray_error("oom", NULL); }
    gid_vec->len = nrows;
    int64_t* gids = (int64_t*)ray_data(gid_vec);

    for (int64_t r = 0; r < nrows; r++) {
        /* Find which group this row belongs to by matching keys */
        gids[r] = -1;
        for (int64_t gi = 0; gi < n_grps; gi++) {
            bool match = true;
            for (int64_t ci = 0; ci < n_idx && match; ci++) {
                int a1 = 0, a2 = 0;
                ray_t* v1 = collection_elem(icols[ci], r, &a1);
                ray_t* v2 = collection_elem(g_icols[ci], gi, &a2);
                if (!atom_eq(v1, v2)) match = false;
                if (a1) ray_release(v1);
                if (a2) ray_release(v2);
            }
            if (match) {
                int a1 = 0, a2 = 0;
                ray_t* v1 = collection_elem(pcol, r, &a1);
                ray_t* v2 = collection_elem(g_pcol, gi, &a2);
                if (!atom_eq(v1, v2)) match = false;
                if (a1) ray_release(v1);
                if (a2) ray_release(v2);
            }
            if (match) { gids[r] = gi; break; }
        }
    }

    /* For each group, gather the value column subset and apply agg_fn */
    ray_t* agg_results = ray_alloc(n_grps * sizeof(ray_t*));
    if (!agg_results) { ray_release(gid_vec); ray_release(dvals); ray_release(grouped); return ray_error("oom", NULL); }
    agg_results->type = RAY_LIST;
    agg_results->len = n_grps;
    ray_t** ar = (ray_t**)ray_data(agg_results);

    for (int64_t gi = 0; gi < n_grps; gi++) {
        /* Count rows in this group */
        int64_t cnt = 0;
        for (int64_t r = 0; r < nrows; r++) if (gids[r] == gi) cnt++;

        /* Gather indices */
        ray_t* idx_block = ray_alloc(cnt * sizeof(int64_t));
        int64_t* idx_data = (int64_t*)ray_data(idx_block);
        int64_t pos = 0;
        for (int64_t r = 0; r < nrows; r++) if (gids[r] == gi) idx_data[pos++] = r;

        ray_t* subset = gather_by_idx(vcol, idx_data, cnt);
        ray_free(idx_block);
        if (RAY_IS_ERR(subset)) {
            for (int64_t j = 0; j < gi; j++) ray_release(ar[j]);
            ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
            return subset;
        }

        ray_t* agg_val = call_fn1(agg_fn, subset);
        ray_release(subset);
        if (RAY_IS_ERR(agg_val)) {
            for (int64_t j = 0; j < gi; j++) ray_release(ar[j]);
            ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
            return agg_val;
        }
        ar[gi] = agg_val;
    }
    ray_release(gid_vec);

    /* Unstack: collect distinct index keys, build wide result.
     * Map each group to (ix_idx, pv_idx). */
    ray_t* ix_list = ray_list_new(16);
    ray_t* gmap = ray_alloc(n_grps * 2 * sizeof(int64_t));
    int64_t* gm_ix = (int64_t*)ray_data(gmap);
    int64_t* gm_pv = gm_ix + n_grps;

    for (int64_t gi = 0; gi < n_grps; gi++) {
        /* Find pivot index */
        int a1 = 0;
        ray_t* pv = collection_elem(g_pcol, gi, &a1);
        gm_pv[gi] = -1;
        for (int64_t p = 0; p < n_pv; p++) {
            int a2 = 0;
            ray_t* dv = collection_elem(dvals, p, &a2);
            bool eq = atom_eq(pv, dv);
            if (a2) ray_release(dv);
            if (eq) { gm_pv[gi] = p; break; }
        }
        if (a1) ray_release(pv);

        /* Find or insert index key */
        gm_ix[gi] = -1;
        int64_t n_ix = ray_len(ix_list);
        ray_t** ix_items = (ray_t**)ray_data(ix_list);
        for (int64_t j = 0; j < n_ix; j++) {
            ray_t** ex = (ray_t**)ray_data(ix_items[j]);
            bool match = true;
            for (int64_t ci = 0; ci < n_idx && match; ci++) {
                int a2 = 0;
                ray_t* v = collection_elem(g_icols[ci], gi, &a2);
                if (!atom_eq(ex[ci], v)) match = false;
                if (a2) ray_release(v);
            }
            if (match) { gm_ix[gi] = j; break; }
        }
        if (gm_ix[gi] < 0) {
            gm_ix[gi] = ray_len(ix_list);
            ray_t* tup = ray_list_new((int32_t)n_idx);
            for (int64_t ci = 0; ci < n_idx; ci++) {
                int a2 = 0;
                ray_t* v = collection_elem(g_icols[ci], gi, &a2);
                if (!a2) ray_retain(v);
                tup = ray_list_append(tup, v);
                ray_release(v);
            }
            ix_list = ray_list_append(ix_list, tup);
            ray_release(tup);
        }
    }

    int64_t n_ix = ray_len(ix_list);

    /* Build result table */
    ray_t* result = ray_table_new(n_idx + n_pv);
    if (RAY_IS_ERR(result)) goto fb_cleanup;

    /* Index columns */
    { ray_t** ix_items = (ray_t**)ray_data(ix_list);
    for (int64_t ci = 0; ci < n_idx; ci++) {
        ray_t* col_vals = ray_list_new((int32_t)n_ix);
        for (int64_t r = 0; r < n_ix; r++) {
            ray_t* v = ((ray_t**)ray_data(ix_items[r]))[ci];
            ray_retain(v);
            col_vals = ray_list_append(col_vals, v);
            ray_release(v);
        }
        ray_t* col_vec = list_to_typed_vec(col_vals, icols[ci]->type);
        if (RAY_IS_ERR(col_vec)) { ray_release(result); result = col_vec; goto fb_cleanup; }
        result = ray_table_add_col(result, idx_syms[ci], col_vec);
        ray_release(col_vec);
        if (RAY_IS_ERR(result)) goto fb_cleanup;
    }
    }

    /* Value columns */
    for (int64_t p = 0; p < n_pv; p++) {
        ray_t* col_vals = ray_list_new((int32_t)n_ix);
        for (int64_t r = 0; r < n_ix; r++) {
            ray_t* zero = ray_i64(0);
            col_vals = ray_list_append(col_vals, zero);
            ray_release(zero);
        }

        for (int64_t gi = 0; gi < n_grps; gi++) {
            if (gm_pv[gi] != p) continue;
            ray_t** cv = (ray_t**)ray_data(col_vals);
            ray_release(cv[gm_ix[gi]]);
            ray_retain(ar[gi]);
            cv[gm_ix[gi]] = ar[gi];
        }

        int8_t agg_type = RAY_I64;
        { ray_t** cv = (ray_t**)ray_data(col_vals);
          for (int64_t r = 0; r < n_ix; r++)
            if (cv[r]->type == -RAY_F64) { agg_type = RAY_F64; break; }
        }
        ray_t* agg_vec = list_to_typed_vec(col_vals, agg_type);
        if (RAY_IS_ERR(agg_vec)) { ray_release(result); result = agg_vec; goto fb_cleanup; }

        /* Column name */
        int a1 = 0;
        ray_t* pval = collection_elem(dvals, p, &a1);
        int64_t col_sym;
        if (pval->type == -RAY_SYM) {
            col_sym = pval->i64;
        } else if (pval->type == -RAY_I64) {
            char buf[64]; int len = snprintf(buf, sizeof(buf), "%ld", (long)pval->i64);
            col_sym = ray_sym_intern(buf, (size_t)len);
        } else if (pval->type == -RAY_F64) {
            char buf[64]; int len = snprintf(buf, sizeof(buf), "%g", pval->f64);
            col_sym = ray_sym_intern(buf, (size_t)len);
        } else if (pval->type == -RAY_BOOL) {
            col_sym = ray_sym_intern(pval->b8 ? "true" : "false", pval->b8 ? 4 : 5);
        } else {
            char buf[64]; int len = snprintf(buf, sizeof(buf), "col%ld", (long)pval->i64);
            col_sym = ray_sym_intern(buf, (size_t)len);
        }
        if (a1) ray_release(pval);

        result = ray_table_add_col(result, col_sym, agg_vec);
        ray_release(agg_vec);
        if (RAY_IS_ERR(result)) goto fb_cleanup;
    }

fb_cleanup:
    ray_free(gmap);
    ray_release(ix_list);
    for (int64_t gi = 0; gi < n_grps; gi++) ray_release(ar[gi]);
    ray_free(agg_results);
    ray_release(dvals);
    ray_release(grouped);
    return result;
}

/* (sysinfo) — return system information */
static ray_t* ray_sysinfo_fn(ray_t* x) {
    (void)x;
    ray_t* dict = ray_list_new(6);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

#if !defined(_WIN32)
    int64_t s1 = ray_sym_intern("cores", 5);
    ray_t* k1 = ray_sym(s1); dict = ray_list_append(dict, k1); ray_release(k1);
    ray_t* v1 = make_i64(sysconf(_SC_NPROCESSORS_ONLN));
    dict = ray_list_append(dict, v1); ray_release(v1);

    int64_t s2 = ray_sym_intern("page-size", 9);
    ray_t* k2 = ray_sym(s2); dict = ray_list_append(dict, k2); ray_release(k2);
    ray_t* v2 = make_i64(sysconf(_SC_PAGESIZE));
    dict = ray_list_append(dict, v2); ray_release(v2);

    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    int64_t s3 = ray_sym_intern("total-mem", 9);
    ray_t* k3 = ray_sym(s3); dict = ray_list_append(dict, k3); ray_release(k3);
    ray_t* v3 = make_i64((int64_t)pages * (int64_t)psize);
    dict = ray_list_append(dict, v3); ray_release(v3);
#else
    int64_t s1 = ray_sym_intern("cores", 5);
    ray_t* k1 = ray_sym(s1); dict = ray_list_append(dict, k1); ray_release(k1);
    ray_t* v1 = make_i64(1);
    dict = ray_list_append(dict, v1); ray_release(v1);
#endif

    return dict;
}

/* (unify a b) — return list of two vectors promoted to a common type */
static ray_t* ray_unify_fn(ray_t* a, ray_t* b) {
    /* Build a 2-element list containing both values */
    ray_t* result = ray_list_new(2);
    if (RAY_IS_ERR(result)) return result;

    if (a->type == b->type || ray_is_atom(a) || ray_is_atom(b)) {
        /* Same type or atoms: return as-is */
        ray_retain(a); ray_retain(b);
        result = ray_list_append(result, a); ray_release(a);
        result = ray_list_append(result, b); ray_release(b);
        return result;
    }

    /* Different vector types: attempt numeric promotion */
    /* For now: wrap both without conversion */
    ray_retain(a); ray_retain(b);
    result = ray_list_append(result, a); ray_release(a);
    result = ray_list_append(result, b); ray_release(b);
    return result;
}

/* (xrank n vec) — assign each element to one of n rank groups */
static ray_t* ray_xrank_fn(ray_t* n_obj, ray_t* vec) {
    if (!is_numeric(n_obj))
        return ray_error("type", "xrank: first arg must be integer");
    if (!ray_is_vec(vec))
        return ray_error("type", "xrank: second arg must be a vector");

    int64_t n_groups = as_i64(n_obj);
    int64_t len = ray_len(vec);
    if (n_groups <= 0 || len == 0) return ray_vec_new(RAY_I64, 0);

    /* Build index array and sort by value */
    int64_t* idx = (int64_t*)ray_sys_alloc(len * sizeof(int64_t));
    if (!idx) return ray_error("oom", NULL);
    for (int64_t i = 0; i < len; i++) idx[i] = i;

    /* Simple insertion sort on values (works for all numeric types) */
    for (int64_t i = 1; i < len; i++) {
        int64_t key = idx[i];
        ray_t* key_elem = ray_vec_get(vec, key);
        int64_t j = i - 1;
        while (j >= 0) {
            ray_t* cmp_elem = ray_vec_get(vec, idx[j]);
            double kv = key_elem ? (is_numeric(key_elem) ? (key_elem->type == -RAY_F64 ? key_elem->f64 : (double)as_i64(key_elem)) : 0.0) : 0.0;
            double cv = cmp_elem ? (is_numeric(cmp_elem) ? (cmp_elem->type == -RAY_F64 ? cmp_elem->f64 : (double)as_i64(cmp_elem)) : 0.0) : 0.0;
            if (cmp_elem) ray_release(cmp_elem);
            if (cv <= kv) break;
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
        if (key_elem) ray_release(key_elem);
    }

    /* Assign groups: element at sorted position i gets group (i * n_groups / len) */
    ray_t* result = ray_vec_new(RAY_I64, len);
    if (RAY_IS_ERR(result)) { ray_sys_free(idx); return result; }
    result->len = len;
    int64_t* out = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < len; i++) {
        out[idx[i]] = i * n_groups / len;
    }
    ray_sys_free(idx);
    return result;
}

/* ══════════════════════════════════════════
 * Builtin registration
 * ══════════════════════════════════════════ */

static void register_binary(const char* name, uint8_t attrs, ray_binary_fn fn) {
    int64_t sym = ray_sym_intern(name, strlen(name));
    ray_t* obj = ray_fn_binary(name, attrs, fn);
    ray_env_set(sym, obj);
    ray_release(obj);
}

/* Register binary with a DAG opcode for vectorized execution */
static void register_binary_op(const char* name, uint8_t attrs, ray_binary_fn fn, uint16_t opcode) {
    int64_t sym = ray_sym_intern(name, strlen(name));
    ray_t* obj = ray_fn_binary(name, attrs, fn);
    RAY_FN_SET_OPCODE(obj, opcode);
    ray_env_set(sym, obj);
    ray_release(obj);
}

static void register_unary(const char* name, uint8_t attrs, ray_unary_fn fn) {
    int64_t sym = ray_sym_intern(name, strlen(name));
    ray_t* obj = ray_fn_unary(name, attrs, fn);
    ray_env_set(sym, obj);
    ray_release(obj);
}

static void register_vary(const char* name, uint8_t attrs, ray_vary_fn fn) {
    int64_t sym = ray_sym_intern(name, strlen(name));
    ray_t* obj = ray_fn_vary(name, attrs, fn);
    ray_env_set(sym, obj);
    ray_release(obj);
}

static void ray_register_builtins(void) {
    register_binary_op("+",   RAY_FN_ATOMIC, ray_add_fn, OP_ADD);
    register_binary_op("-",   RAY_FN_ATOMIC, ray_sub_fn, OP_SUB);
    register_binary_op("*",   RAY_FN_ATOMIC, ray_mul_fn, OP_MUL);
    register_binary_op("/",   RAY_FN_ATOMIC, ray_div_fn, OP_DIV);
    register_binary_op("%",   RAY_FN_ATOMIC, ray_mod_fn, OP_MOD);
    register_binary_op(">",   RAY_FN_ATOMIC, ray_gt_fn,  OP_GT);
    register_binary_op("<",   RAY_FN_ATOMIC, ray_lt_fn,  OP_LT);
    register_binary_op(">=",  RAY_FN_ATOMIC, ray_gte,    OP_GE);
    register_binary_op("<=",  RAY_FN_ATOMIC, ray_lte,    OP_LE);
    register_binary_op("==",  RAY_FN_ATOMIC, ray_eq_fn,  OP_EQ);
    register_binary_op("!=",  RAY_FN_ATOMIC, ray_neq,    OP_NE);
    register_binary("and", RAY_FN_NONE,   ray_and_fn);
    register_binary("or",  RAY_FN_NONE,   ray_or_fn);
    register_unary("not",  RAY_FN_NONE,   ray_not_fn);
    register_unary("neg",   RAY_FN_ATOMIC, ray_neg_fn);
    register_unary("round", RAY_FN_ATOMIC, ray_round_fn);
    register_unary("floor", RAY_FN_ATOMIC, ray_floor_fn);
    register_unary("ceil",  RAY_FN_ATOMIC, ray_ceil_fn);

    /* Special forms */
    register_binary("set", RAY_FN_SPECIAL_FORM, ray_set);
    register_binary("let", RAY_FN_SPECIAL_FORM, ray_let);
    register_vary("if",    RAY_FN_SPECIAL_FORM, ray_cond);
    register_vary("do",    RAY_FN_SPECIAL_FORM, ray_do);
    register_vary("fn",    RAY_FN_SPECIAL_FORM, ray_fn);

    /* Aggregation builtins */
    register_unary("sum",   RAY_FN_AGGR, ray_sum_fn);
    register_unary("count", RAY_FN_AGGR, ray_count_fn);
    register_unary("avg",   RAY_FN_AGGR, ray_avg_fn);
    register_unary("min",   RAY_FN_AGGR, ray_min);
    register_unary("max",   RAY_FN_AGGR, ray_max);
    register_unary("first", RAY_FN_NONE, ray_first_fn);
    register_unary("last",  RAY_FN_NONE, ray_last_fn);
    register_unary("med",   RAY_FN_AGGR, ray_med);
    register_unary("dev",   RAY_FN_AGGR, ray_dev);

    /* Error handling */
    register_unary("raise", RAY_FN_NONE, ray_raise);
    register_binary("try",  RAY_FN_SPECIAL_FORM, ray_try);

    /* Higher-order functions */
    register_vary("map",    RAY_FN_NONE, ray_map);
    register_vary("pmap",   RAY_FN_NONE, ray_pmap);
    register_vary("fold",   RAY_FN_NONE, ray_fold);
    register_vary("scan",   RAY_FN_NONE, ray_scan_fn);
    register_binary("filter", RAY_FN_NONE, ray_filter_fn);
    register_vary("apply",  RAY_FN_NONE, ray_apply);

    /* Collection operations */
    register_unary("distinct", RAY_FN_NONE, ray_distinct_fn);
    register_binary("in",      RAY_FN_NONE, ray_in);
    register_binary("except",  RAY_FN_NONE, ray_except);
    register_binary("union",   RAY_FN_NONE, ray_union);
    register_binary("sect",    RAY_FN_NONE, ray_sect);
    register_binary("take",    RAY_FN_NONE, ray_take);
    register_binary("at",      RAY_FN_NONE, ray_at);
    register_binary("find",    RAY_FN_NONE, ray_find);
    register_unary("reverse",  RAY_FN_NONE, ray_reverse);
    register_unary("til",      RAY_FN_NONE, ray_til);

    /* Sorting operations */
    register_unary("asc",      RAY_FN_NONE, ray_asc_fn);
    register_unary("desc",     RAY_FN_NONE, ray_desc_fn);
    register_unary("iasc",     RAY_FN_NONE, ray_iasc_fn);
    register_unary("idesc",    RAY_FN_NONE, ray_idesc_fn);
    register_unary("rank",     RAY_FN_NONE, ray_rank_fn);
    register_binary("xasc",    RAY_FN_NONE, ray_xasc_fn);
    register_binary("xdesc",   RAY_FN_NONE, ray_xdesc_fn);

    /* Table operations */
    register_vary("list",      RAY_FN_NONE, ray_list);
    register_binary("table",   RAY_FN_NONE, ray_table);
    register_unary("key",      RAY_FN_NONE, ray_key);
    register_unary("value",    RAY_FN_NONE, ray_value);

    /* Query operations */
    register_vary("select",    RAY_FN_SPECIAL_FORM, ray_select_fn);
    register_vary("update",    RAY_FN_SPECIAL_FORM, ray_update);
    register_vary("insert",    RAY_FN_SPECIAL_FORM, ray_insert);
    register_vary("upsert",    RAY_FN_SPECIAL_FORM, ray_upsert);
    register_binary("xbar",    RAY_FN_ATOMIC, ray_xbar);

    /* Join operations */
    register_vary("left-join",   RAY_FN_NONE, ray_left_join);
    register_vary("inner-join",  RAY_FN_NONE, ray_inner_join);
    register_vary("window-join", RAY_FN_SPECIAL_FORM, ray_window_join);
    register_vary("window-join1", RAY_FN_SPECIAL_FORM, ray_window_join);
    register_vary("asof-join",   RAY_FN_NONE, ray_asof_join_fn);

    /* I/O builtins */
    register_vary("println",    RAY_FN_NONE, ray_println);
    register_vary("show",       RAY_FN_NONE, ray_show);
    register_vary("format",     RAY_FN_NONE, ray_format_fn);
    register_vary("read-csv",   RAY_FN_NONE, ray_read_csv_fn);
    register_vary("write-csv",  RAY_FN_NONE, ray_write_csv_fn);
    register_binary("as",       RAY_FN_NONE, ray_cast_fn);
    register_unary("type",      RAY_FN_NONE, ray_type_fn);
    register_unary("read",      RAY_FN_NONE, ray_read_file);
    register_binary("write",    RAY_FN_NONE, ray_write_file);
    register_unary("load",      RAY_FN_NONE, ray_load_file);
    register_unary("exit",      RAY_FN_NONE, ray_exit_fn);
    register_vary("resolve",    RAY_FN_SPECIAL_FORM, ray_resolve_fn);
    register_vary("timeit",     RAY_FN_SPECIAL_FORM, ray_timeit_fn);

    /* Additional builtins (ported from rayforce) */
    register_vary("enlist",     RAY_FN_NONE, ray_enlist);
    register_binary("dict",     RAY_FN_NONE, ray_dict_fn);
    register_unary("nil?",      RAY_FN_NONE, ray_nil_fn);
    register_unary("where",     RAY_FN_NONE, ray_where_fn);
    register_unary("group",     RAY_FN_NONE, ray_group_fn);
    register_binary("concat",   RAY_FN_NONE, ray_concat_fn);
    register_unary("raze",      RAY_FN_NONE, ray_raze_fn);
    register_binary("within",   RAY_FN_NONE, ray_within_fn);
    register_binary("div",      RAY_FN_ATOMIC, ray_fdiv_fn);
    register_binary("rand",     RAY_FN_NONE, ray_rand_fn);
    register_binary("bin",      RAY_FN_NONE, ray_bin_fn);
    register_binary("binr",     RAY_FN_NONE, ray_binr_fn);
    register_vary("map-left",   RAY_FN_NONE, ray_map_left);
    register_vary("map-right",  RAY_FN_NONE, ray_map_right);

    /* String operations */
    register_binary("split",     RAY_FN_NONE, ray_split_fn);

    /* Serialization stubs */
    register_unary("ser",        RAY_FN_NONE, ray_ser_fn);
    register_unary("de",         RAY_FN_NONE, ray_de_fn);

    /* GUID generation */
    register_unary("guid",       RAY_FN_NONE, ray_guid_fn);

    /* In-place mutation */
    register_vary("alter",       RAY_FN_SPECIAL_FORM, ray_alter_fn);

    /* Pattern matching */
    register_binary("like",      RAY_FN_NONE, ray_like_fn);

    /* Temporal clocks */
    register_unary("date",       RAY_FN_NONE, ray_date_clock);
    register_unary("time",       RAY_FN_NONE, ray_time_clock);
    register_unary("timestamp",  RAY_FN_NONE, ray_timestamp_clock);

    /* Eval, parse, print, meta */
    register_unary("eval",       RAY_FN_NONE, ray_eval_builtin);
    register_unary("parse",      RAY_FN_NONE, ray_parse_builtin);
    register_unary("print",      RAY_FN_NONE, ray_print_fn);
    register_unary("meta",       RAY_FN_NONE, ray_meta_fn);

    /* System builtins */
    register_unary("gc",         RAY_FN_NONE, ray_gc_fn);
    register_unary("system",     RAY_FN_NONE, ray_system_fn);
    register_unary("getenv",     RAY_FN_NONE, ray_getenv_fn);
    register_binary("setenv",    RAY_FN_NONE, ray_setenv_fn);
    register_unary("os-get-var", RAY_FN_NONE, ray_getenv_fn);
    register_binary("os-set-var", RAY_FN_NONE, ray_setenv_fn);

    /* quote — special form (unevaluated argument) */
    register_vary("quote",       RAY_FN_SPECIAL_FORM, ray_quote_fn);

    /* return — early return (identity) */
    register_unary("return",     RAY_FN_NONE, ray_return_fn);

    /* args — command line arguments */
    register_unary("args",       RAY_FN_NONE, ray_args_fn);

    /* rc — reference count */
    register_unary("rc",         RAY_FN_NONE, ray_rc_fn);

    /* diverse — check if all elements unique */
    register_unary("diverse",    RAY_FN_NONE, ray_diverse_fn);

    /* get — dictionary/table lookup (alias for at) */
    register_binary("get",       RAY_FN_NONE, ray_get_fn);

    /* remove — remove key from dict */
    register_binary("remove",    RAY_FN_NONE, ray_remove_fn);

    /* row — single row from table */
    register_binary("row",       RAY_FN_NONE, ray_row_fn);

    /* timer — high-res monotonic nanosecond timestamp */
    register_unary("timer",      RAY_FN_NONE, ray_timer_fn);

    /* env — list all global environment bindings */
    register_unary("env",        RAY_FN_NONE, ray_env_fn);

    /* Directional fold/scan variants */
    register_vary("fold-left",   RAY_FN_NONE, ray_fold_left);
    register_vary("fold-right",  RAY_FN_NONE, ray_fold_right);
    register_vary("scan-left",   RAY_FN_NONE, ray_scan_left);
    register_vary("scan-right",  RAY_FN_NONE, ray_scan_right);

    /* del, internals, memstat, modify, pivot, sysinfo, unify, xrank */
    register_vary("del",          RAY_FN_SPECIAL_FORM, ray_del_fn);
    register_unary("internals",   RAY_FN_NONE, ray_internals_fn);
    register_unary("memstat",     RAY_FN_NONE, ray_memstat_fn);
    register_vary("modify",      RAY_FN_NONE, ray_modify_fn);
    register_vary("pivot",       RAY_FN_NONE, ray_pivot_fn);
    register_unary("sysinfo",    RAY_FN_NONE, ray_sysinfo_fn);
    register_binary("unify",     RAY_FN_NONE, ray_unify_fn);
    register_binary("xrank",     RAY_FN_NONE, ray_xrank_fn);
}

/* ══════════════════════════════════════════
 * Runtime lifecycle
 * ══════════════════════════════════════════ */

ray_err_t ray_lang_init(void) {
    ray_err_t err = ray_env_init();
    if (err != RAY_OK) return err;
    ray_register_builtins();
    return RAY_OK;
}

void ray_lang_destroy(void) {
    if (__raise_val) { ray_release(__raise_val); __raise_val = NULL; }
    ray_env_destroy();
    ray_compile_reset();
}

/* ══════════════════════════════════════════
 * Tree-walking evaluator
 * ══════════════════════════════════════════ */

ray_t* ray_eval(ray_t* obj) {
    if (!obj || RAY_IS_ERR(obj)) return obj;

    /* Check for external interrupt (e.g. Ctrl-C from REPL) */
    if (g_eval_interrupted) return ray_error("limit", "interrupted");

    if (++eval_depth > RAY_EVAL_MAX_DEPTH) {
        eval_depth--;
        return ray_error("limit", "eval depth exceeded");
    }

    ray_t* ret;

    /* Atoms: return themselves (retain) */
    if (ray_is_atom(obj)) {
        /* Name reference: resolve from env */
        if (obj->type == -RAY_SYM && (obj->attrs & RAY_ATTR_NAME)) {
            /* Check for null keyword — compare by string, not cached sym_id,
             * because sym table may be reinitialized between test runs */
            {
                ray_t* name_str = ray_sym_str(obj->i64);
                if (name_str && ray_str_len(name_str) == 4 &&
                    memcmp(ray_str_ptr(name_str), "null", 4) == 0) {
                    ray_release(name_str);
                    ret = NULL; goto out;
                }
                if (name_str) ray_release(name_str);
            }

            ray_t* val = ray_env_get(obj->i64);
            if (!val) {
                ray_t* ns = ray_sym_str(obj->i64);
                if (ns) {
                    ret = ray_error("name", "'%.*s' undefined",
                                    (int)ray_str_len(ns), ray_str_ptr(ns));
                    ray_release(ns);
                } else {
                    ret = ray_error("name", NULL);
                }
                goto out;
            }
            ray_retain(val);
            ret = val; goto out;
        }
        ray_retain(obj);
        ret = obj; goto out;
    }

    /* Non-list vectors: return themselves */
    if (obj->type != RAY_LIST) { ray_retain(obj); ret = obj; goto out; }

    /* Empty list */
    if (ray_len(obj) == 0) { ray_retain(obj); ret = obj; goto out; }

    /* Dict literal: evaluate values, keep keys */
    if (obj->attrs & RAY_ATTR_DICT) {
        int64_t n2 = ray_len(obj);
        ray_t* dict = ray_alloc(n2 * sizeof(ray_t*));
        if (!dict) { ret = ray_error("oom", NULL); goto out; }
        dict->type = RAY_LIST;
        dict->attrs |= RAY_ATTR_DICT;
        dict->len = n2;
        ray_t** src = (ray_t**)ray_data(obj);
        ray_t** dst = (ray_t**)ray_data(dict);
        for (int64_t i = 0; i < n2; i += 2) {
            /* key: retain as-is */
            ray_retain(src[i]);
            dst[i] = src[i];
            /* value: evaluate */
            ray_t* v = ray_eval(src[i + 1]);
            if (v && RAY_IS_ERR(v)) {
                for (int64_t j = 0; j < i; j++) ray_release(dst[j]);
                ray_release(dst[i]);
                ray_release(dict);
                ret = v; goto out;
            }
            dst[i + 1] = v ? v : NULL;
        }
        ret = dict; goto out;
    }

    /* List: evaluate first element, dispatch by type */
    ray_t** elems = (ray_t**)ray_data(obj);
    ray_t* head = ray_eval(elems[0]);
    if (RAY_IS_ERR(head)) { ret = head; goto out; }

    int64_t n = ray_len(obj);

    switch (head->type) {
        case RAY_UNARY: {
            if (n < 2) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            ray_unary_fn fn = (ray_unary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            ray_t* arg = ray_eval(elems[1]);
            ray_release(head);
            if (arg && RAY_IS_ERR(arg)) { ret = arg; goto out; }
            ray_t* result;
            if (!arg) {
                /* Only nil? safely handles NULL — check by function pointer */
                result = (fn == (ray_unary_fn)ray_nil_fn || fn == (ray_unary_fn)ray_ser_fn) ? fn(NULL) : ray_error("type", NULL);
            } else if ((fn_attrs & RAY_FN_ATOMIC) && is_collection(arg))
                result = atomic_map_unary(fn, arg);
            else
                result = fn(arg);
            if (arg) ray_release(arg);
            ret = result; goto out;
        }
        case RAY_BINARY: {
            if (n < 3) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            ray_binary_fn fn = (ray_binary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            if (fn_attrs & RAY_FN_SPECIAL_FORM) {
                ray_release(head);
                ret = fn(elems[1], elems[2]); goto out;
            }
            ray_t* left = ray_eval(elems[1]);
            if (left && RAY_IS_ERR(left)) {
                ray_release(head);
                ret = left; goto out;
            }
            ray_t* right = ray_eval(elems[2]);
            if (right && RAY_IS_ERR(right)) {
                ray_release(head); if (left) ray_release(left);
                ret = right; goto out;
            }
            /* If either arg is NULL (null keyword), only == and != can handle it */
            if (!left || !right) {
                if (fn == (ray_binary_fn)ray_eq_fn || fn == (ray_binary_fn)ray_neq) {
                    ray_release(head);
                    ray_t* result = fn(left, right);
                    if (left) ray_release(left);
                    if (right) ray_release(right);
                    ret = result; goto out;
                }
                ray_release(head);
                if (left) ray_release(left);
                if (right) ray_release(right);
                ret = ray_error("type", NULL); goto out;
            }
            uint16_t fn_opcode = RAY_FN_OPCODE(head);
            ray_release(head);
            ray_t* result;
            if ((fn_attrs & RAY_FN_ATOMIC) && (is_collection(left) || is_collection(right)))
                result = atomic_map_binary_op(fn, fn_opcode, left, right);
            else
                result = fn(left, right);
            ray_release(left);
            ray_release(right);
            ret = result; goto out;
        }
        case RAY_VARY: {
            ray_vary_fn fn = (ray_vary_fn)(uintptr_t)head->i64;
            if (head->attrs & RAY_FN_SPECIAL_FORM) {
                ray_release(head);
                ret = fn(elems + 1, n - 1); goto out;
            }
            int64_t argc = n - 1;
            if (argc > 64) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            ray_t* args[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                if (!args[i] || RAY_IS_ERR(args[i])) {
                    ray_t* err = (!args[i]) ? ray_error("type", NULL) : args[i];
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = err; goto out;
                }
            }
            ray_release(head);
            ray_t* result = fn(args, argc);
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ret = result; goto out;
        }
        case RAY_LAMBDA: {
            int64_t argc = n - 1;
            if (argc > 64) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            ray_t* args[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                if (!args[i] || RAY_IS_ERR(args[i])) {
                    ray_t* err = (!args[i]) ? ray_error("type", NULL) : args[i];
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = err; goto out;
                }
            }
            ray_t* result = call_lambda(head, args, argc);
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ray_release(head);
            if (RAY_IS_ERR(result))
                add_eval_error_frame(g_eval_nfo, obj);
            ret = result; goto out;
        }
        default:
            ray_release(head);
            ret = ray_error("type", NULL); goto out;
    }

out:
    eval_depth--;
    return ret;
}

ray_t* ray_eval_str(const char* source) {
    ray_clear_error_trace();
    ray_t* nfo = ray_nfo_create("repl", 4, source, strlen(source));
    ray_t* parsed = ray_parse_with_nfo(source, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); return parsed; }

    ray_t* prev_nfo = g_eval_nfo;
    g_eval_nfo = nfo;
    ray_t* result = ray_eval(parsed);
    g_eval_nfo = prev_nfo;

    ray_release(parsed);
    ray_release(nfo);
    return result;
}
