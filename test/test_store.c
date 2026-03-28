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
#include "ops/ops.h"
#include "store/col.h"
#include "store/fileio.h"
#include "store/splay.h"
#include "store/part.h"
#include "table/sym.h"
#include "table/table.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#define TMP_COL_PATH  "/tmp/rayforce_test_col.dat"
#define TMP_SPLAY_DIR "/tmp/rayforce_test_splay"

/* ---- Setup / Teardown -------------------------------------------------- */

static void* store_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    (void)ray_sym_init();
    return NULL;
}

static void store_teardown(void* fixture) {
    (void)fixture;
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- test_col_mmap_i64 ------------------------------------------------- */

static MunitResult test_col_mmap_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));

    /* Save to file */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load via mmap */
    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    munit_assert_ptr_not_null(mapped);
    munit_assert_false(RAY_IS_ERR(mapped));

    /* Verify mmod==1 */
    munit_assert_uint(mapped->mmod, ==, 1);

    /* Verify type, len, data */
    munit_assert_int(mapped->type, ==, RAY_I64);
    munit_assert_int(mapped->len, ==, 5);

    int64_t* data = (int64_t*)ray_data(mapped);
    for (int i = 0; i < 5; i++) {
        munit_assert_int(data[i], ==, raw[i]);
    }

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_mmap_f64 ------------------------------------------------- */

static MunitResult test_col_mmap_f64(const void* params, void* fixture) {
    (void)params; (void)fixture;

    double raw[] = {1.1, 2.2, 3.3, 4.4};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 4);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    munit_assert_ptr_not_null(mapped);
    munit_assert_false(RAY_IS_ERR(mapped));

    munit_assert_uint(mapped->mmod, ==, 1);
    munit_assert_int(mapped->type, ==, RAY_F64);
    munit_assert_int(mapped->len, ==, 4);

    double* data = (double*)ray_data(mapped);
    for (int i = 0; i < 4; i++) {
        munit_assert_double(data[i], ==, raw[i]);
    }

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_mmap_cow ------------------------------------------------- */

static MunitResult test_col_mmap_cow(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {100, 200, 300};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_false(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    munit_assert_false(RAY_IS_ERR(mapped));
    munit_assert_uint(mapped->mmod, ==, 1);

    /* Retain so rc==2, forcing ray_cow to make a real copy */
    ray_retain(mapped);
    munit_assert_uint(mapped->rc, ==, 2);

    /* COW: ray_cow should produce a buddy-allocated copy */
    ray_t* copy = ray_cow(mapped);
    munit_assert_ptr_not_null(copy);
    munit_assert_false(RAY_IS_ERR(copy));
    munit_assert_uint(copy->mmod, ==, 0);

    /* ray_cow called ray_release on mapped (rc 2->1), so mapped still alive */

    /* Verify data in copy */
    int64_t* data = (int64_t*)ray_data(copy);
    for (int i = 0; i < 3; i++) {
        munit_assert_int(data[i], ==, raw[i]);
    }

    ray_release(copy);
    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_mmap_refcount -------------------------------------------- */

static MunitResult test_col_mmap_refcount(const void* params, void* fixture) {
    (void)params; (void)fixture;

    int64_t raw[] = {7, 8, 9};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    munit_assert_false(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    munit_assert_false(RAY_IS_ERR(mapped));
    munit_assert_uint(mapped->rc, ==, 1);

    /* Retain: rc should be 2 */
    ray_retain(mapped);
    munit_assert_uint(mapped->rc, ==, 2);

    /* Release once: rc==1, still readable */
    ray_release(mapped);
    munit_assert_uint(mapped->rc, ==, 1);

    int64_t* data = (int64_t*)ray_data(mapped);
    munit_assert_int(data[0], ==, 7);
    munit_assert_int(data[1], ==, 8);
    munit_assert_int(data[2], ==, 9);

    /* Release again: munmap */
    ray_release(mapped);

    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_mmap_corrupt --------------------------------------------- */

static MunitResult test_col_mmap_corrupt(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Write a 16-byte file (too small for a valid column header) */
    FILE* f = fopen(TMP_COL_PATH, "wb");
    munit_assert_ptr_not_null(f);
    uint8_t junk[16] = {0};
    fwrite(junk, 1, 16, f);
    fclose(f);

    ray_t* result = ray_col_mmap(TMP_COL_PATH);
    munit_assert_true(RAY_IS_ERR(result));
    munit_assert_int(RAY_ERR_CODE(result), ==, RAY_ERR_CORRUPT);

    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_mmap_nofile ---------------------------------------------- */

static MunitResult test_col_mmap_nofile(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* result = ray_col_mmap("/tmp/rayforce_nonexistent_file_xyz.dat");
    munit_assert_true(RAY_IS_ERR(result));
    munit_assert_int(RAY_ERR_CODE(result), ==, RAY_ERR_IO);

    return MUNIT_OK;
}

/* ---- test_splay_open_roundtrip ----------------------------------------- */

static MunitResult test_splay_open_roundtrip(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Clean up any leftover splay dir */
    (void)!system("rm -rf " TMP_SPLAY_DIR);

    /* Build a 3-column table: I64, F64, I32 */
    ray_t* tbl = ray_table_new(4);
    munit_assert_ptr_not_null(tbl);
    munit_assert_false(RAY_IS_ERR(tbl));

    int64_t id_a = ray_sym_intern("col_a", 5);
    int64_t id_b = ray_sym_intern("col_b", 5);
    int64_t id_c = ray_sym_intern("col_c", 5);

    int64_t raw_a[] = {1, 2, 3, 4, 5};
    double  raw_b[] = {1.5, 2.5, 3.5, 4.5, 5.5};
    int32_t raw_c[] = {10, 20, 30, 40, 50};

    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw_a, 5);
    ray_t* col_b = ray_vec_from_raw(RAY_F64, raw_b, 5);
    ray_t* col_c = ray_vec_from_raw(RAY_I32, raw_c, 5);
    munit_assert_false(RAY_IS_ERR(col_a));
    munit_assert_false(RAY_IS_ERR(col_b));
    munit_assert_false(RAY_IS_ERR(col_c));

    tbl = ray_table_add_col(tbl, id_a, col_a);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_b);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_c, col_c);
    munit_assert_false(RAY_IS_ERR(tbl));

    /* Save to splay directory */
    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_DIR, NULL);
    munit_assert_int(err, ==, RAY_OK);

    /* Open via mmap (zero-copy) */
    ray_t* loaded = ray_read_splayed(TMP_SPLAY_DIR, NULL);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));

    /* Verify ncols and nrows */
    munit_assert_int(ray_table_ncols(loaded), ==, 3);
    munit_assert_int(ray_table_nrows(loaded), ==, 5);

    /* Verify column mmod==1 (mmap'd) */
    ray_t* la = ray_table_get_col(loaded, id_a);
    ray_t* lb = ray_table_get_col(loaded, id_b);
    ray_t* lc = ray_table_get_col(loaded, id_c);
    munit_assert_ptr_not_null(la);
    munit_assert_ptr_not_null(lb);
    munit_assert_ptr_not_null(lc);

    munit_assert_uint(la->mmod, ==, 1);
    munit_assert_uint(lb->mmod, ==, 1);
    munit_assert_uint(lc->mmod, ==, 1);

    /* Verify data */
    int64_t* da = (int64_t*)ray_data(la);
    double*  db = (double*)ray_data(lb);
    int32_t* dc = (int32_t*)ray_data(lc);

    for (int i = 0; i < 5; i++) {
        munit_assert_int(da[i], ==, raw_a[i]);
        munit_assert_double(db[i], ==, raw_b[i]);
        munit_assert_int(dc[i], ==, raw_c[i]);
    }

    ray_release(loaded);
    ray_release(col_a);
    ray_release(col_b);
    ray_release(col_c);
    ray_release(tbl);

    /* Cleanup */
    (void)!system("rm -rf " TMP_SPLAY_DIR);
    return MUNIT_OK;
}

