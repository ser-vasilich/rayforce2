#include "lang/eval.h"
#include "lang/env.h"
#include "lang/parse.h"
#include <string.h>

/* ══════════════════════════════════════════
 * Arithmetic builtins
 * ══════════════════════════════════════════ */

static td_t* make_i64(int64_t v) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_I64;
    obj->i64 = v;
    return obj;
}

static td_t* make_f64(double v) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_F64;
    obj->f64 = v;
    return obj;
}

static td_t* make_bool(uint8_t v) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_BOOL;
    obj->b8 = v;
    return obj;
}

/* Helpers to extract numeric value as double */
static int is_numeric(td_t* x) {
    return x->type == TD_ATOM_I64 || x->type == TD_ATOM_F64;
}

static double as_f64(td_t* x) {
    return (x->type == TD_ATOM_F64) ? x->f64 : (double)x->i64;
}

static int is_float_op(td_t* a, td_t* b) {
    return a->type == TD_ATOM_F64 || b->type == TD_ATOM_F64;
}

/* Binary arithmetic */
static td_t* ray_add(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) + as_f64(b));
    return make_i64(a->i64 + b->i64);
}

static td_t* ray_sub(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) - as_f64(b));
    return make_i64(a->i64 - b->i64);
}

static td_t* ray_mul(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) * as_f64(b));
    return make_i64(a->i64 * b->i64);
}

static td_t* ray_div(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) / as_f64(b));
    if (b->i64 == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    return make_i64(a->i64 / b->i64);
}

static td_t* ray_mod(td_t* a, td_t* b) {
    if (a->type != TD_ATOM_I64 || b->type != TD_ATOM_I64)
        return TD_ERR_PTR(TD_ERR_TYPE);
    if (b->i64 == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    return make_i64(a->i64 % b->i64);
}

/* Comparison */
static td_t* ray_gt(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) > as_f64(b) ? 1 : 0);
}

static td_t* ray_lt(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) < as_f64(b) ? 1 : 0);
}

static td_t* ray_gte(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) >= as_f64(b) ? 1 : 0);
}

static td_t* ray_lte(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

static td_t* ray_eq(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
}

static td_t* ray_neq(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
}

/* Logical */
static td_t* ray_and(td_t* a, td_t* b) {
    return make_bool((a->b8 && b->b8) ? 1 : 0);
}

static td_t* ray_or(td_t* a, td_t* b) {
    return make_bool((a->b8 || b->b8) ? 1 : 0);
}

/* Unary */
static td_t* ray_not(td_t* x) {
    return make_bool(x->b8 ? 0 : 1);
}

static td_t* ray_neg(td_t* x) {
    if (x->type == TD_ATOM_I64) return make_i64(-x->i64);
    if (x->type == TD_ATOM_F64) return make_f64(-x->f64);
    return TD_ERR_PTR(TD_ERR_TYPE);
}

/* ══════════════════════════════════════════
 * Error handling: try / raise
 * ══════════════════════════════════════════ */

static _Thread_local td_t *__raise_val = NULL;

/* (raise value) — raise an error with the given value */
static td_t* ray_raise(td_t* val) {
    if (__raise_val) td_release(__raise_val);
    td_retain(val);
    __raise_val = val;
    return TD_ERR_PTR(TD_ERR_DOMAIN);
}

/* Forward declaration for call_lambda (used by ray_try) */
static td_t* call_lambda(td_t* lambda, td_t** call_args, int64_t argc);

/* (try expr handler) — evaluate expr, if error call handler with error value.
 * Special form: receives unevaluated args. */
static td_t* ray_try(td_t* expr, td_t* handler_expr) {
    td_t* result = td_eval(expr);
    if (!TD_IS_ERR(result)) return result;

    /* Get error value (set by raise, or default for runtime errors) */
    td_t* err_val = __raise_val;
    __raise_val = NULL;
    if (!err_val) err_val = make_i64(0);

    /* Evaluate handler expression */
    td_t* handler = td_eval(handler_expr);
    if (TD_IS_ERR(handler)) {
        td_release(err_val);
        return handler;
    }

    /* Call handler with error value */
    td_t* handler_result;
    if (handler->type == TD_ATOM_LAMBDA) {
        td_t* args[1] = { err_val };
        handler_result = call_lambda(handler, args, 1);
    } else if (handler->type == TD_ATOM_UNARY) {
        td_unary_fn fn = (td_unary_fn)(uintptr_t)handler->i64;
        handler_result = fn(err_val);
    } else {
        handler_result = TD_ERR_PTR(TD_ERR_TYPE);
    }

    td_release(err_val);
    td_release(handler);
    return handler_result;
}

/* ══════════════════════════════════════════
 * FN_ATOMIC auto-mapping helpers
 * ══════════════════════════════════════════ */

static int is_list(td_t* x) {
    return x && !TD_IS_ERR(x) && x->type == TD_LIST;
}

/* Map a binary function element-wise over lists.
 * Both args can be lists (zip-map) or one scalar (broadcast). */
static td_t* atomic_map_binary(td_binary_fn fn, td_t* left, td_t* right) {
    int left_list = is_list(left);
    int right_list = is_list(right);

    if (!left_list && !right_list) return fn(left, right);

    int64_t len;
    if (left_list && right_list) {
        len = td_len(left) < td_len(right) ? td_len(left) : td_len(right);
    } else {
        len = left_list ? td_len(left) : td_len(right);
    }

    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = len;
    td_t** out = (td_t**)td_data(result);
    td_t** le = left_list ? (td_t**)td_data(left) : NULL;
    td_t** re = right_list ? (td_t**)td_data(right) : NULL;

    for (int64_t i = 0; i < len; i++) {
        td_t* a = left_list ? le[i] : left;
        td_t* b = right_list ? re[i] : right;
        td_t* elem = fn(a, b);
        if (TD_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) td_release(out[j]);
            td_release(result);
            return elem;
        }
        out[i] = elem;
    }
    return result;
}

/* Map a unary function element-wise over a list. */
static td_t* atomic_map_unary(td_unary_fn fn, td_t* arg) {
    if (!is_list(arg)) return fn(arg);

    int64_t len = td_len(arg);
    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = len;
    td_t** out = (td_t**)td_data(result);
    td_t** elems = (td_t**)td_data(arg);

    for (int64_t i = 0; i < len; i++) {
        td_t* elem = fn(elems[i]);
        if (TD_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) td_release(out[j]);
            td_release(result);
            return elem;
        }
        out[i] = elem;
    }
    return result;
}

/* ══════════════════════════════════════════
 * Aggregation builtins
 * ══════════════════════════════════════════ */

