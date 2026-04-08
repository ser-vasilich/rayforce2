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

#include "lang/eval_internal.h"

/* Helper: compare char atom vs string atom.
 * Returns: -1 if no char/string pair, else memcmp-like result via *out. */
int char_str_cmp(ray_t* a, ray_t* b, int *out) {
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
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return make_bool(RAY_ATOM_IS_NULL(b) && !RAY_ATOM_IS_NULL(a) ? 1 : 0);
        return make_bool(temporal_as_ns(a) > temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
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
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return make_bool(RAY_ATOM_IS_NULL(a) && !RAY_ATOM_IS_NULL(b) ? 1 : 0);
        return make_bool(temporal_as_ns(a) < temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(0);       /* null == null → not < */
    if (na) return make_bool(1);             /* null < X → true */
    if (nb) return make_bool(0);             /* X < null → false */
    return make_bool(as_f64(a) < as_f64(b) ? 1 : 0);
}

ray_t* ray_gte_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c >= 0 ? 1 : 0); }
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) >= 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) && RAY_ATOM_IS_NULL(b)) return make_bool(1);
        if (RAY_ATOM_IS_NULL(a)) return make_bool(0);
        if (RAY_ATOM_IS_NULL(b)) return make_bool(1);
        return make_bool(temporal_as_ns(a) >= temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(1);       /* null == null → >= true */
    if (na) return make_bool(0);             /* null >= X → false */
    if (nb) return make_bool(1);             /* X >= null → true */
    return make_bool(as_f64(a) >= as_f64(b) ? 1 : 0);
}

ray_t* ray_lte_fn(ray_t* a, ray_t* b) {
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c <= 0 ? 1 : 0); }
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) <= 0 ? 1 : 0);
    if (is_temporal(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) && RAY_ATOM_IS_NULL(b)) return make_bool(1);
        if (RAY_ATOM_IS_NULL(a)) return make_bool(1);
        if (RAY_ATOM_IS_NULL(b)) return make_bool(0);
        return make_bool(temporal_as_ns(a) <= temporal_as_ns(b) ? 1 : 0);
    }
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot compare %s and %s",
                         ray_type_name(abs(a->type)), ray_type_name(abs(b->type)));
    int na = RAY_ATOM_IS_NULL(a), nb = RAY_ATOM_IS_NULL(b);
    if (na && nb) return make_bool(1);       /* null == null → <= true */
    if (na) return make_bool(1);             /* null <= X → true */
    if (nb) return make_bool(0);             /* X <= null → false */
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

/* Check if comparable (numeric or temporal) */
int is_comparable(ray_t* x) {
    return is_numeric(x) || is_temporal(x);
}

ray_t* ray_eq_fn(ray_t* a, ray_t* b) {
    /* Handle all null forms (C NULL, RAY_NULL_OBJ, sentinel nulls) */
    int na = (!a || RAY_ATOM_IS_NULL(a)), nb = (!b || RAY_ATOM_IS_NULL(b));
    if (na && nb) return make_bool(1);
    if (na || nb) return make_bool(0);
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c == 0 ? 1 : 0); }
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 == b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 == b->i64 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0 ? 1 : 0);
    /* null == null → true */
    if (is_comparable(a) && is_comparable(b) && RAY_ATOM_IS_NULL(a) && RAY_ATOM_IS_NULL(b))
        return make_bool(1);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b))
        return make_bool(temporal_as_ns(a) == temporal_as_ns(b) ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    if (is_float_op(a, b))
        return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) == as_i64(b) ? 1 : 0);
}

ray_t* ray_neq_fn(ray_t* a, ray_t* b) {
    /* Handle all null forms (C NULL, RAY_NULL_OBJ, sentinel nulls) */
    int na = (!a || RAY_ATOM_IS_NULL(a)), nb = (!b || RAY_ATOM_IS_NULL(b));
    if (na && nb) return make_bool(0);
    if (na || nb) return make_bool(1);
    { int c; if (char_str_cmp(a, b, &c) == 0) return make_bool(c != 0 ? 1 : 0); }
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return make_bool(a->b8 != b->b8 ? 1 : 0);
    if (a->type == -RAY_SYM && b->type == -RAY_SYM)
        return make_bool(a->i64 != b->i64 ? 1 : 0);
    if (a->type == -RAY_GUID && b->type == -RAY_GUID)
        return make_bool(memcmp(ray_data(a->obj), ray_data(b->obj), 16) != 0 ? 1 : 0);
    /* null != null → false (all nulls are equal) */
    if (is_comparable(a) && is_comparable(b) && RAY_ATOM_IS_NULL(a) && RAY_ATOM_IS_NULL(b))
        return make_bool(0);
    /* Temporal comparison (same or cross-temporal via nanosecond conversion) */
    if (is_temporal(a) && is_temporal(b))
        return make_bool(temporal_as_ns(a) != temporal_as_ns(b) ? 1 : 0);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    if (is_float_op(a, b))
        return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
    return make_bool(as_i64(a) != as_i64(b) ? 1 : 0);
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
