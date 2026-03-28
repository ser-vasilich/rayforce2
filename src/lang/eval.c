#include "lang/eval.h"
#include "lang/env.h"
#include "lang/parse.h"
#include "io/csv.h"
#include "ops/pool.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>

/* Maximum recursion depth for ray_eval() to prevent stack overflow */
#define RAY_EVAL_MAX_DEPTH 512
_Thread_local static int eval_depth = 0;

/* Interrupt flag — set by REPL signal handler, checked by eval/VM loops */
static volatile sig_atomic_t g_eval_interrupted = 0;

void ray_eval_request_interrupt(void) { g_eval_interrupted = 1; }
void ray_eval_clear_interrupt(void)   { g_eval_interrupted = 0; }
int  ray_eval_is_interrupted(void)    { return g_eval_interrupted != 0; }

/* ══════════════════════════════════════════
 * Arithmetic builtins
 * ══════════════════════════════════════════ */

static ray_t* make_i64(int64_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return RAY_ERR_PTR(RAY_ERR_OOM);
    obj->type = -RAY_I64;
    obj->i64 = v;
    return obj;
}

static ray_t* make_f64(double v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return RAY_ERR_PTR(RAY_ERR_OOM);
    obj->type = -RAY_F64;
    obj->f64 = v;
    return obj;
}

static ray_t* make_bool(uint8_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return RAY_ERR_PTR(RAY_ERR_OOM);
    obj->type = -RAY_BOOL;
    obj->b8 = v;
    return obj;
}

/* Helpers to extract numeric value as double */
static int is_numeric(ray_t* x) {
    return x->type == -RAY_I64 || x->type == -RAY_F64;
}

static double as_f64(ray_t* x) {
    return (x->type == -RAY_F64) ? x->f64 : (double)x->i64;
}

static int is_float_op(ray_t* a, ray_t* b) {
    return a->type == -RAY_F64 || b->type == -RAY_F64;
}

/* Binary arithmetic */
ray_t* ray_add_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) + as_f64(b));
    return make_i64(a->i64 + b->i64);
}

ray_t* ray_sub_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) - as_f64(b));
    return make_i64(a->i64 - b->i64);
}

ray_t* ray_mul_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) * as_f64(b));
    return make_i64(a->i64 * b->i64);
}

ray_t* ray_div_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (is_float_op(a, b)) {
        if (as_f64(b) == 0.0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        return make_f64(as_f64(a) / as_f64(b));
    }
    if (b->i64 == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    return make_i64(a->i64 / b->i64);
}

ray_t* ray_mod_fn(ray_t* a, ray_t* b) {
    if (a->type != -RAY_I64 || b->type != -RAY_I64)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (b->i64 == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    return make_i64(a->i64 % b->i64);
}

/* Comparison */
ray_t* ray_gt_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    return make_bool(as_f64(a) > as_f64(b) ? 1 : 0);
}

ray_t* ray_lt_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    return make_bool(as_f64(a) < as_f64(b) ? 1 : 0);
}

ray_t* ray_gte(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    return make_bool(as_f64(a) >= as_f64(b) ? 1 : 0);
}

ray_t* ray_lte(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

ray_t* ray_eq_fn(ray_t* a, ray_t* b) {
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 == b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 == b->i64 ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (a->type == -RAY_I64 && b->type == -RAY_I64)
        return make_bool(a->i64 == b->i64 ? 1 : 0);
    return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
}

ray_t* ray_neq(ray_t* a, ray_t* b) {
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 != b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 != b->i64 ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (a->type == -RAY_I64 && b->type == -RAY_I64)
        return make_bool(a->i64 != b->i64 ? 1 : 0);
    return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
}

/* Logical — coerce to truthiness (0/nil/false = falsy, else truthy) */
static inline int is_truthy(ray_t* x) {
    if (x->type == -RAY_BOOL) return x->b8;
    if (x->type == -RAY_I64)  return x->i64 != 0;
    if (x->type == -RAY_F64)  return x->f64 != 0.0;
    return 1; /* non-null objects are truthy */
}

ray_t* ray_and_fn(ray_t* a, ray_t* b) {
    return make_bool((is_truthy(a) && is_truthy(b)) ? 1 : 0);
}

ray_t* ray_or_fn(ray_t* a, ray_t* b) {
    return make_bool((is_truthy(a) || is_truthy(b)) ? 1 : 0);
}

/* Unary */
ray_t* ray_not_fn(ray_t* x) {
    return make_bool(is_truthy(x) ? 0 : 1);
}

ray_t* ray_neg_fn(ray_t* x) {
    if (x->type == -RAY_I64) return make_i64(-x->i64);
    if (x->type == -RAY_F64) return make_f64(-x->f64);
    return RAY_ERR_PTR(RAY_ERR_TYPE);
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
    return RAY_ERR_PTR(RAY_ERR_DOMAIN);
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
        handler_result = RAY_ERR_PTR(RAY_ERR_TYPE);
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

/* Map a binary function element-wise over lists.
 * Both args can be lists (zip-map) or one scalar (broadcast). */
static ray_t* atomic_map_binary(ray_binary_fn fn, ray_t* left, ray_t* right) {
    int left_list = is_list(left);
    int right_list = is_list(right);

    if (!left_list && !right_list) return fn(left, right);

    int64_t len;
    if (left_list && right_list) {
        len = ray_len(left) < ray_len(right) ? ray_len(left) : ray_len(right);
    } else {
        len = left_list ? ray_len(left) : ray_len(right);
    }

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** le = left_list ? (ray_t**)ray_data(left) : NULL;
    ray_t** re = right_list ? (ray_t**)ray_data(right) : NULL;

    for (int64_t i = 0; i < len; i++) {
        ray_t* a = left_list ? le[i] : left;
        ray_t* b = right_list ? re[i] : right;
        ray_t* elem = fn(a, b);
        if (RAY_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return elem;
        }
        out[i] = elem;
    }
    return result;
}

/* Map a unary function element-wise over a list. */
static ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg) {
    if (!is_list(arg)) return fn(arg);

    int64_t len = ray_len(arg);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(arg);

    for (int64_t i = 0; i < len; i++) {
        ray_t* elem = fn(elems[i]);
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
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_sum(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) return make_i64(0);
    ray_t** elems = (ray_t**)ray_data(x);
    int has_float = 0;
    double fsum = 0.0;
    int64_t isum = 0;
    for (int64_t i = 0; i < len; i++) {
        if (elems[i]->type == -RAY_F64) { has_float = 1; fsum += elems[i]->f64; }
        else if (elems[i]->type == -RAY_I64) { isum += elems[i]->i64; fsum += (double)elems[i]->i64; }
        else return RAY_ERR_PTR(RAY_ERR_TYPE);
    }
    return has_float ? make_f64(fsum) : make_i64(isum);
}

ray_t* ray_count_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_COUNT);
    if (x->type == RAY_TABLE) return make_i64(ray_table_nrows(x));
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_count(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    return make_i64(ray_len(x));
}

ray_t* ray_avg_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_AVG);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_avg(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t** elems = (ray_t**)ray_data(x);
    double sum = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return RAY_ERR_PTR(RAY_ERR_TYPE);
        sum += as_f64(elems[i]);
    }
    return make_f64(sum / (double)len);
}