static td_t* ray_sum(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return make_i64(0);
    td_t** elems = (td_t**)td_data(x);
    int has_float = 0;
    double fsum = 0.0;
    int64_t isum = 0;
    for (int64_t i = 0; i < len; i++) {
        if (elems[i]->type == TD_ATOM_F64) { has_float = 1; fsum += elems[i]->f64; }
        else if (elems[i]->type == TD_ATOM_I64) { isum += elems[i]->i64; fsum += (double)elems[i]->i64; }
        else return TD_ERR_PTR(TD_ERR_TYPE);
    }
    return has_float ? make_f64(fsum) : make_i64(isum);
}

static td_t* ray_count(td_t* x) {
    if (x->type == TD_TABLE) return make_i64(td_table_nrows(x));
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_i64(td_len(x));
}

static td_t* ray_avg(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t** elems = (td_t**)td_data(x);
    double sum = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return TD_ERR_PTR(TD_ERR_TYPE);
        sum += as_f64(elems[i]);
    }
    return make_f64(sum / (double)len);
}

static td_t* ray_min(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t** elems = (td_t**)td_data(x);
    if (!is_numeric(elems[0])) return TD_ERR_PTR(TD_ERR_TYPE);
    int has_float = elems[0]->type == TD_ATOM_F64;
    double fmin = as_f64(elems[0]);
    int64_t imin = elems[0]->type == TD_ATOM_I64 ? elems[0]->i64 : 0;
    for (int64_t i = 1; i < len; i++) {
        if (!is_numeric(elems[i])) return TD_ERR_PTR(TD_ERR_TYPE);
        if (elems[i]->type == TD_ATOM_F64) has_float = 1;
        double v = as_f64(elems[i]);
        if (v < fmin) { fmin = v; imin = elems[i]->type == TD_ATOM_I64 ? elems[i]->i64 : 0; }
    }
    return has_float ? make_f64(fmin) : make_i64(imin);
}

static td_t* ray_max(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t** elems = (td_t**)td_data(x);
    if (!is_numeric(elems[0])) return TD_ERR_PTR(TD_ERR_TYPE);
    int has_float = elems[0]->type == TD_ATOM_F64;
    double fmax = as_f64(elems[0]);
    int64_t imax = elems[0]->type == TD_ATOM_I64 ? elems[0]->i64 : 0;
    for (int64_t i = 1; i < len; i++) {
        if (!is_numeric(elems[i])) return TD_ERR_PTR(TD_ERR_TYPE);
        if (elems[i]->type == TD_ATOM_F64) has_float = 1;
        double v = as_f64(elems[i]);
        if (v > fmax) { fmax = v; imax = elems[i]->type == TD_ATOM_I64 ? elems[i]->i64 : 0; }
    }
    return has_float ? make_f64(fmax) : make_i64(imax);
}

static td_t* ray_first(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (td_len(x) == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t* elem = ((td_t**)td_data(x))[0];
    td_retain(elem);
    return elem;
}

static td_t* ray_last(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t* elem = ((td_t**)td_data(x))[len - 1];
    td_retain(elem);
    return elem;
}

static td_t* ray_med(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t** elems = (td_t**)td_data(x);
    /* Allocate scratch as a td_t vector to use td_release for cleanup */
    td_t* scratch = td_alloc(len * sizeof(double));
    if (!scratch) return TD_ERR_PTR(TD_ERR_OOM);
    scratch->type = TD_F64;
    scratch->len = len;
    double* vals = (double*)td_data(scratch);
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) { td_release(scratch); return TD_ERR_PTR(TD_ERR_TYPE); }
        vals[i] = as_f64(elems[i]);
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
    td_release(scratch);
    return make_f64(median);
}

static td_t* ray_dev(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t** elems = (td_t**)td_data(x);
    double sum = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return TD_ERR_PTR(TD_ERR_TYPE);
        sum += as_f64(elems[i]);
    }
    double mean = sum / (double)len;
    double var = 0.0;
    for (int64_t i = 0; i < len; i++) {
        double d = as_f64(elems[i]) - mean;
        var += d * d;
    }
    /* Use sqrt approximation — no math.h dependency */
    double s = var / (double)len;
    /* Newton's method for sqrt */
    if (s == 0.0) return make_f64(0.0);
    double g = s;
    for (int i = 0; i < 50; i++) g = (g + s / g) * 0.5;
    return make_f64(g);
}

/* ══════════════════════════════════════════
 * Higher-order functions: map, pmap, fold, scan, filter, apply
 * ══════════════════════════════════════════ */

/* Helper: call a function object with 1 arg, returning result.
 * Handles UNARY, BINARY, LAMBDA types. Does not release fn or arg. */
static td_t* call_fn1(td_t* fn, td_t* arg) {
    if (fn->type == TD_ATOM_UNARY) {
        td_unary_fn f = (td_unary_fn)(uintptr_t)fn->i64;
        return f(arg);
    }
    if (fn->type == TD_ATOM_LAMBDA) {
        td_t* args[1] = { arg };
        return call_lambda(fn, args, 1);
    }
    return TD_ERR_PTR(TD_ERR_TYPE);
}

/* Helper: call a function object with 2 args. Does not release fn or args. */
static td_t* call_fn2(td_t* fn, td_t* a, td_t* b) {
    if (fn->type == TD_ATOM_BINARY) {
        td_binary_fn f = (td_binary_fn)(uintptr_t)fn->i64;
        return f(a, b);
    }
    if (fn->type == TD_ATOM_LAMBDA) {
        td_t* args[2] = { a, b };
        return call_lambda(fn, args, 2);
    }
    if (fn->type == TD_ATOM_UNARY) {
        /* Partial application not supported, just call with first arg */
        td_unary_fn f = (td_unary_fn)(uintptr_t)fn->i64;
        return f(a);
    }
    return TD_ERR_PTR(TD_ERR_TYPE);
}

/* (map fn val vec) — apply binary fn(val, elem) to each element of vec.
 * Also supports (map fn vec) for unary mapping. */
static td_t* ray_map(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);

    td_t* fn = args[0];

    if (n == 2) {
        /* Unary map: (map fn vec) */
        td_t* vec = args[1];
        if (!is_list(vec)) return TD_ERR_PTR(TD_ERR_TYPE);
        int64_t len = td_len(vec);
        td_t* result = td_alloc(len * sizeof(td_t*));
        if (!result) return TD_ERR_PTR(TD_ERR_OOM);
        result->type = TD_LIST;
        result->len = len;
        td_t** out = (td_t**)td_data(result);
        td_t** elems = (td_t**)td_data(vec);
        for (int64_t i = 0; i < len; i++) {
            out[i] = call_fn1(fn, elems[i]);
            if (TD_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) td_release(out[j]);
                td_release(result);
                return out[i];
            }
        }
        return result;
    }

    /* Binary map: (map fn val vec) — apply fn(val, elem) */
    td_t* val = args[1];
    td_t* vec = args[2];
    if (!is_list(vec)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(vec);
    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = len;
    td_t** out = (td_t**)td_data(result);
    td_t** elems = (td_t**)td_data(vec);
    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, val, elems[i]);
        if (TD_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) td_release(out[j]);
            td_release(result);
            return out[i];
        }
    }
    return result;
}

