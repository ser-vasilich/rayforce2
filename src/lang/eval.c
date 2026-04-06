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
#include "lang/eval_internal.h"
#include "lang/env.h"
#include "lang/nfo.h"
#include "lang/parse.h"
#include "core/types.h"
#include "io/csv.h"
#include "ops/ops.h"
#include "ops/datalog.h"
#include "table/sym.h"
#include "core/pool.h"
#include "core/profile.h"
#include "table/sym.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "lang/format.h"
/* store/serde.h, store/splay.h, store/part.h moved to system.c */
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

/* Convert a typed vector to a boxed list.  If already a list, retains
 * and returns it directly.  Caller owns the returned object. */
ray_t* to_boxed_list(ray_t* x) {
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
ray_t* unbox_vec_arg(ray_t* x, ray_t** _bx) {
    *_bx = NULL;
    if (x && !RAY_IS_ERR(x) && ray_is_vec(x)) {
        *_bx = to_boxed_list(x);
        return *_bx;
    }
    return x;
}

/* Map a binary function element-wise over collections.
 * Both args can be collections (zip-map) or one scalar (broadcast).
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise. */
ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right) {
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
ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg) {
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
 * Higher-order functions: map, pmap, fold, scan, filter, apply
 * ══════════════════════════════════════════ */

/* Helper: call a function object with 1 arg, returning result.
 * Handles UNARY, BINARY, LAMBDA types. Does not release fn or arg. */
ray_t* call_fn1(ray_t* fn, ray_t* arg) {
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
ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b) {
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


/* ══════════════════════════════════════════
 * Sorting builtins
 * ══════════════════════════════════════════ */

/* Reorder vector elements by an index array */
ray_t* gather_by_idx(ray_t* vec, int64_t* idx, int64_t n) {
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
/* (resolve tbl) — replace I64 columns with SYM columns where values are valid sym IDs.
 * This makes query results human-readable (sym names instead of intern IDs).
 * Also accepts (resolve db tbl) for compat — just ignores db. */
static ray_t* ray_resolve_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("arity", "resolve expects at least 1 argument");

    /* Evaluate all args */
    ray_t* tbl = NULL;
    if (n == 1) {
        tbl = ray_eval(args[0]);
    } else {
        /* (resolve db tbl) — ignore db, use tbl */
        ray_t* db = ray_eval(args[0]);
        if (db && !RAY_IS_ERR(db)) ray_release(db);
        tbl = ray_eval(args[1]);
    }
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", "resolve: null argument");

    /* Materialize lazy tables */
    if (ray_is_lazy(tbl)) {
        ray_t* mat = ray_lazy_materialize(tbl);
        ray_release(tbl);
        if (!mat || RAY_IS_ERR(mat)) return mat ? mat : ray_error("domain", "resolve: materialization failed");
        tbl = mat;
    }

    /* If not a table, return as-is (backward compat: resolve a variable name) */
    if (tbl->type != RAY_TABLE) {
        if (tbl->type == -RAY_SYM) {
            ray_t* val = ray_env_get(tbl->i64);
            ray_release(tbl);
            if (!val) return NULL;
            ray_retain(val);
            return val;
        }
        return tbl;
    }

    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    /* Build a new table replacing I64 columns with SYM columns where possible */
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        int64_t col_name = ray_table_col_name(tbl, c);
        if (!col) continue;

        if (col->type == RAY_I64) {
            /* Try to resolve: convert to SYM only if ALL positive values
             * are valid sym IDs. This avoids converting entity-ID columns
             * where values are plain integers that happen to collide with
             * low sym IDs. */
            int64_t* data = (int64_t*)ray_data(col);
            bool all_user_sym = (nrows > 0);
            /* Only convert if ALL values resolve to user-defined symbols
             * (length >= 2, not single-char operators). This distinguishes
             * symbol references (name='Alice') from entity IDs (e=1). */
            for (int64_t r = 0; r < nrows; r++) {
                if (data[r] <= 0) { all_user_sym = false; break; }
                ray_t* sn = ray_sym_str(data[r]);
                if (!sn) { all_user_sym = false; break; }
                size_t slen = ray_str_len(sn);
                const char* sp = ray_str_ptr(sn);
                /* Single-char or starts with digit/operator → not a user symbol */
                if (slen < 2 || (sp[0] >= '0' && sp[0] <= '9') ||
                    sp[0] == '+' || sp[0] == '-' || sp[0] == '*' || sp[0] == '/' ||
                    sp[0] == '<' || sp[0] == '>' || sp[0] == '=' || sp[0] == '!' ||
                    sp[0] == '?' || sp[0] == '_') {
                    all_user_sym = false; break;
                }
            }
            if (all_user_sym) {
                /* Convert to SYM column */
                ray_t* sym_col = ray_vec_new(RAY_SYM, nrows);
                if (RAY_IS_ERR(sym_col)) { ray_release(result); ray_release(tbl); return sym_col; }
                for (int64_t r = 0; r < nrows; r++) {
                    sym_col = ray_vec_append(sym_col, &data[r]);
                    if (RAY_IS_ERR(sym_col)) { ray_release(result); ray_release(tbl); return sym_col; }
                }
                result = ray_table_add_col(result, col_name, sym_col);
                ray_release(sym_col);
            } else {
                /* Keep as I64 */
                result = ray_table_add_col(result, col_name, col);
            }
        } else {
            /* Non-I64 column: keep as-is */
            result = ray_table_add_col(result, col_name, col);
        }
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    ray_release(tbl);
    return result;
}

/* (timeit expr) — evaluate expression and return time in ms as F64.
 * SPECIAL_FORM: does not pre-evaluate args. */
static ray_t* ray_timeit_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    int64_t t0 = ray_profile_now_ns();
    ray_t* result = ray_eval(args[0]);
    int64_t t1 = ray_profile_now_ns();
    if (result && !RAY_IS_ERR(result)) ray_release(result);
    double ms = (double)(t1 - t0) / 1e6;
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
/* type_sym_name moved to eval_internal.h */

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
ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc) {
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
ray_t* ray_enlist(ray_t** args, int64_t n) {
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
ray_t* ray_group_fn(ray_t* x) {
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
ray_t* ray_concat_fn(ray_t* a, ray_t* b) {
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


/* ray_split_fn moved to str_builtin.c */

/* str_to_cpath, ser/de, splay, parted, guid, eval_builtin, parse_builtin,
 * print, meta, gc, system, getenv, setenv, quote, return, args, rc,
 * diverse, get, remove, timer, env, internals, memstat, sysinfo
 * moved to system.c */

/* ray_datoms_fn, ray_assert_fact_fn, ray_retract_fact_fn, ray_scan_eav_fn,
 * ray_pull_fn, ray_rule_fn, ray_query_fn, ray_dl_program_fn, ray_dl_add_edb_fn,
 * ray_dl_stratify_fn, ray_dl_eval_fn, ray_dl_query_fn, ray_dl_provenance_fn
 * moved to datalog_builtin.c */


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
    register_binary("union-all",      RAY_FN_NONE, ray_union_all_fn);
    register_unary("table-distinct",  RAY_FN_NONE, ray_table_distinct_fn);

    /* Query operations */
    register_vary("select",    RAY_FN_SPECIAL_FORM, ray_select_fn);
    register_vary("update",    RAY_FN_SPECIAL_FORM, ray_update);
    register_vary("insert",    RAY_FN_SPECIAL_FORM, ray_insert);
    register_vary("upsert",    RAY_FN_SPECIAL_FORM, ray_upsert);
    register_binary("xbar",    RAY_FN_ATOMIC, ray_xbar);

    /* Join operations */
    register_vary("left-join",   RAY_FN_NONE, ray_left_join);
    register_vary("inner-join",  RAY_FN_NONE, ray_inner_join);
    register_vary("antijoin",    RAY_FN_NONE, ray_antijoin_fn);
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

    /* Serialization */
    register_unary("ser",        RAY_FN_NONE, ray_ser_fn);
    register_unary("de",         RAY_FN_NONE, ray_de_fn);

    /* Splayed / partitioned table I/O */
    register_vary("set-splayed", RAY_FN_NONE, ray_set_splayed_fn);
    register_vary("get-splayed", RAY_FN_NONE, ray_get_splayed_fn);
    register_vary("get-parted",  RAY_FN_NONE, ray_get_parted_fn);

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
    register_unary("sym-name",   RAY_FN_NONE, ray_sym_name_fn);
    register_binary("unify",     RAY_FN_NONE, ray_unify_fn);
    register_binary("xrank",     RAY_FN_NONE, ray_xrank_fn);

    /* EAV triple storage */
    register_vary("datoms",        RAY_FN_NONE, ray_datoms_fn);
    register_vary("assert-fact",   RAY_FN_NONE, ray_assert_fact_fn);
    register_vary("retract-fact",  RAY_FN_NONE, ray_retract_fact_fn);
    register_vary("scan-eav",      RAY_FN_NONE, ray_scan_eav_fn);
    register_vary("pull",          RAY_FN_NONE, ray_pull_fn);

    /* Datalog */
    register_vary("rule",         RAY_FN_SPECIAL_FORM, ray_rule_fn);
    register_vary("query",        RAY_FN_SPECIAL_FORM, ray_query_fn);

    /* Programmatic Datalog API */
    register_vary("dl-program",    RAY_FN_NONE, ray_dl_program_fn);
    register_vary("dl-add-edb",    RAY_FN_NONE, ray_dl_add_edb_fn);
    register_unary("dl-stratify",  RAY_FN_NONE, ray_dl_stratify_fn);
    register_unary("dl-eval",      RAY_FN_NONE, ray_dl_eval_fn);
    register_binary("dl-query",    RAY_FN_NONE, ray_dl_query_fn);
    register_binary("dl-provenance", RAY_FN_NONE, ray_dl_provenance_fn);
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
    /* Reset global Datalog rule storage */
    ray_dl_reset_rules();
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
