#include "munit.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "app/format.h"
#include <string.h>
#include <limits.h>
#include <math.h>

/* ---- Setup / Teardown ---- */

static void* fmt_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    (void)ray_sym_init();
    return NULL;
}

static void fmt_teardown(void* fixture) {
    (void)fixture;
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- Test: format i64 atom ---- */
static MunitResult test_fmt_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_i64(42), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_string_equal(s, "42");
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format negative i64 atom ---- */
static MunitResult test_fmt_i64_neg(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_i64(-1), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "-1"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format f64 with 2-decimal precision ---- */
static MunitResult test_fmt_f64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_f64(3.14159), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "3.14"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format f64 scientific notation ---- */
static MunitResult test_fmt_f64_sci(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_f64(1e7), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* Scientific notation: may be "e+07" or "e+7" depending on platform */
    munit_assert_true(strstr(s, "e+07") != NULL || strstr(s, "e+7") != NULL);
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format f64 zero ---- */
static MunitResult test_fmt_f64_zero(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_f64(0.0), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "0.00"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format bool true ---- */
static MunitResult test_fmt_bool_true(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_bool(true), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "true"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format bool false ---- */
static MunitResult test_fmt_bool_false(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_bool(false), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "false"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format null i64 (INT64_MIN -> 0Nl) ---- */
static MunitResult test_fmt_null_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_i64(INT64_MIN), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "0Nl"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format null f64 (NaN -> 0Nf) ---- */
static MunitResult test_fmt_null_f64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_fmt(ray_f64(NAN), 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "0Nf"));
    ray_release(result);

    return MUNIT_OK;
}

/* ---- Test: format i64 vector ---- */
static MunitResult test_fmt_vec_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));

    ray_t* result = ray_fmt(vec, 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "[1 2 3]"));
    ray_release(result);
    ray_release(vec);

    return MUNIT_OK;
}

/* ---- Test: format empty i64 vector ---- */
static MunitResult test_fmt_vec_empty(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* vec = ray_vec_new(RAY_I64, 0);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));

    ray_t* result = ray_fmt(vec, 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    munit_assert_ptr_not_null(strstr(s, "[]"));
    ray_release(result);
    ray_release(vec);

    return MUNIT_OK;
}

/* ---- Test: format table with box-drawing ---- */
static MunitResult test_fmt_table(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build a 2-column, 3-row table */
    ray_t* tbl = ray_table_new(3);
    munit_assert_ptr_not_null(tbl);
    munit_assert_false(RAY_IS_ERR(tbl));

    int64_t id_x = ray_sym_intern("x", 1);
    int64_t id_y = ray_sym_intern("y", 1);

    int64_t raw_x[] = {10, 20, 30};
    double  raw_y[] = {1.1, 2.2, 3.3};

    ray_t* col_x = ray_vec_from_raw(RAY_I64, raw_x, 3);
    ray_t* col_y = ray_vec_from_raw(RAY_F64, raw_y, 3);
    munit_assert_false(RAY_IS_ERR(col_x));
    munit_assert_false(RAY_IS_ERR(col_y));

    tbl = ray_table_add_col(tbl, id_x, col_x);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_y, col_y);
    munit_assert_false(RAY_IS_ERR(tbl));

    ray_t* result = ray_fmt(tbl, 1);
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);

    /* Check box-drawing characters */
    munit_assert_ptr_not_null(strstr(s, "\xe2\x94\x8c")); /* U+250C top-left corner */
    munit_assert_ptr_not_null(strstr(s, "\xe2\x94\x82")); /* U+2502 vertical bar */
    munit_assert_ptr_not_null(strstr(s, "\xe2\x94\x94")); /* U+2514 bottom-left corner */

    /* Check column names present */
    munit_assert_ptr_not_null(strstr(s, "x"));
    munit_assert_ptr_not_null(strstr(s, "y"));

    /* Check type names present */
    munit_assert_ptr_not_null(strstr(s, "i64"));
    munit_assert_ptr_not_null(strstr(s, "f64"));

    /* Check footer with row count */
    munit_assert_ptr_not_null(strstr(s, "3 rows"));

    ray_release(result);
    ray_release(tbl);

    return MUNIT_OK;
}

/* ---- Test: ray_type_name ---- */
static MunitResult test_type_name_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_type_name(RAY_I64), "i64");
    return MUNIT_OK;
}

static MunitResult test_type_name_f64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_type_name(RAY_F64), "f64");
    return MUNIT_OK;
}

static MunitResult test_type_name_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_type_name(RAY_TABLE), "table");
    return MUNIT_OK;
}

static MunitResult test_type_name_sym(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_type_name(RAY_SYM), "sym");
    return MUNIT_OK;
}

/* ---- Suite definition ---- */

static MunitTest format_tests[] = {
    { "/atom/i64",         test_fmt_i64,         fmt_setup, fmt_teardown, 0, NULL },
    { "/atom/i64_neg",     test_fmt_i64_neg,     fmt_setup, fmt_teardown, 0, NULL },
    { "/atom/f64",         test_fmt_f64,         fmt_setup, fmt_teardown, 0, NULL },
    { "/atom/f64_sci",     test_fmt_f64_sci,     fmt_setup, fmt_teardown, 0, NULL },
    { "/atom/f64_zero",    test_fmt_f64_zero,    fmt_setup, fmt_teardown, 0, NULL },
    { "/atom/bool_true",   test_fmt_bool_true,   fmt_setup, fmt_teardown, 0, NULL },
    { "/atom/bool_false",  test_fmt_bool_false,  fmt_setup, fmt_teardown, 0, NULL },
    { "/null/i64",         test_fmt_null_i64,    fmt_setup, fmt_teardown, 0, NULL },
    { "/null/f64",         test_fmt_null_f64,    fmt_setup, fmt_teardown, 0, NULL },
    { "/vec/i64",          test_fmt_vec_i64,     fmt_setup, fmt_teardown, 0, NULL },
    { "/vec/empty",        test_fmt_vec_empty,   fmt_setup, fmt_teardown, 0, NULL },
    { "/table/box",        test_fmt_table,       fmt_setup, fmt_teardown, 0, NULL },
    { "/type/i64",         test_type_name_i64,   fmt_setup, fmt_teardown, 0, NULL },
    { "/type/f64",         test_type_name_f64,   fmt_setup, fmt_teardown, 0, NULL },
    { "/type/table",       test_type_name_table, fmt_setup, fmt_teardown, 0, NULL },
    { "/type/sym",         test_type_name_sym,   fmt_setup, fmt_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_format_suite = { "/format", format_tests, NULL, 1, 0 };