/* ---- test_parted_nrows ------------------------------------------------- */

static MunitResult test_parted_nrows(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build 3 segment vectors: 100, 200, 300 rows */
    ray_t* seg0 = ray_vec_new(RAY_I64, 100);
    ray_t* seg1 = ray_vec_new(RAY_I64, 200);
    ray_t* seg2 = ray_vec_new(RAY_I64, 300);
    munit_assert_false(RAY_IS_ERR(seg0));
    munit_assert_false(RAY_IS_ERR(seg1));
    munit_assert_false(RAY_IS_ERR(seg2));
    seg0->len = 100;
    seg1->len = 200;
    seg2->len = 300;

    /* Build a parted column: type = RAY_PARTED_BASE + RAY_I64, len = 3 segments */
    size_t data_size = 3 * sizeof(ray_t*);
    ray_t* parted = ray_alloc(data_size);
    munit_assert_ptr_not_null(parted);
    munit_assert_false(RAY_IS_ERR(parted));
    parted->type = RAY_PARTED_BASE + RAY_I64;
    parted->len = 3;
    parted->attrs = 0;
    memset(parted->nullmap, 0, 16);

    ray_t** segs = (ray_t**)ray_data(parted);
    segs[0] = seg0; ray_retain(seg0);
    segs[1] = seg1; ray_retain(seg1);
    segs[2] = seg2; ray_retain(seg2);

    /* Verify ray_parted_nrows returns 600 */
    int64_t total = ray_parted_nrows(parted);
    munit_assert_int(total, ==, 600);

    /* Non-parted vector falls through to v->len */
    munit_assert_int(ray_parted_nrows(seg0), ==, 100);

    ray_release(parted);
    ray_release(seg0);
    ray_release(seg1);
    ray_release(seg2);
    return MUNIT_OK;
}

/* ---- test_table_nrows_parted ------------------------------------------- */

static MunitResult test_table_nrows_parted(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build 2 segment vectors: 50 and 75 rows */
    ray_t* seg0 = ray_vec_new(RAY_I64, 50);
    ray_t* seg1 = ray_vec_new(RAY_I64, 75);
    munit_assert_false(RAY_IS_ERR(seg0));
    munit_assert_false(RAY_IS_ERR(seg1));
    seg0->len = 50;
    seg1->len = 75;

    /* Build a parted column */
    size_t data_size = 2 * sizeof(ray_t*);
    ray_t* parted = ray_alloc(data_size);
    munit_assert_false(RAY_IS_ERR(parted));
    parted->type = RAY_PARTED_BASE + RAY_I64;
    parted->len = 2;
    parted->attrs = 0;
    memset(parted->nullmap, 0, 16);

    ray_t** segs = (ray_t**)ray_data(parted);
    segs[0] = seg0; ray_retain(seg0);
    segs[1] = seg1; ray_retain(seg1);

    /* Build a table with this parted column */
    int64_t name_id = ray_sym_intern("pcol", 4);
    ray_t* tbl = ray_table_new(2);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, name_id, parted);
    munit_assert_false(RAY_IS_ERR(tbl));

    /* Verify ray_table_nrows returns 125 */
    munit_assert_int(ray_table_nrows(tbl), ==, 125);

    ray_release(tbl);
    ray_release(parted);
    ray_release(seg0);
    ray_release(seg1);
    return MUNIT_OK;
}

/* ---- test_parted_release ----------------------------------------------- */

static MunitResult test_parted_release(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build 2 segment vectors */
    ray_t* seg0 = ray_vec_new(RAY_I64, 10);
    ray_t* seg1 = ray_vec_new(RAY_I64, 20);
    munit_assert_false(RAY_IS_ERR(seg0));
    munit_assert_false(RAY_IS_ERR(seg1));
    seg0->len = 10;
    seg1->len = 20;

    /* Build a parted column */
    size_t data_size = 2 * sizeof(ray_t*);
    ray_t* parted = ray_alloc(data_size);
    munit_assert_false(RAY_IS_ERR(parted));
    parted->type = RAY_PARTED_BASE + RAY_I64;
    parted->len = 2;
    parted->attrs = 0;
    memset(parted->nullmap, 0, 16);

    ray_t** segs = (ray_t**)ray_data(parted);
    segs[0] = seg0; ray_retain(seg0);
    segs[1] = seg1; ray_retain(seg1);

    /* Segments should have rc=2 (original + parted ref) */
    munit_assert_uint(seg0->rc, ==, 2);
    munit_assert_uint(seg1->rc, ==, 2);

    /* Release parted column — segments' rc should drop to 1 */
    ray_release(parted);
    munit_assert_uint(seg0->rc, ==, 1);
    munit_assert_uint(seg1->rc, ==, 1);

    ray_release(seg0);
    ray_release(seg1);
    return MUNIT_OK;
}

/* ---- test_part_open ---------------------------------------------------- */

#define TMP_PART_DB "/tmp/rayforce_test_parted_db"
#define TMP_TABLE_NAME "test_tbl"