ray_t* ray_min(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MIN);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_min_op(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t** elems = (ray_t**)ray_data(x);
    if (!is_numeric(elems[0])) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int has_float = elems[0]->type == -RAY_F64;
    double fmin = as_f64(elems[0]);
    int64_t imin = elems[0]->type == -RAY_I64 ? elems[0]->i64 : 0;
    for (int64_t i = 1; i < len; i++) {
        if (!is_numeric(elems[i])) return RAY_ERR_PTR(RAY_ERR_TYPE);
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
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_max_op(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t** elems = (ray_t**)ray_data(x);
    if (!is_numeric(elems[0])) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int has_float = elems[0]->type == -RAY_F64;
    double fmax = as_f64(elems[0]);
    int64_t imax = elems[0]->type == -RAY_I64 ? elems[0]->i64 : 0;
    for (int64_t i = 1; i < len; i++) {
        if (!is_numeric(elems[i])) return RAY_ERR_PTR(RAY_ERR_TYPE);
        if (elems[i]->type == -RAY_F64) has_float = 1;
        double v = as_f64(elems[i]);
        if (v > fmax) { fmax = v; imax = elems[i]->type == -RAY_I64 ? elems[i]->i64 : 0; }
    }
    return has_float ? make_f64(fmax) : make_i64(imax);
}

ray_t* ray_first_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_FIRST);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_first(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (ray_len(x) == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* elem = ((ray_t**)ray_data(x))[0];
    ray_retain(elem);
    return elem;
}

ray_t* ray_last_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_LAST);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_last(g, in);
        return ray_lazy_materialize(ray_lazy_wrap(g, op));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* elem = ((ray_t**)ray_data(x))[len - 1];
    ray_retain(elem);
    return elem;
}

/* Helper: copy typed vec elements to double scratch buffer.
 * Returns scratch ray_t* (caller must ray_release), or error. */
static ray_t* vec_to_f64_scratch(ray_t* x, double** out_vals) {
    int64_t len = ray_len(x);
    ray_t* scratch = ray_alloc(len * sizeof(double));
    if (!scratch) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    } else {
        ray_release(scratch);
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    }
    *out_vals = vals;
    return scratch;
}

ray_t* ray_med(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (RAY_IS_ERR(x)) return x;
    int64_t len;
    ray_t* scratch;
    double* vals;

    if (ray_is_vec(x)) {
        len = ray_len(x);
        if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        scratch = vec_to_f64_scratch(x, &vals);
        if (RAY_IS_ERR(scratch)) return scratch;
    } else if (is_list(x)) {
        len = ray_len(x);
        if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        ray_t** elems = (ray_t**)ray_data(x);
        scratch = ray_alloc(len * sizeof(double));
        if (!scratch) return RAY_ERR_PTR(RAY_ERR_OOM);
        scratch->type = RAY_F64;
        scratch->len = len;
        vals = (double*)ray_data(scratch);
        for (int64_t i = 0; i < len; i++) {
            if (!is_numeric(elems[i])) { ray_release(scratch); return RAY_ERR_PTR(RAY_ERR_TYPE); }
            vals[i] = as_f64(elems[i]);
        }
    } else {
        return RAY_ERR_PTR(RAY_ERR_TYPE);
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

ray_t* ray_dev(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (RAY_IS_ERR(x)) return x;
    if (ray_is_vec(x)) {
        int64_t len = ray_len(x);
        if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        double* vals;
        ray_t* scratch = vec_to_f64_scratch(x, &vals);
        if (RAY_IS_ERR(scratch)) return scratch;
        double sum = 0.0;
        for (int64_t i = 0; i < len; i++) sum += vals[i];
        double mean = sum / (double)len;
        double var = 0.0;
        for (int64_t i = 0; i < len; i++) {
            double d = vals[i] - mean;
            var += d * d;
        }
        ray_release(scratch);
        return make_f64(sqrt(var / (double)len));
    }
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t** elems = (ray_t**)ray_data(x);
    double sum = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return RAY_ERR_PTR(RAY_ERR_TYPE);
        sum += as_f64(elems[i]);
    }
    double mean = sum / (double)len;
    double var = 0.0;
    for (int64_t i = 0; i < len; i++) {
        double d = as_f64(elems[i]) - mean;
        var += d * d;
    }
    return make_f64(sqrt(var / (double)len));
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
    return RAY_ERR_PTR(RAY_ERR_TYPE);
}

/* Helper: call a function object with 2 args. Does not release fn or args. */
static ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b) {
    if (fn->type == RAY_BINARY) {
        ray_binary_fn f = (ray_binary_fn)(uintptr_t)fn->i64;
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
    return RAY_ERR_PTR(RAY_ERR_TYPE);
}

/* (map fn val vec) — apply binary fn(val, elem) to each element of vec.
 * Also supports (map fn vec) for unary mapping. */
ray_t* ray_map(ray_t** args, int64_t n) {
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];

    if (n == 2) {
        /* Unary map: (map fn vec) */
        ray_t* vec = args[1];
        if (!is_list(vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);
        int64_t len = ray_len(vec);
        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
        result->type = RAY_LIST;
        result->len = len;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** elems = (ray_t**)ray_data(vec);
        for (int64_t i = 0; i < len; i++) {
            out[i] = call_fn1(fn, elems[i]);
            if (RAY_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) ray_release(out[j]);
                ray_release(result);
                return out[i];
            }
        }
        return result;
    }

    /* Binary map: (map fn val vec) — apply fn(val, elem) */
    ray_t* val = args[1];
    ray_t* vec = args[2];
    if (!is_list(vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(vec);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, val, elems[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return out[i];
        }
    }
    return result;
}

/* (pmap fn val vec) — same as map, parallel not implemented yet (sequential fallback) */
ray_t* ray_pmap(ray_t** args, int64_t n) {
    return ray_map(args, n);
}

/* (fold fn vec) or (fold fn init vec) — reduce with binary fn */
ray_t* ray_fold(ray_t** args, int64_t n) {
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* vec;
    ray_t* acc;
    if (n == 2) {
        /* (fold fn vec) — use first element as initial value */
        vec = args[1];
        if (!is_list(vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);
        int64_t len = ray_len(vec);
        if (len == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        ray_t** elems = (ray_t**)ray_data(vec);
        ray_retain(elems[0]);
        acc = elems[0];
        for (int64_t i = 1; i < len; i++) {
            ray_t* next = call_fn2(fn, acc, elems[i]);
            ray_release(acc);
            if (RAY_IS_ERR(next)) return next;
            acc = next;
        }
        return acc;
    }

    /* (fold fn init vec) */
    ray_retain(args[1]);
    acc = args[1];
    vec = args[2];
    if (!is_list(vec)) { ray_release(acc); return RAY_ERR_PTR(RAY_ERR_TYPE); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        ray_t* next = call_fn2(fn, acc, elems[i]);
        ray_release(acc);
        if (RAY_IS_ERR(next)) return next;
        acc = next;
    }
    return acc;
}

/* (scan fn vec) — running fold, returns vector of partial results */
ray_t* ray_scan_fn(ray_t** args, int64_t n) {
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* vec = args[1];
    if (!is_list(vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(vec);
    if (len == 0) {
        ray_t* result = ray_alloc(0);
        if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
        result->type = RAY_LIST;
        result->len = 0;
        return result;
    }

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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
            ray_release(result);
            return out[i];
        }
    }
    return result;
}

/* (filter vec mask) — filter vector by boolean mask */
ray_t* ray_filter_fn(ray_t* vec, ray_t* mask) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (ray_is_lazy(mask)) mask = ray_lazy_materialize(mask);
    if (!is_list(vec) || !is_list(mask)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(vec);
    int64_t mlen = ray_len(mask);
    if (len != mlen) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

    ray_t** velems = (ray_t**)ray_data(vec);
    ray_t** melems = (ray_t**)ray_data(mask);

    /* Count true values */
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type == -RAY_BOOL && melems[i]->b8) count++;
    }

    ray_t* result = ray_alloc(count * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = count;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t j = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type == -RAY_BOOL && melems[i]->b8) {
            ray_retain(velems[i]);
            out[j++] = velems[i];
        }
    }
    return result;
}

