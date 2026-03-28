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

#include "table.h"
#include "ops/ops.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Data layout helpers
 *
 * Data region of a TABLE block:
 *   [0]                          = ray_t* schema (I64 vector of name IDs)
 *   [sizeof(ray_t*)]              = ray_t* col_0
 *   [sizeof(ray_t*) * 2]          = ray_t* col_1
 *   ...
 *   [sizeof(ray_t*) * (ncols)]    = ray_t* col_{ncols-1}
 *
 * tbl->len = current column count
 * -------------------------------------------------------------------------- */

static ray_t** tbl_schema_slot(ray_t* tbl) {
    return (ray_t**)ray_data(tbl);
}

static ray_t** tbl_col_slots(ray_t* tbl) {
    return (ray_t**)((char*)ray_data(tbl) + sizeof(ray_t*));
}

/* --------------------------------------------------------------------------
 * ray_table_new
 * -------------------------------------------------------------------------- */

ray_t* ray_table_new(int64_t ncols) {
    if (ncols < 0) return RAY_ERR_PTR(RAY_ERR_RANGE);
    if ((uint64_t)ncols > SIZE_MAX / sizeof(ray_t*) - 1)
        return RAY_ERR_PTR(RAY_ERR_OOM);
    /* Allocate: 1 schema pointer + ncols column pointers */
    size_t data_size = (size_t)(1 + ncols) * sizeof(ray_t*);

    ray_t* tbl = ray_alloc(data_size);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    tbl->type = RAY_TABLE;
    tbl->len = 0;  /* no columns yet */
    tbl->attrs = 0;
    memset(tbl->nullmap, 0, 16);

    /* Zero the data region */
    memset(ray_data(tbl), 0, data_size);

    /* Create schema: I64 vector with capacity = ncols */
    ray_t* schema = ray_vec_new(RAY_I64, ncols);
    if (!schema || RAY_IS_ERR(schema)) {
        ray_free(tbl);
        return schema;
    }
    *tbl_schema_slot(tbl) = schema;

    return tbl;
}

/* --------------------------------------------------------------------------
 * ray_table_add_col
 * -------------------------------------------------------------------------- */

ray_t* ray_table_add_col(ray_t* tbl, int64_t name_id, ray_t* col_vec) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    if (!col_vec || RAY_IS_ERR(col_vec)) return RAY_ERR_PTR(RAY_ERR_TYPE);

    /* COW the tbl */
    tbl = ray_cow(tbl);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    int64_t idx = tbl->len;

    /* Check capacity: we need (1 + idx + 1) pointers in data region */
    size_t block_size = (size_t)1 << tbl->order;
    size_t data_space = block_size - 32;  /* 32B ray_t header */
    int64_t max_cols = (int64_t)(data_space / sizeof(ray_t*)) - 1;  /* minus schema slot */

    if (idx >= max_cols) {
        /* Need to grow the tbl block */
        size_t new_data_size = (size_t)(1 + (idx + 1) * 2) * sizeof(ray_t*);
        ray_t* new_df = ray_scratch_realloc(tbl, new_data_size);
        if (!new_df || RAY_IS_ERR(new_df)) return new_df;
        tbl = new_df;
    }

    /* Append name_id to schema vector */
    ray_t* schema = *tbl_schema_slot(tbl);
    schema = ray_vec_append(schema, &name_id);
    if (!schema || RAY_IS_ERR(schema)) return RAY_ERR_PTR(RAY_ERR_OOM);

    /* vec_append returns the owned schema reference (possibly moved). */
    *tbl_schema_slot(tbl) = schema;

    /* Store column vector pointer and retain it */
    ray_t** cols = tbl_col_slots(tbl);
    cols[idx] = col_vec;
    ray_retain(col_vec);

    tbl->len = idx + 1;

    return tbl;
}

/* --------------------------------------------------------------------------
 * ray_table_get_col
 * -------------------------------------------------------------------------- */

