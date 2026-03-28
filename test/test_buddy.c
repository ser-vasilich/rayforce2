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
#include <string.h>
#include <stdatomic.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void* buddy_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_heap_init();
    return NULL;
}

static void buddy_teardown(void* fixture) {
    (void)fixture;
    ray_heap_destroy();
}

/* ---- Basic alloc/free -------------------------------------------------- */

static MunitResult test_alloc_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_alloc(0);  /* minimum: just a header */
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));
    munit_assert_uint(v->mmod, ==, 0);
    munit_assert_uint(v->order, >=, RAY_ORDER_MIN);
    munit_assert_uint(atomic_load_explicit(&v->rc, memory_order_relaxed), ==, 1);

    ray_free(v);
    return MUNIT_OK;
}

static MunitResult test_alloc_small_atom(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Atom: 0 bytes of data beyond the 32-byte header */
    ray_t* v = ray_alloc(0);
    munit_assert_ptr_not_null(v);
    /* Header should be zeroed (except mmod, order, rc set by allocator) */
    munit_assert_int(v->type, ==, 0);
    munit_assert_uint(v->attrs, ==, 0);
    ray_free(v);
    return MUNIT_OK;
}

static MunitResult test_alloc_medium_vector(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Vector of 100 i64s: 100 * 8 = 800 bytes of data */
    size_t data_size = 100 * sizeof(int64_t);
    ray_t* v = ray_alloc(data_size);
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));

    /* We should be able to write and read data */
    int64_t* data = (int64_t*)ray_data(v);
    for (int i = 0; i < 100; i++) {
        data[i] = (int64_t)(i * 42);
    }
    for (int i = 0; i < 100; i++) {
        munit_assert_int(data[i], ==, (int64_t)(i * 42));
    }

    ray_free(v);
    return MUNIT_OK;
}

static MunitResult test_alloc_large(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Large allocation: 1 MiB */
    size_t data_size = 1024 * 1024;
    ray_t* v = ray_alloc(data_size);
    munit_assert_ptr_not_null(v);
    munit_assert_false(RAY_IS_ERR(v));
    munit_assert_uint(v->mmod, ==, 0);  /* still within buddy range */

    /* Write pattern */
    uint8_t* data = (uint8_t*)ray_data(v);
    memset(data, 0xAB, data_size);
    munit_assert_uint(data[0], ==, 0xAB);
    munit_assert_uint(data[data_size - 1], ==, 0xAB);

    ray_free(v);
    return MUNIT_OK;
}

/* ---- Header zeroing ---------------------------------------------------- */

static MunitResult test_header_zeroed(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* v = ray_alloc(64);
    munit_assert_ptr_not_null(v);

    /* Check nullmap region is zeroed */
    for (int i = 0; i < 16; i++) {
        munit_assert_uint(v->nullmap[i], ==, 0);
    }
    /* type and attrs should be 0 */
    munit_assert_int(v->type, ==, 0);
    munit_assert_uint(v->attrs, ==, 0);
    /* mmod should be 0 (heap) */
    munit_assert_uint(v->mmod, ==, 0);
    /* rc should be 1 */
    munit_assert_uint(atomic_load_explicit(&v->rc, memory_order_relaxed), ==, 1);

    ray_free(v);
    return MUNIT_OK;
}

/* ---- Slab cache -------------------------------------------------------- */

static MunitResult test_small_block_reuse(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Allocate and free many small blocks; verify they can be re-allocated */
    ray_t* blocks[128];
    for (int i = 0; i < 128; i++) {
        blocks[i] = ray_alloc(0);
        munit_assert_ptr_not_null(blocks[i]);
    }
    for (int i = 0; i < 128; i++) {
        ray_free(blocks[i]);
    }
    /* Re-allocate after freeing -- buddy should reuse freed blocks */
    for (int i = 0; i < 128; i++) {
        blocks[i] = ray_alloc(0);
        munit_assert_ptr_not_null(blocks[i]);
    }

    ray_mem_stats_t stats;
    ray_mem_stats(&stats);
    munit_assert_size(stats.alloc_count, >=, 256);

    for (int i = 0; i < 128; i++) {
        ray_free(blocks[i]);
    }
    return MUNIT_OK;
}

/* ---- Pool growth -------------------------------------------------------- */

