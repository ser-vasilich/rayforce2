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
#include <stdatomic.h>
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void* vec_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    return NULL;
}

static void vec_teardown(void* fixture) {
    (void)fixture;
    ray_heap_destroy();
}

/* ---- vec_new ----------------------------------------------------------- */

static MunitResult test_vec_new(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_I64, 10);
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));
    munit_assert_true(ray_is_vec(v));
    munit_assert_int(v->type, ==, RAY_I64);
    munit_assert_int(v->len, ==, 0);
    ray_release(v);

    return MUNIT_OK;
}

/* ---- vec_new invalid type ---------------------------------------------- */

static MunitResult test_vec_new_invalid(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(-1, 10);
    munit_assert_true(RAY_IS_ERR(v));
    munit_assert_int(RAY_ERR_CODE(v), ==, RAY_ERR_TYPE);

    return MUNIT_OK;
}

/* ---- vec_append -------------------------------------------------------- */

static MunitResult test_vec_append(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_I64, 4);
    munit_assert_ptr_not_null(v);

    int64_t vals[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) {
        v = ray_vec_append(v, &vals[i]);
        munit_assert_false(RAY_IS_ERR(v));
    }

    munit_assert_int(v->len, ==, 3);

    int64_t* data = (int64_t*)ray_data(v);
    munit_assert_int(data[0], ==, 10);
    munit_assert_int(data[1], ==, 20);
    munit_assert_int(data[2], ==, 30);

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_get ----------------------------------------------------------- */

static MunitResult test_vec_get(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {100, 200, 300, 400};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));

    int64_t* p0 = (int64_t*)ray_vec_get(v, 0);
    munit_assert_int(*p0, ==, 100);

    int64_t* p3 = (int64_t*)ray_vec_get(v, 3);
    munit_assert_int(*p3, ==, 400);

    /* Out of range */
    void* oob = ray_vec_get(v, 4);
    munit_assert_null(oob);
    oob = ray_vec_get(v, -1);
    munit_assert_null(oob);

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_set ----------------------------------------------------------- */

static MunitResult test_vec_set(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_ptr_not_null(v);

    int64_t new_val = 99;
    v = ray_vec_set(v, 1, &new_val);
    munit_assert_false(RAY_IS_ERR(v));

    int64_t* p1 = (int64_t*)ray_vec_get(v, 1);
    munit_assert_int(*p1, ==, 99);

    /* Out of range returns error */
    ray_t* err = ray_vec_set(v, 10, &new_val);
    munit_assert_true(RAY_IS_ERR(err));

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_from_raw ------------------------------------------------------ */

static MunitResult test_vec_from_raw(const void* params, void* fixture) {
    (void)params; (void)fixture;

    double raw[] = {1.1, 2.2, 3.3, 4.4, 5.5};
    ray_t* v = ray_vec_from_raw(RAY_F64, raw, 5);
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));
    munit_assert_int(v->type, ==, RAY_F64);
    munit_assert_int(v->len, ==, 5);

    double* data = (double*)ray_data(v);
    munit_assert_double(data[0], ==, 1.1);
    munit_assert_double(data[4], ==, 5.5);

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_slice_basic --------------------------------------------------- */

static MunitResult test_vec_slice_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    munit_assert_ptr_not_null(v);

    ray_t* s = ray_vec_slice(v, 1, 3);
    munit_assert_ptr_not_null(s);
    munit_assert_false(RAY_IS_ERR(s));
    munit_assert_int(s->type, ==, RAY_I64);
    munit_assert_int(s->len, ==, 3);
    munit_assert_uint(s->attrs & RAY_ATTR_SLICE, !=, 0);

    /* Clean up: release slice first (drops parent ref), then parent */
    ray_release(s);
    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_slice_access -------------------------------------------------- */

static MunitResult test_vec_slice_access(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);

    ray_t* s = ray_vec_slice(v, 2, 2);  /* [30, 40] */
    munit_assert_ptr_not_null(s);

    int64_t* p0 = (int64_t*)ray_vec_get(s, 0);
    munit_assert_int(*p0, ==, 30);

    int64_t* p1 = (int64_t*)ray_vec_get(s, 1);
    munit_assert_int(*p1, ==, 40);

    /* Out of range on slice */
    void* oob = ray_vec_get(s, 2);
    munit_assert_null(oob);

    ray_release(s);
    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_concat -------------------------------------------------------- */