/* (apply fn vec1 vec2) — zip-apply fn element-wise over two vectors */
ray_t* ray_apply(ray_t** args, int64_t n) {
    if (n < 3) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* vec1 = args[1];
    ray_t* vec2 = args[2];
    if (!is_list(vec1) || !is_list(vec2)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    int64_t len = len1 < len2 ? len1 : len2;

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, e1[i], e2[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return out[i];
        }
    }
    return result;
}

/* ══════════════════════════════════════════
 * Collection operations
 * ══════════════════════════════════════════ */

/* Helper: compare two atoms for equality (value-based) */
static int atom_eq(ray_t* a, ray_t* b) {
    if (a->type != b->type) {
        if (is_numeric(a) && is_numeric(b))
            return as_f64(a) == as_f64(b);
        return 0;
    }
    switch (a->type) {
    case -RAY_I64:  return a->i64 == b->i64;
    case -RAY_F64:  return a->f64 == b->f64;
    case -RAY_BOOL: return a->b8 == b->b8;
    case -RAY_SYM:  return a->i64 == b->i64;
    default: return 0;
    }
}

/* (distinct vec) — remove duplicates, preserving first occurrence */
ray_t* ray_distinct_fn(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    if (len == 0) { ray_retain(x); return x; }
    ray_t** elems = (ray_t**)ray_data(x);

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    return result;
}

/* (in val vec) — check membership */
ray_t* ray_in(ray_t* val, ray_t* vec) {
    if (ray_is_lazy(val)) val = ray_lazy_materialize(val);
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (!is_list(vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(val, elems[i])) return make_bool(1);
    }
    return make_bool(0);
}

/* (except vec1 vec2) — elements in vec1 not in vec2 */
ray_t* ray_except(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);
    if (!is_list(vec1) || !is_list(vec2)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc(len1 * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    return result;
}

/* (union vec1 vec2) — elements in vec1 + elements in vec2 not already in vec1 */
ray_t* ray_union(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);
    if (!is_list(vec1) || !is_list(vec2)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc((len1 + len2) * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    return result;
}

/* (sect vec1 vec2) — intersection: elements in both */
ray_t* ray_sect(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);
    if (!is_list(vec1) || !is_list(vec2)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc(len1 * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    return result;
}

/* (take vec n) — first n elements (positive) or last |n| elements (negative) */
ray_t* ray_take(ray_t* vec, ray_t* n_obj) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (!is_list(vec) || n_obj->type != -RAY_I64)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(vec);
    int64_t n = n_obj->i64;
    ray_t** elems = (ray_t**)ray_data(vec);

    int64_t start, count;
    if (n >= 0) {
        start = 0;
        count = n < len ? n : len;
    } else {
        count = -n < len ? -n : len;
        start = len - count;
    }

    ray_t* result = ray_alloc(count * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = count;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < count; i++) {
        ray_retain(elems[start + i]);
        out[i] = elems[start + i];
    }
    return result;
}

/* (at vec idx) or (at table 'col) — index into vector or table */
ray_t* ray_at(ray_t* vec, ray_t* idx) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* Table column access by symbol key */
    if (vec->type == RAY_TABLE && idx->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(vec, idx->i64);
        if (!col) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        /* Convert typed column vector to a Rayfall list */
        int64_t nrows = col->len;
        ray_t* result = ray_alloc(nrows * sizeof(ray_t*));
        if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
        result->type = RAY_LIST;
        result->len = nrows;
        ray_t** out = (ray_t**)ray_data(result);
        int8_t ctype = col->type;
        for (int64_t i = 0; i < nrows; i++) {
            if (ctype == RAY_I64) {
                out[i] = make_i64(((int64_t*)ray_data(col))[i]);
            } else if (ctype == RAY_F64) {
                out[i] = make_f64(((double*)ray_data(col))[i]);
            } else if (ctype == RAY_BOOL) {
                out[i] = make_bool(((uint8_t*)ray_data(col))[i]);
            } else if (ctype == RAY_SYM) {
                ray_t* s = ray_alloc(0);
                if (!s) { ray_release(result); return RAY_ERR_PTR(RAY_ERR_OOM); }
                s->type = -RAY_SYM;
                s->i64 = ((int64_t*)ray_data(col))[i];
                out[i] = s;
            } else if (ctype == RAY_STR) {
                size_t slen;
                const char *sptr = ray_str_vec_get(col, i, &slen);
                out[i] = ray_str(sptr ? sptr : "", sptr ? slen : 0);
            } else {
                out[i] = make_i64(0);
            }
            if (RAY_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) ray_release(out[j]);
                ray_release(result);
                return out[i];
            }
        }
        return result;
    }

    if (!is_list(vec) || idx->type != -RAY_I64)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t i = idx->i64;
    int64_t len = ray_len(vec);
    if (i < 0 || i >= len) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* elem = ((ray_t**)ray_data(vec))[i];
    ray_retain(elem);
    return elem;
}

/* (find vec val) — index of first occurrence, or -1 */
ray_t* ray_find(ray_t* vec, ray_t* val) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (!is_list(vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(elems[i], val)) return make_i64(i);
    }
    return make_i64(-1);
}

/* (til n) — generate integer sequence [0, 1, ..., n-1] */
static void til_fill(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    int64_t* out = (int64_t*)ctx;
    for (int64_t i = start; i < end; i++)
        out[i] = i;
}

ray_t* ray_til(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_I64) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t n = x->i64;
    if (n < 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
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
    if (!is_list(x)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t len = ray_len(x);
    ray_t** elems = (ray_t**)ray_data(x);

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < len; i++) {
        ray_retain(elems[len - 1 - i]);
        out[i] = elems[len - 1 - i];
    }
    return result;
}

/* ══════════════════════════════════════════
 * Table construction and access
 * ══════════════════════════════════════════ */