static MunitResult test_pool_growth(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Allocate blocks until we exhaust the first 32 MiB pool.
     * Each block is 2^15 = 32 KiB, so ~1024 blocks to fill 32 MiB. */
    size_t block_data_size = (1 << 15) - 32;  /* order 15 (32B ray_t header) */
    int count = 0;
    ray_t* blocks[4096];
    for (int i = 0; i < 4096; i++) {
        blocks[i] = ray_alloc(block_data_size);
        if (!blocks[i]) break;
        count++;
    }
    /* Should have allocated many blocks (some from first pool, some from second) */
    munit_assert_int(count, >, 1000);

    for (int i = 0; i < count; i++) {
        ray_free(blocks[i]);
    }
    return MUNIT_OK;
}

/* ---- Stats tracking ---------------------------------------------------- */

static MunitResult test_mem_stats(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_mem_stats_t stats0;
    ray_mem_stats(&stats0);
    size_t base_alloc = stats0.alloc_count;
    size_t base_free = stats0.free_count;

    ray_t* a = ray_alloc(64);
    ray_t* b = ray_alloc(64);
    ray_t* c = ray_alloc(64);

    ray_mem_stats_t stats1;
    ray_mem_stats(&stats1);
    munit_assert_size(stats1.alloc_count, ==, base_alloc + 3);
    munit_assert_size(stats1.bytes_allocated, >, 0);

    ray_free(a);
    ray_free(b);

    ray_mem_stats_t stats2;
    ray_mem_stats(&stats2);
    munit_assert_size(stats2.free_count, ==, base_free + 2);
    munit_assert_size(stats2.alloc_count, ==, base_alloc + 3);

    ray_free(c);
    return MUNIT_OK;
}

/* ---- Coalescing -------------------------------------------------------- */

static MunitResult test_coalescing(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Allocate two blocks of order 10 (1024 bytes each), then free them.
     * After freeing both, they should coalesce into a larger block.
     * Verify by allocating one block of order 11 (2048 bytes). */
    size_t data_size = (1 << 10) - 32;  /* order 10 (32B ray_t header) */
    ray_t* a = ray_alloc(data_size);
    ray_t* b = ray_alloc(data_size);
    munit_assert_ptr_not_null(a);
    munit_assert_ptr_not_null(b);

    ray_free(a);
    ray_free(b);

    /* Now allocate a block that requires order 11 */
    size_t bigger = (1 << 11) - 32;
    ray_t* c = ray_alloc(bigger);
    munit_assert_ptr_not_null(c);

    ray_free(c);
    return MUNIT_OK;
}

/* ---- ray_alloc_copy ----------------------------------------------------- */

static MunitResult test_alloc_copy(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Create a block with some data */
    size_t data_size = 100 * sizeof(int64_t);
    ray_t* orig = ray_alloc(data_size);
    munit_assert_ptr_not_null(orig);

    orig->type = RAY_I64;
    orig->len = 100;
    int64_t* data = (int64_t*)ray_data(orig);
    for (int i = 0; i < 100; i++) {
        data[i] = (int64_t)(i * 7 + 13);
    }

    /* Copy */
    ray_t* copy = ray_alloc_copy(orig);
    munit_assert_ptr_not_null(copy);
    munit_assert_true((void*)copy != (void*)orig);

    /* Verify copy has same content */
    munit_assert_int(copy->type, ==, RAY_I64);
    munit_assert_int(copy->len, ==, 100);
    int64_t* copy_data = (int64_t*)ray_data(copy);
    for (int i = 0; i < 100; i++) {
        munit_assert_int(copy_data[i], ==, (int64_t)(i * 7 + 13));
    }

    /* Copy should have rc=1, independent of original */
    munit_assert_uint(atomic_load_explicit(&copy->rc, memory_order_relaxed), ==, 1);

    ray_free(orig);
    ray_free(copy);
    return MUNIT_OK;
}

/* ---- Multiple alloc/free cycles ---------------------------------------- */

static MunitResult test_alloc_free_cycles(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Repeated alloc/free should not leak or crash */
    for (int round = 0; round < 10; round++) {
        ray_t* blocks[64];
        for (int i = 0; i < 64; i++) {
            blocks[i] = ray_alloc((size_t)(i * 8));
            munit_assert_ptr_not_null(blocks[i]);
        }
        for (int i = 63; i >= 0; i--) {
            ray_free(blocks[i]);
        }
    }
    return MUNIT_OK;
}

