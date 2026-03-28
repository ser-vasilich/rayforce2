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

#include "munit.h"
#include <rayforce.h>
#include "mem/heap.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void* table_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    (void)ray_sym_init();
    return NULL;
}

static void table_teardown(void* fixture) {
    (void)fixture;
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- table_new ------------------------------------------------------------ */

static MunitResult test_table_new(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    munit_assert_ptr_not_null(tbl);
    munit_assert_false(RAY_IS_ERR(tbl));
    munit_assert_int(tbl->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_ncols(tbl), ==, 0);
    ray_t* schema = ray_table_schema(tbl);
    munit_assert_ptr_not_null(schema);
    munit_assert_uint(schema->rc, ==, 1);

    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_add_col -------------------------------------------------------- */

static MunitResult test_table_add_col(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    munit_assert_ptr_not_null(tbl);

    int64_t name_id = ray_sym_intern("x", 1);
    int64_t raw[] = {10, 20, 30};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_ptr_not_null(col);

    tbl = ray_table_add_col(tbl, name_id, col);
    munit_assert_false(RAY_IS_ERR(tbl));
    munit_assert_int(ray_table_ncols(tbl), ==, 1);

    ray_release(col);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_get_col_by_name ------------------------------------------------ */

static MunitResult test_table_get_col_by_name(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    int64_t name_id = ray_sym_intern("price", 5);
    double raw[] = {1.5, 2.5, 3.5};
    ray_t* col = ray_vec_from_raw(RAY_F64, raw, 3);

    tbl = ray_table_add_col(tbl, name_id, col);
    munit_assert_false(RAY_IS_ERR(tbl));

    ray_t* got = ray_table_get_col(tbl, name_id);
    munit_assert_ptr_not_null(got);
    munit_assert_ptr_equal(got, col);

    /* Non-existent column returns NULL */
    int64_t other_id = ray_sym_intern("missing", 7);
    ray_t* missing = ray_table_get_col(tbl, other_id);
    munit_assert_null(missing);

    ray_release(col);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_get_col_by_idx ------------------------------------------------- */

static MunitResult test_table_get_col_by_idx(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    int64_t name_id = ray_sym_intern("val", 3);
    int64_t raw[] = {100, 200};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 2);

    tbl = ray_table_add_col(tbl, name_id, col);

    ray_t* got = ray_table_get_col_idx(tbl, 0);
    munit_assert_ptr_not_null(got);
    munit_assert_ptr_equal(got, col);

    /* Out of range */
    ray_t* oob = ray_table_get_col_idx(tbl, 1);
    munit_assert_null(oob);
    oob = ray_table_get_col_idx(tbl, -1);
    munit_assert_null(oob);

    ray_release(col);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_col_name ------------------------------------------------------- */

static MunitResult test_table_col_name(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    int64_t id_a = ray_sym_intern("alpha", 5);
    int64_t id_b = ray_sym_intern("beta", 4);
    int64_t raw[] = {1, 2, 3};
    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_I64, raw, 3);

    tbl = ray_table_add_col(tbl, id_a, col_a);
    tbl = ray_table_add_col(tbl, id_b, col_b);

    munit_assert_int(ray_table_col_name(tbl, 0), ==, id_a);
    munit_assert_int(ray_table_col_name(tbl, 1), ==, id_b);

    /* Out of range */
    munit_assert_int(ray_table_col_name(tbl, 2), ==, -1);

    ray_release(col_a);
    ray_release(col_b);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_nrows ---------------------------------------------------------- */

static MunitResult test_table_nrows(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    /* Empty tbl has 0 rows */
    munit_assert_int(ray_table_nrows(tbl), ==, 0);

    int64_t name_id = ray_sym_intern("col1", 4);
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 5);

    tbl = ray_table_add_col(tbl, name_id, col);
    munit_assert_int(ray_table_nrows(tbl), ==, 5);

    ray_release(col);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_schema --------------------------------------------------------- */

static MunitResult test_table_schema(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(4);
    ray_t* schema = ray_table_schema(tbl);
    munit_assert_ptr_not_null(schema);
    munit_assert_int(schema->type, ==, RAY_I64);
    munit_assert_int(schema->len, ==, 0);  /* no columns yet */

    int64_t id_x = ray_sym_intern("x", 1);
    int64_t raw[] = {1};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 1);
    tbl = ray_table_add_col(tbl, id_x, col);

    schema = ray_table_schema(tbl);
    munit_assert_int(schema->len, ==, 1);
    int64_t* ids = (int64_t*)ray_data(schema);
    munit_assert_int(ids[0], ==, id_x);

    ray_release(col);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_multiple_cols -------------------------------------------------- */

static MunitResult test_table_multiple_cols(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(8);

    int64_t id_a = ray_sym_intern("a", 1);
    int64_t id_b = ray_sym_intern("b", 1);
    int64_t id_c = ray_sym_intern("c", 1);

    int64_t raw_a[] = {1, 2, 3};
    double  raw_b[] = {1.1, 2.2, 3.3};
    uint8_t raw_c[] = {1, 0, 1};

    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw_a, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_F64, raw_b, 3);
    ray_t* col_c = ray_vec_from_raw(RAY_BOOL, raw_c, 3);

    tbl = ray_table_add_col(tbl, id_a, col_a);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_b);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_c, col_c);
    munit_assert_false(RAY_IS_ERR(tbl));

    munit_assert_int(ray_table_ncols(tbl), ==, 3);
    munit_assert_int(ray_table_nrows(tbl), ==, 3);

    /* Verify by name */
    munit_assert_ptr_equal(ray_table_get_col(tbl, id_a), col_a);
    munit_assert_ptr_equal(ray_table_get_col(tbl, id_b), col_b);
    munit_assert_ptr_equal(ray_table_get_col(tbl, id_c), col_c);

    /* Verify by index */
    munit_assert_ptr_equal(ray_table_get_col_idx(tbl, 0), col_a);
    munit_assert_ptr_equal(ray_table_get_col_idx(tbl, 1), col_b);
    munit_assert_ptr_equal(ray_table_get_col_idx(tbl, 2), col_c);

    /* Verify column names */
    munit_assert_int(ray_table_col_name(tbl, 0), ==, id_a);
    munit_assert_int(ray_table_col_name(tbl, 1), ==, id_b);
    munit_assert_int(ray_table_col_name(tbl, 2), ==, id_c);

    /* Verify schema */
    ray_t* schema = ray_table_schema(tbl);
    munit_assert_int(schema->len, ==, 3);
    int64_t* ids = (int64_t*)ray_data(schema);
    munit_assert_int(ids[0], ==, id_a);
    munit_assert_int(ids[1], ==, id_b);
    munit_assert_int(ids[2], ==, id_c);

    /* Verify data integrity */
    int64_t* data_a = (int64_t*)ray_data(col_a);
    munit_assert_int(data_a[0], ==, 1);
    munit_assert_int(data_a[2], ==, 3);

    double* data_b = (double*)ray_data(col_b);
    munit_assert_double(data_b[1], ==, 2.2);

    ray_release(col_a);
    ray_release(col_b);
    ray_release(col_c);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_realloc_preserves_all_cols ----------------------------------- */

static MunitResult test_table_realloc_preserves_all_cols(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* tbl = ray_table_new(1); /* force growth while appending */
    munit_assert_ptr_not_null(tbl);
    munit_assert_false(RAY_IS_ERR(tbl));

    ray_t* cols[4] = {0};
    int64_t vals[4] = {11, 22, 33, 44};

    for (int i = 0; i < 4; i++) {
        cols[i] = ray_vec_from_raw(RAY_I64, &vals[i], 1);
        munit_assert_ptr_not_null(cols[i]);
        munit_assert_false(RAY_IS_ERR(cols[i]));

        char name[8];
        int n = snprintf(name, sizeof(name), "c%d", i);
        munit_assert_int(n, >, 0);
        int64_t name_id = ray_sym_intern(name, (size_t)n);

        tbl = ray_table_add_col(tbl, name_id, cols[i]);
        munit_assert_ptr_not_null(tbl);
        munit_assert_false(RAY_IS_ERR(tbl));
    }

    munit_assert_int(ray_table_ncols(tbl), ==, 4);
    for (int i = 0; i < 4; i++) {
        ray_t* col = ray_table_get_col_idx(tbl, i);
        munit_assert_ptr_not_null(col);
        munit_assert_int(col->type, ==, RAY_I64);
        munit_assert_int(col->len, ==, 1);
        int64_t* data = (int64_t*)ray_data(col);
        munit_assert_int(data[0], ==, vals[i]);
    }

    for (int i = 0; i < 4; i++) ray_release(cols[i]);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- table_release_drops_col_ref ---------------------------------------- */

static MunitResult test_table_release_drops_col_ref(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {7, 8, 9};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_ptr_not_null(col);
    munit_assert_false(RAY_IS_ERR(col));

    ray_t* tbl = ray_table_new(1);
    int64_t name_id = ray_sym_intern("x", 1);
    tbl = ray_table_add_col(tbl, name_id, col);
    munit_assert_ptr_not_null(tbl);
    munit_assert_false(RAY_IS_ERR(tbl));

    munit_assert_uint(col->rc, ==, 2);
    ray_release(tbl);
    munit_assert_uint(col->rc, ==, 1);

    ray_release(col);
    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest table_tests[] = {
    { "/new",              test_table_new,              table_setup, table_teardown, 0, NULL },
    { "/add_col",          test_table_add_col,          table_setup, table_teardown, 0, NULL },
    { "/get_col_by_name",  test_table_get_col_by_name,  table_setup, table_teardown, 0, NULL },
    { "/get_col_by_idx",   test_table_get_col_by_idx,   table_setup, table_teardown, 0, NULL },
    { "/col_name",         test_table_col_name,         table_setup, table_teardown, 0, NULL },
    { "/nrows",            test_table_nrows,            table_setup, table_teardown, 0, NULL },
    { "/schema",           test_table_schema,           table_setup, table_teardown, 0, NULL },
    { "/multiple_cols",    test_table_multiple_cols,    table_setup, table_teardown, 0, NULL },
    { "/realloc_preserves_all_cols", test_table_realloc_preserves_all_cols, table_setup, table_teardown, 0, NULL },
    { "/release_drops_col_ref", test_table_release_drops_col_ref, table_setup, table_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_table_suite = {
    "/table",
    table_tests,
    NULL,
    0,
    0,
};
