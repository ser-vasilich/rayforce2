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

/* ══════════════════════════════════════════
 * Sort builtins
 * ══════════════════════════════════════════ */

/* (asc v) — sort vector ascending */
ray_t* ray_asc_fn(ray_t* x) {
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
ray_t* ray_desc_fn(ray_t* x) {
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
ray_t* ray_iasc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "iasc expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 0;
    return ray_sort_indices(&x, &desc, NULL, 1, n);
}

/* (idesc v) — descending sort indices */
ray_t* ray_idesc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "idesc expects a vector");

    int64_t n = ray_len(x);
    uint8_t desc = 1;
    return ray_sort_indices(&x, &desc, NULL, 1, n);
}

/* (rank v) — rank positions (inverse permutation of iasc) */
ray_t* ray_rank_fn(ray_t* x) {
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
ray_t* sort_table_by_keys(ray_t* tbl, ray_t* keys, uint8_t descending) {
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
ray_t* ray_xasc_fn(ray_t* tbl, ray_t* keys) {
    return sort_table_by_keys(tbl, keys, 0);
}

/* (xdesc tbl keys) — sort table descending by key columns */
ray_t* ray_xdesc_fn(ray_t* tbl, ray_t* keys) {
    return sort_table_by_keys(tbl, keys, 1);
}

/* (xrank n vec) — cross-rank: assign each element to one of n groups */
ray_t* ray_xrank_fn(ray_t* n_obj, ray_t* vec) {
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