static MunitResult test_part_open(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Setup: create a 2-partition db with 2 columns each */
    (void)!system("rm -rf " TMP_PART_DB);
    (void)!system("mkdir -p " TMP_PART_DB "/2024.01.01/" TMP_TABLE_NAME);
    (void)!system("mkdir -p " TMP_PART_DB "/2024.01.02/" TMP_TABLE_NAME);

    /* Partition 1: 3 rows */
    int64_t raw_a1[] = {10, 20, 30};
    double  raw_b1[] = {1.1, 2.2, 3.3};
    ray_t* a1 = ray_vec_from_raw(RAY_I64, raw_a1, 3);
    ray_t* b1 = ray_vec_from_raw(RAY_F64, raw_b1, 3);

    ray_t* tbl1 = ray_table_new(3);
    int64_t name_a = ray_sym_intern("a", 1);
    int64_t name_b = ray_sym_intern("b", 1);
    tbl1 = ray_table_add_col(tbl1, name_a, a1);
    tbl1 = ray_table_add_col(tbl1, name_b, b1);
    ray_err_t err = ray_splay_save(tbl1, TMP_PART_DB "/2024.01.01/" TMP_TABLE_NAME, NULL);
    munit_assert_int(err, ==, RAY_OK);

    /* Partition 2: 5 rows */
    int64_t raw_a2[] = {40, 50, 60, 70, 80};
    double  raw_b2[] = {4.4, 5.5, 6.6, 7.7, 8.8};
    ray_t* a2 = ray_vec_from_raw(RAY_I64, raw_a2, 5);
    ray_t* b2 = ray_vec_from_raw(RAY_F64, raw_b2, 5);

    ray_t* tbl2 = ray_table_new(3);
    tbl2 = ray_table_add_col(tbl2, name_a, a2);
    tbl2 = ray_table_add_col(tbl2, name_b, b2);
    err = ray_splay_save(tbl2, TMP_PART_DB "/2024.01.02/" TMP_TABLE_NAME, NULL);
    munit_assert_int(err, ==, RAY_OK);

    /* Save symfile */
    err = ray_sym_save(TMP_PART_DB "/sym");
    munit_assert_int(err, ==, RAY_OK);

    /* Cleanup in-memory tables */
    ray_release(a1); ray_release(b1); ray_release(tbl1);
    ray_release(a2); ray_release(b2); ray_release(tbl2);

    /* Open via ray_read_parted */
    ray_t* parted = ray_read_parted(TMP_PART_DB, TMP_TABLE_NAME);
    munit_assert_ptr_not_null(parted);
    munit_assert_false(RAY_IS_ERR(parted));

    /* Should have 3 columns: date (MAPCOMMON), a (parted I64), b (parted F64) */
    int64_t ncols = ray_table_ncols(parted);
    munit_assert_int(ncols, ==, 3);

    /* Total rows should be 8 */
    int64_t nrows = ray_table_nrows(parted);
    munit_assert_int(nrows, ==, 8);

    /* Verify first column is MAPCOMMON (date-inferred) */
    ray_t* mapcommon = ray_table_get_col_idx(parted, 0);
    munit_assert_ptr_not_null(mapcommon);
    munit_assert_int(mapcommon->type, ==, RAY_MAPCOMMON);
    munit_assert_uint(mapcommon->attrs, ==, RAY_MC_DATE);

    /* MAPCOMMON: [key_values (RAY_DATE), row_counts (RAY_I64)] */
    ray_t** mc_ptrs = (ray_t**)ray_data(mapcommon);
    ray_t* key_values = mc_ptrs[0];
    ray_t* row_counts = mc_ptrs[1];

    /* key_values should be RAY_DATE with parsed days-since-2000 */
    munit_assert_int(key_values->type, ==, RAY_DATE);
    munit_assert_int(key_values->len, ==, 2);
    int32_t* kv_data = (int32_t*)ray_data(key_values);
    /* 2024.01.01 = 8766 days since 2000-01-01 */
    munit_assert_int(kv_data[0], ==, 8766);
    /* 2024.01.02 = 8767 */
    munit_assert_int(kv_data[1], ==, 8767);

    munit_assert_int(row_counts->len, ==, 2);
    int64_t* rc_data = (int64_t*)ray_data(row_counts);
    munit_assert_int(rc_data[0], ==, 3);
    munit_assert_int(rc_data[1], ==, 5);

    /* Verify second column is parted I64 */
    ray_t* col_a = ray_table_get_col_idx(parted, 1);
    munit_assert_ptr_not_null(col_a);
    munit_assert_true(RAY_IS_PARTED(col_a->type));
    munit_assert_int(RAY_PARTED_BASETYPE(col_a->type), ==, RAY_I64);
    munit_assert_int(col_a->len, ==, 2);

    /* Verify segment 0 has 3 rows, mmod=1 (mmap'd) */
    ray_t** segs_a = (ray_t**)ray_data(col_a);
    munit_assert_int(segs_a[0]->len, ==, 3);
    munit_assert_uint(segs_a[0]->mmod, ==, 1);
    munit_assert_int(segs_a[1]->len, ==, 5);
    munit_assert_uint(segs_a[1]->mmod, ==, 1);

    /* Verify data in segment 0 */
    int64_t* data_a0 = (int64_t*)ray_data(segs_a[0]);
    munit_assert_int(data_a0[0], ==, 10);
    munit_assert_int(data_a0[2], ==, 30);

    /* Verify third column is parted F64 */
    ray_t* col_b = ray_table_get_col_idx(parted, 2);
    munit_assert_true(RAY_IS_PARTED(col_b->type));
    munit_assert_int(RAY_PARTED_BASETYPE(col_b->type), ==, RAY_F64);

    /* Release — should unmap all segments */
    ray_release(parted);

    (void)!system("rm -rf " TMP_PART_DB);
    return MUNIT_OK;
}

/* ---- test_group_parted ------------------------------------------------- */