/* (list v1 v2 ...) — package args into a list */
ray_t* ray_list(ray_t** args, int64_t n) {
    ray_t* result = ray_alloc(n * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    if (!is_list(names) || !is_list(cols)) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t ncols = ray_len(names);
    if (ray_len(cols) != ncols) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

    ray_t** name_elems = (ray_t**)ray_data(names);
    ray_t** col_elems = (ray_t**)ray_data(cols);

    ray_t* tbl = ray_table_new(ncols);
    if (RAY_IS_ERR(tbl)) return tbl;

    for (int64_t i = 0; i < ncols; i++) {
        if (name_elems[i]->type != -RAY_SYM)
            { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_TYPE); }
        int64_t name_id = name_elems[i]->i64;

        /* Convert Rayfall list to typed column vector */
        ray_t* col_list = col_elems[i];
        if (!is_list(col_list))
            { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_TYPE); }
        int64_t nrows = ray_len(col_list);

        /* Validate all columns have consistent row count */
        if (i == 0) {
            /* first column sets the expected row count */
        } else {
            int64_t expected = ray_len(col_elems[0]);
            if (nrows != expected)
                { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
        }
        ray_t** row_elems = (ray_t**)ray_data(col_list);

        /* Determine column type from elements (scan for mixed I64/F64 → F64) */
        int8_t col_type = RAY_I64;
        if (nrows > 0) {
            if (row_elems[0]->type == -RAY_F64) col_type = RAY_F64;
            else if (row_elems[0]->type == -RAY_BOOL) col_type = RAY_BOOL;
            else if (row_elems[0]->type == -RAY_SYM) col_type = RAY_SYM;
            else if (row_elems[0]->type == -RAY_STR) col_type = RAY_STR;
        }
        /* Promote I64 → F64 if any element is F64 */
        if (col_type == RAY_I64) {
            for (int64_t j = 0; j < nrows; j++) {
                if (row_elems[j]->type == -RAY_F64) { col_type = RAY_F64; break; }
            }
        }

        ray_t* col_vec = ray_vec_new(col_type, nrows);
        if (RAY_IS_ERR(col_vec))
            { ray_release(tbl); return col_vec; }

        for (int64_t j = 0; j < nrows; j++) {
            if (col_type == RAY_STR) {
                if (row_elems[j]->type != -RAY_STR) {
                    ray_release(col_vec); ray_release(tbl);
                    return RAY_ERR_PTR(RAY_ERR_TYPE);
                }
                const char *sptr = ray_str_ptr(row_elems[j]);
                size_t slen = ray_str_len(row_elems[j]);
                col_vec = ray_str_vec_append(col_vec, sptr, slen);
            } else {
                /* Validate each element matches the column type (allow I64→F64 promotion) */
                int type_ok = (row_elems[j]->type == -col_type);
                if (!type_ok && col_type == RAY_F64 && row_elems[j]->type == -RAY_I64) type_ok = 1;
                if (!type_ok) {
                    ray_release(col_vec); ray_release(tbl);
                    return RAY_ERR_PTR(RAY_ERR_TYPE);
                }
                void* val_ptr;
                double promoted;
                if (col_type == RAY_F64 && row_elems[j]->type == -RAY_I64) {
                    promoted = (double)row_elems[j]->i64;
                    val_ptr = &promoted;
                } else if (col_type == RAY_I64) val_ptr = &row_elems[j]->i64;
                else if (col_type == RAY_F64) val_ptr = &row_elems[j]->f64;
                else if (col_type == RAY_BOOL) val_ptr = &row_elems[j]->b8;
                else val_ptr = &row_elems[j]->i64; /* SYM stored as i64 */
                col_vec = ray_vec_append(col_vec, val_ptr);
            }
            if (RAY_IS_ERR(col_vec))
                { ray_release(tbl); return col_vec; }
        }

        tbl = ray_table_add_col(tbl, name_id, col_vec);
        ray_release(col_vec);
        if (RAY_IS_ERR(tbl)) return tbl;
    }

    return tbl;
}

/* (key table) — return column names as a list of symbols */
ray_t* ray_key(ray_t* x) {
    if (x->type != RAY_TABLE) return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t ncols = ray_table_ncols(x);
    ray_t* result = ray_alloc(ncols * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
    result->type = RAY_LIST;
    result->len = ncols;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < ncols; i++) {
        int64_t name_id = ray_table_col_name(x, i);
        ray_t* sym = ray_alloc(0);
        if (!sym) { ray_release(result); return RAY_ERR_PTR(RAY_ERR_OOM); }
        sym->type = -RAY_SYM;
        sym->i64 = name_id;
        out[i] = sym;
    }
    return result;
}

/* (value dict) — extract values from a dict as a list */
ray_t* ray_value(ray_t* x) {
    if (x->type != RAY_LIST || !(x->attrs & RAY_ATTR_DICT))
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    int64_t n = ray_len(x);
    int64_t nvals = n / 2;
    ray_t* result = ray_alloc(nvals * sizeof(ray_t*));
    if (!result) return RAY_ERR_PTR(RAY_ERR_OOM);
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

    /* Symbol literal → const i64 (sym IDs are integer indices) */
    if (expr->type == -RAY_SYM && !(expr->attrs & RAY_ATTR_NAME))
        return ray_const_i64(g, expr->i64);

    /* Name reference → column scan */
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        ray_t* s = ray_sym_str(expr->i64);
        if (!s) return NULL;
        return ray_scan(g, ray_str_ptr(s));
    }

    /* List → function call: (fn arg1 arg2 ...) */
    if (expr->type == RAY_LIST && !(expr->attrs & (RAY_ATTR_VECTOR | RAY_ATTR_DICT))) {
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
    if (expr->attrs & (RAY_ATTR_VECTOR | RAY_ATTR_DICT)) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (elems[0]->type != -RAY_SYM) return 0;
    return resolve_agg_opcode(elems[0]->i64) != 0;
}

/* (select {from: t [where: pred] [by: key] [col: expr ...]})
 * Special form — receives unevaluated dict arg. */
ray_t* ray_select_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return RAY_ERR_PTR(RAY_ERR_TYPE);

    /* Evaluate 'from:' to get the source table */
    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_TYPE); }

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
    if (!g) { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_OOM); }

    ray_op_t* root = ray_const_table(g, tbl);

    /* Apply WHERE filter */
    if (where_expr) {
        ray_op_t* pred = compile_expr_dag(g, where_expr);
        if (!pred) { ray_graph_free(g); ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
        root = ray_filter(g, root, pred);
    }

    /* GROUP BY */
    if (by_expr) {
        /* Compile group key(s) */
        ray_op_t* key_ops[16];
        uint8_t n_keys = 0;

        if (by_expr->type == RAY_LIST && (by_expr->attrs & RAY_ATTR_VECTOR)) {
            /* Multiple keys: [key1 key2 ...] */
            int64_t nk = ray_len(by_expr);
            ray_t** key_elems = (ray_t**)ray_data(by_expr);
            for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                key_ops[n_keys] = compile_expr_dag(g, key_elems[i]);
                if (!key_ops[n_keys]) { ray_graph_free(g); ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
                n_keys++;
            }
        } else {
            /* Single key expression */
            key_ops[0] = compile_expr_dag(g, by_expr);
            if (!key_ops[0]) { ray_graph_free(g); ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
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
                if (!agg_ins[n_aggs]) { ray_graph_free(g); ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
                n_aggs++;
            }
        }

        if (n_aggs > 0)
            root = ray_group(g, key_ops, n_keys, agg_ops, agg_ins, n_aggs);
    } else if (n_out > 0) {
        /* Projection only (no group by) — select specific columns */
        ray_op_t* col_ops[16];
        uint8_t nc = 0;
        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id) continue;
            if (nc < 16) {
                col_ops[nc] = compile_expr_dag(g, dict_elems[i + 1]);
                if (!col_ops[nc]) { ray_graph_free(g); ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
                nc++;
            }
        }
        root = ray_select(g, root, col_ops, nc);
    }

    /* Optimize and execute */
    root = ray_optimize(g, root);
    ray_t* result = ray_execute(g, root);

    ray_graph_free(g);
    ray_release(tbl);
    return result;
}

