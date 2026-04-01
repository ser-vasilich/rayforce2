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
#include "core/block.h"
#include "table/sym.h"

/* ---- Accessor macro tests ---------------------------------------------- */

static MunitResult test_type_macros(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t atom;
    memset(&atom, 0, sizeof(atom));
    atom.type = -RAY_I64;  /* atom */
    munit_assert_int(ray_type(&atom), ==, -RAY_I64);
    munit_assert_true(ray_is_atom(&atom));
    munit_assert_false(ray_is_vec(&atom));

    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_F64;    /* vector */
    vec.len  = 100;
    munit_assert_int(ray_type(&vec), ==, RAY_F64);
    munit_assert_false(ray_is_atom(&vec));
    munit_assert_true(ray_is_vec(&vec));
    munit_assert_int(ray_len(&vec), ==, 100);

    ray_t list;
    memset(&list, 0, sizeof(list));
    list.type = RAY_LIST;  /* neither atom nor vec */
    munit_assert_false(ray_is_atom(&list));
    munit_assert_false(ray_is_vec(&list));

    return MUNIT_OK;
}

static MunitResult test_ray_data(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t block;
    memset(&block, 0, sizeof(block));
    void* data = ray_data(&block);
    /* Data should be exactly 32 bytes past the start of the block */
    munit_assert_int((char*)data - (char*)&block, ==, 32);

    return MUNIT_OK;
}

static MunitResult test_elem_size(const void* params, void* fixture) {
    (void)params; (void)fixture;

    munit_assert_int(ray_elem_size(RAY_BOOL), ==, 1);
    munit_assert_int(ray_elem_size(RAY_U8),   ==, 1);
    munit_assert_int(ray_elem_size(RAY_I16),  ==, 2);
    munit_assert_int(ray_elem_size(RAY_I32),  ==, 4);
    munit_assert_int(ray_elem_size(RAY_I64),  ==, 8);
    munit_assert_int(ray_elem_size(RAY_F64),  ==, 8);
    munit_assert_int(ray_elem_size(RAY_SYM), ==, 8);  /* W64 default */
    munit_assert_int(ray_sym_elem_size(RAY_SYM, RAY_SYM_W8),  ==, 1);
    munit_assert_int(ray_sym_elem_size(RAY_SYM, RAY_SYM_W16), ==, 2);
    munit_assert_int(ray_sym_elem_size(RAY_SYM, RAY_SYM_W32), ==, 4);
    munit_assert_int(ray_sym_elem_size(RAY_SYM, RAY_SYM_W64), ==, 8);
    munit_assert_int(ray_elem_size(RAY_GUID), ==, 16);

    return MUNIT_OK;
}

/* ---- ray_block_size tests ----------------------------------------------- */

static MunitResult test_block_size_atom(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t atom;
    memset(&atom, 0, sizeof(atom));
    atom.type = -RAY_F64;  /* atom */
    atom.f64  = 3.14;

    size_t sz = ray_block_size(&atom);
    munit_assert_size(sz, ==, 32);

    return MUNIT_OK;
}

static MunitResult test_block_size_vec(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_I64;
    vec.len  = 10;

    size_t sz = ray_block_size(&vec);
    /* 32 header + 10 * 8 bytes = 112 */
    munit_assert_size(sz, ==, 112);

    return MUNIT_OK;
}

static MunitResult test_block_size_vec_bool(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_BOOL;
    vec.len  = 1024;

    size_t sz = ray_block_size(&vec);
    /* 32 header + 1024 * 1 = 1056 */
    munit_assert_size(sz, ==, 1056);

    return MUNIT_OK;
}

static MunitResult test_block_size_empty_vec(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_F64;
    vec.len  = 0;

    size_t sz = ray_block_size(&vec);
    munit_assert_size(sz, ==, 32);

    return MUNIT_OK;
}

/* ---- ray_t struct size check -------------------------------------------- */

static MunitResult test_ray_t_size(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* ray_t must be exactly 32 bytes */
    munit_assert_size(sizeof(ray_t), ==, 32);

    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest block_tests[] = {
    { "/type_macros",      test_type_macros,         NULL, NULL, 0, NULL },
    { "/ray_data",          test_ray_data,             NULL, NULL, 0, NULL },
    { "/elem_size",        test_elem_size,           NULL, NULL, 0, NULL },
    { "/block_size_atom",  test_block_size_atom,     NULL, NULL, 0, NULL },
    { "/block_size_vec",   test_block_size_vec,      NULL, NULL, 0, NULL },
    { "/block_size_bool",  test_block_size_vec_bool, NULL, NULL, 0, NULL },
    { "/block_size_empty", test_block_size_empty_vec, NULL, NULL, 0, NULL },
    { "/ray_t_size",        test_ray_t_size,           NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_block_suite = {
    "/block",
    block_tests,
    NULL,
    0,
    0,
};