static MunitResult test_group_parted(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build a 2-partition parted table with columns id1 (I64) and v1 (I64).
     * Partition 0: id1=[0,0,1,1,2], v1=[10,20,30,40,50]
     * Partition 1: id1=[0,1,1,2,2], v1=[60,70,80,90,100]
     * GROUP BY id1 SUM(v1) should give:
     *   id1=0: 10+20+60 = 90
     *   id1=1: 30+40+70+80 = 220
     *   id1=2: 50+90+100 = 240
     */

    /* Build segment vectors */
    ray_t* id1_0 = ray_vec_new(RAY_I64, 5);
    ray_t* v1_0  = ray_vec_new(RAY_I64, 5);
    munit_assert_ptr_not_null(id1_0);
    munit_assert_ptr_not_null(v1_0);
    id1_0->len = v1_0->len = 5;
    int64_t id1_0_data[] = {0,0,1,1,2};
    int64_t v1_0_data[]  = {10,20,30,40,50};
    memcpy(ray_data(id1_0), id1_0_data, sizeof(id1_0_data));
    memcpy(ray_data(v1_0),  v1_0_data,  sizeof(v1_0_data));

    ray_t* id1_1 = ray_vec_new(RAY_I64, 5);
    ray_t* v1_1  = ray_vec_new(RAY_I64, 5);
    munit_assert_ptr_not_null(id1_1);
    munit_assert_ptr_not_null(v1_1);
    id1_1->len = v1_1->len = 5;
    int64_t id1_1_data[] = {0,1,1,2,2};
    int64_t v1_1_data[]  = {60,70,80,90,100};
    memcpy(ray_data(id1_1), id1_1_data, sizeof(id1_1_data));
    memcpy(ray_data(v1_1),  v1_1_data,  sizeof(v1_1_data));

    /* Build parted columns (2 segments each) */
    ray_t* id1_parted = ray_alloc(2 * sizeof(ray_t*));
    munit_assert_ptr_not_null(id1_parted);
    id1_parted->type = RAY_PARTED_BASE + RAY_I64;
    id1_parted->len = 2;
    ((ray_t**)ray_data(id1_parted))[0] = id1_0;
    ((ray_t**)ray_data(id1_parted))[1] = id1_1;

    ray_t* v1_parted = ray_alloc(2 * sizeof(ray_t*));
    munit_assert_ptr_not_null(v1_parted);
    v1_parted->type = RAY_PARTED_BASE + RAY_I64;
    v1_parted->len = 2;
    ((ray_t**)ray_data(v1_parted))[0] = v1_0;
    ((ray_t**)ray_data(v1_parted))[1] = v1_1;

    /* Build parted table */
    int64_t sym_id1 = ray_sym_intern("id1", 3);
    int64_t sym_v1  = ray_sym_intern("v1",  2);

    ray_t* tbl = ray_table_new(2);
    munit_assert_ptr_not_null(tbl);
    tbl = ray_table_add_col(tbl, sym_id1, id1_parted);
    tbl = ray_table_add_col(tbl, sym_v1,  v1_parted);
    munit_assert_int(ray_table_nrows(tbl), ==, 10);

    /* Build graph: GROUP BY id1 SUM(v1) */
    ray_graph_t* g = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g);
    ray_op_t* scan_id1 = ray_scan(g, "id1");
    ray_op_t* scan_v1  = ray_scan(g, "v1");
    ray_op_t* keys[] = { scan_id1 };
    uint16_t ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v1 };
    ray_op_t* root = ray_group(g, keys, 1, ops, ins, 1);
    root = ray_optimize(g, root);
    ray_t* result = ray_execute(g, root);

    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(ray_table_nrows(result), ==, 3); /* groups: 0, 1, 2 */

    /* Verify sums — extract key and agg columns, match by key value */
    ray_t* rk = ray_table_get_col_idx(result, 0); /* id1 key column */
    ray_t* rv = ray_table_get_col_idx(result, 1); /* v1_sum agg column */
    munit_assert_ptr_not_null(rk);
    munit_assert_ptr_not_null(rv);

    int64_t* rk_data = (int64_t*)ray_data(rk);
    int64_t* rv_data = (int64_t*)ray_data(rv);
    int64_t expected_sums[3] = {0, 0, 0}; /* for keys 0, 1, 2 */
    for (int i = 0; i < 3; i++) {
        int64_t key = rk_data[i];
        munit_assert_true(key >= 0 && key <= 2);
        expected_sums[key] = rv_data[i];
    }
    munit_assert_int(expected_sums[0], ==, 90);
    munit_assert_int(expected_sums[1], ==, 220);
    munit_assert_int(expected_sums[2], ==, 240);

    ray_release(result);
    ray_graph_free(g);
    ray_release(id1_parted);
    ray_release(v1_parted);
    ray_release(tbl);
    return MUNIT_OK;
}

/* ---- test_col_ext_nullmap_roundtrip ------------------------------------- */

#define EXT_NM_LEN 256  /* >128 to trigger ext_nullmap */

static MunitResult test_col_ext_nullmap_roundtrip(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Create a 256-element I64 vector with nulls at various positions */
    ray_t* vec = ray_vec_new(RAY_I64, EXT_NM_LEN);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));
    vec->len = EXT_NM_LEN;

    int64_t* data = (int64_t*)ray_data(vec);
    for (int i = 0; i < EXT_NM_LEN; i++) data[i] = i * 10;

    /* Set nulls at positions: 0, 5, 127, 128, 200, 255 */
    int null_positions[] = { 0, 5, 127, 128, 200, 255 };
    int n_nulls = (int)(sizeof(null_positions) / sizeof(null_positions[0]));
    for (int i = 0; i < n_nulls; i++)
        ray_vec_set_null(vec, null_positions[i], true);

    /* Verify ext_nullmap was created (>128 elements forces external) */
    munit_assert_true((vec->attrs & RAY_ATTR_HAS_NULLS) != 0);
    munit_assert_true((vec->attrs & RAY_ATTR_NULLMAP_EXT) != 0);
    munit_assert_ptr_not_null(vec->ext_nullmap);

    /* --- Round-trip via ray_col_load --- */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));

    munit_assert_int(loaded->type, ==, RAY_I64);
    munit_assert_int(loaded->len, ==, EXT_NM_LEN);
    munit_assert_true((loaded->attrs & RAY_ATTR_HAS_NULLS) != 0);
    munit_assert_true((loaded->attrs & RAY_ATTR_NULLMAP_EXT) != 0);
    munit_assert_ptr_not_null(loaded->ext_nullmap);

    /* Verify null positions preserved */
    for (int i = 0; i < n_nulls; i++)
        munit_assert_true(ray_vec_is_null(loaded, null_positions[i]));

    /* Verify non-null positions */
    munit_assert_false(ray_vec_is_null(loaded, 1));
    munit_assert_false(ray_vec_is_null(loaded, 129));
    munit_assert_false(ray_vec_is_null(loaded, 254));

    /* Verify data values at non-null positions */
    int64_t* ld = (int64_t*)ray_data(loaded);
    munit_assert_int(ld[1], ==, 10);
    munit_assert_int(ld[129], ==, 1290);
    munit_assert_int(ld[254], ==, 2540);

    ray_release(loaded);

    /* --- Round-trip via ray_col_mmap --- */
    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    munit_assert_ptr_not_null(mapped);
    munit_assert_false(RAY_IS_ERR(mapped));

    munit_assert_uint(mapped->mmod, ==, 1);
    munit_assert_int(mapped->type, ==, RAY_I64);
    munit_assert_int(mapped->len, ==, EXT_NM_LEN);
    munit_assert_true((mapped->attrs & RAY_ATTR_HAS_NULLS) != 0);
    munit_assert_true((mapped->attrs & RAY_ATTR_NULLMAP_EXT) != 0);
    munit_assert_ptr_not_null(mapped->ext_nullmap);

    /* Verify null positions preserved in mmap path */
    for (int i = 0; i < n_nulls; i++)
        munit_assert_true(ray_vec_is_null(mapped, null_positions[i]));

    munit_assert_false(ray_vec_is_null(mapped, 1));
    munit_assert_false(ray_vec_is_null(mapped, 129));

    /* Verify data */
    int64_t* md = (int64_t*)ray_data(mapped);
    munit_assert_int(md[1], ==, 10);
    munit_assert_int(md[129], ==, 1290);

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_save_load_str -------------------------------------------- */