/* (xbar col bucket) — time/value bucketing: floor(col/bucket)*bucket */
ray_t* ray_xbar(ray_t* col, ray_t* bucket) {
    if (col->type == -RAY_I64 && bucket->type == -RAY_I64) {
        int64_t a = col->i64, b = bucket->i64;
        if (b == 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        /* Floor division: truncate toward negative infinity */
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        return make_i64(q * b);
    }
    if ((col->type == -RAY_F64 || col->type == -RAY_I64) &&
        (bucket->type == -RAY_F64 || bucket->type == -RAY_I64)) {
        double c = col->type == -RAY_F64 ? col->f64 : (double)col->i64;
        double b = bucket->type == -RAY_F64 ? bucket->f64 : (double)bucket->i64;
        if (b == 0.0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        /* Floor division for correct negative bucketing */
        double fq = floor(c / b);
        return make_f64(fq * b);
    }
    return RAY_ERR_PTR(RAY_ERR_TYPE);
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
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_SYM) {
        if (atom->type != -RAY_SYM)
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_F64) {
        if (atom->type != -RAY_F64 && atom->type != -RAY_I64)
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        double v = (atom->type == -RAY_F64) ? atom->f64 : (double)atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_BOOL) {
        if (atom->type != -RAY_BOOL)
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        uint8_t v = atom->b8;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_STR && atom->type == -RAY_STR) {
        const char *sptr = ray_str_ptr(atom);
        size_t slen = ray_str_len(atom);
        return ray_str_vec_append(col_vec, sptr, slen);
    }
    return RAY_ERR_PTR(RAY_ERR_TYPE);
}

/* (update {col: expr ... from: t [where: pred]})
 * Special form — receives unevaluated dict arg.
 * For rows matching where (or all if no where), evaluate column expressions
 * and replace those column values. Returns a new table. */
ray_t* ray_update(ray_t** args, int64_t n) {
    if (n < 1) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return RAY_ERR_PTR(RAY_ERR_TYPE);

    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_TYPE); }

    ray_t* where_expr = dict_get(dict, "where");

    /* Evaluate WHERE using the DAG to get a boolean mask */
    int64_t nrows = ray_table_nrows(tbl);
    uint8_t* mask = NULL;

    if (where_expr) {
        /* Evaluate the predicate as a DAG to get a boolean mask vector */
        ray_graph_t* g = ray_graph_new(tbl);
        if (!g) { ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_OOM); }
        ray_op_t* pred = compile_expr_dag(g, where_expr);
        if (!pred) { ray_graph_free(g); ray_release(tbl); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
        pred = ray_optimize(g, pred);
        ray_t* mask_vec = ray_execute(g, pred);
        ray_graph_free(g);

        if (RAY_IS_ERR(mask_vec)) { ray_release(tbl); return mask_vec; }
        if (mask_vec->type != RAY_BOOL || mask_vec->len != nrows) {
            ray_release(mask_vec);
            ray_release(tbl);
            return RAY_ERR_PTR(RAY_ERR_TYPE);
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

                /* Evaluate expression via DAG */
                ray_graph_t* ug = ray_graph_new(tbl);
                ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                if (!expr_op) { ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); ray_graph_free(ug); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
                expr_op = ray_optimize(ug, expr_op);
                ray_t* expr_vec = ray_execute(ug, expr_op);
                ray_graph_free(ug);

                if (RAY_IS_ERR(expr_vec)) { ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return expr_vec; }

                /* Broadcast scalar atom to full column vector if needed */
                if (expr_vec->type < 0) {
                    /* Type check atom against column type BEFORE broadcast */
                    int ok = (expr_vec->type == -ct);
                    if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1; /* allow I64→F64 promotion */
                    if (!ok) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return RAY_ERR_PTR(RAY_ERR_TYPE);
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
                    return RAY_ERR_PTR(RAY_ERR_TYPE);
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
            ray_graph_t* ug = ray_graph_new(tbl);
            ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
            if (!expr_op) { ray_release(result); ray_release(tbl); ray_graph_free(ug); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
            expr_op = ray_optimize(ug, expr_op);
            ray_t* expr_vec = ray_execute(ug, expr_op);
            ray_graph_free(ug);
            if (RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec; }

            /* Broadcast scalar atom to full column vector if needed */
            if (expr_vec->type < 0) {
                int64_t nrows = ray_table_nrows(tbl);
                int8_t ct = orig_col->type;
                /* Type check atom against column type BEFORE broadcast */
                int ok = (expr_vec->type == -ct);
                if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                if (!ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return RAY_ERR_PTR(RAY_ERR_TYPE);
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

            /* Type check: expr_vec must match original column type */
            if (expr_vec->type != orig_col->type) {
                ray_release(expr_vec); ray_release(result); ray_release(tbl);
                return RAY_ERR_PTR(RAY_ERR_TYPE);
            }

            result = ray_table_add_col(result, col_name, expr_vec);
            ray_release(expr_vec);
        }
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    ray_release(tbl);
    return result;
}

/* (insert table (list val1 val2 ...)) — append a row to a table */
ray_t* ray_insert(ray_t** args, int64_t n) {
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* tbl = args[0];
    ray_t* row = args[1];

    if (tbl->type != RAY_TABLE) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (!is_list(row)) return RAY_ERR_PTR(RAY_ERR_TYPE);

    int64_t ncols = ray_table_ncols(tbl);
    if (ray_len(row) != ncols) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

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

        /* Append new row value */
        new_col = append_atom_to_col(new_col, row_elems[c]);
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    return result;
}

/* (upsert table key_col (list val1 val2 ...)) — update row if key matches, else insert */
ray_t* ray_upsert(ray_t** args, int64_t n) {
    if (n < 3) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* tbl = args[0];
    ray_t* key_sym = args[1];
    ray_t* row = args[2];

    if (tbl->type != RAY_TABLE) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (key_sym->type != -RAY_SYM) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (!is_list(row)) return RAY_ERR_PTR(RAY_ERR_TYPE);

    int64_t ncols = ray_table_ncols(tbl);
    if (ray_len(row) != ncols) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    /* Find the key column index */
    int64_t key_col_idx = -1;
    for (int64_t c = 0; c < ncols; c++) {
        if (ray_table_col_name(tbl, c) == key_sym->i64) {
            key_col_idx = c;
            break;
        }
    }
    if (key_col_idx < 0) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

    /* Find the row to update by key value */
    ray_t* key_col = ray_table_get_col_idx(tbl, key_col_idx);
    int64_t match_row = -1;
    int8_t kt = key_col->type;
    ray_t* key_atom = row_elems[key_col_idx];

    if (kt == RAY_I64) {
        if (key_atom->type != -RAY_I64) return RAY_ERR_PTR(RAY_ERR_TYPE);
        int64_t key_val = key_atom->i64;
        int64_t* kdata = (int64_t*)ray_data(key_col);
        for (int64_t r = 0; r < nrows; r++) {
            if (kdata[r] == key_val) { match_row = r; break; }
        }
    } else if (kt == RAY_SYM) {
        if (key_atom->type != -RAY_SYM) return RAY_ERR_PTR(RAY_ERR_TYPE);
        int64_t key_val = key_atom->i64;
        for (int64_t r = 0; r < nrows; r++) {
            if (ray_read_sym(ray_data(key_col), r, key_col->type, key_col->attrs) == key_val) { match_row = r; break; }
        }
    } else if (kt == RAY_F64) {
        if (key_atom->type != -RAY_F64 && key_atom->type != -RAY_I64)
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        double needle = (key_atom->type == -RAY_F64) ? key_atom->f64
                                                        : (double)key_atom->i64;
        double* kdata = (double*)ray_data(key_col);
        for (int64_t r = 0; r < nrows; r++) {
            if (kdata[r] == needle) { match_row = r; break; }
        }
    } else if (kt == RAY_BOOL) {
        if (key_atom->type != -RAY_BOOL) return RAY_ERR_PTR(RAY_ERR_TYPE);
        uint8_t needle = key_atom->b8;
        uint8_t* kdata = (uint8_t*)ray_data(key_col);
        for (int64_t r = 0; r < nrows; r++) {
            if (kdata[r] == needle) { match_row = r; break; }
        }
    } else if (kt == RAY_STR) {
        if (key_atom->type != -RAY_STR) return RAY_ERR_PTR(RAY_ERR_TYPE);
        const char* needle_s = ray_str_ptr(key_atom);
        size_t needle_len = ray_str_len(key_atom);
        for (int64_t r = 0; r < nrows; r++) {
            size_t rlen = 0;
            const char* rs = ray_str_vec_get(key_col, r, &rlen);
            if (rlen == needle_len && (needle_len == 0 ||
                (rs && needle_s && memcmp(rs, needle_s, rlen) == 0))) {
                match_row = r;
                break;
            }
        }
    }

    if (match_row < 0) {
        /* Key not found — insert: ray_insert expects (table, row) */
        ray_t* insert_args[2] = { tbl, row };
        return ray_insert(insert_args, 2);
    }

    /* Key found — update that row */
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows);
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(orig_col, r, &slen);
                    new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                    new_col = ray_vec_append(new_col, &sym_val);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    new_col = ray_vec_append(new_col, src + r * elem_sz);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    return result;
}

/* ══════════════════════════════════════════
 * Join operations
 * ══════════════════════════════════════════ */

/* Shared implementation for left-join (join_type=1) and inner-join (join_type=0).
 * (left-join t1 t2 [key ...]) / (inner-join t1 t2 [key ...]) */
static ray_t* join_impl(ray_t** args, int64_t n, uint8_t join_type) {
    if (n < 3) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* keys      = args[2];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (keys->type != RAY_LIST || !(keys->attrs & RAY_ATTR_VECTOR))
        return RAY_ERR_PTR(RAY_ERR_TYPE);

    int64_t nk = ray_len(keys);
    if (nk == 0 || nk > 16) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t** key_elems = (ray_t**)ray_data(keys);

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_op_t* lk[16], *rk[16];
    for (int64_t i = 0; i < nk; i++) {
        if (key_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g);
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        }
        ray_t* name_str = ray_sym_str(key_elems[i]->i64);
        if (!name_str) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
        lk[i] = ray_scan(g, ray_str_ptr(name_str));
        rk[i] = ray_scan(g, ray_str_ptr(name_str));
        if (!lk[i] || !rk[i]) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
    }

    ray_op_t* jn = ray_join(g, left_node, lk, right_node, rk,
                           (uint8_t)nk, join_type);
    if (!jn) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_OOM); }

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
    if (n < 4) return RAY_ERR_PTR(RAY_ERR_DOMAIN);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* eq_keys   = args[2];
    ray_t* time_sym  = args[3];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (time_sym->type != -RAY_SYM)
        return RAY_ERR_PTR(RAY_ERR_TYPE);

    uint8_t n_eq = 0;
    ray_t** eq_elems = NULL;
    if (eq_keys->type == RAY_LIST && (eq_keys->attrs & RAY_ATTR_VECTOR)) {
        n_eq = (uint8_t)ray_len(eq_keys);
        eq_elems = (ray_t**)ray_data(eq_keys);
    }

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) return RAY_ERR_PTR(RAY_ERR_OOM);

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g);
            return RAY_ERR_PTR(RAY_ERR_TYPE);
        }
        ray_t* nm = ray_sym_str(eq_elems[i]->i64);
        if (!nm) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_DOMAIN); }
    }

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return RAY_ERR_PTR(RAY_ERR_OOM); }

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

