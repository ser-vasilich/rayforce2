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
#include "core/runtime.h"
#include "mem/heap.h"
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static ray_vm_t test_vm;

static void* err_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    memset(&test_vm, 0, sizeof(test_vm));
    __VM = &test_vm;
    return NULL;
}

static void err_teardown(void* fixture) {
    (void)fixture;
    __VM = NULL;
    ray_heap_destroy();
}

/* ---- Tests ------------------------------------------------------------- */

static MunitResult test_error_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* err = ray_error("type", NULL);
    munit_assert_ptr_not_null(err);
    munit_assert_true(RAY_IS_ERR(err));
    munit_assert_uint(err->slen, ==, 4);
    munit_assert_memory_equal(4, err->sdata, "type");
    return MUNIT_OK;
}

static MunitResult test_error_with_message(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* err = ray_error("arity", "expected %d args, got %d", 2, 1);
    munit_assert_ptr_not_null(err);
    munit_assert_true(RAY_IS_ERR(err));
    munit_assert_uint(err->slen, ==, 5);
    munit_assert_memory_equal(5, err->sdata, "arity");
    const char* msg = ray_error_msg();
    munit_assert_ptr_not_null(msg);
    munit_assert_string_equal(msg, "expected 2 args, got 1");
    return MUNIT_OK;
}

static MunitResult test_error_code_max_length(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* err = ray_error("longcode", "detail");
    munit_assert_ptr_not_null(err);
    munit_assert_true(RAY_IS_ERR(err));
    /* sdata is 7 bytes max, so "longcode" (8 chars) truncated to "longcod" */
    munit_assert_uint(err->slen, ==, 7);
    munit_assert_memory_equal(7, err->sdata, "longcod");
    return MUNIT_OK;
}

static MunitResult test_error_null_not_error(const void* params, void* fixture) {
    (void)params; (void)fixture;
    munit_assert_false(RAY_IS_ERR(NULL));
    return MUNIT_OK;
}

static MunitResult test_error_normal_obj_not_error(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* i = ray_i64(42);
    munit_assert_ptr_not_null(i);
    munit_assert_false(RAY_IS_ERR(i));
    return MUNIT_OK;
}

static MunitResult test_error_clear(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_error("test", "detail message");
    munit_assert_ptr_not_null(ray_error_msg());
    ray_error_clear();
    munit_assert_null(ray_error_msg());
    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest err_tests[] = {
    { "/basic",              test_error_basic,              err_setup, err_teardown, 0, NULL },
    { "/with_message",       test_error_with_message,       err_setup, err_teardown, 0, NULL },
    { "/code_max_length",    test_error_code_max_length,    err_setup, err_teardown, 0, NULL },
    { "/null_not_error",     test_error_null_not_error,     err_setup, err_teardown, 0, NULL },
    { "/normal_obj_not_err", test_error_normal_obj_not_error, err_setup, err_teardown, 0, NULL },
    { "/clear",              test_error_clear,              err_setup, err_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_err_suite = {
    "/err",
    err_tests,
    NULL,
    0,
    0,
};
