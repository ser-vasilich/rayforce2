/*
 *   Copyright (c) 2024-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "munit.h"
#include <rayforce.h>
#include "ops/fvec.h"

static MunitResult test_ftable_new_free(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_ftable_t* ft = ray_ftable_new(3);
    munit_assert_ptr_not_null(ft);
    munit_assert_uint(ft->n_cols, ==, 3);

    ray_ftable_free(ft);
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_ftable_materialize_flat(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    ray_ftable_t* ft = ray_ftable_new(1);

    /* Create a flat fvec: single value at index 0, cardinality 5 */
    int64_t vals[] = {42};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 1);
    ft->columns[0].vec = vec;
    ft->columns[0].cur_idx = 0;
    ft->columns[0].cardinality = 5;
    ft->n_tuples = 5;

    ray_t* result = ray_ftable_materialize(ft);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 5);

    /* All rows should be 42 */
    ray_t* col = ray_table_get_col_idx(result, 0);
    int64_t* data_ptr = (int64_t*)ray_data(col);
    for (int i = 0; i < 5; i++)
        munit_assert_int(data_ptr[i], ==, 42);

    ray_release(result);
    ray_ftable_free(ft);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_ftable_materialize_unflat(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    ray_ftable_t* ft = ray_ftable_new(1);

    int64_t vals[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    ft->columns[0].vec = vec;
    ft->columns[0].cur_idx = -1;  /* unflat */
    ft->columns[0].cardinality = 3;
    ft->n_tuples = 3;

    ray_t* result = ray_ftable_materialize(ft);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(ray_table_nrows(result), ==, 3);

    ray_t* col = ray_table_get_col_idx(result, 0);
    int64_t* data_ptr = (int64_t*)ray_data(col);
    munit_assert_int(data_ptr[0], ==, 10);
    munit_assert_int(data_ptr[1], ==, 20);
    munit_assert_int(data_ptr[2], ==, 30);

    ray_release(result);
    ray_ftable_free(ft);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitTest fvec_tests[] = {
    { "/new_free",          test_ftable_new_free,          NULL, NULL, 0, NULL },
    { "/materialize_flat",  test_ftable_materialize_flat,  NULL, NULL, 0, NULL },
    { "/materialize_unflat", test_ftable_materialize_unflat, NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_fvec_suite = {
    "/fvec", fvec_tests, NULL, 1, 0
};
