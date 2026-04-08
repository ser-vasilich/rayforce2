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
#include "ops/ops.h"

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
        /* Direct reduction — null bitmap skipping */
        if (x->type == RAY_I32) {
            int64_t n = x->len;
            int32_t* d = (int32_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) sum += d[i];
            return make_i32((int32_t)sum);
        }
        if (x->type == RAY_I16 || x->type == RAY_U8) {
            int64_t n = x->len, sum = 0;
            if (x->type == RAY_I16) {
                int16_t* d = (int16_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (!ray_vec_is_null(x, i)) sum += d[i];
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
                if (!ray_vec_is_null(x, i)) sum += d[i];
            return make_i64(sum);
        }
        if (x->type == RAY_F64) {
            int64_t n = x->len;
            double* d = (double*)ray_data(x);
            double sum = 0.0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) sum += d[i];
            return make_f64(sum);
        }
        if (x->type == RAY_TIME) {
            int64_t n = x->len;
            int32_t* d = (int32_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) sum += d[i];
            return ray_time(sum);
        }
        if (x->type == RAY_TIMESTAMP) {
            int64_t n = x->len;
            int64_t* d = (int64_t*)ray_data(x);
            int64_t sum = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) sum += d[i];
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
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
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
                    if (!ray_vec_is_null(x, i)) { sum += (double)d[i]; cnt++; }
            } else if (x->type == RAY_I32) {
                int32_t* d = (int32_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (!ray_vec_is_null(x, i)) { sum += (double)d[i]; cnt++; }
            } else if (x->type == RAY_I16) {
                int16_t* d = (int16_t*)ray_data(x);
                for (int64_t i = 0; i < n; i++)
                    if (!ray_vec_is_null(x, i)) { sum += (double)d[i]; cnt++; }
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
                if (!ray_vec_is_null(x, i)) { sum += d[i]; cnt++; }
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

ray_t* ray_min_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MIN);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        int64_t n = x->len;
        if (n == 0) return ray_typed_null(-RAY_I64);
        if (x->type == RAY_I32 || x->type == RAY_DATE || x->type == RAY_TIME) {
            int32_t* d = (int32_t*)ray_data(x);
            int32_t m = INT32_MAX; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] < m) m = d[i]; found = 1; }
            if (!found) return ray_typed_null(-x->type);
            if (x->type == RAY_TIME) return ray_time(m);
            if (x->type == RAY_DATE) return ray_date(m);
            return make_i32(m);
        }
        if (x->type == RAY_I64 || x->type == RAY_TIMESTAMP) {
            int64_t* d = (int64_t*)ray_data(x);
            int64_t m = INT64_MAX; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] < m) m = d[i]; found = 1; }
            if (!found) return ray_typed_null(-x->type);
            return (x->type == RAY_TIMESTAMP) ? ray_timestamp(m) : make_i64(m);
        }
        if (x->type == RAY_F64) {
            double* d = (double*)ray_data(x);
            double m = 0; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] < m) m = d[i]; found = 1; }
            return found ? make_f64(m) : ray_typed_null(-RAY_F64);
        }
        if (x->type == RAY_I16) {
            int16_t* d = (int16_t*)ray_data(x);
            int16_t m = INT16_MAX; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] < m) m = d[i]; found = 1; }
            return found ? make_i16(m) : ray_typed_null(-RAY_I16);
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