static MunitResult test_col_save_load_str(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build a list of 3 string atoms */
    ray_t* list = ray_list_new(4);
    munit_assert_ptr_not_null(list);
    munit_assert_false(RAY_IS_ERR(list));

    ray_t* s0 = ray_str("hello", 5);
    ray_t* s1 = ray_str("world", 5);
    ray_t* s2 = ray_str("rayforce", 8);
    munit_assert_false(RAY_IS_ERR(s0));
    munit_assert_false(RAY_IS_ERR(s1));
    munit_assert_false(RAY_IS_ERR(s2));

    list = ray_list_append(list, s0);
    list = ray_list_append(list, s1);
    list = ray_list_append(list, s2);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_int(list->len, ==, 3);

    /* Save */
    ray_err_t err = ray_col_save(list, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));

    /* Verify */
    munit_assert_int(loaded->type, ==, RAY_LIST);
    munit_assert_int(loaded->len, ==, 3);

    ray_t* l0 = ray_list_get(loaded, 0);
    ray_t* l1 = ray_list_get(loaded, 1);
    ray_t* l2 = ray_list_get(loaded, 2);
    munit_assert_ptr_not_null(l0);
    munit_assert_ptr_not_null(l1);
    munit_assert_ptr_not_null(l2);
    munit_assert_int(l0->type, ==, -RAY_STR);
    munit_assert_int(l1->type, ==, -RAY_STR);
    munit_assert_int(l2->type, ==, -RAY_STR);

    munit_assert_size(ray_str_len(l0), ==, 5);
    munit_assert_size(ray_str_len(l1), ==, 5);
    munit_assert_size(ray_str_len(l2), ==, 8);
    munit_assert_string_equal(ray_str_ptr(l0), "hello");
    munit_assert_string_equal(ray_str_ptr(l1), "world");
    munit_assert_string_equal(ray_str_ptr(l2), "rayforce");

    ray_release(loaded);
    ray_release(s0);
    ray_release(s1);
    ray_release(s2);
    ray_release(list);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_save_load_list ------------------------------------------- */

static MunitResult test_col_save_load_list(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build a list of two I64 vectors */
    int64_t raw0[] = {10, 20, 30};
    int64_t raw1[] = {40, 50};
    ray_t* v0 = ray_vec_from_raw(RAY_I64, raw0, 3);
    ray_t* v1 = ray_vec_from_raw(RAY_I64, raw1, 2);
    munit_assert_false(RAY_IS_ERR(v0));
    munit_assert_false(RAY_IS_ERR(v1));

    ray_t* list = ray_list_new(4);
    munit_assert_false(RAY_IS_ERR(list));
    list = ray_list_append(list, v0);
    list = ray_list_append(list, v1);
    munit_assert_false(RAY_IS_ERR(list));
    munit_assert_int(list->len, ==, 2);

    /* Save */
    ray_err_t err = ray_col_save(list, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));

    /* Verify */
    munit_assert_int(loaded->type, ==, RAY_LIST);
    munit_assert_int(loaded->len, ==, 2);

    ray_t* lv0 = ray_list_get(loaded, 0);
    ray_t* lv1 = ray_list_get(loaded, 1);
    munit_assert_ptr_not_null(lv0);
    munit_assert_ptr_not_null(lv1);
    munit_assert_int(lv0->type, ==, RAY_I64);
    munit_assert_int(lv1->type, ==, RAY_I64);
    munit_assert_int(lv0->len, ==, 3);
    munit_assert_int(lv1->len, ==, 2);

    int64_t* d0 = (int64_t*)ray_data(lv0);
    munit_assert_int(d0[0], ==, 10);
    munit_assert_int(d0[1], ==, 20);
    munit_assert_int(d0[2], ==, 30);

    int64_t* d1 = (int64_t*)ray_data(lv1);
    munit_assert_int(d1[0], ==, 40);
    munit_assert_int(d1[1], ==, 50);

    ray_release(loaded);
    ray_release(v0);
    ray_release(v1);
    ray_release(list);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_col_save_load_table ------------------------------------------ */

static MunitResult test_col_save_load_table(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build a 2-column table: I64 + F64 */
    int64_t id_a = ray_sym_intern("col_x", 5);
    int64_t id_b = ray_sym_intern("col_y", 5);

    int64_t raw_a[] = {1, 2, 3};
    double  raw_b[] = {1.5, 2.5, 3.5};
    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw_a, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_F64, raw_b, 3);
    munit_assert_false(RAY_IS_ERR(col_a));
    munit_assert_false(RAY_IS_ERR(col_b));

    ray_t* tbl = ray_table_new(4);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_a, col_a);
    munit_assert_false(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_b);
    munit_assert_false(RAY_IS_ERR(tbl));

    /* Save */
    ray_err_t err = ray_col_save(tbl, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));

    /* Verify */
    munit_assert_int(loaded->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_ncols(loaded), ==, 2);
    munit_assert_int(ray_table_nrows(loaded), ==, 3);

    /* Verify column names */
    munit_assert_int(ray_table_col_name(loaded, 0), ==, id_a);
    munit_assert_int(ray_table_col_name(loaded, 1), ==, id_b);

    /* Verify I64 column */
    ray_t* la = ray_table_get_col(loaded, id_a);
    munit_assert_ptr_not_null(la);
    munit_assert_int(la->type, ==, RAY_I64);
    munit_assert_int(la->len, ==, 3);
    int64_t* da = (int64_t*)ray_data(la);
    munit_assert_int(da[0], ==, 1);
    munit_assert_int(da[1], ==, 2);
    munit_assert_int(da[2], ==, 3);

    /* Verify F64 column */
    ray_t* lb = ray_table_get_col(loaded, id_b);
    munit_assert_ptr_not_null(lb);
    munit_assert_int(lb->type, ==, RAY_F64);
    munit_assert_int(lb->len, ==, 3);
    double* db = (double*)ray_data(lb);
    munit_assert_double(db[0], ==, 1.5);
    munit_assert_double(db[1], ==, 2.5);
    munit_assert_double(db[2], ==, 3.5);

    ray_release(loaded);
    ray_release(col_a);
    ray_release(col_b);
    ray_release(tbl);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_file_open_close ---------------------------------------------- */