/* (println val1 val2 ...) — print values to stdout, newline at end */
ray_t* ray_println(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        /* Materialize lazy handles before printing */
        if (ray_is_lazy(args[i]))
            args[i] = ray_lazy_materialize(args[i]);
        if (i > 0) fputc(' ', stdout);
        ray_lang_print(stdout, args[i]);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return make_i64(0);
}

/* (read-csv path) — read CSV file, return RAY_TABLE */
ray_t* ray_read_csv_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* path_obj = args[0];
    const char* path = NULL;
    if (path_obj->type == -RAY_STR)
        path = ray_str_ptr(path_obj);
    else
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (!path) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* tbl = ray_read_csv(path);
    if (!tbl || RAY_IS_ERR(tbl)) return RAY_ERR_PTR(RAY_ERR_IO);
    return tbl;
}

/* (write-csv table path) — write table to CSV file */
ray_t* ray_write_csv_fn(ray_t** args, int64_t n) {
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_t* tbl = args[0];
    ray_t* path_obj = args[1];
    if (tbl->type != RAY_TABLE) return RAY_ERR_PTR(RAY_ERR_TYPE);
    const char* path = NULL;
    if (path_obj->type == -RAY_STR)
        path = ray_str_ptr(path_obj);
    else
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (!path) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    ray_err_t err = ray_write_csv(tbl, path);
    if (err != RAY_OK) return RAY_ERR_PTR(err);
    return make_i64(0);
}

/* (as 'TypeName value) — type cast */
ray_t* ray_cast_fn(ray_t* type_sym, ray_t* val) {
    if (type_sym->type != -RAY_SYM) return RAY_ERR_PTR(RAY_ERR_TYPE);
    ray_t* s = ray_sym_str(type_sym->i64);
    if (!s) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    const char* tname = ray_str_ptr(s);
    size_t tlen = ray_str_len(s);

    /* Cast to I64 */
    if (tlen == 3 && memcmp(tname, "I64", 3) == 0) {
        if (val->type == -RAY_I64) { ray_retain(val); return val; }
        if (val->type == -RAY_F64) return make_i64((int64_t)val->f64);
        if (val->type == -RAY_BOOL) return make_i64(val->b8 ? 1 : 0);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            if (!sp) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
            char* end;
            int64_t v = strtoll(sp, &end, 10);
            if (end == sp) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
            return make_i64(v);
        }
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    }
    /* Cast to F64 */
    if (tlen == 3 && memcmp(tname, "F64", 3) == 0) {
        if (val->type == -RAY_F64) { ray_retain(val); return val; }
        if (val->type == -RAY_I64) return make_f64((double)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            if (!sp) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
            char* end;
            double v = strtod(sp, &end);
            if (end == sp) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
            return make_f64(v);
        }
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    }
    /* Cast to BOOL */
    if (tlen == 4 && memcmp(tname, "BOOL", 4) == 0) {
        if (val->type == -RAY_BOOL) { ray_retain(val); return val; }
        if (val->type == -RAY_I64) return make_bool(val->i64 != 0 ? 1 : 0);
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    }
    /* Cast to STR */
    if (tlen == 3 && memcmp(tname, "STR", 3) == 0) {
        if (val->type == -RAY_STR) { ray_retain(val); return val; }
        if (val->type == -RAY_I64) {
            char buf[32];
            int n2 = snprintf(buf, sizeof(buf), "%ld", (long)val->i64);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_F64) {
            char buf[32];
            int n2 = snprintf(buf, sizeof(buf), "%g", val->f64);
            return ray_str(buf, (size_t)n2);
        }
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    }
    return RAY_ERR_PTR(RAY_ERR_DOMAIN);
}

/* (type val) — return the type code of a value */
ray_t* ray_type_fn(ray_t* val) {
    return make_i64(val->type);
}

/* (read path) — read a file's contents as a string */
ray_t* ray_read_file(ray_t* path_obj) {
    if (path_obj->type != -RAY_STR) return RAY_ERR_PTR(RAY_ERR_TYPE);
    const char* path = ray_str_ptr(path_obj);
    if (!path) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    FILE* fp = fopen(path, "rb");
    if (!fp) return RAY_ERR_PTR(RAY_ERR_IO);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return RAY_ERR_PTR(RAY_ERR_IO); }
    /* Use ray_alloc for the buffer */
    ray_t* buf = ray_alloc((size_t)sz + 1);
    if (!buf || RAY_IS_ERR(buf)) { fclose(fp); return RAY_ERR_PTR(RAY_ERR_OOM); }
    char* data = (char*)ray_data(buf);
    size_t rd = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    data[rd] = '\0';
    ray_t* result = ray_str(data, rd);
    ray_release(buf);
    return result;
}