/* (pmap fn val vec) — same as map, parallel not implemented yet (sequential fallback) */
static td_t* ray_pmap(td_t** args, int64_t n) {
    return ray_map(args, n);
}

/* (fold fn vec) or (fold fn init vec) — reduce with binary fn */
static td_t* ray_fold(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);

    td_t* fn = args[0];
    td_t* vec;
    td_t* acc;
    int free_acc = 0;

    if (n == 2) {
        /* (fold fn vec) — use first element as initial value */
        vec = args[1];
        if (!is_list(vec)) return TD_ERR_PTR(TD_ERR_TYPE);
        int64_t len = td_len(vec);
        if (len == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
        td_t** elems = (td_t**)td_data(vec);
        td_retain(elems[0]);
        acc = elems[0];
        for (int64_t i = 1; i < len; i++) {
            td_t* next = call_fn2(fn, acc, elems[i]);
            td_release(acc);
            if (TD_IS_ERR(next)) return next;
            acc = next;
        }
        return acc;
    }

    /* (fold fn init vec) */
    td_retain(args[1]);
    acc = args[1];
    vec = args[2];
    if (!is_list(vec)) { td_release(acc); return TD_ERR_PTR(TD_ERR_TYPE); }
    int64_t len = td_len(vec);
    td_t** elems = (td_t**)td_data(vec);
    for (int64_t i = 0; i < len; i++) {
        td_t* next = call_fn2(fn, acc, elems[i]);
        td_release(acc);
        if (TD_IS_ERR(next)) return next;
        acc = next;
    }
    return acc;
}

/* (scan fn vec) — running fold, returns vector of partial results */
static td_t* ray_scan(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);

    td_t* fn = args[0];
    td_t* vec = args[1];
    if (!is_list(vec)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(vec);
    if (len == 0) {
        td_t* result = td_alloc(0);
        if (!result) return TD_ERR_PTR(TD_ERR_OOM);
        result->type = TD_LIST;
        result->len = 0;
        return result;
    }

    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = len;
    td_t** out = (td_t**)td_data(result);
    td_t** elems = (td_t**)td_data(vec);

    td_retain(elems[0]);
    out[0] = elems[0];
    for (int64_t i = 1; i < len; i++) {
        out[i] = call_fn2(fn, out[i - 1], elems[i]);
        if (TD_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) td_release(out[j]);
            td_release(result);
            return out[i];
        }
    }
    return result;
}

/* (filter vec mask) — filter vector by boolean mask */
static td_t* ray_filter(td_t* vec, td_t* mask) {
    if (!is_list(vec) || !is_list(mask)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(vec);
    int64_t mlen = td_len(mask);
    if (len != mlen) return TD_ERR_PTR(TD_ERR_DOMAIN);

    td_t** velems = (td_t**)td_data(vec);
    td_t** melems = (td_t**)td_data(mask);

    /* Count true values */
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type == TD_ATOM_BOOL && melems[i]->b8) count++;
    }

    td_t* result = td_alloc(count * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = count;
    td_t** out = (td_t**)td_data(result);
    int64_t j = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type == TD_ATOM_BOOL && melems[i]->b8) {
            td_retain(velems[i]);
            out[j++] = velems[i];
        }
    }
    return result;
}

/* (apply fn vec1 vec2) — zip-apply fn element-wise over two vectors */
static td_t* ray_apply(td_t** args, int64_t n) {
    if (n < 3) return TD_ERR_PTR(TD_ERR_DOMAIN);

    td_t* fn = args[0];
    td_t* vec1 = args[1];
    td_t* vec2 = args[2];
    if (!is_list(vec1) || !is_list(vec2)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len1 = td_len(vec1);
    int64_t len2 = td_len(vec2);
    int64_t len = len1 < len2 ? len1 : len2;

    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = len;
    td_t** out = (td_t**)td_data(result);
    td_t** e1 = (td_t**)td_data(vec1);
    td_t** e2 = (td_t**)td_data(vec2);

    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, e1[i], e2[i]);
        if (TD_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) td_release(out[j]);
            td_release(result);
            return out[i];
        }
    }
    return result;
}

/* ══════════════════════════════════════════
 * Collection operations
 * ══════════════════════════════════════════ */

/* Helper: compare two atoms for equality (value-based) */
static int atom_eq(td_t* a, td_t* b) {
    if (a->type != b->type) {
        if (is_numeric(a) && is_numeric(b))
            return as_f64(a) == as_f64(b);
        return 0;
    }
    switch (a->type) {
    case TD_ATOM_I64:  return a->i64 == b->i64;
    case TD_ATOM_F64:  return a->f64 == b->f64;
    case TD_ATOM_BOOL: return a->b8 == b->b8;
    case TD_ATOM_SYM:  return a->i64 == b->i64;
    default: return 0;
    }
}

/* (distinct vec) — remove duplicates, preserving first occurrence */
static td_t* ray_distinct(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    if (len == 0) { td_retain(x); return x; }
    td_t** elems = (td_t**)td_data(x);

    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    td_t** out = (td_t**)td_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len; i++) {
        int dup = 0;
        for (int64_t j = 0; j < count; j++) {
            if (atom_eq(out[j], elems[i])) { dup = 1; break; }
        }
        if (!dup) {
            td_retain(elems[i]);
            out[count++] = elems[i];
        }
    }
    result->len = count;
    return result;
}

/* (in val vec) — check membership */
static td_t* ray_in(td_t* val, td_t* vec) {
    if (!is_list(vec)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(vec);
    td_t** elems = (td_t**)td_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(val, elems[i])) return make_bool(1);
    }
    return make_bool(0);
}