#define TMP_FILEIO_PATH "/tmp/rayforce_test_fileio.dat"

static MunitResult test_file_open_close(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Open for write+create, then close */
    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    munit_assert_int(fd, !=, RAY_FD_INVALID);
    ray_file_close(fd);

    /* Open for read (file now exists) */
    fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    munit_assert_int(fd, !=, RAY_FD_INVALID);
    ray_file_close(fd);

    /* Open nonexistent for read (no create) → fail */
    unlink(TMP_FILEIO_PATH);
    fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    munit_assert_int(fd, ==, RAY_FD_INVALID);

    /* NULL path → fail */
    fd = ray_file_open(NULL, 0);
    munit_assert_int(fd, ==, RAY_FD_INVALID);

    return MUNIT_OK;
}

/* ---- test_file_lock_unlock --------------------------------------------- */

static MunitResult test_file_lock_unlock(const void* params, void* fixture) {
    (void)params; (void)fixture;

    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    munit_assert_int(fd, !=, RAY_FD_INVALID);

    /* Exclusive lock + unlock */
    ray_err_t err = ray_file_lock_ex(fd);
    munit_assert_int(err, ==, RAY_OK);
    err = ray_file_unlock(fd);
    munit_assert_int(err, ==, RAY_OK);

    /* Shared lock + unlock */
    err = ray_file_lock_sh(fd);
    munit_assert_int(err, ==, RAY_OK);
    err = ray_file_unlock(fd);
    munit_assert_int(err, ==, RAY_OK);

    /* Invalid fd → error */
    munit_assert_int(ray_file_lock_ex(RAY_FD_INVALID), ==, RAY_ERR_IO);
    munit_assert_int(ray_file_lock_sh(RAY_FD_INVALID), ==, RAY_ERR_IO);
    munit_assert_int(ray_file_unlock(RAY_FD_INVALID), ==, RAY_OK);

    ray_file_close(fd);
    unlink(TMP_FILEIO_PATH);
    return MUNIT_OK;
}

/* ---- test_file_sync_op ------------------------------------------------- */

static MunitResult test_file_sync_op(const void* params, void* fixture) {
    (void)params; (void)fixture;

    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    munit_assert_int(fd, !=, RAY_FD_INVALID);

    /* fsync on valid fd */
    ray_err_t err = ray_file_sync(fd);
    munit_assert_int(err, ==, RAY_OK);

    /* Invalid fd → error */
    munit_assert_int(ray_file_sync(RAY_FD_INVALID), ==, RAY_ERR_IO);

    ray_file_close(fd);
    unlink(TMP_FILEIO_PATH);
    return MUNIT_OK;
}

/* ---- test_file_rename_op ----------------------------------------------- */

#define TMP_FILEIO_PATH2 "/tmp/rayforce_test_fileio2.dat"

static MunitResult test_file_rename_op(const void* params, void* fixture) {
    (void)params; (void)fixture;

    unlink(TMP_FILEIO_PATH);
    unlink(TMP_FILEIO_PATH2);

    /* Create source file */
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    munit_assert_int(fd, !=, RAY_FD_INVALID);
    ray_file_close(fd);

    /* Rename */
    ray_err_t err = ray_file_rename(TMP_FILEIO_PATH, TMP_FILEIO_PATH2);
    munit_assert_int(err, ==, RAY_OK);

    /* Old path should not exist, new should */
    fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    munit_assert_int(fd, ==, RAY_FD_INVALID);

    fd = ray_file_open(TMP_FILEIO_PATH2, RAY_OPEN_READ);
    munit_assert_int(fd, !=, RAY_FD_INVALID);
    ray_file_close(fd);

    /* Rename nonexistent → error */
    err = ray_file_rename("/tmp/rayforce_nonexistent_xyz", TMP_FILEIO_PATH2);
    munit_assert_int(err, ==, RAY_ERR_IO);

    /* NULL args → error */
    munit_assert_int(ray_file_rename(NULL, TMP_FILEIO_PATH2), ==, RAY_ERR_IO);
    munit_assert_int(ray_file_rename(TMP_FILEIO_PATH, NULL), ==, RAY_ERR_IO);

    unlink(TMP_FILEIO_PATH2);
    return MUNIT_OK;
}

/* ---- test_file_shared_lock_concurrent ---------------------------------- */

static MunitResult test_file_shared_lock_concurrent(const void* params, void* fixture) {
    (void)params; (void)fixture;

    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd1 = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    ray_fd_t fd2 = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    munit_assert_int(fd1, !=, RAY_FD_INVALID);
    munit_assert_int(fd2, !=, RAY_FD_INVALID);

    /* Two shared locks should not conflict */
    ray_err_t err1 = ray_file_lock_sh(fd1);
    ray_err_t err2 = ray_file_lock_sh(fd2);
    munit_assert_int(err1, ==, RAY_OK);
    munit_assert_int(err2, ==, RAY_OK);

    ray_file_unlock(fd1);
    ray_file_unlock(fd2);
    ray_file_close(fd1);
    ray_file_close(fd2);
    unlink(TMP_FILEIO_PATH);
    return MUNIT_OK;
}

/* ---- test_sym_col_bounds_reject ----------------------------------------- */