static MunitResult test_vec_concat(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t a_raw[] = {1, 2, 3};
    int64_t b_raw[] = {4, 5};
    ray_t* a = ray_vec_from_raw(RAY_I64, a_raw, 3);
    ray_t* b = ray_vec_from_raw(RAY_I64, b_raw, 2);

    ray_t* c = ray_vec_concat(a, b);
    munit_assert_ptr_not_null(c);
    munit_assert_false(RAY_IS_ERR(c));
    munit_assert_int(c->len, ==, 5);

    int64_t* data = (int64_t*)ray_data(c);
    munit_assert_int(data[0], ==, 1);
    munit_assert_int(data[2], ==, 3);
    munit_assert_int(data[3], ==, 4);
    munit_assert_int(data[4], ==, 5);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    return MUNIT_OK;
}

/* ---- null_inline (<=128 elements) -------------------------------------- */

static MunitResult test_vec_null_inline(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_I64, 10);
    /* Manually set len for null testing */
    int64_t vals[10];
    for (int i = 0; i < 10; i++) {
        vals[i] = (int64_t)(i * 10);
        v = ray_vec_append(v, &vals[i]);
    }
    munit_assert_int(v->len, ==, 10);

    /* Initially no nulls */
    munit_assert_false(ray_vec_is_null(v, 0));
    munit_assert_false(ray_vec_is_null(v, 5));

    /* Set some nulls */
    ray_vec_set_null(v, 3, true);
    ray_vec_set_null(v, 7, true);

    munit_assert_true(ray_vec_is_null(v, 3));
    munit_assert_true(ray_vec_is_null(v, 7));
    munit_assert_false(ray_vec_is_null(v, 0));
    munit_assert_false(ray_vec_is_null(v, 4));

    /* Clear a null */
    ray_vec_set_null(v, 3, false);
    munit_assert_false(ray_vec_is_null(v, 3));

    ray_release(v);
    return MUNIT_OK;
}

/* ---- null_external (>128 elements) ------------------------------------- */

static MunitResult test_vec_null_external(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_U8, 200);

    /* Append 200 elements */
    for (int i = 0; i < 200; i++) {
        uint8_t val = (uint8_t)(i & 0xFF);
        v = ray_vec_append(v, &val);
        munit_assert_false(RAY_IS_ERR(v));
    }
    munit_assert_int(v->len, ==, 200);

    /* Set null at index 150 (forces external nullmap) */
    ray_vec_set_null(v, 150, true);
    munit_assert_true(v->attrs & RAY_ATTR_NULLMAP_EXT);
    munit_assert_true(v->attrs & RAY_ATTR_HAS_NULLS);
    munit_assert_true(ray_vec_is_null(v, 150));
    munit_assert_false(ray_vec_is_null(v, 0));
    munit_assert_false(ray_vec_is_null(v, 149));

    /* External nullmap is owned by the vector and released with it. */
    ray_release(v);
    return MUNIT_OK;
}

/* ---- slice_release_parent_ref ------------------------------------------- */

static MunitResult test_vec_slice_release_parent_ref(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {10, 20, 30};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_ptr_not_null(v);

    ray_retain(v); /* guard ref for observing parent rc after slice release */
    munit_assert_uint(atomic_load_explicit(&v->rc, memory_order_relaxed), ==, 2);

    ray_t* s = ray_vec_slice(v, 1, 2);
    munit_assert_ptr_not_null(s);
    munit_assert_false(RAY_IS_ERR(s));
    munit_assert_uint(atomic_load_explicit(&v->rc, memory_order_relaxed), ==, 3);

    ray_release(s);
    munit_assert_uint(atomic_load_explicit(&v->rc, memory_order_relaxed), ==, 2);

    ray_release(v);
    ray_release(v);
    return MUNIT_OK;
}

/* ---- null_external_release_ext_ref -------------------------------------- */

static MunitResult test_vec_null_external_release_ext_ref(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_U8, 200);
    munit_assert_ptr_not_null(v);

    for (int i = 0; i < 200; i++) {
        uint8_t val = (uint8_t)(i & 0xFF);
        v = ray_vec_append(v, &val);
        munit_assert_false(RAY_IS_ERR(v));
    }

    ray_vec_set_null(v, 150, true);
    munit_assert_true(v->attrs & RAY_ATTR_NULLMAP_EXT);
    ray_t* ext = v->ext_nullmap;
    munit_assert_ptr_not_null(ext);

    ray_retain(ext); /* guard ref */
    munit_assert_uint(atomic_load_explicit(&ext->rc, memory_order_relaxed), ==, 2);

    ray_release(v);
    munit_assert_uint(atomic_load_explicit(&ext->rc, memory_order_relaxed), ==, 1);

    ray_release(ext);
    return MUNIT_OK;
}