ray_t* ray_table_get_col(ray_t* tbl, int64_t name_id) {
    if (!tbl || RAY_IS_ERR(tbl)) return NULL;

    ray_t* schema = *tbl_schema_slot(tbl);
    if (!schema || RAY_IS_ERR(schema)) return NULL;

    int64_t* ids = (int64_t*)ray_data(schema);
    int64_t ncols = tbl->len;

    for (int64_t i = 0; i < ncols; i++) {
        if (ids[i] == name_id) {
            ray_t** cols = tbl_col_slots(tbl);
            return cols[i];
        }
    }

    return NULL;  /* column not found */
}

/* --------------------------------------------------------------------------
 * ray_table_get_col_idx
 * -------------------------------------------------------------------------- */

ray_t* ray_table_get_col_idx(ray_t* tbl, int64_t idx) {
    if (!tbl || RAY_IS_ERR(tbl)) return NULL;
    if (idx < 0 || idx >= tbl->len) return NULL;

    ray_t** cols = tbl_col_slots(tbl);
    return cols[idx];
}

/* --------------------------------------------------------------------------
 * ray_table_col_name
 * -------------------------------------------------------------------------- */

int64_t ray_table_col_name(ray_t* tbl, int64_t idx) {
    if (!tbl || RAY_IS_ERR(tbl)) return -1;
    if (idx < 0 || idx >= tbl->len) return -1;

    ray_t* schema = *tbl_schema_slot(tbl);
    if (!schema || RAY_IS_ERR(schema)) return -1;

    int64_t* ids = (int64_t*)ray_data(schema);
    return ids[idx];
}

/* --------------------------------------------------------------------------
 * ray_table_set_col_name
 * -------------------------------------------------------------------------- */

void ray_table_set_col_name(ray_t* tbl, int64_t idx, int64_t name_id) {
    if (!tbl || RAY_IS_ERR(tbl)) return;
    if (idx < 0 || idx >= tbl->len) return;

    /* NOTE: This function returns void so it cannot return a new COW'd pointer.
     * Caller must ensure exclusive ownership (rc==1) before calling, e.g. via
     * ray_cow(tbl) beforehand. Mutating a shared table here is undefined. */
    ray_t* schema = *tbl_schema_slot(tbl);
    if (!schema || RAY_IS_ERR(schema)) return;

    /* COW the schema vector to avoid mutating shared schema */
    schema = ray_cow(schema);
    if (!schema || RAY_IS_ERR(schema)) return;
    *tbl_schema_slot(tbl) = schema;

    int64_t* ids = (int64_t*)ray_data(schema);
    ids[idx] = name_id;
}

/* --------------------------------------------------------------------------
 * ray_table_ncols
 * -------------------------------------------------------------------------- */

int64_t ray_table_ncols(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return 0;
    return tbl->len;
}

/* --------------------------------------------------------------------------
 * ray_table_nrows
 * -------------------------------------------------------------------------- */

int64_t ray_table_nrows(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return 0;
    if (tbl->len <= 0) return 0;

    ray_t** cols = tbl_col_slots(tbl);
    ray_t* first_col = cols[0];
    if (!first_col || RAY_IS_ERR(first_col)) return 0;

    if (RAY_IS_PARTED(first_col->type) || first_col->type == RAY_MAPCOMMON)
        return ray_parted_nrows(first_col);

    return first_col->len;
}

/* --------------------------------------------------------------------------
 * ray_parted_nrows
 * -------------------------------------------------------------------------- */

int64_t ray_parted_nrows(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return 0;
    if (!RAY_IS_PARTED(v->type) && v->type != RAY_MAPCOMMON) return v->len;

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        ray_t* counts = ptrs[1];
        if (!counts || RAY_IS_ERR(counts)) return 0;
        int64_t total = 0;
        int64_t* cdata = (int64_t*)ray_data(counts);
        for (int64_t i = 0; i < counts->len; i++)
            total += cdata[i];
        return total;
    }

    int64_t n_segs = v->len;
    ray_t** segs = (ray_t**)ray_data(v);
    int64_t total = 0;
    for (int64_t i = 0; i < n_segs; i++) {
        if (segs[i] && !RAY_IS_ERR(segs[i]))
            total += segs[i]->len;
    }
    return total;
}

/* --------------------------------------------------------------------------
 * ray_table_schema
 * -------------------------------------------------------------------------- */

ray_t* ray_table_schema(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return NULL;
    return *tbl_schema_slot(tbl);
}