static MunitResult test_sym_col_bounds_reject(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Intern a few symbols so sym_count > 0 */
    ray_sym_intern("sym_a", 5);
    ray_sym_intern("sym_b", 5);
    uint32_t sc = ray_sym_count();
    munit_assert_uint(sc, >=, 2);

    /* Build a W8 RAY_SYM column with valid indices */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W8, 4);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));
    vec->len = 4;
    uint8_t* data = (uint8_t*)ray_data(vec);
    data[0] = 0; data[1] = 1; data[2] = 0; data[3] = 1;

    /* Save — should embed sym count in header rc field */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load back — should succeed since all indices < sym_count */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(loaded->type, ==, RAY_SYM);
    munit_assert_int(loaded->len, ==, 4);
    ray_release(loaded);

    /* Now craft a column with an out-of-range index */
    data[2] = (uint8_t)(sc + 10);  /* beyond sym table */
    err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Load should fail with RAY_ERR_CORRUPT */
    ray_t* bad = ray_col_load(TMP_COL_PATH);
    munit_assert_true(RAY_IS_ERR(bad));
    munit_assert_int(RAY_ERR_CODE(bad), ==, RAY_ERR_CORRUPT);

    /* Same test via mmap */
    bad = ray_col_mmap(TMP_COL_PATH);
    munit_assert_true(RAY_IS_ERR(bad));
    munit_assert_int(RAY_ERR_CODE(bad), ==, RAY_ERR_CORRUPT);

    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_sym_col_count_mismatch --------------------------------------- */

static MunitResult test_sym_col_count_mismatch(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Intern enough symbols to have a known count */
    ray_sym_intern("cnt_a", 5);
    ray_sym_intern("cnt_b", 5);
    ray_sym_intern("cnt_c", 5);
    ray_sym_intern("cnt_d", 5);
    uint32_t sc = ray_sym_count();
    munit_assert_uint(sc, >=, 4);

    /* Build a W8 RAY_SYM column with valid indices */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W8, 3);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));
    vec->len = 3;
    uint8_t* data = (uint8_t*)ray_data(vec);
    data[0] = 0; data[1] = 1; data[2] = 2;

    /* Save with current sym count */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);
    ray_release(vec);

    /* Destroy sym table and re-init with fewer symbols.
     * This simulates loading a column against a smaller sym table. */
    ray_sym_destroy();
    (void)ray_sym_init();
    ray_sym_intern("only_one", 8);
    uint32_t new_sc = ray_sym_count();
    munit_assert_uint(new_sc, <, sc);

    /* Load should fail: saved sym count > current sym count (fast-reject) */
    ray_t* bad = ray_col_load(TMP_COL_PATH);
    munit_assert_true(RAY_IS_ERR(bad));
    munit_assert_int(RAY_ERR_CODE(bad), ==, RAY_ERR_CORRUPT);

    /* Same via mmap */
    bad = ray_col_mmap(TMP_COL_PATH);
    munit_assert_true(RAY_IS_ERR(bad));
    munit_assert_int(RAY_ERR_CODE(bad), ==, RAY_ERR_CORRUPT);

    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_sym_col_valid_roundtrip -------------------------------------- */

static MunitResult test_sym_col_valid_roundtrip(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Intern symbols */
    int64_t id0 = ray_sym_intern("rt_alpha", 8);
    int64_t id1 = ray_sym_intern("rt_beta", 7);
    int64_t id2 = ray_sym_intern("rt_gamma", 8);

    /* Build W16 RAY_SYM column */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W16, 5);
    munit_assert_ptr_not_null(vec);
    munit_assert_false(RAY_IS_ERR(vec));
    vec->len = 5;
    uint16_t* data = (uint16_t*)ray_data(vec);
    data[0] = (uint16_t)id0;
    data[1] = (uint16_t)id1;
    data[2] = (uint16_t)id2;
    data[3] = (uint16_t)id0;
    data[4] = (uint16_t)id1;

    /* Save + load roundtrip */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    munit_assert_int(err, ==, RAY_OK);

    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(loaded->type, ==, RAY_SYM);
    munit_assert_int(loaded->len, ==, 5);
    munit_assert_uint(loaded->attrs & RAY_SYM_W_MASK, ==, RAY_SYM_W16);

    uint16_t* ld = (uint16_t*)ray_data(loaded);
    munit_assert_int(ld[0], ==, id0);
    munit_assert_int(ld[1], ==, id1);
    munit_assert_int(ld[2], ==, id2);
    munit_assert_int(ld[3], ==, id0);
    munit_assert_int(ld[4], ==, id1);

    ray_release(loaded);

    /* Save + mmap roundtrip */
    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    munit_assert_ptr_not_null(mapped);
    munit_assert_false(RAY_IS_ERR(mapped));
    munit_assert_int(mapped->type, ==, RAY_SYM);
    munit_assert_int(mapped->len, ==, 5);

    uint16_t* md = (uint16_t*)ray_data(mapped);
    munit_assert_int(md[0], ==, id0);
    munit_assert_int(md[2], ==, id2);

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    return MUNIT_OK;
}

/* ---- test_splay_load_with_sym ------------------------------------------ */

#define TMP_SPLAY_SYM_DIR "/tmp/rayforce_test_splay_sym"
#define TMP_SYM_PATH      "/tmp/rayforce_test_splay_sym_file"

static MunitResult test_splay_load_with_sym(const void* params, void* fixture) {
    (void)params; (void)fixture;

    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);

    /* Intern symbols and build a table with a RAY_SYM column */
    int64_t id_name = ray_sym_intern("name", 4);
    int64_t id_age  = ray_sym_intern("age", 3);
    int64_t sym_alice = ray_sym_intern("alice", 5);
    int64_t sym_bob   = ray_sym_intern("bob", 3);

    /* Build I64 column */
    int64_t raw_age[] = {30, 25};
    ray_t* col_age = ray_vec_from_raw(RAY_I64, raw_age, 2);
    munit_assert_false(RAY_IS_ERR(col_age));

    /* Build RAY_SYM W8 column */
    ray_t* col_name = ray_sym_vec_new(RAY_SYM_W8, 4);
    munit_assert_false(RAY_IS_ERR(col_name));
    col_name->len = 2;
    uint8_t* sym_data = (uint8_t*)ray_data(col_name);
    sym_data[0] = (uint8_t)sym_alice;
    sym_data[1] = (uint8_t)sym_bob;

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_name, col_name);
    tbl = ray_table_add_col(tbl, id_age, col_age);
    munit_assert_false(RAY_IS_ERR(tbl));

    /* Save splay + sym */
    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_SYM_DIR, TMP_SYM_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Reset sym table, then load via ray_splay_load with sym_path */
    ray_sym_destroy();
    (void)ray_sym_init();
    munit_assert_uint(ray_sym_count(), ==, 0);

    ray_t* loaded = ray_splay_load(TMP_SPLAY_SYM_DIR, TMP_SYM_PATH);
    munit_assert_ptr_not_null(loaded);
    munit_assert_false(RAY_IS_ERR(loaded));
    munit_assert_int(ray_table_ncols(loaded), ==, 2);
    munit_assert_int(ray_table_nrows(loaded), ==, 2);

    /* Sym table should be populated again */
    munit_assert_uint(ray_sym_count(), >, 0);

    ray_release(loaded);
    ray_release(col_name);
    ray_release(col_age);
    ray_release(tbl);
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);
    unlink(TMP_SYM_PATH ".lk");
    return MUNIT_OK;
}