/* ---- append_grow (test reallocation) ----------------------------------- */

static MunitResult test_vec_append_grow(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_I32, 1);  /* Start with tiny capacity */

    /* Append many elements to force multiple reallocs */
    for (int i = 0; i < 100; i++) {
        int32_t val = (int32_t)(i * 3);
        v = ray_vec_append(v, &val);
        munit_assert_false(RAY_IS_ERR(v));
    }

    munit_assert_int(v->len, ==, 100);

    /* Verify all values are correct after reallocs */
    int32_t* data = (int32_t*)ray_data(v);
    for (int i = 0; i < 100; i++) {
        munit_assert_int(data[i], ==, (int32_t)(i * 3));
    }

    ray_release(v);
    return MUNIT_OK;
}

/* ---- type_correctness -------------------------------------------------- */

static MunitResult test_vec_type_correctness(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Test different vector types */
    ray_t* v_bool = ray_vec_new(RAY_BOOL, 4);
    munit_assert_int(v_bool->type, ==, RAY_BOOL);
    munit_assert_true(ray_is_vec(v_bool));
    munit_assert_false(ray_is_atom(v_bool));
    ray_release(v_bool);

    ray_t* v_f64 = ray_vec_new(RAY_F64, 4);
    munit_assert_int(v_f64->type, ==, RAY_F64);
    ray_release(v_f64);

    ray_t* v_u8 = ray_vec_new(RAY_U8, 4);
    munit_assert_int(v_u8->type, ==, RAY_U8);
    ray_release(v_u8);

    return MUNIT_OK;
}

/* ---- empty_vec --------------------------------------------------------- */

static MunitResult test_vec_empty(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_I64, 0);
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));
    munit_assert_int(v->len, ==, 0);

    /* Get on empty returns NULL */
    void* p = ray_vec_get(v, 0);
    munit_assert_null(p);

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_bool ---------------------------------------------------------- */

