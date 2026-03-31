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
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"

static MunitResult test_sel_new(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* sel = ray_sel_new(100);
    munit_assert_ptr_not_null(sel);
    munit_assert_false(RAY_IS_ERR(sel));
    munit_assert_int(sel->type, ==, RAY_SEL);

    ray_release(sel);
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_sel_from_pred(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    /* Create bool vector: [true, false, true, false, true] */
    uint8_t bools[] = {1, 0, 1, 0, 1};
    ray_t* bvec = ray_vec_from_raw(RAY_BOOL, bools, 5);
    munit_assert_ptr_not_null(bvec);

    ray_t* sel = ray_sel_from_pred(bvec);
    munit_assert_ptr_not_null(sel);
    munit_assert_false(RAY_IS_ERR(sel));
    munit_assert_int(sel->type, ==, RAY_SEL);
    munit_assert_int(ray_sel_meta(sel)->total_pass, ==, 3);

    ray_release(sel);
    ray_release(bvec);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_sel_and(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    uint8_t a_data[] = {1, 1, 0, 0, 1};
    uint8_t b_data[] = {1, 0, 1, 0, 1};
    ray_t* a_vec = ray_vec_from_raw(RAY_BOOL, a_data, 5);
    ray_t* b_vec = ray_vec_from_raw(RAY_BOOL, b_data, 5);

    ray_t* sel_a = ray_sel_from_pred(a_vec);
    ray_t* sel_b = ray_sel_from_pred(b_vec);
    ray_t* sel_and = ray_sel_and(sel_a, sel_b);

    munit_assert_ptr_not_null(sel_and);
    munit_assert_false(RAY_IS_ERR(sel_and));
    munit_assert_int(sel_and->type, ==, RAY_SEL);
    /* AND of {1,1,0,0,1} and {1,0,1,0,1} = indices {0,4} -> 2 passing */
    munit_assert_int(ray_sel_meta(sel_and)->total_pass, ==, 2);

    ray_release(sel_and);
    ray_release(sel_a);
    ray_release(sel_b);
    ray_release(a_vec);
    ray_release(b_vec);
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitResult test_sel_filter_integration(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    /* Test that selection vectors work end-to-end through executor */
    int64_t vals[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* c25 = ray_const_i64(g, 25);
    ray_op_t* pred = ray_gt(g, x, c25);
    ray_op_t* filtered = ray_filter(g, x, pred);
    ray_op_t* s = ray_sum(g, filtered);

    ray_t* result = ray_execute(g, s);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 120);  /* 30+40+50 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitTest sel_tests[] = {
    { "/new",              test_sel_new,               NULL, NULL, 0, NULL },
    { "/from_pred",        test_sel_from_pred,         NULL, NULL, 0, NULL },
    { "/and",              test_sel_and,               NULL, NULL, 0, NULL },
    { "/filter_integration", test_sel_filter_integration, NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_sel_suite = {
    "/sel", sel_tests, NULL, 1, 0
};