/* (write path content) — write string to a file */
ray_t* ray_write_file(ray_t* path_obj, ray_t* content) {
    if (path_obj->type != -RAY_STR) return RAY_ERR_PTR(RAY_ERR_TYPE);
    if (content->type != -RAY_STR) return RAY_ERR_PTR(RAY_ERR_TYPE);
    const char* path = ray_str_ptr(path_obj);
    const char* data = ray_str_ptr(content);
    size_t len = ray_str_len(content);
    if (!path || !data) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    FILE* fp = fopen(path, "wb");
    if (!fp) return RAY_ERR_PTR(RAY_ERR_IO);
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len) return RAY_ERR_PTR(RAY_ERR_IO);
    return make_i64(0);
}

/* ══════════════════════════════════════════
 * Special forms: set, let, if, do
 * ══════════════════════════════════════════ */

/* (set name value) — bind in global env. Receives unevaluated args. */
ray_t* ray_set(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    if (ray_env_set(name_obj->i64, val) != RAY_OK) {
        ray_release(val);
        return RAY_ERR_PTR(RAY_ERR_OOM);
    }
    return val;  /* set returns the value */
}

/* (let name value) — bind in local scope. Receives unevaluated args. */
ray_t* ray_let(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return RAY_ERR_PTR(RAY_ERR_TYPE);
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    ray_err_t err = ray_env_set_local(name_obj->i64, val);
    if (err != RAY_OK) { ray_release(val); return RAY_ERR_PTR(err); }
    return val;
}

/* (if cond then else?) — conditional. Receives unevaluated args. */
ray_t* ray_cond(ray_t** args, int64_t n) {
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
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
    if (ray_env_push_scope() != RAY_OK) return RAY_ERR_PTR(RAY_ERR_OOM);
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
    if (n < 2) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
    /* args[0] = param vector (list of name symbols), args[1..n-1] = body exprs */
    ray_t* params_list = args[0];

    /* Create lambda object with space for 5 slots:
     * [0] params, [1] body, [2] bytecode, [3] constants, [4] n_locals */
    ray_t* lambda = ray_alloc(5 * sizeof(ray_t*));
    if (!lambda) return RAY_ERR_PTR(RAY_ERR_OOM);
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
        return RAY_ERR_PTR(RAY_ERR_OOM);
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

    return lambda;
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
    ray_t** param_syms = (ray_t**)ray_data(params_list);

    if (ray_env_push_scope() != RAY_OK) return RAY_ERR_PTR(RAY_ERR_OOM);

    for (int64_t i = 0; i < param_count && i < argc; i++) {
        (void)ray_env_set_local(param_syms[i]->i64, call_args[i]);
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

    ray_t *vm_block = ray_alloc(sizeof(ray_vm_t));
    if (!vm_block || RAY_IS_ERR(vm_block)) return RAY_ERR_PTR(RAY_ERR_OOM);
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

#define DISPATCH() goto *dispatch[code[ip++]]
#define PUSH(v)    do { if (vm.sp >= VM_STACK_SIZE) goto vm_error; vm.ps[vm.sp++] = (v); } while(0)
#define POP()      ({ if (vm.sp <= vm.fp + n_locals) goto vm_error; vm.ps[--vm.sp]; })
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
    if (!val) goto vm_error;
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_resolve_w: {
    uint16_t idx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *name_obj = cpool[idx];
    ray_t *val = ray_env_get(name_obj->i64);
    if (!val) goto vm_error;
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_jmp: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ip += offset;
    if (offset < 0 && g_eval_interrupted) goto vm_error;
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
    if ((fn_obj->attrs & RAY_FN_ATOMIC) && is_list(arg))
        result = atomic_map_unary(fn, arg);
    else
        result = fn(arg);
    ray_release(arg);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) goto vm_error;
    PUSH(result);
    DISPATCH();
}

op_call2: {
    ray_t *right = POP();
    ray_t *left = POP();
    ray_t *fn_obj = POP();
    ray_binary_fn fn = (ray_binary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result;
    if ((fn_obj->attrs & RAY_FN_ATOMIC) && (is_list(left) || is_list(right)))
        result = atomic_map_binary(fn, left, right);
    else
        result = fn(left, right);
    ray_release(left);
    ray_release(right);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) goto vm_error;
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
    if (RAY_IS_ERR(result)) goto vm_error;
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
            if (vm.rp >= VM_STACK_SIZE) goto vm_error;
            vm.rs[vm.rp++] = (vm_ctx_t){ .fn = vm.fn, .fp = vm.fp, .ip = ip };

            /* Set up new frame */
            vm.fn = fn_obj;  /* takes ownership of stack ref */
            vm.fp = vm.sp;
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            if (vm.sp + callee_locals >= VM_STACK_SIZE) goto vm_error;
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
            if (g_eval_interrupted) goto vm_error;

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
            result = RAY_ERR_PTR(RAY_ERR_TYPE);
            break;
        }
        ray_release(fn_obj);
        if (RAY_IS_ERR(result)) goto vm_error;
        PUSH(result);
        DISPATCH();
    }
}

op_calls: {
    /* Tail call: reuse current frame (no return stack push) */
    uint8_t n = code[ip++];
    if (n > 64) goto vm_error;
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();

    if (fn_obj->type == RAY_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            ray_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* Clean up current frame locals */
            for (int32_t i = 0; i < n_locals; i++)
                if (LOCAL(i)) { ray_release(LOCAL(i)); LOCAL(i) = NULL; }

            /* Reuse frame: reset sp to fp, don't push return context */
            vm.sp = vm.fp;
            ray_release(vm.fn);
            vm.fn = fn_obj;  /* takes ownership */
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            if (vm.sp + callee_locals >= VM_STACK_SIZE) goto vm_error;
            vm.sp += callee_locals;
            n_locals = callee_locals;

            int64_t pcnt = ray_len(LAMBDA_PARAMS(fn_obj));
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                ray_release(fn_args[i]);

            code = (uint8_t *)ray_data(LAMBDA_BC(fn_obj));
            cpool = (ray_t **)ray_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Fallback: same as CALLF non-lambda path */
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
        default:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        }
        ray_release(fn_obj);
        if (RAY_IS_ERR(result)) goto vm_error;
        PUSH(result);
        DISPATCH();
    }
}