static MunitResult test_vec_bool(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_vec_new(RAY_BOOL, 4);
    munit_assert_ptr_not_null(v);

    uint8_t vals[] = {1, 0, 1, 0};
    for (int i = 0; i < 4; i++) {
        v = ray_vec_append(v, &vals[i]);
        munit_assert_false(RAY_IS_ERR(v));
    }

    munit_assert_int(v->len, ==, 4);

    uint8_t* data = (uint8_t*)ray_data(v);
    munit_assert_uint(data[0], ==, 1);
    munit_assert_uint(data[1], ==, 0);
    munit_assert_uint(data[2], ==, 1);
    munit_assert_uint(data[3], ==, 0);

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_concat_type_mismatch ------------------------------------------ */

static MunitResult test_vec_concat_type_mismatch(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t a_raw[] = {1};
    double b_raw[] = {2.0};
    ray_t* a = ray_vec_from_raw(RAY_I64, a_raw, 1);
    ray_t* b = ray_vec_from_raw(RAY_F64, b_raw, 1);

    ray_t* c = ray_vec_concat(a, b);
    munit_assert_true(RAY_IS_ERR(c));
    munit_assert_int(RAY_ERR_CODE(c), ==, RAY_ERR_TYPE);

    ray_release(a);
    ray_release(b);
    return MUNIT_OK;
}

/* ---- vec_slice_out_of_range -------------------------------------------- */

static MunitResult test_vec_slice_range(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);

    ray_t* s = ray_vec_slice(v, 2, 2);  /* offset+len > vec->len */
    munit_assert_true(RAY_IS_ERR(s));

    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_slice_null ---------------------------------------------------- */

static MunitResult test_vec_slice_null(const void* params, void* fixture) {
    (void)params; (void)fixture;
    int64_t vals[] = {10, 20, 30, 40};
    ray_t* v = ray_vec_from_raw(RAY_I64, vals, 4);
    ray_vec_set_null(v, 1, true);
    ray_vec_set_null(v, 3, true);

    ray_t* s = ray_vec_slice(v, 1, 2);
    munit_assert_ptr_not_null(s);
    munit_assert_false(RAY_IS_ERR(s));

    /* Slice index 0 = parent index 1 = null */
    munit_assert_true(ray_vec_is_null(s, 0));
    /* Slice index 1 = parent index 2 = not null */
    munit_assert_false(ray_vec_is_null(s, 1));

    ray_release(s);
    ray_release(v);
    return MUNIT_OK;
}

/* ---- vec_concat_null_propagation --------------------------------------- */

static MunitResult test_vec_concat_null(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t a_raw[] = {1, 2, 3};
    int64_t b_raw[] = {4, 5};
    ray_t* a = ray_vec_from_raw(RAY_I64, a_raw, 3);
    ray_t* b = ray_vec_from_raw(RAY_I64, b_raw, 2);

    ray_vec_set_null(a, 1, true);   /* a[1] = null */
    ray_vec_set_null(b, 0, true);   /* b[0] = null */

    ray_t* c = ray_vec_concat(a, b);
    munit_assert_ptr_not_null(c);
    munit_assert_false(RAY_IS_ERR(c));
    munit_assert_int(c->len, ==, 5);

    munit_assert_false(ray_vec_is_null(c, 0));  /* a[0]=1 */
    munit_assert_true(ray_vec_is_null(c, 1));   /* a[1]=null */
    munit_assert_false(ray_vec_is_null(c, 2));  /* a[2]=3 */
    munit_assert_true(ray_vec_is_null(c, 3));   /* b[0]=null */
    munit_assert_false(ray_vec_is_null(c, 4));  /* b[1]=5 */

    ray_release(c);
    ray_release(a);
    ray_release(b);
    return MUNIT_OK;
}

/* ---- vec_concat_slice_null_propagation --------------------------------- */

static MunitResult test_vec_concat_slice_null(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {10, 20, 30, 40};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    ray_vec_set_null(v, 1, true);  /* v[1] = null */

    ray_t* s = ray_vec_slice(v, 1, 2);  /* s = [null, 30] */

    int64_t b_raw[] = {50};
    ray_t* b = ray_vec_from_raw(RAY_I64, b_raw, 1);

    ray_t* c = ray_vec_concat(s, b);
    munit_assert_ptr_not_null(c);
    munit_assert_false(RAY_IS_ERR(c));
    munit_assert_int(c->len, ==, 3);

    munit_assert_true(ray_vec_is_null(c, 0));   /* slice[0]=null */
    munit_assert_false(ray_vec_is_null(c, 1));  /* slice[1]=30 */
    munit_assert_false(ray_vec_is_null(c, 2));  /* b[0]=50 */

    ray_release(c);
    ray_release(s);
    ray_release(b);
    ray_release(v);
    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest vec_tests[] = {
    { "/new",                test_vec_new,                 vec_setup, vec_teardown, 0, NULL },
    { "/new_invalid",        test_vec_new_invalid,         vec_setup, vec_teardown, 0, NULL },
    { "/append",             test_vec_append,              vec_setup, vec_teardown, 0, NULL },
    { "/get",                test_vec_get,                 vec_setup, vec_teardown, 0, NULL },
    { "/set",                test_vec_set,                 vec_setup, vec_teardown, 0, NULL },
    { "/from_raw",           test_vec_from_raw,            vec_setup, vec_teardown, 0, NULL },
    { "/slice_basic",        test_vec_slice_basic,         vec_setup, vec_teardown, 0, NULL },
    { "/slice_access",       test_vec_slice_access,        vec_setup, vec_teardown, 0, NULL },
    { "/concat",             test_vec_concat,              vec_setup, vec_teardown, 0, NULL },
    { "/null_inline",        test_vec_null_inline,         vec_setup, vec_teardown, 0, NULL },
    { "/null_external",      test_vec_null_external,       vec_setup, vec_teardown, 0, NULL },
    { "/slice_release_parent_ref", test_vec_slice_release_parent_ref, vec_setup, vec_teardown, 0, NULL },
    { "/null_external_release_ext_ref", test_vec_null_external_release_ext_ref, vec_setup, vec_teardown, 0, NULL },
    { "/append_grow",        test_vec_append_grow,         vec_setup, vec_teardown, 0, NULL },
    { "/type_correctness",   test_vec_type_correctness,    vec_setup, vec_teardown, 0, NULL },
    { "/empty",              test_vec_empty,               vec_setup, vec_teardown, 0, NULL },
    { "/bool",               test_vec_bool,                vec_setup, vec_teardown, 0, NULL },
    { "/concat_type_mismatch", test_vec_concat_type_mismatch, vec_setup, vec_teardown, 0, NULL },
    { "/slice_range",        test_vec_slice_range,         vec_setup, vec_teardown, 0, NULL },
    { "/slice_null",         test_vec_slice_null,          vec_setup, vec_teardown, 0, NULL },
    { "/concat_null",        test_vec_concat_null,         vec_setup, vec_teardown, 0, NULL },
    { "/concat_slice_null",  test_vec_concat_slice_null,   vec_setup, vec_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_vec_suite = {
    "/vec",
    vec_tests,
    NULL,
    0,
    0,
};