/* (except vec1 vec2) — elements in vec1 not in vec2 */
static td_t* ray_except(td_t* vec1, td_t* vec2) {
    if (!is_list(vec1) || !is_list(vec2)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len1 = td_len(vec1);
    int64_t len2 = td_len(vec2);
    td_t** e1 = (td_t**)td_data(vec1);
    td_t** e2 = (td_t**)td_data(vec2);

    td_t* result = td_alloc(len1 * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    td_t** out = (td_t**)td_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len1; i++) {
        int found = 0;
        for (int64_t j = 0; j < len2; j++) {
            if (atom_eq(e1[i], e2[j])) { found = 1; break; }
        }
        if (!found) {
            td_retain(e1[i]);
            out[count++] = e1[i];
        }
    }
    result->len = count;
    return result;
}

/* (union vec1 vec2) — elements in vec1 + elements in vec2 not already in vec1 */
static td_t* ray_union(td_t* vec1, td_t* vec2) {
    if (!is_list(vec1) || !is_list(vec2)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len1 = td_len(vec1);
    int64_t len2 = td_len(vec2);
    td_t** e1 = (td_t**)td_data(vec1);
    td_t** e2 = (td_t**)td_data(vec2);

    td_t* result = td_alloc((len1 + len2) * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    td_t** out = (td_t**)td_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len1; i++) {
        td_retain(e1[i]);
        out[count++] = e1[i];
    }
    for (int64_t i = 0; i < len2; i++) {
        int found = 0;
        for (int64_t j = 0; j < count; j++) {
            if (atom_eq(out[j], e2[i])) { found = 1; break; }
        }
        if (!found) {
            td_retain(e2[i]);
            out[count++] = e2[i];
        }
    }
    result->len = count;
    return result;
}

/* (sect vec1 vec2) — intersection: elements in both */
static td_t* ray_sect(td_t* vec1, td_t* vec2) {
    if (!is_list(vec1) || !is_list(vec2)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len1 = td_len(vec1);
    int64_t len2 = td_len(vec2);
    td_t** e1 = (td_t**)td_data(vec1);
    td_t** e2 = (td_t**)td_data(vec2);

    td_t* result = td_alloc(len1 * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    td_t** out = (td_t**)td_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len1; i++) {
        for (int64_t j = 0; j < len2; j++) {
            if (atom_eq(e1[i], e2[j])) {
                td_retain(e1[i]);
                out[count++] = e1[i];
                break;
            }
        }
    }
    result->len = count;
    return result;
}

/* (take vec n) — first n elements (positive) or last |n| elements (negative) */
static td_t* ray_take(td_t* vec, td_t* n_obj) {
    if (!is_list(vec) || n_obj->type != TD_ATOM_I64)
        return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(vec);
    int64_t n = n_obj->i64;
    td_t** elems = (td_t**)td_data(vec);

    int64_t start, count;
    if (n >= 0) {
        start = 0;
        count = n < len ? n : len;
    } else {
        count = -n < len ? -n : len;
        start = len - count;
    }

    td_t* result = td_alloc(count * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = count;
    td_t** out = (td_t**)td_data(result);
    for (int64_t i = 0; i < count; i++) {
        td_retain(elems[start + i]);
        out[i] = elems[start + i];
    }
    return result;
}

/* (at vec idx) or (at table 'col) — index into vector or table */
static td_t* ray_at(td_t* vec, td_t* idx) {
    /* Table column access by symbol key */
    if (vec->type == TD_TABLE && idx->type == TD_ATOM_SYM) {
        td_t* col = td_table_get_col(vec, idx->i64);
        if (!col) return TD_ERR_PTR(TD_ERR_DOMAIN);
        /* Convert typed column vector to a Rayfall list */
        int64_t nrows = col->len;
        td_t* result = td_alloc(nrows * sizeof(td_t*));
        if (!result) return TD_ERR_PTR(TD_ERR_OOM);
        result->type = TD_LIST;
        result->len = nrows;
        td_t** out = (td_t**)td_data(result);
        int8_t ctype = col->type;
        for (int64_t i = 0; i < nrows; i++) {
            if (ctype == TD_I64) {
                out[i] = make_i64(((int64_t*)td_data(col))[i]);
            } else if (ctype == TD_F64) {
                out[i] = make_f64(((double*)td_data(col))[i]);
            } else if (ctype == TD_BOOL) {
                out[i] = make_bool(((uint8_t*)td_data(col))[i]);
            } else if (ctype == TD_SYM) {
                td_t* s = td_alloc(0);
                if (!s) { td_release(result); return TD_ERR_PTR(TD_ERR_OOM); }
                s->type = TD_ATOM_SYM;
                s->i64 = ((int64_t*)td_data(col))[i];
                out[i] = s;
            } else {
                out[i] = make_i64(0);
            }
            if (TD_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) td_release(out[j]);
                td_release(result);
                return out[i];
            }
        }
        return result;
    }

    if (!is_list(vec) || idx->type != TD_ATOM_I64)
        return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t i = idx->i64;
    int64_t len = td_len(vec);
    if (i < 0 || i >= len) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t* elem = ((td_t**)td_data(vec))[i];
    td_retain(elem);
    return elem;
}

/* (find vec val) — index of first occurrence, or -1 */
static td_t* ray_find(td_t* vec, td_t* val) {
    if (!is_list(vec)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(vec);
    td_t** elems = (td_t**)td_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(elems[i], val)) return make_i64(i);
    }
    return make_i64(-1);
}

/* (reverse vec) — reverse a vector */
static td_t* ray_reverse(td_t* x) {
    if (!is_list(x)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t len = td_len(x);
    td_t** elems = (td_t**)td_data(x);

    td_t* result = td_alloc(len * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = len;
    td_t** out = (td_t**)td_data(result);
    for (int64_t i = 0; i < len; i++) {
        td_retain(elems[len - 1 - i]);
        out[i] = elems[len - 1 - i];
    }
    return result;
}

/* ══════════════════════════════════════════
 * Table construction and access
 * ══════════════════════════════════════════ */

/* (list v1 v2 ...) — package args into a list */
static td_t* ray_list(td_t** args, int64_t n) {
    td_t* result = td_alloc(n * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = n;
    td_t** out = (td_t**)td_data(result);
    for (int64_t i = 0; i < n; i++) {
        td_retain(args[i]);
        out[i] = args[i];
    }
    return result;
}

/* (table [col_names] (list col1 col2 ...)) — build a TD_TABLE */
static td_t* ray_table(td_t* names, td_t* cols) {
    if (!is_list(names) || !is_list(cols)) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t ncols = td_len(names);
    if (td_len(cols) != ncols) return TD_ERR_PTR(TD_ERR_DOMAIN);

    td_t** name_elems = (td_t**)td_data(names);
    td_t** col_elems = (td_t**)td_data(cols);

    td_t* tbl = td_table_new(ncols);
    if (TD_IS_ERR(tbl)) return tbl;

    for (int64_t i = 0; i < ncols; i++) {
        if (name_elems[i]->type != TD_ATOM_SYM)
            { td_release(tbl); return TD_ERR_PTR(TD_ERR_TYPE); }
        int64_t name_id = name_elems[i]->i64;

        /* Convert Rayfall list to typed column vector */
        td_t* col_list = col_elems[i];
        if (!is_list(col_list))
            { td_release(tbl); return TD_ERR_PTR(TD_ERR_TYPE); }
        int64_t nrows = td_len(col_list);
        td_t** row_elems = (td_t**)td_data(col_list);

        /* Determine column type from first element */
        int8_t col_type = TD_I64;
        if (nrows > 0) {
            if (row_elems[0]->type == TD_ATOM_F64) col_type = TD_F64;
            else if (row_elems[0]->type == TD_ATOM_BOOL) col_type = TD_BOOL;
            else if (row_elems[0]->type == TD_ATOM_SYM) col_type = TD_SYM;
        }

        td_t* col_vec = td_vec_new(col_type, nrows);
        if (TD_IS_ERR(col_vec))
            { td_release(tbl); return col_vec; }

        for (int64_t j = 0; j < nrows; j++) {
            void* val_ptr;
            if (col_type == TD_I64) val_ptr = &row_elems[j]->i64;
            else if (col_type == TD_F64) val_ptr = &row_elems[j]->f64;
            else if (col_type == TD_BOOL) val_ptr = &row_elems[j]->b8;
            else val_ptr = &row_elems[j]->i64; /* SYM stored as i64 */
            col_vec = td_vec_append(col_vec, val_ptr);
            if (TD_IS_ERR(col_vec))
                { td_release(tbl); return col_vec; }
        }

        tbl = td_table_add_col(tbl, name_id, col_vec);
        td_release(col_vec);
        if (TD_IS_ERR(tbl)) return tbl;
    }

    return tbl;
}

/* (key table) — return column names as a list of symbols */
static td_t* ray_key(td_t* x) {
    if (x->type != TD_TABLE) return TD_ERR_PTR(TD_ERR_TYPE);
    int64_t ncols = td_table_ncols(x);
    td_t* result = td_alloc(ncols * sizeof(td_t*));
    if (!result) return TD_ERR_PTR(TD_ERR_OOM);
    result->type = TD_LIST;
    result->len = ncols;
    td_t** out = (td_t**)td_data(result);
    for (int64_t i = 0; i < ncols; i++) {
        int64_t name_id = td_table_col_name(x, i);
        td_t* sym = td_alloc(0);
        if (!sym) { td_release(result); return TD_ERR_PTR(TD_ERR_OOM); }
        sym->type = TD_ATOM_SYM;
        sym->i64 = name_id;
        out[i] = sym;
    }
    return result;
}

/* (value dict) — placeholder for dict value extraction */
static td_t* ray_value(td_t* x) {
    (void)x;
    return TD_ERR_PTR(TD_ERR_NYI);
}

/* ══════════════════════════════════════════
 * Special forms: set, let, if, do
 * ══════════════════════════════════════════ */

/* (set name value) — bind in global env. Receives unevaluated args. */
static td_t* ray_set(td_t* name_obj, td_t* val_expr) {
    if (name_obj->type != TD_ATOM_SYM)
        return TD_ERR_PTR(TD_ERR_TYPE);
    td_t* val = td_eval(val_expr);
    if (TD_IS_ERR(val)) return val;
    td_env_set(name_obj->i64, val);
    return val;  /* set returns the value */
}

/* (let name value) — bind in local scope. Receives unevaluated args. */
static td_t* ray_let(td_t* name_obj, td_t* val_expr) {
    if (name_obj->type != TD_ATOM_SYM)
        return TD_ERR_PTR(TD_ERR_TYPE);
    td_t* val = td_eval(val_expr);
    if (TD_IS_ERR(val)) return val;
    td_env_set_local(name_obj->i64, val);
    return val;
}

/* (if cond then else?) — conditional. Receives unevaluated args. */
static td_t* ray_cond(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t* cond = td_eval(args[0]);
    if (TD_IS_ERR(cond)) return cond;
    int truthy = 0;
    if (cond->type == TD_ATOM_BOOL) truthy = cond->b8;
    else if (cond->type == TD_ATOM_I64) truthy = cond->i64 != 0;
    else truthy = 1;  /* non-null is truthy */
    td_release(cond);
    if (truthy) return td_eval(args[1]);
    if (n >= 3) return td_eval(args[2]);
    /* No else branch: return 0 */
    return make_i64(0);
}

/* (do expr1 expr2 ...) — evaluate in sequence, return last. Pushes local scope. */
static td_t* ray_do(td_t** args, int64_t n) {
    if (n == 0) return make_i64(0);
    td_env_push_scope();
    td_t* result = NULL;
    for (int64_t i = 0; i < n; i++) {
        if (result) td_release(result);
        result = td_eval(args[i]);
        if (TD_IS_ERR(result)) {
            td_env_pop_scope();
            return result;
        }
    }
    td_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Lambda functions
 * ══════════════════════════════════════════ */

/* (fn [params...] body...) — create a lambda object.
 * Stores params list and body expressions in data area. */
static td_t* ray_fn(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);
    /* args[0] = param vector (list of name symbols), args[1..n-1] = body exprs */
    td_t* params_list = args[0];

    /* Create lambda object with space for 5 slots:
     * [0] params, [1] body, [2] bytecode, [3] constants, [4] n_locals */
    td_t* lambda = td_alloc(5 * sizeof(td_t*));
    if (!lambda) return TD_ERR_PTR(TD_ERR_OOM);
    lambda->type = TD_ATOM_LAMBDA;
    lambda->attrs = 0;
    lambda->len = 0;

    /* Store params list */
    td_retain(params_list);
    LAMBDA_PARAMS(lambda) = params_list;

    /* Build body list: wrap body expressions in a TD_LIST */
    int64_t body_count = n - 1;
    td_t* body = td_alloc(body_count * sizeof(td_t*));
    if (!body) {
        td_release(params_list);
        td_release(lambda);
        return TD_ERR_PTR(TD_ERR_OOM);
    }
    body->type = TD_LIST;
    body->len = body_count;
    td_t** body_elems = (td_t**)td_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        td_retain(args[i + 1]);
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
static td_t* vm_exec(td_t* lambda, td_t** call_args, int64_t argc);

/* Call a lambda: compile on first call, then execute bytecode. */
static td_t* call_lambda(td_t* lambda, td_t** call_args, int64_t argc) {
    /* Lazy compilation on first call */
    if (!LAMBDA_IS_COMPILED(lambda)) {
        td_compile(lambda);
    }

    /* If compilation succeeded, run bytecode; otherwise fall back to tree-walk */
    if (LAMBDA_IS_COMPILED(lambda)) {
        return vm_exec(lambda, call_args, argc);
    }

    /* Fallback: tree-walking interpreter */
    td_t* params_list = LAMBDA_PARAMS(lambda);
    td_t* body = LAMBDA_BODY(lambda);

    int64_t param_count = td_len(params_list);
    td_t** param_syms = (td_t**)td_data(params_list);

    td_env_push_scope();

    for (int64_t i = 0; i < param_count && i < argc; i++) {
        td_env_set_local(param_syms[i]->i64, call_args[i]);
    }

    int64_t body_count = td_len(body);
    td_t** body_exprs = (td_t**)td_data(body);
    td_t* result = NULL;
    for (int64_t i = 0; i < body_count; i++) {
        if (result) td_release(result);
        result = td_eval(body_exprs[i]);
        if (TD_IS_ERR(result)) {
            td_env_pop_scope();
            return result;
        }
    }

    td_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Stack-based VM executor (computed goto, frame-based)
 * ══════════════════════════════════════════ */

static _Thread_local td_vm_t *__VM = NULL;

static td_t* vm_exec(td_t* lambda, td_t** call_args, int64_t argc) {
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
        [OP_TRAP]       = &&op_trap,
        [OP_TRAP_END]   = &&op_trap_end,
    };

    td_vm_t vm;
    memset(&vm, 0, sizeof(vm));
    __VM = &vm;

    /* Set up initial frame */
    vm.fn = lambda;
    td_retain(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    vm.fp = 0;
    vm.sp = n_locals;

    /* Bind parameters into local slots */
    int64_t param_count = td_len(LAMBDA_PARAMS(lambda));
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        td_retain(call_args[i]);
        vm.ps[i] = call_args[i];
    }

    uint8_t *code = (uint8_t *)td_data(LAMBDA_BC(lambda));
    td_t **cpool = (td_t **)td_data(LAMBDA_CONSTS(lambda));
    int32_t ip = 0;

#define DISPATCH() goto *dispatch[code[ip++]]
#define PUSH(v)    (vm.ps[vm.sp++] = (v))
#define POP()      (vm.ps[--vm.sp])
#define PEEK()     (vm.ps[vm.sp - 1])
#define LOCAL(s)   (vm.ps[vm.fp + (s)])

    DISPATCH();

op_loadconst: {
    uint8_t idx = code[ip++];
    td_t *val = cpool[idx];
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadconst_w: {
    uint16_t idx = (uint16_t)(code[ip] << 8) | code[ip + 1];
    ip += 2;
    td_t *val = cpool[idx];
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadenv: {
    uint8_t slot = code[ip++];
    td_t *val = LOCAL(slot);
    if (val) td_retain(val);
    else val = make_i64(0);
    PUSH(val);
    DISPATCH();
}

op_storeenv: {
    uint8_t slot = code[ip++];
    td_t *val = POP();
    if (LOCAL(slot)) td_release(LOCAL(slot));
    LOCAL(slot) = val;
    DISPATCH();
}

op_pop: {
    if (vm.sp > vm.fp + n_locals) {
        td_t *val = POP();
        if (val) td_release(val);
    }
    DISPATCH();
}

op_dup: {
    td_t *val = PEEK();
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_resolve: {
    uint8_t idx = code[ip++];
    td_t *name_obj = cpool[idx];
    td_t *val = td_env_get(name_obj->i64);
    if (!val) goto vm_error;
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_jmp: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ip += offset;
    DISPATCH();
}

op_jmpf: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    td_t *cond = POP();
    int truthy = 0;
    if (cond->type == TD_ATOM_BOOL) truthy = cond->b8;
    else if (cond->type == TD_ATOM_I64) truthy = cond->i64 != 0;
    else truthy = 1;
    td_release(cond);
    if (!truthy) ip += offset;
    DISPATCH();
}

op_call1: {
    td_t *arg = POP();
    td_t *fn_obj = POP();
    td_unary_fn fn = (td_unary_fn)(uintptr_t)fn_obj->i64;
    td_t *result = fn(arg);
    td_release(arg);
    td_release(fn_obj);
    PUSH(result);
    DISPATCH();
}

op_call2: {
    td_t *right = POP();
    td_t *left = POP();
    td_t *fn_obj = POP();
    td_binary_fn fn = (td_binary_fn)(uintptr_t)fn_obj->i64;
    td_t *result = fn(left, right);
    td_release(left);
    td_release(right);
    td_release(fn_obj);
    PUSH(result);
    DISPATCH();
}

op_calln: {
    uint8_t n = code[ip++];
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();
    td_vary_fn fn = (td_vary_fn)(uintptr_t)fn_obj->i64;
    td_t *result = fn(fn_args, n);
    for (int32_t i = 0; i < n; i++)
        td_release(fn_args[i]);
    td_release(fn_obj);
    PUSH(result);
    DISPATCH();
}

op_callf: {
    uint8_t n = code[ip++];
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();

    /* Compiled lambda: push frame, switch to callee bytecode */
    if (fn_obj->type == TD_ATOM_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            td_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* Push return frame */
            vm.rs[vm.rp++] = (vm_ctx_t){ .fn = vm.fn, .fp = vm.fp, .ip = ip };

            /* Set up new frame */
            vm.fn = fn_obj;  /* takes ownership of stack ref */
            vm.fp = vm.sp;
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            vm.sp += callee_locals;
            n_locals = callee_locals;

            /* Bind parameters */
            int64_t pcnt = td_len(LAMBDA_PARAMS(fn_obj));
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];  /* transfer ownership from args */
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                td_release(fn_args[i]);  /* excess args */

            /* Switch to callee bytecode */
            code = (uint8_t *)td_data(LAMBDA_BC(fn_obj));
            cpool = (td_t **)td_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Non-lambda or uncompiled: dispatch by type */
    {
        td_t *result;
        switch (fn_obj->type) {
        case TD_ATOM_UNARY:
            result = ((td_unary_fn)(uintptr_t)fn_obj->i64)(fn_args[0]);
            td_release(fn_args[0]);
            for (int32_t i = 1; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_BINARY:
            result = ((td_binary_fn)(uintptr_t)fn_obj->i64)(fn_args[0], fn_args[1]);
            td_release(fn_args[0]);
            td_release(fn_args[1]);
            for (int32_t i = 2; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_VARY:
            result = ((td_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_LAMBDA:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        default:
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            result = TD_ERR_PTR(TD_ERR_TYPE);
            break;
        }
        td_release(fn_obj);
        PUSH(result);
        DISPATCH();
    }
}

op_calls: {
    /* Tail call: reuse current frame (no return stack push) */
    uint8_t n = code[ip++];
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();

    if (fn_obj->type == TD_ATOM_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            td_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* Clean up current frame locals */
            for (int32_t i = 0; i < n_locals; i++)
                if (LOCAL(i)) { td_release(LOCAL(i)); LOCAL(i) = NULL; }

            /* Reuse frame: reset sp to fp, don't push return context */
            vm.sp = vm.fp;
            td_release(vm.fn);
            vm.fn = fn_obj;  /* takes ownership */
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            vm.sp += callee_locals;
            n_locals = callee_locals;

            int64_t pcnt = td_len(LAMBDA_PARAMS(fn_obj));
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                td_release(fn_args[i]);

            code = (uint8_t *)td_data(LAMBDA_BC(fn_obj));
            cpool = (td_t **)td_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Fallback: same as CALLF non-lambda path */
    {
        td_t *result;
        switch (fn_obj->type) {
        case TD_ATOM_UNARY:
            result = ((td_unary_fn)(uintptr_t)fn_obj->i64)(fn_args[0]);
            td_release(fn_args[0]);
            for (int32_t i = 1; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_BINARY:
            result = ((td_binary_fn)(uintptr_t)fn_obj->i64)(fn_args[0], fn_args[1]);
            td_release(fn_args[0]);
            td_release(fn_args[1]);
            for (int32_t i = 2; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_VARY:
            result = ((td_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        default:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        }
        td_release(fn_obj);
        PUSH(result);
        DISPATCH();
    }
}

op_calld: {
    /* Dynamic dispatch: evaluate AST directly via td_eval */
    uint8_t n = code[ip++];
    if (n == 0) {
        /* n=0: the AST itself is on the stack, eval it directly */
        td_t *ast = POP();
        td_t *result = td_eval(ast);
        td_release(ast);
        PUSH(result);
        DISPATCH();
    }
    /* n>0: build call list and eval */
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();

    td_t *call_list = td_alloc((n + 1) * sizeof(td_t *));
    call_list->type = TD_LIST;
    call_list->len = n + 1;
    td_t **elems = (td_t **)td_data(call_list);
    elems[0] = fn_obj;
    for (int32_t i = 0; i < n; i++)
        elems[i + 1] = fn_args[i];

    td_t *result = td_eval(call_list);
    td_release(call_list);
    PUSH(result);
    DISPATCH();
}

op_ret: {
    td_t *result = (vm.sp > vm.fp + n_locals) ? POP() : make_i64(0);
    td_retain(result);  /* protect from cleanup aliasing */

    /* Clean up current frame */
    while (vm.sp > vm.fp) {
        td_t *v = vm.ps[--vm.sp];
        if (v) td_release(v);
    }

    if (vm.rp == 0) {
        /* Top-level return */
        td_release(vm.fn);
        __VM = NULL;
        return result;  /* caller owns the retain */
    }

    /* Pop return frame */
    td_release(vm.fn);
    vm.rp--;
    vm.fn = vm.rs[vm.rp].fn;
    vm.fp = vm.rs[vm.rp].fp;
    ip = vm.rs[vm.rp].ip;
    code = (uint8_t *)td_data(LAMBDA_BC(vm.fn));
    cpool = (td_t **)td_data(LAMBDA_CONSTS(vm.fn));
    n_locals = LAMBDA_NLOCALS(vm.fn);
    PUSH(result);
    DISPATCH();
}

op_trap: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    vm.ts[vm.tp++] = (vm_trap_t){
        .rp = vm.rp, .sp = vm.sp, .handler_ip = ip + offset,
        .fn = vm.fn, .fp = vm.fp, .n_locals = n_locals
    };
    td_retain(vm.fn);
    DISPATCH();
}

op_trap_end: {
    if (vm.tp > 0) {
        vm.tp--;
        td_release(vm.ts[vm.tp].fn);
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
            if (vm.rs[vm.rp].fn) td_release(vm.rs[vm.rp].fn);
        }

        /* Clean up stack above trap point */
        while (vm.sp > trap.sp) {
            td_t *v = vm.ps[--vm.sp];
            if (v) td_release(v);
        }

        /* Get error value */
        td_t *err_val = __raise_val;
        __raise_val = NULL;
        if (!err_val) err_val = make_i64(0);

        /* Restore context and push error value */
        td_release(vm.fn);
        vm.fn = trap.fn;  /* takes ownership from trap frame */
        vm.fp = trap.fp;
        n_locals = trap.n_locals;
        code = (uint8_t *)td_data(LAMBDA_BC(vm.fn));
        cpool = (td_t **)td_data(LAMBDA_CONSTS(vm.fn));
        ip = trap.handler_ip;
        PUSH(err_val);
        DISPATCH();
    }

    /* No trap frame — regular error cleanup */
    for (int32_t i = 0; i < vm.sp; i++)
        if (vm.ps[i]) td_release(vm.ps[i]);
    td_release(vm.fn);
    for (int32_t i = 0; i < vm.rp; i++)
        td_release(vm.rs[i].fn);
    for (int32_t i = 0; i < vm.tp; i++)
        td_release(vm.ts[i].fn);
    __VM = NULL;
    return TD_ERR_PTR(TD_ERR_DOMAIN);
}

#undef DISPATCH
#undef PUSH
#undef POP
#undef PEEK
#undef LOCAL
}

/* ══════════════════════════════════════════
 * Builtin registration
 * ══════════════════════════════════════════ */

static void register_binary(const char* name, uint8_t attrs, td_binary_fn fn) {
    int64_t sym = td_sym_intern(name, strlen(name));
    td_t* obj = td_fn_binary(name, attrs, fn);
    td_env_set(sym, obj);
    td_release(obj);
}

static void register_unary(const char* name, uint8_t attrs, td_unary_fn fn) {
    int64_t sym = td_sym_intern(name, strlen(name));
    td_t* obj = td_fn_unary(name, attrs, fn);
    td_env_set(sym, obj);
    td_release(obj);
}

static void register_vary(const char* name, uint8_t attrs, td_vary_fn fn) {
    int64_t sym = td_sym_intern(name, strlen(name));
    td_t* obj = td_fn_vary(name, attrs, fn);
    td_env_set(sym, obj);
    td_release(obj);
}

static void td_register_builtins(void) {
    register_binary("+",   TD_FN_ATOMIC, ray_add);
    register_binary("-",   TD_FN_ATOMIC, ray_sub);
    register_binary("*",   TD_FN_ATOMIC, ray_mul);
    register_binary("/",   TD_FN_ATOMIC, ray_div);
    register_binary("%",   TD_FN_ATOMIC, ray_mod);
    register_binary(">",   TD_FN_NONE,   ray_gt);
    register_binary("<",   TD_FN_NONE,   ray_lt);
    register_binary(">=",  TD_FN_NONE,   ray_gte);
    register_binary("<=",  TD_FN_NONE,   ray_lte);
    register_binary("==",  TD_FN_NONE,   ray_eq);
    register_binary("!=",  TD_FN_NONE,   ray_neq);
    register_binary("and", TD_FN_NONE,   ray_and);
    register_binary("or",  TD_FN_NONE,   ray_or);
    register_unary("not",  TD_FN_NONE,   ray_not);
    register_unary("neg",  TD_FN_ATOMIC, ray_neg);

    /* Special forms */
    register_binary("set", TD_FN_SPECIAL_FORM, ray_set);
    register_binary("let", TD_FN_SPECIAL_FORM, ray_let);
    register_vary("if",    TD_FN_SPECIAL_FORM, ray_cond);
    register_vary("do",    TD_FN_SPECIAL_FORM, ray_do);
    register_vary("fn",    TD_FN_SPECIAL_FORM, ray_fn);

    /* Aggregation builtins */
    register_unary("sum",   TD_FN_AGGR, ray_sum);
    register_unary("count", TD_FN_AGGR, ray_count);
    register_unary("avg",   TD_FN_AGGR, ray_avg);
    register_unary("min",   TD_FN_AGGR, ray_min);
    register_unary("max",   TD_FN_AGGR, ray_max);
    register_unary("first", TD_FN_NONE, ray_first);
    register_unary("last",  TD_FN_NONE, ray_last);
    register_unary("med",   TD_FN_AGGR, ray_med);
    register_unary("dev",   TD_FN_AGGR, ray_dev);

    /* Error handling */
    register_unary("raise", TD_FN_NONE, ray_raise);
    register_binary("try",  TD_FN_SPECIAL_FORM, ray_try);

    /* Higher-order functions */
    register_vary("map",    TD_FN_NONE, ray_map);
    register_vary("pmap",   TD_FN_NONE, ray_pmap);
    register_vary("fold",   TD_FN_NONE, ray_fold);
    register_vary("scan",   TD_FN_NONE, ray_scan);
    register_binary("filter", TD_FN_NONE, ray_filter);
    register_vary("apply",  TD_FN_NONE, ray_apply);

    /* Collection operations */
    register_unary("distinct", TD_FN_NONE, ray_distinct);
    register_binary("in",      TD_FN_NONE, ray_in);
    register_binary("except",  TD_FN_NONE, ray_except);
    register_binary("union",   TD_FN_NONE, ray_union);
    register_binary("sect",    TD_FN_NONE, ray_sect);
    register_binary("take",    TD_FN_NONE, ray_take);
    register_binary("at",      TD_FN_NONE, ray_at);
    register_binary("find",    TD_FN_NONE, ray_find);
    register_unary("reverse",  TD_FN_NONE, ray_reverse);

    /* Table operations */
    register_vary("list",      TD_FN_NONE, ray_list);
    register_binary("table",   TD_FN_NONE, ray_table);
    register_unary("key",      TD_FN_NONE, ray_key);
    register_unary("value",    TD_FN_NONE, ray_value);
}

/* ══════════════════════════════════════════
 * Runtime lifecycle
 * ══════════════════════════════════════════ */

td_err_t td_lang_init(void) {
    td_err_t err = td_env_init();
    if (err != TD_OK) return err;
    td_register_builtins();
    return TD_OK;
}

void td_lang_destroy(void) {
    td_env_destroy();
}

/* ══════════════════════════════════════════
 * Tree-walking evaluator
 * ══════════════════════════════════════════ */

td_t* td_eval(td_t* obj) {
    if (!obj || TD_IS_ERR(obj)) return obj;

    /* Atoms: return themselves (retain) */
    if (td_is_atom(obj)) {
        /* Name reference: resolve from env */
        if (obj->type == TD_ATOM_SYM && (obj->attrs & TD_ATTR_NAME)) {
            td_t* val = td_env_get(obj->i64);
            if (!val) return TD_ERR_PTR(TD_ERR_DOMAIN);
            td_retain(val);
            return val;
        }
        td_retain(obj);
        return obj;
    }

    /* Non-list vectors: return themselves */
    if (obj->type != TD_LIST) { td_retain(obj); return obj; }

    /* Empty list */
    if (td_len(obj) == 0) { td_retain(obj); return obj; }

    /* Vector literal [x y z]: evaluate each element, return as data list */
    if (obj->attrs & TD_ATTR_VECTOR) {
        int64_t len = td_len(obj);
        td_t** src = (td_t**)td_data(obj);
        td_t* result = td_alloc(len * sizeof(td_t*));
        if (!result) return TD_ERR_PTR(TD_ERR_OOM);
        result->type = TD_LIST;
        result->attrs = TD_ATTR_VECTOR;
        result->len = len;
        td_t** dst = (td_t**)td_data(result);
        for (int64_t i = 0; i < len; i++) {
            dst[i] = td_eval(src[i]);
            if (TD_IS_ERR(dst[i])) {
                for (int64_t j = 0; j < i; j++) td_release(dst[j]);
                td_release(result);
                return dst[i];
            }
        }
        return result;
    }

    /* List: evaluate first element, dispatch by type */
    td_t** elems = (td_t**)td_data(obj);
    td_t* head = td_eval(elems[0]);
    if (TD_IS_ERR(head)) return head;

    int64_t n = td_len(obj);

    switch (head->type) {
        case TD_ATOM_UNARY: {
            if (n < 2) { td_release(head); return TD_ERR_PTR(TD_ERR_DOMAIN); }
            td_unary_fn fn = (td_unary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            td_t* arg = td_eval(elems[1]);
            td_release(head);
            if (TD_IS_ERR(arg)) return arg;
            td_t* result;
            if ((fn_attrs & TD_FN_ATOMIC) && is_list(arg))
                result = atomic_map_unary(fn, arg);
            else
                result = fn(arg);
            td_release(arg);
            return result;
        }
        case TD_ATOM_BINARY: {
            if (n < 3) { td_release(head); return TD_ERR_PTR(TD_ERR_DOMAIN); }
            td_binary_fn fn = (td_binary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            if (fn_attrs & TD_FN_SPECIAL_FORM) {
                td_release(head);
                return fn(elems[1], elems[2]);
            }
            td_t* left = td_eval(elems[1]);
            if (TD_IS_ERR(left)) { td_release(head); return left; }
            td_t* right = td_eval(elems[2]);
            if (TD_IS_ERR(right)) { td_release(head); td_release(left); return right; }
            td_release(head);
            td_t* result;
            if ((fn_attrs & TD_FN_ATOMIC) && (is_list(left) || is_list(right)))
                result = atomic_map_binary(fn, left, right);
            else
                result = fn(left, right);
            td_release(left);
            td_release(right);
            return result;
        }
        case TD_ATOM_VARY: {
            td_vary_fn fn = (td_vary_fn)(uintptr_t)head->i64;
            if (head->attrs & TD_FN_SPECIAL_FORM) {
                td_release(head);
                return fn(elems + 1, n - 1);
            }
            int64_t argc = n - 1;
            td_t* args[64];
            for (int64_t i = 0; i < argc && i < 64; i++) {
                args[i] = td_eval(elems[i + 1]);
                if (TD_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) td_release(args[j]);
                    td_release(head);
                    return args[i];
                }
            }
            td_release(head);
            td_t* result = fn(args, argc);
            for (int64_t i = 0; i < argc; i++) td_release(args[i]);
            return result;
        }
        case TD_ATOM_LAMBDA: {
            int64_t argc = n - 1;
            td_t* args[64];
            for (int64_t i = 0; i < argc && i < 64; i++) {
                args[i] = td_eval(elems[i + 1]);
                if (TD_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) td_release(args[j]);
                    td_release(head);
                    return args[i];
                }
            }
            td_t* result = call_lambda(head, args, argc);
            for (int64_t i = 0; i < argc; i++) td_release(args[i]);
            td_release(head);
            return result;
        }
        default:
            td_release(head);
            return TD_ERR_PTR(TD_ERR_TYPE);
    }
}

td_t* td_eval_str(const char* source) {
    td_t* parsed = td_parse(source);
    if (TD_IS_ERR(parsed)) return parsed;
    td_t* result = td_eval(parsed);
    td_release(parsed);
    return result;
}
