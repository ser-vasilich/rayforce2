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

#include "munit.h"
#include "core/err.h"
#include "core/types.h"

/* ---- ray_err_str tests -------------------------------------------------- */

static MunitResult test_err_str_ok(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_err_str(RAY_OK), "ok");
    return MUNIT_OK;
}

static MunitResult test_err_str_all(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_err_str(RAY_ERR_OOM),     "out of memory");
    munit_assert_string_equal(ray_err_str(RAY_ERR_TYPE),    "type error");
    munit_assert_string_equal(ray_err_str(RAY_ERR_RANGE),   "range error");
    munit_assert_string_equal(ray_err_str(RAY_ERR_LENGTH),  "length mismatch");
    munit_assert_string_equal(ray_err_str(RAY_ERR_RANK),    "rank error");
    munit_assert_string_equal(ray_err_str(RAY_ERR_DOMAIN),  "domain error");
    munit_assert_string_equal(ray_err_str(RAY_ERR_NYI),     "not yet implemented");
    munit_assert_string_equal(ray_err_str(RAY_ERR_IO),      "I/O error");
    munit_assert_string_equal(ray_err_str(RAY_ERR_SCHEMA),  "schema error");
    munit_assert_string_equal(ray_err_str(RAY_ERR_CORRUPT), "corrupt data");
    munit_assert_string_equal(ray_err_str(RAY_ERR_CANCEL),  "query cancelled");
    return MUNIT_OK;
}

static MunitResult test_err_str_unknown(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_string_equal(ray_err_str((ray_err_t)99), "unknown error");
    return MUNIT_OK;
}

/* ---- DEPRECATED RAY_ERR_PTR / RAY_ERR_CODE macro tests -------------------- */

/* Legacy sentinel-pointer check (pre-RAY_ERROR migration) */
#define RAY_IS_ERR_LEGACY(p)  ((uintptr_t)(p) < 32)

static MunitResult test_err_ptr_encoding(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Legacy sentinel pointers detected via RAY_IS_ERR_LEGACY */
    ray_t* err_oom = RAY_ERR_PTR(RAY_ERR_OOM);
    munit_assert_true(RAY_IS_ERR_LEGACY(err_oom));
    munit_assert_int(RAY_ERR_CODE(err_oom), ==, RAY_ERR_OOM);

    ray_t* err_type = RAY_ERR_PTR(RAY_ERR_TYPE);
    munit_assert_true(RAY_IS_ERR_LEGACY(err_type));
    munit_assert_int(RAY_ERR_CODE(err_type), ==, RAY_ERR_TYPE);

    ray_t* err_corrupt = RAY_ERR_PTR(RAY_ERR_CORRUPT);
    munit_assert_true(RAY_IS_ERR_LEGACY(err_corrupt));
    munit_assert_int(RAY_ERR_CODE(err_corrupt), ==, RAY_ERR_CORRUPT);

    /* NULL is detected by legacy check (value 0 < 32) */
    munit_assert_true(RAY_IS_ERR_LEGACY(NULL));
    munit_assert_int(RAY_ERR_CODE(NULL), ==, RAY_OK);

    /* New RAY_IS_ERR checks for first-class RAY_ERROR objects */
    munit_assert_false(RAY_IS_ERR(NULL));

    return MUNIT_OK;
}

static MunitResult test_err_valid_ptr_not_error(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* A properly aligned ray_t on the stack: address will be >= 32 */
    ray_t block;
    memset(&block, 0, sizeof(block));
    munit_assert_false(RAY_IS_ERR(&block));

    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest err_tests[] = {
    { "/str_ok",            test_err_str_ok,            NULL, NULL, 0, NULL },
    { "/str_all",           test_err_str_all,           NULL, NULL, 0, NULL },
    { "/str_unknown",       test_err_str_unknown,       NULL, NULL, 0, NULL },
    { "/ptr_encoding",      test_err_ptr_encoding,      NULL, NULL, 0, NULL },
    { "/valid_ptr_not_err", test_err_valid_ptr_not_error, NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_err_suite = {
    "/err",
    err_tests,
    NULL,
    0,
    0,
};