op_calld: {
    /* Dynamic dispatch: evaluate AST directly via ray_eval */
    uint8_t n = code[ip++];
    if (n == 0) {
        /* n=0: the AST itself is on the stack, eval it directly */
        ray_t *ast = POP();
        ray_t *result = ray_eval(ast);
        ray_release(ast);
        if (RAY_IS_ERR(result)) goto vm_error;
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
    if (RAY_IS_ERR(result)) goto vm_error;
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
    ray_release(vm.fn);
    vm.rp--;
    vm.fn = vm.rs[vm.rp].fn;
    vm.fp = vm.rs[vm.rp].fp;
    ip = vm.rs[vm.rp].ip;
    code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
    cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
    n_locals = LAMBDA_NLOCALS(vm.fn);
    PUSH(result);
    DISPATCH();
}

op_trap: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    if (vm.tp >= VM_TRAP_SIZE) goto vm_error;
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

vm_error: {
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
    for (int32_t i = 0; i < vm.sp; i++)
        if (vm.ps[i]) ray_release(vm.ps[i]);
    ray_release(vm.fn);
    for (int32_t i = 0; i < vm.rp; i++)
        ray_release(vm.rs[i].fn);
    for (int32_t i = 0; i < vm.tp; i++)
        ray_release(vm.ts[i].fn);
    __VM = NULL;
#undef vm
    ray_free(vm_block);
    return RAY_ERR_PTR(RAY_ERR_DOMAIN);
}

#undef DISPATCH
#undef PUSH
#undef POP
#undef PEEK
#undef LOCAL
#undef vm
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
    register_binary("+",   RAY_FN_ATOMIC, ray_add_fn);
    register_binary("-",   RAY_FN_ATOMIC, ray_sub_fn);
    register_binary("*",   RAY_FN_ATOMIC, ray_mul_fn);
    register_binary("/",   RAY_FN_ATOMIC, ray_div_fn);
    register_binary("%",   RAY_FN_ATOMIC, ray_mod_fn);
    register_binary(">",   RAY_FN_ATOMIC, ray_gt_fn);
    register_binary("<",   RAY_FN_ATOMIC, ray_lt_fn);
    register_binary(">=",  RAY_FN_ATOMIC, ray_gte);
    register_binary("<=",  RAY_FN_ATOMIC, ray_lte);
    register_binary("==",  RAY_FN_ATOMIC, ray_eq_fn);
    register_binary("!=",  RAY_FN_ATOMIC, ray_neq);
    register_binary("and", RAY_FN_NONE,   ray_and_fn);
    register_binary("or",  RAY_FN_NONE,   ray_or_fn);
    register_unary("not",  RAY_FN_NONE,   ray_not_fn);
    register_unary("neg",  RAY_FN_ATOMIC, ray_neg_fn);

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

    /* Table operations */
    register_vary("list",      RAY_FN_NONE, ray_list);
    register_binary("table",   RAY_FN_NONE, ray_table);
    register_unary("key",      RAY_FN_NONE, ray_key);
    register_unary("value",    RAY_FN_NONE, ray_value);

    /* Query operations */
    register_vary("select",    RAY_FN_SPECIAL_FORM, ray_select_fn);
    register_vary("update",    RAY_FN_SPECIAL_FORM, ray_update);
    register_vary("insert",    RAY_FN_NONE, ray_insert);
    register_vary("upsert",    RAY_FN_NONE, ray_upsert);
    register_binary("xbar",    RAY_FN_ATOMIC, ray_xbar);

    /* Join operations */
    register_vary("left-join",   RAY_FN_NONE, ray_left_join);
    register_vary("inner-join",  RAY_FN_NONE, ray_inner_join);
    register_vary("window-join", RAY_FN_NONE, ray_window_join);

    /* I/O builtins */
    register_vary("println",    RAY_FN_NONE, ray_println);
    register_vary("read-csv",   RAY_FN_NONE, ray_read_csv_fn);
    register_vary("write-csv",  RAY_FN_NONE, ray_write_csv_fn);
    register_binary("as",       RAY_FN_NONE, ray_cast_fn);
    register_unary("type",      RAY_FN_NONE, ray_type_fn);
    register_unary("read",      RAY_FN_NONE, ray_read_file);
    register_binary("write",    RAY_FN_NONE, ray_write_file);
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
    if (g_eval_interrupted) return RAY_ERR_PTR(RAY_ERR_LIMIT);

    if (++eval_depth > RAY_EVAL_MAX_DEPTH) {
        eval_depth--;
        return RAY_ERR_PTR(RAY_ERR_LIMIT);
    }

    ray_t* ret;

    /* Atoms: return themselves (retain) */
    if (ray_is_atom(obj)) {
        /* Name reference: resolve from env */
        if (obj->type == -RAY_SYM && (obj->attrs & RAY_ATTR_NAME)) {
            ray_t* val = ray_env_get(obj->i64);
            if (!val) { ret = RAY_ERR_PTR(RAY_ERR_NAME); goto out; }
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

    /* Vector literal [x y z]: evaluate each element, return as data list */
    if (obj->attrs & RAY_ATTR_VECTOR) {
        int64_t len = ray_len(obj);
        ray_t** src = (ray_t**)ray_data(obj);
        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) { ret = RAY_ERR_PTR(RAY_ERR_OOM); goto out; }
        result->type = RAY_LIST;
        result->attrs = RAY_ATTR_VECTOR;
        result->len = len;
        ray_t** dst = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            dst[i] = ray_eval(src[i]);
            if (RAY_IS_ERR(dst[i])) {
                for (int64_t j = 0; j < i; j++) ray_release(dst[j]);
                ray_release(result);
                ret = dst[i]; goto out;
            }
        }
        ret = result; goto out;
    }

    /* List: evaluate first element, dispatch by type */
    ray_t** elems = (ray_t**)ray_data(obj);
    ray_t* head = ray_eval(elems[0]);
    if (RAY_IS_ERR(head)) { ret = head; goto out; }

    int64_t n = ray_len(obj);

    switch (head->type) {
        case RAY_UNARY: {
            if (n < 2) { ray_release(head); ret = RAY_ERR_PTR(RAY_ERR_DOMAIN); goto out; }
            ray_unary_fn fn = (ray_unary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            ray_t* arg = ray_eval(elems[1]);
            ray_release(head);
            if (RAY_IS_ERR(arg)) { ret = arg; goto out; }
            ray_t* result;
            if ((fn_attrs & RAY_FN_ATOMIC) && is_list(arg))
                result = atomic_map_unary(fn, arg);
            else
                result = fn(arg);
            ray_release(arg);
            ret = result; goto out;
        }
        case RAY_BINARY: {
            if (n < 3) { ray_release(head); ret = RAY_ERR_PTR(RAY_ERR_DOMAIN); goto out; }
            ray_binary_fn fn = (ray_binary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            if (fn_attrs & RAY_FN_SPECIAL_FORM) {
                ray_release(head);
                ret = fn(elems[1], elems[2]); goto out;
            }
            ray_t* left = ray_eval(elems[1]);
            if (RAY_IS_ERR(left)) { ray_release(head); ret = left; goto out; }
            ray_t* right = ray_eval(elems[2]);
            if (RAY_IS_ERR(right)) { ray_release(head); ray_release(left); ret = right; goto out; }
            ray_release(head);
            ray_t* result;
            if ((fn_attrs & RAY_FN_ATOMIC) && (is_list(left) || is_list(right)))
                result = atomic_map_binary(fn, left, right);
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
            if (argc > 64) { ray_release(head); ret = RAY_ERR_PTR(RAY_ERR_DOMAIN); goto out; }
            ray_t* args[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                if (RAY_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = args[i]; goto out;
                }
            }
            ray_release(head);
            ray_t* result = fn(args, argc);
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ret = result; goto out;
        }
        case RAY_LAMBDA: {
            int64_t argc = n - 1;
            if (argc > 64) { ray_release(head); ret = RAY_ERR_PTR(RAY_ERR_DOMAIN); goto out; }
            ray_t* args[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                if (RAY_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = args[i]; goto out;
                }
            }
            ray_t* result = call_lambda(head, args, argc);
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ray_release(head);
            ret = result; goto out;
        }
        default:
            ray_release(head);
            ret = RAY_ERR_PTR(RAY_ERR_TYPE); goto out;
    }

out:
    eval_depth--;
    return ret;
}

ray_t* ray_eval_str(const char* source) {
    ray_t* parsed = ray_parse(source);
    if (RAY_IS_ERR(parsed)) return parsed;
    ray_t* result = ray_eval(parsed);
    ray_release(parsed);
    return result;
}