/* ---- Various sizes ----------------------------------------------------- */

static MunitResult test_various_sizes(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Test a range of allocation sizes */
    size_t sizes[] = { 0, 1, 7, 8, 16, 31, 32, 33, 64, 100, 255, 256,
                       512, 1000, 1024, 4096, 8192, 65536, 1048576 };
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    ray_t* blocks[20];
    for (int i = 0; i < n; i++) {
        blocks[i] = ray_alloc(sizes[i]);
        munit_assert_ptr_not_null(blocks[i]);
        munit_assert_false(RAY_IS_ERR(blocks[i]));
        /* Write some data */
        if (sizes[i] > 0) {
            memset(ray_data(blocks[i]), 0xFF, sizes[i]);
        }
    }
    for (int i = 0; i < n; i++) {
        ray_free(blocks[i]);
    }
    return MUNIT_OK;
}

/* ---- Order computation ------------------------------------------------- */

static MunitResult test_order_for_size(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* 0 data bytes -> need 32 bytes total (32B ray_t header) -> order 6 (2^6=64) */
    munit_assert_uint(ray_order_for_size(0), ==, 6);

    /* 1 data byte -> 33 bytes -> order 6 (2^6=64) */
    munit_assert_uint(ray_order_for_size(1), ==, 6);

    /* 32 data bytes -> 64 bytes -> order 6 (exact fit) */
    munit_assert_uint(ray_order_for_size(32), ==, 6);

    /* 33 data bytes -> 65 bytes -> order 7 (2^7=128) */
    munit_assert_uint(ray_order_for_size(33), ==, 7);

    /* 800 data bytes (100 i64s) -> 832 bytes -> order 10 (2^10=1024) */
    munit_assert_uint(ray_order_for_size(800), ==, 10);

    return MUNIT_OK;
}

/* ---- Pool alignment ---------------------------------------------------- */

static MunitResult test_pool_alignment(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Allocate a block and verify self-aligned pool derivation */
    ray_t* v = ray_alloc(64);
    munit_assert_ptr_not_null(v);

    /* Block must be inside a self-aligned pool */
    uintptr_t addr = (uintptr_t)v;
    size_t pool_size = (size_t)1 << 25;  /* 32 MB standard pool */
    uintptr_t pool_base = addr & ~(pool_size - 1);

    /* Pool base must be self-aligned */
    munit_assert_uint(pool_base % pool_size, ==, 0);
    /* Block must be within pool */
    munit_assert_true(addr >= pool_base && addr < pool_base + pool_size);

    ray_free(v);
    return MUNIT_OK;
}

/* ---- Heap ID derivation ------------------------------------------------ */

static MunitResult test_heap_id_derivation(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Allocate blocks and verify pool header heap_id matches current heap */
    ray_t* v1 = ray_alloc(0);
    ray_t* v2 = ray_alloc(1024);
    ray_t* v3 = ray_alloc(65536);
    munit_assert_ptr_not_null(v1);
    munit_assert_ptr_not_null(v2);
    munit_assert_ptr_not_null(v3);

    ray_heap_t* h = ray_tl_heap;

    /* Pool header heap_id should match current heap for all blocks */
    ray_pool_hdr_t* phdr1 = ray_pool_of(v1);
    ray_pool_hdr_t* phdr2 = ray_pool_of(v2);
    ray_pool_hdr_t* phdr3 = ray_pool_of(v3);
    munit_assert_uint(phdr1->heap_id, ==, h->id);
    munit_assert_uint(phdr2->heap_id, ==, h->id);
    munit_assert_uint(phdr3->heap_id, ==, h->id);

    ray_free(v1);
    ray_free(v2);
    ray_free(v3);
    return MUNIT_OK;
}

/* ---- Cross-heap free --------------------------------------------------- */