/* ---- test_splay_load_sym_missing_corrupt ------------------------------- */

static MunitResult test_splay_load_sym_missing_corrupt(const void* params, void* fixture) {
    (void)params; (void)fixture;

    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);

    /* Intern symbols and build a table with a RAY_SYM column */
    int64_t id_col = ray_sym_intern("scol", 4);
    int64_t sym_val = ray_sym_intern("val_x", 5);

    ray_t* col = ray_sym_vec_new(RAY_SYM_W8, 4);
    munit_assert_false(RAY_IS_ERR(col));
    col->len = 1;
    ((uint8_t*)ray_data(col))[0] = (uint8_t)sym_val;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_col, col);
    munit_assert_false(RAY_IS_ERR(tbl));

    /* Save splay + sym */
    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_SYM_DIR, TMP_SYM_PATH);
    munit_assert_int(err, ==, RAY_OK);

    /* Reset sym table — simulate loading without sym */
    ray_sym_destroy();
    (void)ray_sym_init();
    munit_assert_uint(ray_sym_count(), ==, 0);

    /* Load with NULL sym_path — should fail because RAY_SYM column exists
     * but sym table is empty. Note: col.c bounds check catches this first
     * since sym_count==0 skips validation, but the post-load check in
     * ray_splay_load catches RAY_SYM + empty sym table. */
    ray_t* loaded = ray_splay_load(TMP_SPLAY_SYM_DIR, NULL);
    munit_assert_true(RAY_IS_ERR(loaded));
    munit_assert_int(RAY_ERR_CODE(loaded), ==, RAY_ERR_CORRUPT);

    ray_release(col);
    ray_release(tbl);
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);
    unlink(TMP_SYM_PATH ".lk");
    return MUNIT_OK;
}

/* ---- test_read_splayed_bad_sym_fatal ----------------------------------- */

static MunitResult test_read_splayed_bad_sym_fatal(const void* params, void* fixture) {
    (void)params; (void)fixture;

    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);

    /* Build a simple table (no RAY_SYM columns needed) */
    int64_t id_x = ray_sym_intern("x", 1);
    int64_t raw[] = {1, 2, 3};
    ray_t* col_x = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_x, col_x);
    munit_assert_false(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_SYM_DIR, NULL);
    munit_assert_int(err, ==, RAY_OK);

    /* ray_read_splayed with nonexistent sym_path — should fail fatally */
    ray_t* loaded = ray_read_splayed(TMP_SPLAY_SYM_DIR, "/tmp/rayforce_nonexistent_sym_xyz");
    munit_assert_true(RAY_IS_ERR(loaded));

    ray_release(col_x);
    ray_release(tbl);
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest store_tests[] = {
    { "/col_mmap_i64",         test_col_mmap_i64,         store_setup, store_teardown, 0, NULL },
    { "/col_mmap_f64",         test_col_mmap_f64,         store_setup, store_teardown, 0, NULL },
    { "/col_mmap_cow",         test_col_mmap_cow,         store_setup, store_teardown, 0, NULL },
    { "/col_mmap_refcount",    test_col_mmap_refcount,    store_setup, store_teardown, 0, NULL },
    { "/col_mmap_corrupt",     test_col_mmap_corrupt,     store_setup, store_teardown, 0, NULL },
    { "/col_mmap_nofile",      test_col_mmap_nofile,      store_setup, store_teardown, 0, NULL },
    { "/splay_open_roundtrip", test_splay_open_roundtrip, store_setup, store_teardown, 0, NULL },
    { "/parted_nrows",        test_parted_nrows,         store_setup, store_teardown, 0, NULL },
    { "/table_nrows_parted",  test_table_nrows_parted,   store_setup, store_teardown, 0, NULL },
    { "/parted_release",      test_parted_release,        store_setup, store_teardown, 0, NULL },
    { "/part_open",            test_part_open,            store_setup, store_teardown, 0, NULL },
    { "/group_parted",         test_group_parted,         store_setup, store_teardown, 0, NULL },
    { "/col_ext_nullmap_roundtrip", test_col_ext_nullmap_roundtrip, store_setup, store_teardown, 0, NULL },
    { "/col_save_load_str",   test_col_save_load_str,   store_setup, store_teardown, 0, NULL },
    { "/col_save_load_list",  test_col_save_load_list,  store_setup, store_teardown, 0, NULL },
    { "/col_save_load_table", test_col_save_load_table, store_setup, store_teardown, 0, NULL },
    { "/file_open_close",     test_file_open_close,     store_setup, store_teardown, 0, NULL },
    { "/file_lock_unlock",    test_file_lock_unlock,    store_setup, store_teardown, 0, NULL },
    { "/file_sync",           test_file_sync_op,        store_setup, store_teardown, 0, NULL },
    { "/file_rename",         test_file_rename_op,      store_setup, store_teardown, 0, NULL },
    { "/file_shared_lock",    test_file_shared_lock_concurrent, store_setup, store_teardown, 0, NULL },
    { "/sym_col_bounds_reject", test_sym_col_bounds_reject, store_setup, store_teardown, 0, NULL },
    { "/sym_col_count_mismatch", test_sym_col_count_mismatch, store_setup, store_teardown, 0, NULL },
    { "/sym_col_valid_roundtrip", test_sym_col_valid_roundtrip, store_setup, store_teardown, 0, NULL },
    { "/splay_load_with_sym",    test_splay_load_with_sym,    store_setup, store_teardown, 0, NULL },
    { "/splay_load_sym_missing", test_splay_load_sym_missing_corrupt, store_setup, store_teardown, 0, NULL },
    { "/read_splayed_bad_sym",   test_read_splayed_bad_sym_fatal, store_setup, store_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_store_suite = {
    "/store",
    store_tests,
    NULL,
    0,
    0,
};