ray_t* ray_max_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MAX);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) {
        int64_t n = x->len;
        if (n == 0) return ray_typed_null(-RAY_I64);
        if (x->type == RAY_I32 || x->type == RAY_DATE || x->type == RAY_TIME) {
            int32_t* d = (int32_t*)ray_data(x);
            int32_t m = INT32_MIN; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] > m) m = d[i]; found = 1; }
            if (!found) return ray_typed_null(-x->type);
            if (x->type == RAY_TIME) return ray_time(m);
            if (x->type == RAY_DATE) return ray_date(m);
            return make_i32(m);
        }
        if (x->type == RAY_I64 || x->type == RAY_TIMESTAMP) {
            int64_t* d = (int64_t*)ray_data(x);
            int64_t m = INT64_MIN; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] > m) m = d[i]; found = 1; }
            if (!found) return ray_typed_null(-x->type);
            return (x->type == RAY_TIMESTAMP) ? ray_timestamp(m) : make_i64(m);
        }
        if (x->type == RAY_F64) {
            double* d = (double*)ray_data(x);
            double m = 0; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] > m) m = d[i]; found = 1; }
            return found ? make_f64(m) : ray_typed_null(-RAY_F64);
        }
        if (x->type == RAY_I16) {
            int16_t* d = (int16_t*)ray_data(x);
            int16_t m = INT16_MIN; int found = 0;
            for (int64_t i = 0; i < n; i++)
                if (!ray_vec_is_null(x, i)) { if (!found || d[i] > m) m = d[i]; found = 1; }
            return found ? make_i16(m) : ray_typed_null(-RAY_I16);
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
        ray_t* result = ray_at_fn(x, idx);
        ray_release(idx);
        return result;
    }
    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return ray_typed_null(-RAY_I64); /* 0Nl for empty vec */
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
    if (ray_len(x) == 0) return ray_typed_null(-RAY_I64); /* 0Nl for empty list */
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
        ray_t* result = ray_at_fn(x, idx);
        ray_release(idx);
        return result;
    }
    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return ray_typed_null(-RAY_I64); /* 0Nl for empty vec */
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
    if (len == 0) return ray_typed_null(-RAY_I64); /* 0Nl for empty list */
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

ray_t* ray_med_fn(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (RAY_IS_ERR(x)) return x;
    /* Scalar: median of single value → f64 */
    if (ray_is_atom(x)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
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

ray_t* ray_dev_fn(ray_t* x) {
    if (ray_is_lazy(x)) x = ray_lazy_materialize(x);
    if (RAY_IS_ERR(x)) return x;
    if (ray_is_atom(x)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
        if (is_numeric(x)) return make_f64(0.0);
        return ray_error("type", NULL);
    }
    if (ray_is_vec(x)) {
        int64_t len = ray_len(x);
        if (len == 0) return make_f64(NAN);
        /* Build f64 scratch — null bitmap elements become NaN */
        ray_t* scratch = ray_alloc(len * sizeof(double));
        if (!scratch) return ray_error("oom", NULL);
        scratch->type = RAY_F64; scratch->len = len;
        double* vals = (double*)ray_data(scratch);
        if (x->type == RAY_I64) {
            int64_t* d = (int64_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = ray_vec_is_null(x, i) ? NAN : (double)d[i];
        } else if (x->type == RAY_I32) {
            int32_t* d = (int32_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = ray_vec_is_null(x, i) ? NAN : (double)d[i];
        } else if (x->type == RAY_I16) {
            int16_t* d = (int16_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = ray_vec_is_null(x, i) ? NAN : (double)d[i];
        } else if (x->type == RAY_U8) {
            uint8_t* d = (uint8_t*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = ray_vec_is_null(x, i) ? NAN : (double)d[i];
        } else if (x->type == RAY_F64) {
            double* d = (double*)ray_data(x);
            for (int64_t i = 0; i < len; i++) vals[i] = ray_vec_is_null(x, i) ? NAN : d[i];
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
        if (!RAY_ATOM_IS_NULL(elems[i])) { sum += as_f64(elems[i]); cnt++; }
    }
    if (cnt == 0) return make_f64(NAN);
    double mean = sum / (double)cnt;
    double var = 0.0;
    for (int64_t i = 0; i < len; i++) {
        if (!RAY_ATOM_IS_NULL(elems[i])) { double d = as_f64(elems[i]) - mean; var += d * d; }
    }
    return make_f64(sqrt(var / (double)cnt));
}
