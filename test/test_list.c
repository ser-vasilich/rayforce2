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
#include <stdatomic.h>
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void* list_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    return NULL;
}

static void list_teardown(void* fixture) {
    (void)fixture;
    ray_heap_destroy();
}

/* ---- list_new ---------------------------------------------------------- */

static MunitResult test_list_new(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(4);
    munit_assert_ptr_not_null(list);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_int(list->type, ==, RAY_LIST);
    munit_assert_int(list->len, ==, 0);
    munit_assert_false(ray_is_atom(list));
    munit_assert_false(ray_is_vec(list));  /* type==0, neither atom nor vec */

    ray_release(list);
    return MUNIT_OK;
}

/* ---- list_append_get --------------------------------------------------- */

static MunitResult test_list_append_get(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(4);

    ray_t* a = ray_i64(42);
    ray_t* b = ray_f64(3.14);

    list = ray_list_append(list, a);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_int(list->len, ==, 1);

    list = ray_list_append(list, b);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_int(list->len, ==, 2);

    ray_t* got0 = ray_list_get(list, 0);
    munit_assert_ptr_equal(got0, a);
    munit_assert_int(got0->i64, ==, 42);

    ray_t* got1 = ray_list_get(list, 1);
    munit_assert_ptr_equal(got1, b);
    munit_assert_double(got1->f64, ==, 3.14);

    /* Out of range */
    ray_t* oob = ray_list_get(list, 2);
    munit_assert_null(oob);

    /* Release items, then list.
     * list_append retained a and b, so we release our original refs. */
    ray_release(a);
    ray_release(b);
    /* Now the list holds the only refs. Destroy arena cleans up. */

    ray_release(list);
    return MUNIT_OK;
}

/* ---- list_set ---------------------------------------------------------- */

static MunitResult test_list_set(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(4);
    ray_t* a = ray_i64(10);
    ray_t* b = ray_i64(20);
    ray_t* c = ray_i64(30);

    list = ray_list_append(list, a);
    list = ray_list_append(list, b);
    munit_assert_int(list->len, ==, 2);

    /* Replace index 0 with c */
    list = ray_list_set(list, 0, c);
    munit_assert_false(RAY_IS_ERR(list));

    ray_t* got = ray_list_get(list, 0);
    munit_assert_ptr_equal(got, c);
    munit_assert_int(got->i64, ==, 30);

    /* Out of range */
    ray_t* err = ray_list_set(list, 5, a);
    munit_assert_true(RAY_IS_ERR(err));

    ray_release(a);
    ray_release(b);
    ray_release(c);
    ray_release(list);
    return MUNIT_OK;
}

/* ---- list_grow --------------------------------------------------------- */

static MunitResult test_list_grow(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(1);

    /* Append many items to force reallocation */
    ray_t* items[20];
    for (int i = 0; i < 20; i++) {
        items[i] = ray_i64((int64_t)i);
        list = ray_list_append(list, items[i]);
        munit_assert_false(RAY_IS_ERR(list));
    }

    munit_assert_int(list->len, ==, 20);

    /* Verify all items */
    for (int i = 0; i < 20; i++) {
        ray_t* got = ray_list_get(list, (int64_t)i);
        munit_assert_ptr_not_null(got);
        munit_assert_int(got->i64, ==, (int64_t)i);
    }

    for (int i = 0; i < 20; i++) ray_release(items[i]);
    ray_release(list);
    return MUNIT_OK;
}

/* ---- list_empty -------------------------------------------------------- */

static MunitResult test_list_empty(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(0);
    munit_assert_ptr_not_null(list);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_int(list->len, ==, 0);

    ray_t* got = ray_list_get(list, 0);
    munit_assert_null(got);

    ray_release(list);
    return MUNIT_OK;
}

/* ---- list_mixed_types -------------------------------------------------- */

static MunitResult test_list_mixed_types(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(4);

    ray_t* a = ray_i64(42);
    ray_t* b = ray_f64(2.718);
    ray_t* c = ray_bool(true);
    ray_t* d = ray_str("hi", 2);

    list = ray_list_append(list, a);
    list = ray_list_append(list, b);
    list = ray_list_append(list, c);
    list = ray_list_append(list, d);

    munit_assert_int(list->len, ==, 4);

    ray_t* g0 = ray_list_get(list, 0);
    munit_assert_int(g0->type, ==, RAY_ATOM_I64);
    munit_assert_int(g0->i64, ==, 42);

    ray_t* g1 = ray_list_get(list, 1);
    munit_assert_int(g1->type, ==, RAY_ATOM_F64);
    munit_assert_double(g1->f64, ==, 2.718);

    ray_t* g2 = ray_list_get(list, 2);
    munit_assert_int(g2->type, ==, RAY_ATOM_BOOL);
    munit_assert_uint(g2->b8, ==, 1);

    ray_t* g3 = ray_list_get(list, 3);
    munit_assert_int(g3->type, ==, RAY_ATOM_STR);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    ray_release(d);
    ray_release(list);
    return MUNIT_OK;
}

/* ---- list_release_drops_item_ref ---------------------------------------- */

static MunitResult test_list_release_drops_item_ref(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* list = ray_list_new(1);
    munit_assert_ptr_not_null(list);
    munit_assert_false(RAY_IS_ERR(list));

    ray_t* item = ray_i64(42);
    munit_assert_ptr_not_null(item);
    munit_assert_false(RAY_IS_ERR(item));

    list = ray_list_append(list, item);
    munit_assert_ptr_not_null(list);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_uint(atomic_load_explicit(&item->rc, memory_order_relaxed), ==, 2);

    ray_release(list);
    munit_assert_uint(atomic_load_explicit(&item->rc, memory_order_relaxed), ==, 1);

    ray_release(item);
    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest list_tests[] = {
    { "/new",          test_list_new,          list_setup, list_teardown, 0, NULL },
    { "/append_get",   test_list_append_get,   list_setup, list_teardown, 0, NULL },
    { "/set",          test_list_set,          list_setup, list_teardown, 0, NULL },
    { "/grow",         test_list_grow,         list_setup, list_teardown, 0, NULL },
    { "/empty",        test_list_empty,        list_setup, list_teardown, 0, NULL },
    { "/mixed_types",  test_list_mixed_types,  list_setup, list_teardown, 0, NULL },
    { "/release_drops_item_ref", test_list_release_drops_item_ref, list_setup, list_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_list_suite = {
    "/list",
    list_tests,
    NULL,
    0,
    0,
};
