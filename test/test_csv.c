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
#include <stdio.h>
#include <unistd.h>

static char tmp_csv_path[64];
static const char* tmp_csv(void) {
    if (!tmp_csv_path[0])
        snprintf(tmp_csv_path, sizeof(tmp_csv_path),
                 "/tmp/rayforce_test_%d.csv", (int)getpid());
    return tmp_csv_path;
}
#define TMP_CSV tmp_csv()

static MunitResult test_csv_roundtrip_i64(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    int64_t vals[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(loaded->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(loaded), ==, 3);
    munit_assert_int(ray_table_ncols(loaded), ==, 1);

    /* Verify actual data values survived the roundtrip */
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_ptr_not_null(col);
    int64_t* loaded_data = (int64_t*)ray_data(col);
    munit_assert_int(loaded_data[0], ==, 10);
    munit_assert_int(loaded_data[1], ==, 20);
    munit_assert_int(loaded_data[2], ==, 30);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_roundtrip_f64(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    double vals[] = {1.5, 2.5, 3.5};
    ray_t* vec = ray_vec_from_raw(RAY_F64, vals, 3);
    int64_t name = ray_sym_intern("price", 5);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(loaded->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(loaded), ==, 3);

    /* Verify F64 values survived the roundtrip */
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_ptr_not_null(col);
    munit_assert_int(col->type, ==, RAY_F64);
    double* loaded_data = (double*)ray_data(col);
    munit_assert_double_equal(loaded_data[0], 1.5, 6);
    munit_assert_double_equal(loaded_data[1], 2.5, 6);
    munit_assert_double_equal(loaded_data[2], 3.5, 6);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_multi_column(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    int64_t ids[] = {1, 2, 3};
    double vals[] = {10.5, 20.5, 30.5};
    ray_t* id_v = ray_vec_from_raw(RAY_I64, ids, 3);
    ray_t* val_v = ray_vec_from_raw(RAY_F64, vals, 3);
    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_id, id_v);
    tbl = ray_table_add_col(tbl, n_val, val_v);
    ray_release(id_v);
    ray_release(val_v);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(ray_table_ncols(loaded), ==, 2);
    munit_assert_int(ray_table_nrows(loaded), ==, 3);

    /* Verify both columns' data values */
    ray_t* id_col = ray_table_get_col_idx(loaded, 0);
    munit_assert_ptr_not_null(id_col);
    int64_t* id_data = (int64_t*)ray_data(id_col);
    munit_assert_int(id_data[0], ==, 1);
    munit_assert_int(id_data[1], ==, 2);
    munit_assert_int(id_data[2], ==, 3);
    ray_t* val_col = ray_table_get_col_idx(loaded, 1);
    munit_assert_ptr_not_null(val_col);
    double* val_data = (double*)ray_data(val_col);
    munit_assert_double_equal(val_data[0], 10.5, 6);
    munit_assert_double_equal(val_data[1], 20.5, 6);
    munit_assert_double_equal(val_data[2], 30.5, 6);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_empty_table(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = ray_table_new(0);
    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    /* Empty table (0 cols) should return RAY_ERR_TYPE */
    munit_assert_int(err, ==, RAY_ERR_TYPE);

    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_null_i64(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\n\n30\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(ray_table_nrows(loaded), ==, 3);

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_ptr_not_null(col);

    munit_assert_false(ray_vec_is_null(col, 0));
    munit_assert_int(((int64_t*)ray_data(col))[0], ==, 10);
    munit_assert_true(ray_vec_is_null(col, 1));
    munit_assert_false(ray_vec_is_null(col, 2));
    munit_assert_int(((int64_t*)ray_data(col))[2], ==, 30);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_null_i64_unparseable(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\nN/A\n30\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_false(ray_vec_is_null(col, 0));
    munit_assert_int(((int64_t*)ray_data(col))[0], ==, 10);
    munit_assert_true(ray_vec_is_null(col, 1));
    munit_assert_false(ray_vec_is_null(col, 2));
    munit_assert_int(((int64_t*)ray_data(col))[2], ==, 30);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_null_f64(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n1.5\n\n3.5\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_false(ray_vec_is_null(col, 0));
    munit_assert_double_equal(((double*)ray_data(col))[0], 1.5, 6);
    munit_assert_true(ray_vec_is_null(col, 1));
    munit_assert_false(ray_vec_is_null(col, 2));
    munit_assert_double_equal(((double*)ray_data(col))[2], 3.5, 6);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_null_bool(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "flag\ntrue\n\nfalse\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_false(ray_vec_is_null(col, 0));
    munit_assert_int((int)((uint8_t*)ray_data(col))[0], ==, 1);
    munit_assert_true(ray_vec_is_null(col, 1));  /* empty */
    munit_assert_false(ray_vec_is_null(col, 2));
    munit_assert_int((int)((uint8_t*)ray_data(col))[2], ==, 0);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_null_sym(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "name\nalice\n\nbob\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    munit_assert_false(ray_vec_is_null(col, 0));
    munit_assert_true(ray_vec_is_null(col, 1));  /* empty → NULL */
    munit_assert_false(ray_vec_is_null(col, 2));

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_no_nulls_no_nullmap(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\n20\n30\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    /* No nulls → HAS_NULLS flag should be stripped */
    munit_assert_false(col->attrs & RAY_ATTR_HAS_NULLS);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_csv_null_mixed_columns(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "id,val,name\n1,1.5,alice\n,2.5,\n3,,bob\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(ray_table_nrows(loaded), ==, 3);
    munit_assert_int(ray_table_ncols(loaded), ==, 3);

    ray_t* id_col = ray_table_get_col_idx(loaded, 0);
    ray_t* val_col = ray_table_get_col_idx(loaded, 1);
    ray_t* name_col = ray_table_get_col_idx(loaded, 2);

    /* id column: 1, NULL, 3 */
    munit_assert_false(ray_vec_is_null(id_col, 0));
    munit_assert_true(ray_vec_is_null(id_col, 1));
    munit_assert_false(ray_vec_is_null(id_col, 2));

    /* val column: 1.5, 2.5, NULL */
    munit_assert_false(ray_vec_is_null(val_col, 0));
    munit_assert_false(ray_vec_is_null(val_col, 1));
    munit_assert_true(ray_vec_is_null(val_col, 2));

    /* name column: alice, NULL, bob */
    munit_assert_false(ray_vec_is_null(name_col, 0));
    munit_assert_true(ray_vec_is_null(name_col, 1));
    munit_assert_false(ray_vec_is_null(name_col, 2));

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitTest csv_tests[] = {
    { "/roundtrip_i64",  test_csv_roundtrip_i64,  NULL, NULL, 0, NULL },
    { "/roundtrip_f64",  test_csv_roundtrip_f64,  NULL, NULL, 0, NULL },
    { "/multi_column",   test_csv_multi_column,   NULL, NULL, 0, NULL },
    { "/empty_table",    test_csv_empty_table,     NULL, NULL, 0, NULL },
    { "/null_i64",             test_csv_null_i64,             NULL, NULL, 0, NULL },
    { "/null_i64_unparseable", test_csv_null_i64_unparseable, NULL, NULL, 0, NULL },
    { "/null_f64",             test_csv_null_f64,             NULL, NULL, 0, NULL },
    { "/null_bool",             test_csv_null_bool,             NULL, NULL, 0, NULL },
    { "/null_sym",              test_csv_null_sym,              NULL, NULL, 0, NULL },
    { "/no_nulls_no_nullmap",   test_csv_no_nulls_no_nullmap,  NULL, NULL, 0, NULL },
    { "/null_mixed_columns",    test_csv_null_mixed_columns,    NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_csv_suite = {
    "/csv", csv_tests, NULL, 1, 0
};