static MunitResult test_cross_heap_free(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* heap_a is the current heap (from buddy_setup) */
    ray_heap_t* heap_a = ray_tl_heap;
    uint16_t heap_a_id = heap_a->id;

    /* Create heap_b by switching thread-local pointer */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    munit_assert_ptr_not_null(heap_b);
    munit_assert_uint(heap_b->id, !=, heap_a_id);

    /* Allocate blocks on heap_b */
    ray_t* blk1 = ray_alloc(0);
    ray_t* blk2 = ray_alloc(128);
    ray_t* blk3 = ray_alloc(4096);
    munit_assert_ptr_not_null(blk1);
    munit_assert_ptr_not_null(blk2);
    munit_assert_ptr_not_null(blk3);

    /* Pool headers should carry heap_b's ID */
    munit_assert_uint(ray_pool_of(blk1)->heap_id, ==, heap_b->id);
    munit_assert_uint(ray_pool_of(blk2)->heap_id, ==, heap_b->id);

    /* Switch to heap_a and free blocks from the wrong heap */
    ray_tl_heap = heap_a;
    ray_free(blk1);  /* should go to heap_a->foreign */
    ray_free(blk2);
    ray_free(blk3);

    /* Flush foreign blocks back to their owning heap */
    ray_heap_flush_foreign();

    /* Cleanup: destroy heap_b */
    ray_tl_heap = heap_b;
    ray_heap_destroy();

    /* Restore heap_a for teardown */
    ray_tl_heap = heap_a;
    return MUNIT_OK;
}

/* ---- Pending merge ------------------------------------------------------ */

static MunitResult test_heap_pending_merge(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* heap_a is the current heap (from buddy_setup) */
    ray_heap_t* heap_a = ray_tl_heap;
    uint32_t pools_before = heap_a->pool_count;

    /* Create heap_b */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    munit_assert_ptr_not_null(heap_b);

    /* Allocate on heap_b to force pool creation */
    ray_t* blk = ray_alloc(0);
    munit_assert_ptr_not_null(blk);
    uint32_t heap_b_pools = heap_b->pool_count;
    munit_assert_uint(heap_b_pools, >, 0);

    /* Push heap_b onto pending queue (simulating worker teardown) */
    ray_tl_heap = heap_a;
    ray_heap_push_pending(heap_b);

    /* Drain pending — merges heap_b into heap_a, destroys heap_b */
    ray_heap_drain_pending();

    /* heap_a should have gained heap_b's pools */
    munit_assert_uint(heap_a->pool_count, ==, pools_before + heap_b_pools);

    /* The block should still be accessible (pool transferred) */
    munit_assert_uint(blk->mmod, ==, 0);

    /* Free the block — goes via foreign path (heap_id mismatch)
     * then flush reclaims it locally */
    ray_free(blk);
    ray_heap_flush_foreign();

    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest buddy_tests[] = {
    { "/alloc_basic",       test_alloc_basic,       buddy_setup, buddy_teardown, 0, NULL },
    { "/alloc_small_atom",  test_alloc_small_atom,  buddy_setup, buddy_teardown, 0, NULL },
    { "/alloc_medium_vec",  test_alloc_medium_vector, buddy_setup, buddy_teardown, 0, NULL },
    { "/alloc_large",       test_alloc_large,       buddy_setup, buddy_teardown, 0, NULL },
    { "/header_zeroed",     test_header_zeroed,     buddy_setup, buddy_teardown, 0, NULL },
    { "/small_reuse",       test_small_block_reuse, buddy_setup, buddy_teardown, 0, NULL },
    { "/pool_growth",       test_pool_growth,       buddy_setup, buddy_teardown, 0, NULL },
    { "/mem_stats",         test_mem_stats,         buddy_setup, buddy_teardown, 0, NULL },
    { "/coalescing",        test_coalescing,        buddy_setup, buddy_teardown, 0, NULL },
    { "/alloc_copy",        test_alloc_copy,        buddy_setup, buddy_teardown, 0, NULL },
    { "/alloc_free_cycles", test_alloc_free_cycles, buddy_setup, buddy_teardown, 0, NULL },
    { "/various_sizes",     test_various_sizes,     buddy_setup, buddy_teardown, 0, NULL },
    { "/order_for_size",    test_order_for_size,    buddy_setup, buddy_teardown, 0, NULL },
    { "/pool_alignment",    test_pool_alignment,    buddy_setup, buddy_teardown, 0, NULL },
    { "/heap_id_derivation", test_heap_id_derivation, buddy_setup, buddy_teardown, 0, NULL },
    { "/cross_heap_free",   test_cross_heap_free,   buddy_setup, buddy_teardown, 0, NULL },
    { "/pending_merge",     test_heap_pending_merge, buddy_setup, buddy_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_buddy_suite = {
    "/buddy",
    buddy_tests,
    NULL,
    0,
    0,
};
