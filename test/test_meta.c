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
#include "mem/heap.h"
#include <string.h>
#include <unistd.h>

#define TMP_META_PATH "/tmp/rayforce_test_meta.d"

/* ---- Setup / Teardown -------------------------------------------------- */

static void* meta_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    (void)ray_sym_init();
    return NULL;
}

static void meta_teardown(void* fixture) {
    (void)fixture;
    unlink(TMP_META_PATH);
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- test_meta_save_load_roundtrip ------------------------------------- */

static MunitResult test_meta_save_load_roundtrip(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build a small I64 schema vector */
    int64_t ids[] = {100, 200, 300};
    ray_t* schema = ray_vec_from_raw(RAY_I64, ids, 3);
    munit_assert_ptr_not_null(schema);
    munit_assert_false(RAY_IS_ERR(schema));

    /* Save */
    ray_err_t err = ray_meta_save_d(schema, TMP_META_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load back */
    ray_t* loaded = ray_meta_load_d(TMP_META_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));

    /* Verify contents */
    munit_assert_int(ray_type(loaded), ==, RAY_I64);
    munit_assert_int(ray_len(loaded), ==, 3);

    int64_t* out = (int64_t*)ray_data(loaded);
    munit_assert_int(out[0], ==, 100);
    munit_assert_int(out[1], ==, 200);
    munit_assert_int(out[2], ==, 300);

    ray_free(schema);
    ray_free(loaded);
    return MUNIT_OK;
}

/* ---- test_meta_save_null_returns_error --------------------------------- */

static MunitResult test_meta_save_null_returns_error(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_err_t err = ray_meta_save_d(NULL, TMP_META_PATH);
    munit_assert_int(err, !=, RAY_OK);

    return MUNIT_OK;
}

/* ---- test_meta_save_err_ptr_returns_error ------------------------------ */

static MunitResult test_meta_save_err_ptr_returns_error(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* bad = RAY_ERR_PTR(RAY_ERR_TYPE);
    ray_err_t err = ray_meta_save_d(bad, TMP_META_PATH);
    munit_assert_int(err, !=, RAY_OK);

    return MUNIT_OK;
}

/* ---- test_meta_load_nonexistent --------------------------------------- */

static MunitResult test_meta_load_nonexistent(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* loaded = ray_meta_load_d("/tmp/rayforce_no_such_file_meta.d");
    /* Should return NULL or error pointer for missing file */
    munit_assert_true(loaded == NULL || RAY_IS_ERR(loaded));

    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest meta_tests[] = {
    { "/save_load_roundtrip",     test_meta_save_load_roundtrip,     meta_setup, meta_teardown, 0, NULL },
    { "/save_null_returns_error", test_meta_save_null_returns_error, meta_setup, meta_teardown, 0, NULL },
    { "/save_err_ptr_returns_error", test_meta_save_err_ptr_returns_error, meta_setup, meta_teardown, 0, NULL },
    { "/load_nonexistent",        test_meta_load_nonexistent,        meta_setup, meta_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_meta_suite = {
    "/meta",
    meta_tests,
    NULL,
    0,
    0,
};
