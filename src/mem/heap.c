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

#include "heap.h"
#include "cow.h"
#include "sys.h"
#include "core/platform.h"
#include "table/sym.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Static asserts
 * -------------------------------------------------------------------------- */
_Static_assert(sizeof(ray_pool_hdr_t) <= 16,
               "ray_pool_hdr_t must fit in nullmap (16 bytes)");

/* --------------------------------------------------------------------------
 * Thread-local state
 * -------------------------------------------------------------------------- */
RAY_TLS ray_heap_t*     ray_tl_heap  = NULL;
RAY_TLS ray_mem_stats_t ray_tl_stats;

/* Stats tracking — always enabled (plain integer ops, negligible vs atomics) */
#define RAY_STAT(x) (x)

/* --------------------------------------------------------------------------
 * Bitmap-based heap ID allocator (atomic CAS, reusable IDs)
 *
 * Each bit in the bitmap represents one heap ID. Acquiring sets a bit,
 * releasing clears it. IDs are reused after release (unlike a monotonic
 * counter). Cursor rotates to spread contention across words.
 * -------------------------------------------------------------------------- */
static _Atomic(uint64_t) g_heap_id_bitmap[RAY_HEAP_ID_WORDS] = { [0] = 1ULL };
static _Atomic(uint64_t) g_heap_id_cursor = 0;

ray_heap_t* ray_heap_registry[RAY_HEAP_REGISTRY_SIZE];

/* Pending-merge queue head (lock-free LIFO) */
_Atomic(ray_heap_t*) ray_heap_pending_merge = NULL;

static int heap_id_acquire(void) {
    uint64_t start = atomic_fetch_add_explicit(&g_heap_id_cursor, 1,
                                                memory_order_relaxed);
    for (uint64_t off = 0; off < RAY_HEAP_ID_WORDS; off++) {
        uint64_t idx = (start + off) % RAY_HEAP_ID_WORDS;
        uint64_t word = atomic_load_explicit(&g_heap_id_bitmap[idx],
                                              memory_order_relaxed);
        while (~word != 0ULL) {
            uint64_t free_bits = ~word;
            uint64_t bit = (uint64_t)__builtin_ctzll(free_bits);
            uint64_t mask = 1ULL << bit;
            uint64_t new_word = word | mask;
            if (atomic_compare_exchange_weak_explicit(
                    &g_heap_id_bitmap[idx], &word, new_word,
                    memory_order_acq_rel, memory_order_relaxed)) {
                return (int)(idx * 64 + bit);
            }
            /* CAS failed — word updated, retry with new value */
        }
    }
    return -1;  /* pool exhausted */
}

static void heap_id_release(int id) {
    if (id < 0 || id >= (int)RAY_HEAP_ID_BITS) return;
    uint64_t idx = (uint64_t)id >> 6;
    uint64_t bit = (uint64_t)id & 63ULL;
    uint64_t mask = ~(1ULL << bit);
    atomic_fetch_and_explicit(&g_heap_id_bitmap[idx], mask,
                               memory_order_release);
}

/* --------------------------------------------------------------------------
 * Parallel flag
 * -------------------------------------------------------------------------- */
_Atomic(uint32_t) ray_parallel_flag = 0;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static uint8_t ceil_log2(size_t n) {
    if (n <= 1) return 0;
    return (uint8_t)(64 - __builtin_clzll(n - 1));
}

uint8_t ray_order_for_size(size_t data_size) {
    if (data_size > SIZE_MAX - 32) return RAY_HEAP_MAX_ORDER + 1;
    size_t total = data_size + 32;  /* 32B ray_t header (no prefix) */
    uint8_t k = ceil_log2(total);
    if (k < RAY_ORDER_MIN) k = RAY_ORDER_MIN;
    return k;
}

/* --------------------------------------------------------------------------
 * Pool management
 *
 * Self-aligned pools: pool base = ptr & ~(pool_size - 1).
 * First min-block (64B at offset 0) reserved for pool header.
 * Remaining space split via cascading buddy split.
 *
 * For oversized blocks (order > POOL_ORDER), pool_order = order + 1
 * so the cascading split produces a right-half block of the needed order.
 * -------------------------------------------------------------------------- */

static bool heap_add_pool(ray_heap_t* h, uint8_t order);

/* --------------------------------------------------------------------------
 * Freelist operations (circular sentinel via fl_prev/fl_next)
 *
 * Each freelist[order] is a ray_fl_head_t sentinel. fl_remove() unlinks a
 * block from ANY circular list without needing the head pointer — enabling
 * safe cross-heap buddy coalescing.
 * -------------------------------------------------------------------------- */

RAY_INLINE void heap_insert_block(ray_heap_t* h, ray_t* blk, uint8_t order) {
    ray_fl_head_t* head = &h->freelist[order];
    ray_t* first = head->fl_next;
    blk->fl_prev = (ray_t*)head;
    blk->fl_next = first;
    first->fl_prev = blk;
    head->fl_next = blk;
    ray_atomic_store(&blk->rc, 0);  /* free marker */
    blk->order = order;
    h->avail |= (1ULL << order);
}

/* heap_remove_block: currently unused — retained for future coalescing paths */
static void __attribute__((unused))
heap_remove_block(ray_heap_t* h, ray_t* blk, uint8_t order) {
    fl_remove(blk);  /* circular unlink — works across heaps */
    if (fl_empty(&h->freelist[order]))
        h->avail &= ~(1ULL << order);
}

RAY_INLINE void heap_split_block(ray_heap_t* h, ray_t* blk,
                                uint8_t target_order, uint8_t block_order) {
    while (block_order > target_order) {
        block_order--;
        ray_t* buddy = (ray_t*)((char*)blk + BSIZEOF(block_order));
        buddy->mmod  = 0;
        buddy->order = block_order;
        heap_insert_block(h, buddy, block_order);
    }
}

/* --------------------------------------------------------------------------
 * Coalescing: merge block with buddies up to pool_order
 *
 * Pool header at offset 0 has rc=1 and order=RAY_ORDER_MIN, so buddy
 * checks always fail before reaching the header. Safe sentinel.
 * -------------------------------------------------------------------------- */

static void heap_coalesce(ray_heap_t* h, ray_t* blk,
                          uintptr_t pool_base, uint8_t pool_order) {
    uint8_t order = blk->order;

    /* During parallel execution, skip coalescing entirely — buddies may
     * belong to other heaps' freelists, and fl_remove would corrupt them. */
    if (atomic_load_explicit(&ray_parallel_flag, memory_order_relaxed) != 0) {
        heap_insert_block(h, blk, order);
        return;
    }

    for (;; order++) {
        if (order >= pool_order) break;

        ray_t* buddy = ray_buddy_of(blk, order, pool_base);
        __builtin_prefetch(buddy, 0, 1);

        uint32_t buddy_rc = ray_atomic_load(&buddy->rc);
        if (buddy_rc != 0 || buddy->order != order) break;

        fl_remove(buddy);
        if (fl_empty(&h->freelist[order]))
            h->avail &= ~(1ULL << order);

        blk = (buddy < blk) ? buddy : blk;
    }

    heap_insert_block(h, blk, order);
}

/* --------------------------------------------------------------------------
 * heap_add_pool implementation
 * -------------------------------------------------------------------------- */

static bool heap_add_pool(ray_heap_t* h, uint8_t order) {
    if (h->pool_count >= RAY_MAX_POOLS) return false;

    uint8_t pool_order;
    if (order >= RAY_HEAP_POOL_ORDER)
        pool_order = order + 1;  /* need one order larger for header + block */
    else
        pool_order = RAY_HEAP_POOL_ORDER;

    if (pool_order > RAY_HEAP_MAX_ORDER) return false;
    size_t pool_size = BSIZEOF(pool_order);

    void* mem = ray_vm_alloc_aligned(pool_size, pool_size);
    if (!mem) return false;

    /* --- Write pool header at offset 0 --- */
    ray_t* hdr_block = (ray_t*)mem;
    memset(hdr_block, 0, BSIZEOF(RAY_ORDER_MIN));
    hdr_block->mmod  = 0;
    hdr_block->order = RAY_ORDER_MIN;
    ray_atomic_store(&hdr_block->rc, 1);  /* sentinel: never free */

    ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)hdr_block;  /* overlay on nullmap */
    hdr->heap_id    = h->id;
    hdr->pool_order = pool_order;
    hdr->vm_base    = mem;  /* on POSIX, same as aligned base */

    /* --- Cascading split: split from pool_order down to RAY_ORDER_MIN.
     *     Right half of each split → freelist.
     *     Leftmost min-block = pool header (already set, rc=1). --- */
    for (uint8_t o = pool_order; o > RAY_ORDER_MIN; o--) {
        ray_t* right = (ray_t*)((char*)mem + BSIZEOF(o - 1));
        right->mmod  = 0;
        right->order = (uint8_t)(o - 1);
        heap_insert_block(h, right, (uint8_t)(o - 1));
    }

    /* --- Track pool --- */
    h->pools[h->pool_count].base       = mem;
    h->pools[h->pool_count].pool_order = pool_order;
    h->pool_count++;

    return true;
}

/* --------------------------------------------------------------------------
 * Slab cache flush (with coalescing for GC effectiveness)
 * -------------------------------------------------------------------------- */

static void heap_flush_slabs(ray_heap_t* h) {
    for (int i = 0; i < RAY_SLAB_ORDERS; i++) {
        while (h->slabs[i].count > 0) {
            ray_t* blk = h->slabs[i].stack[--h->slabs[i].count];
            int pidx = heap_find_pool(h, blk);
            uintptr_t pb;
            uint8_t po;
            if (pidx >= 0) {
                pb = (uintptr_t)h->pools[pidx].base;
                po = h->pools[pidx].pool_order;
            } else {
                ray_pool_hdr_t* phdr = ray_pool_of(blk);
                pb = (uintptr_t)phdr;
                po = phdr->pool_order;
            }
            heap_coalesce(h, blk, pb, po);
        }
    }
}

/* --------------------------------------------------------------------------
 * Foreign blocks flush
 *
 * When return_to_owner is true, returns each foreign block to its owning
 * heap (via pool header heap_id → global registry). This ensures workers
 * can reuse their pools across queries instead of allocating new ones.
 *
 * return_to_owner must only be true when workers are idle (on semaphore),
 * i.e. ray_parallel_flag == 0. Otherwise coalesce into current heap.
 * -------------------------------------------------------------------------- */

static void heap_flush_foreign(ray_heap_t* h, bool return_to_owner) {
    ray_t* blk = h->foreign;
    while (blk) {
        ray_t* next = blk->fl_next;
        if (return_to_owner) {
            ray_pool_hdr_t* phdr = ray_pool_of(blk);  /* GC path, not hot */
            uint16_t owner_id = phdr->heap_id;
            ray_heap_t* owner = ray_heap_registry[owner_id % RAY_HEAP_REGISTRY_SIZE];
            if (owner && owner->id == owner_id && owner != h) {
                int pidx = heap_find_pool(owner, blk);
                uintptr_t pb;
                uint8_t po;
                if (pidx >= 0) {
                    pb = (uintptr_t)owner->pools[pidx].base;
                    po = owner->pools[pidx].pool_order;
                } else {
                    pb = (uintptr_t)phdr;
                    po = phdr->pool_order;
                }
                heap_coalesce(owner, blk, pb, po);
                blk = next;
                continue;
            }
        }
        /* Local coalesce */
        int pidx = heap_find_pool(h, blk);
        uintptr_t pb;
        uint8_t po;
        if (pidx >= 0) {
            pb = (uintptr_t)h->pools[pidx].base;
            po = h->pools[pidx].pool_order;
        } else {
            ray_pool_hdr_t* phdr = ray_pool_of(blk);
            pb = (uintptr_t)phdr;
            po = phdr->pool_order;
        }
        heap_coalesce(h, blk, pb, po);
        blk = next;
    }
    h->foreign = NULL;
}

/* --------------------------------------------------------------------------
 * Owned-reference helpers
 * -------------------------------------------------------------------------- */

static bool ray_atom_str_is_sso(const ray_t* s) {
    if (s->slen >= 1 && s->slen <= 7) return true;
    if (s->slen == 0 && s->obj == NULL) return true;
    return false;
}

static bool ray_atom_owns_obj(const ray_t* v) {
    if (v->type == -RAY_GUID) return v->obj != NULL;
    if (v->type == -RAY_STR) return !ray_atom_str_is_sso(v);
    return false;
}

static void ray_release_owned_refs(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;

    if (ray_is_atom(v)) {
        if (v->type == RAY_LAMBDA) {
            /* Lambda stores [params, body, bytecode, constants] in ray_data */
            ray_t** slots = (ray_t**)ray_data(v);
            for (int i = 0; i < 4; i++) {
                if (slots[i] && !RAY_IS_ERR(slots[i]))
                    ray_release(slots[i]);
            }
            return;
        }
        if (v->type == RAY_LAZY) {
            ray_graph_t* g = RAY_LAZY_GRAPH(v);
            if (g) {
                ray_graph_free(g);
                RAY_LAZY_GRAPH(v) = NULL;
            }
            return;
        }
        if (ray_atom_owns_obj(v) && v->obj && !RAY_IS_ERR(v->obj))
            ray_release(v->obj);
        return;
    }

    if (v->attrs & RAY_ATTR_SLICE) {
        if (v->slice_parent && !RAY_IS_ERR(v->slice_parent))
            ray_release(v->slice_parent);
        return;
    }

    if ((v->attrs & RAY_ATTR_NULLMAP_EXT) &&
        v->ext_nullmap && !RAY_IS_ERR(v->ext_nullmap))
        ray_release(v->ext_nullmap);

    if (v->type == RAY_STR && v->str_pool && !RAY_IS_ERR(v->str_pool))
        ray_release(v->str_pool);

    if (RAY_IS_PARTED(v->type)) {
        int64_t n_segs = v->len;
        ray_t** segs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n_segs; i++) {
            if (segs[i] && !RAY_IS_ERR(segs[i]))
                ray_release(segs[i]);
        }
        return;
    }

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        if (ptrs[0] && !RAY_IS_ERR(ptrs[0])) ray_release(ptrs[0]);
        if (ptrs[1] && !RAY_IS_ERR(ptrs[1])) ray_release(ptrs[1]);
        return;
    }

    if (v->type == RAY_TABLE) {
        if (v->len < 0) return;
        ray_t** slots = (ray_t**)ray_data(v);
        ray_t* schema = slots[0];
        if (schema && !RAY_IS_ERR(schema)) ray_release(schema);

        ray_t** cols = slots + 1;
        for (int64_t i = 0; i < v->len; i++) {
            ray_t* col = cols[i];
            if (col && !RAY_IS_ERR(col)) ray_release(col);
        }
        return;
    }

    if (v->type == RAY_LIST) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < v->len; i++) {
            ray_t* child = ptrs[i];
            if (child && !RAY_IS_ERR(child)) ray_release(child);
        }
    }
}

void ray_retain_owned_refs(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;

    if (ray_is_atom(v)) {
        if (v->type == RAY_LAMBDA) {
            ray_t** slots = (ray_t**)ray_data(v);
            for (int i = 0; i < 4; i++) {
                if (slots[i] && !RAY_IS_ERR(slots[i]))
                    ray_retain(slots[i]);
            }
            return;
        }
        /* Lazy handles own their graph uniquely — no retain on copy */
        if (v->type == RAY_LAZY) return;
        if (ray_atom_owns_obj(v) && v->obj && !RAY_IS_ERR(v->obj))
            ray_retain(v->obj);
        return;
    }

    if (v->attrs & RAY_ATTR_SLICE) {
        if (v->slice_parent && !RAY_IS_ERR(v->slice_parent))
            ray_retain(v->slice_parent);
        return;
    }

    if ((v->attrs & RAY_ATTR_NULLMAP_EXT) &&
        v->ext_nullmap && !RAY_IS_ERR(v->ext_nullmap))
        ray_retain(v->ext_nullmap);

    if (v->type == RAY_STR && v->str_pool && !RAY_IS_ERR(v->str_pool))
        ray_retain(v->str_pool);

    if (RAY_IS_PARTED(v->type)) {
        int64_t n_segs = v->len;
        ray_t** segs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n_segs; i++) {
            if (segs[i] && !RAY_IS_ERR(segs[i]))
                ray_retain(segs[i]);
        }
        return;
    }

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        if (ptrs[0] && !RAY_IS_ERR(ptrs[0])) ray_retain(ptrs[0]);
        if (ptrs[1] && !RAY_IS_ERR(ptrs[1])) ray_retain(ptrs[1]);
        return;
    }

    if (v->type == RAY_TABLE) {
        ray_t** slots = (ray_t**)ray_data(v);
        ray_t* schema = slots[0];
        if (schema && !RAY_IS_ERR(schema)) ray_retain(schema);

        ray_t** cols = slots + 1;
        for (int64_t i = 0; i < v->len; i++) {
            ray_t* col = cols[i];
            if (col && !RAY_IS_ERR(col)) ray_retain(col);
        }
        return;
    }

    if (v->type == RAY_LIST) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < v->len; i++) {
            ray_t* child = ptrs[i];
            if (child && !RAY_IS_ERR(child)) ray_retain(child);
        }
    }
}

static void ray_detach_owned_refs(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;

    if (ray_is_atom(v)) {
        if (v->type == RAY_LAMBDA) {
            ray_t** slots = (ray_t**)ray_data(v);
            for (int i = 0; i < 4; i++) slots[i] = NULL;
            return;
        }
        if (v->type == RAY_LAZY) {
            RAY_LAZY_GRAPH(v) = NULL;
            RAY_LAZY_OP(v)    = NULL;
            return;
        }
        if (ray_atom_owns_obj(v)) v->obj = NULL;
        return;
    }

    if (v->attrs & RAY_ATTR_SLICE) {
        v->slice_parent = NULL;
        v->slice_offset = 0;
        v->attrs &= (uint8_t)~RAY_ATTR_SLICE;
        return;
    }

    if (v->attrs & RAY_ATTR_NULLMAP_EXT) {
        v->ext_nullmap = NULL;
        v->attrs &= (uint8_t)~RAY_ATTR_NULLMAP_EXT;
    }

    if (v->type == RAY_STR) {
        v->str_pool = NULL;
    }

    if (RAY_IS_PARTED(v->type)) {
        int64_t n_segs = v->len;
        ray_t** segs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n_segs; i++)
            segs[i] = NULL;
        return;
    }

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        ptrs[0] = NULL;
        ptrs[1] = NULL;
        return;
    }

    if (v->type == RAY_TABLE) {
        ray_t** slots = (ray_t**)ray_data(v);
        slots[0] = NULL;
        v->len = 0;
        return;
    }

    if (v->type == RAY_LIST) {
        v->len = 0;
    }
}

/* --------------------------------------------------------------------------
 * ray_alloc
 * -------------------------------------------------------------------------- */

ray_t* ray_alloc(size_t data_size) {
    ray_heap_t* h = ray_tl_heap;
    if (RAY_UNLIKELY(!h)) {
        ray_heap_init();
        h = ray_tl_heap;
        if (!h) return NULL;
    }

    uint8_t order = ray_order_for_size(data_size);
    if (order > RAY_HEAP_MAX_ORDER) return NULL;

    /* Slab fast path */
    if (RAY_LIKELY(IS_SLAB_ORDER(order))) {
        int idx = SLAB_INDEX(order);
        if (RAY_LIKELY(h->slabs[idx].count > 0)) {
            ray_t* v = h->slabs[idx].stack[--h->slabs[idx].count];

            /* Zero header fields without full memset (hot path).
             * order + rc are set below; type/len/attrs/mmod must be 0. */
            v->type = 0;
            v->len  = 0;
            v->attrs = 0;
            v->mmod = 0;
            v->order = order;
            if (RAY_UNLIKELY(ray_rc_sync))
                ray_atomic_store(&v->rc, 1);
            else
                v->rc = 1;

            RAY_STAT(ray_tl_stats.alloc_count++);
            RAY_STAT(ray_tl_stats.slab_hits++);
            RAY_STAT(ray_tl_stats.bytes_allocated += BSIZEOF(order));
            RAY_STAT(ray_tl_stats.peak_bytes = ray_tl_stats.bytes_allocated > ray_tl_stats.peak_bytes
                ? ray_tl_stats.bytes_allocated : ray_tl_stats.peak_bytes);
            return v;
        }
    }

    /* Find free block via avail bitmask.
     * Avail bits can be stale from cross-heap fl_remove, so we loop
     * to find a genuinely non-empty freelist. */
    uint64_t candidates = h->avail & (UINT64_MAX << order);

    if (RAY_UNLIKELY(candidates == 0)) {
        heap_flush_foreign(h, false);  /* always local in ray_alloc */

        candidates = h->avail & (UINT64_MAX << order);

        if (candidates == 0) {
            if (!heap_add_pool(h, order)) return NULL;
            candidates = h->avail & (UINT64_MAX << order);
            if (candidates == 0) return NULL;
        }
    }

    /* Scan past stale avail bits (cross-heap fl_remove may have emptied lists) */
    uint8_t found_order;
    for (;;) {
        if (candidates == 0) {
            if (!heap_add_pool(h, order)) return NULL;
            candidates = h->avail & (UINT64_MAX << order);
            if (candidates == 0) return NULL;
        }
        found_order = (uint8_t)__builtin_ctzll(candidates);
        if (!fl_empty(&h->freelist[found_order])) break;
        /* Clear stale bit and try next */
        h->avail &= ~(1ULL << found_order);
        candidates &= ~(1ULL << found_order);
    }

    /* Pop from circular sentinel freelist */
    ray_fl_head_t* head = &h->freelist[found_order];
    ray_t* blk = head->fl_next;
    fl_remove(blk);
    if (fl_empty(head))
        h->avail &= ~(1ULL << found_order);

    /* Split down to requested order */
    heap_split_block(h, blk, order, found_order);

    /* Zero ray_t header and set metadata */
    memset(blk, 0, 32);
    blk->mmod  = 0;
    blk->order = order;
    if (RAY_UNLIKELY(ray_rc_sync))
        ray_atomic_store(&blk->rc, 1);
    else
        blk->rc = 1;

    RAY_STAT(ray_tl_stats.alloc_count++);
    RAY_STAT(ray_tl_stats.bytes_allocated += BSIZEOF(order));
    RAY_STAT(ray_tl_stats.peak_bytes = ray_tl_stats.bytes_allocated > ray_tl_stats.peak_bytes
        ? ray_tl_stats.bytes_allocated : ray_tl_stats.peak_bytes);

    return blk;
}

/* --------------------------------------------------------------------------
 * ray_free
 * -------------------------------------------------------------------------- */

void ray_free(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    if (v->attrs & RAY_ATTR_ARENA) return;  /* arena-owned, bulk-freed */

    /* Guard: keep rc=1 while releasing children so buddy coalescing
     * won't merge this block prematurely (it checks buddy_rc==0). */
    ray_atomic_store(&v->rc, 1);

    ray_release_owned_refs(v);

    /* File-mapped: munmap */
    if (v->mmod == 1) {
        if (v->type == RAY_TABLE || v->type == RAY_LIST) return;
        if (v->type > 0 && v->type < RAY_TYPE_COUNT) {
            uint8_t esz = ray_sym_elem_size(v->type, v->attrs);
            size_t data_size = 32 + (size_t)v->len * esz;
            if (v->attrs & RAY_ATTR_NULLMAP_EXT)
                data_size += ((size_t)v->len + 7) / 8;
            size_t mapped_size = (data_size + 4095) & ~(size_t)4095;
            ray_vm_unmap_file(v, mapped_size);
        } else {
            ray_vm_unmap_file(v, 4096);
        }
        RAY_STAT(ray_tl_stats.free_count++);
        return;
    }

    /* Legacy mmod==2 guard */
    if (v->mmod == 2) return;

    ray_heap_t* h = ray_tl_heap;
    if (!h) return;

    uint8_t order = v->order;

    if (order < RAY_ORDER_MIN || order > RAY_HEAP_MAX_ORDER) return;

    size_t block_size = BSIZEOF(order);

    /* O(1) ownership check via pool header heap_id.
     * ray_pool_of() derives pool base in O(1) via self-aligned AND mask.
     * Pool header stores heap_id stamped at pool creation. */
    ray_pool_hdr_t* phdr = ray_pool_of(v);
    bool is_local = (phdr->heap_id == h->id);

    /* Slab fast path (same heap only) */
    if (IS_SLAB_ORDER(order) && is_local) {
        int idx = SLAB_INDEX(order);
        if (h->slabs[idx].count < RAY_SLAB_CACHE_SIZE) {
            /* Mark rc=1 so buddy coalescing skips slab-cached blocks.
             * Blocks freed via ray_release arrive with rc=0; without this,
             * a buddy being freed would see rc==0 and incorrectly merge
             * with the slab-cached block, causing overlapping allocations.
             * Must be atomic: buddy coalescing on another thread reads rc. */
            ray_atomic_store(&v->rc, 1);
            h->slabs[idx].stack[h->slabs[idx].count++] = v;
            RAY_STAT(ray_tl_stats.free_count++);
            RAY_STAT(ray_tl_stats.bytes_allocated -= block_size);
            return;
        }
    }

    /* Foreign: different heap — enqueue to foreign list */
    if (!is_local) {
        v->fl_next = h->foreign;
        h->foreign = v;
        RAY_STAT(ray_tl_stats.free_count++);
        RAY_STAT(ray_tl_stats.bytes_allocated -= block_size);
        return;
    }

    /* Local block — coalesce with buddy */
    heap_coalesce(h, v, (uintptr_t)phdr, phdr->pool_order);

    RAY_STAT(ray_tl_stats.free_count++);
    RAY_STAT(ray_tl_stats.bytes_allocated -= block_size);
}

/* --------------------------------------------------------------------------
 * ray_alloc_copy
 * -------------------------------------------------------------------------- */

ray_t* ray_alloc_copy(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return NULL;
    size_t data_size;
    if (ray_is_atom(v)) {
        data_size = 0;
    } else if (v->type == RAY_TABLE) {
        if (v->len < 0) return RAY_ERR_PTR(RAY_ERR_OOM);
        data_size = (size_t)(ray_len(v) + 1) * sizeof(ray_t*);
    } else if (RAY_IS_PARTED(v->type) || v->type == RAY_MAPCOMMON) {
        int64_t n_ptrs = v->len;
        if (v->type == RAY_MAPCOMMON) n_ptrs = 2;
        if (n_ptrs < 0) return RAY_ERR_PTR(RAY_ERR_OOM);
        data_size = (size_t)n_ptrs * sizeof(ray_t*);
    } else {
        int8_t t = ray_type(v);
        if (t <= 0 || t >= RAY_TYPE_COUNT)
            data_size = 0;
        else {
            uint8_t esz = ray_sym_elem_size(t, v->attrs);
            if (v->len < 0 || (esz > 0 && (uint64_t)v->len > SIZE_MAX / esz))
                return RAY_ERR_PTR(RAY_ERR_OOM);
            data_size = (size_t)ray_len(v) * esz;
        }
    }
    ray_t* copy = ray_alloc(data_size);
    if (!copy) return NULL;

    uint8_t new_order = copy->order;
    uint8_t new_mmod  = copy->mmod;
    memcpy(copy, v, 32 + data_size);
    copy->mmod  = new_mmod;
    copy->order = new_order;
    if (RAY_UNLIKELY(ray_rc_sync))
        ray_atomic_store(&copy->rc, 1);
    else
        copy->rc = 1;
    ray_retain_owned_refs(copy);
    return copy;
}

/* --------------------------------------------------------------------------
 * ray_scratch_alloc / ray_scratch_realloc
 * -------------------------------------------------------------------------- */

ray_t* ray_scratch_alloc(size_t data_size) {
    return ray_alloc(data_size);
}

ray_t* ray_scratch_realloc(ray_t* v, size_t new_data_size) {
    ray_t* new_v = ray_alloc(new_data_size);
    if (!new_v) return NULL;
    if (v && !RAY_IS_ERR(v)) {
        size_t old_data;
        if (ray_is_atom(v))
            old_data = 0;
        else if (v->type == RAY_LIST) {
            if (v->len < 0) { old_data = 0; }
            else old_data = (size_t)ray_len(v) * sizeof(ray_t*);
        } else if (v->type == RAY_TABLE) {
            if (v->len < 0) { old_data = 0; }
            else old_data = (size_t)(ray_len(v) + 1) * sizeof(ray_t*);
        } else if (RAY_IS_PARTED(v->type) || v->type == RAY_MAPCOMMON) {
            int64_t n_ptrs = v->len;
            if (v->type == RAY_MAPCOMMON) n_ptrs = 2;
            if (n_ptrs < 0) n_ptrs = 0;
            old_data = (size_t)n_ptrs * sizeof(ray_t*);
        } else {
            int8_t t = ray_type(v);
            old_data = (t > 0 && t < RAY_TYPE_COUNT && v->len >= 0) ?
                       (size_t)ray_len(v) * ray_sym_elem_size(t, v->attrs) : 0;
        }
        /* Clamp old_data to actual allocation size */
        if (v->mmod == 0 && v->order >= RAY_ORDER_MIN) {
            size_t alloc_data = BSIZEOF(v->order) - 32;
            if (old_data > alloc_data) old_data = alloc_data;
        }
        size_t copy_data = old_data < new_data_size ? old_data : new_data_size;
        uint8_t new_mmod = new_v->mmod;
        uint8_t new_order = new_v->order;
        memcpy(new_v, v, 32 + copy_data);
        new_v->mmod = new_mmod;
        new_v->order = new_order;
        if (RAY_UNLIKELY(ray_rc_sync))
            ray_atomic_store(&new_v->rc, 1);
        else
            new_v->rc = 1;
        /* Ownership transfers via memcpy — no retain needed on new_v.
         * Detach nulls old pointers so ray_free won't double-release. */
        ray_detach_owned_refs(v);
        ray_free(v);
    }
    return new_v;
}

/* --------------------------------------------------------------------------
 * ray_mem_stats
 * -------------------------------------------------------------------------- */

void ray_mem_stats(ray_mem_stats_t* out) {
    *out = ray_tl_stats;
    int64_t sc = 0, sp = 0;
    ray_sys_get_stat(&sc, &sp);
    out->sys_current = (size_t)sc;
    out->sys_peak    = (size_t)sp;
}

/* --------------------------------------------------------------------------
 * Heap lifecycle
 * -------------------------------------------------------------------------- */

void ray_heap_init(void) {
    if (ray_tl_heap) return;

    size_t heap_sz = (sizeof(ray_heap_t) + 4095) & ~(size_t)4095;
    ray_heap_t* h = (ray_heap_t*)ray_vm_alloc(heap_sz);
    if (!h) return;
    memset(h, 0, heap_sz);

    /* Bitmap-based ID: acquire reusable ID via atomic CAS */
    int id = heap_id_acquire();
    if (id < 0) {
        ray_vm_free(h, heap_sz);
        return;  /* ID pool exhausted */
    }
    h->id = (uint16_t)id;

    /* Register in global heap registry */
    ray_heap_registry[h->id % RAY_HEAP_REGISTRY_SIZE] = h;

    /* Initialize circular sentinel freelists */
    for (int i = 0; i < RAY_HEAP_FL_SIZE; i++)
        fl_init(&h->freelist[i]);

    ray_tl_heap = h;
    memset(&ray_tl_stats, 0, sizeof(ray_tl_stats));
}

void ray_heap_destroy(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;

    uint16_t saved_id = h->id;

    /* Unregister from global heap registry */
    ray_heap_registry[h->id % RAY_HEAP_REGISTRY_SIZE] = NULL;

    /* Skip flush_slabs and flush_foreign — all pools are about to be
     * munmap'd. Flushing would coalesce blocks and fl_remove buddies
     * from other heaps' freelists, which races with concurrent worker
     * destruction during ray_pool_free(). */

    /* Munmap all tracked pools */
    for (uint32_t i = 0; i < h->pool_count; i++) {
        ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)h->pools[i].base;
        ray_vm_free(hdr->vm_base, BSIZEOF(h->pools[i].pool_order));
    }

    size_t heap_sz = (sizeof(ray_heap_t) + 4095) & ~(size_t)4095;
    ray_vm_free(h, heap_sz);
    ray_tl_heap = NULL;
    memset(&ray_tl_stats, 0, sizeof(ray_tl_stats));

    /* Release bitmap ID after all memory is freed */
    heap_id_release(saved_id);
}

/* --------------------------------------------------------------------------
 * Return worker-pool blocks from this heap's freelists to their owners.
 *
 * After ray_alloc flushes foreign blocks locally (coalesce + madvise),
 * worker-pool blocks sit on main's freelists with released physical pages.
 * This function walks the freelists, finds blocks whose pool header
 * heap_id != ours, removes them, and inserts into the owning worker heap.
 * Workers can then reuse their pools without allocating new ones.
 *
 * ONLY safe when workers are idle (on semaphore, ray_parallel_flag == 0).
 * -------------------------------------------------------------------------- */

static void heap_return_foreign_freelist(ray_heap_t* h) {
    for (int order = RAY_ORDER_MIN; order < RAY_HEAP_FL_SIZE; order++) {
        ray_fl_head_t* head = &h->freelist[order];
        ray_t* blk = head->fl_next;
        while (blk != (ray_t*)head) {
            ray_t* next = blk->fl_next;
            /* Use heap_find_pool on h first — if found, block is local */
            int pidx = heap_find_pool(h, blk);
            if (pidx < 0) {
                /* Foreign block — find owner via pool header (GC path) */
                ray_pool_hdr_t* phdr = ray_pool_of(blk);
                ray_heap_t* owner = ray_heap_registry[phdr->heap_id % RAY_HEAP_REGISTRY_SIZE];
                if (owner && owner->id == phdr->heap_id) {
                    fl_remove(blk);
                    if (fl_empty(head))
                        h->avail &= ~(1ULL << order);
                    /* Coalesce on owner for defragmentation */
                    int opidx = heap_find_pool(owner, blk);
                    uintptr_t pb;
                    uint8_t po;
                    if (opidx >= 0) {
                        pb = (uintptr_t)owner->pools[opidx].base;
                        po = owner->pools[opidx].pool_order;
                    } else {
                        pb = (uintptr_t)phdr;
                        po = phdr->pool_order;
                    }
                    heap_coalesce(owner, blk, pb, po);
                }
            }
            blk = next;
        }
    }
}

void ray_heap_gc(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;

    bool safe = (atomic_load_explicit(&ray_parallel_flag, memory_order_relaxed) == 0);

    /* Phase 1: Flush main heap's foreign blocks and slab caches.
     * When safe (workers idle), return foreign blocks to their owners
     * so worker pools become reusable. */
    heap_flush_foreign(h, safe);
    heap_flush_slabs(h);

    if (safe) {
        /* Phase 2: Return foreign blocks absorbed onto our freelists
         * back to their owning worker heaps. */
        heap_return_foreign_freelist(h);

        /* Phase 3: Flush foreign + slabs on all worker heaps.
         * Workers may have accumulated foreign blocks from other workers,
         * and slab caches prevent buddy coalescing needed for reclamation. */
        for (int hid = 0; hid < RAY_HEAP_REGISTRY_SIZE; hid++) {
            ray_heap_t* wh = ray_heap_registry[hid];
            if (!wh || wh == h) continue;
            heap_flush_foreign(wh, true);
            heap_flush_slabs(wh);
        }

        /* Phase 4: Reclaim OVERSIZED empty pools.
         * Standard pools (pool_order == RAY_HEAP_POOL_ORDER) are never
         * munmapped — physical pages released via madvise (phase 5)
         * re-fault cheaply on next query.
         * Only oversized pools (pool_order > RAY_HEAP_POOL_ORDER) are
         * candidates — these are one-off large allocations.
         *
         * Emptiness is computed by walking all heaps' freelists and slab
         * caches to sum free capacity within the pool. This avoids atomic
         * live_count operations on the alloc/free hot path. */
        for (int hid = 0; hid < RAY_HEAP_REGISTRY_SIZE; hid++) {
            ray_heap_t* gh = ray_heap_registry[hid];
            if (!gh) continue;

            for (uint32_t p = 0; p < gh->pool_count; ) {
                ray_pool_hdr_t* phdr = (ray_pool_hdr_t*)gh->pools[p].base;

                /* Skip standard pools and last-remaining pool */
                if (phdr->pool_order <= RAY_HEAP_POOL_ORDER
                    || gh->pool_count <= 1) {
                    p++;
                    continue;
                }

                uint8_t po = phdr->pool_order;
                uintptr_t pb = (uintptr_t)phdr;
                uintptr_t pe = pb + BSIZEOF(po);
                /* Total usable capacity (minus header block) */
                size_t pool_capacity = BSIZEOF(po) - BSIZEOF(RAY_ORDER_MIN);

                /* Sum free bytes: walk all heaps' freelists + slab caches */
                size_t free_bytes = 0;
                for (int scan_hid = 0; scan_hid < RAY_HEAP_REGISTRY_SIZE; scan_hid++) {
                    ray_heap_t* scan_h = ray_heap_registry[scan_hid];
                    if (!scan_h) continue;
                    for (int ord = RAY_ORDER_MIN; ord < RAY_HEAP_FL_SIZE; ord++) {
                        ray_fl_head_t* fh = &scan_h->freelist[ord];
                        ray_t* blk = fh->fl_next;
                        while (blk != (ray_t*)fh) {
                            if ((uintptr_t)blk >= pb && (uintptr_t)blk < pe)
                                free_bytes += BSIZEOF(ord);
                            blk = blk->fl_next;
                        }
                    }
                    for (int si = 0; si < RAY_SLAB_ORDERS; si++) {
                        for (uint32_t j = 0; j < scan_h->slabs[si].count; j++) {
                            ray_t* sb = scan_h->slabs[si].stack[j];
                            if ((uintptr_t)sb >= pb && (uintptr_t)sb < pe)
                                free_bytes += BSIZEOF(RAY_SLAB_MIN + si);
                        }
                    }
                }

                if (free_bytes < pool_capacity) {
                    p++;
                    continue;  /* pool has live allocations */
                }

                /* Pool is empty — remove all blocks from all freelists
                 * and slab caches before munmap. */
                for (int scan_hid = 0; scan_hid < RAY_HEAP_REGISTRY_SIZE; scan_hid++) {
                    ray_heap_t* scan_h = ray_heap_registry[scan_hid];
                    if (!scan_h) continue;
                    for (int ord = RAY_ORDER_MIN; ord < RAY_HEAP_FL_SIZE; ord++) {
                        ray_fl_head_t* fh = &scan_h->freelist[ord];
                        ray_t* blk = fh->fl_next;
                        while (blk != (ray_t*)fh) {
                            ray_t* next = blk->fl_next;
                            if ((uintptr_t)blk >= pb && (uintptr_t)blk < pe) {
                                fl_remove(blk);
                                if (fl_empty(fh))
                                    scan_h->avail &= ~(1ULL << ord);
                            }
                            blk = next;
                        }
                    }
                    for (int si = 0; si < RAY_SLAB_ORDERS; si++) {
                        uint32_t dst = 0;
                        for (uint32_t j = 0; j < scan_h->slabs[si].count; j++) {
                            ray_t* sb = scan_h->slabs[si].stack[j];
                            if ((uintptr_t)sb >= pb && (uintptr_t)sb < pe)
                                continue;
                            scan_h->slabs[si].stack[dst++] = sb;
                        }
                        scan_h->slabs[si].count = dst;
                    }
                }

                ray_vm_free(phdr->vm_base, BSIZEOF(po));
                gh->pools[p] = gh->pools[--gh->pool_count];
                /* Don't increment p — check swapped entry */
            }
        }
    }

}

void ray_heap_release_pages(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;
    for (int i = 13; i < RAY_HEAP_FL_SIZE; i++) {
        ray_fl_head_t* head = &h->freelist[i];
        ray_t* blk = head->fl_next;
        while (blk != (ray_t*)head) {
            size_t bsize = BSIZEOF(i);
            if (bsize > 4096)
                ray_vm_release((char*)blk + 4096, bsize - 4096);
            blk = blk->fl_next;
        }
    }
}

void ray_heap_merge(ray_heap_t* src) {
    ray_heap_t* dst = ray_tl_heap;
    if (!dst || !src) return;

    /* Transfer slabs: fit into dst cache, coalesce overflow */
    for (int i = 0; i < RAY_SLAB_ORDERS; i++) {
        while (src->slabs[i].count > 0 && dst->slabs[i].count < RAY_SLAB_CACHE_SIZE)
            dst->slabs[i].stack[dst->slabs[i].count++] =
                src->slabs[i].stack[--src->slabs[i].count];
        while (src->slabs[i].count > 0) {
            ray_t* blk = src->slabs[i].stack[--src->slabs[i].count];
            int pidx = heap_find_pool(dst, blk);
            uintptr_t pb;
            uint8_t po;
            if (pidx >= 0) {
                pb = (uintptr_t)dst->pools[pidx].base;
                po = dst->pools[pidx].pool_order;
            } else {
                ray_pool_hdr_t* phdr = ray_pool_of(blk);
                pb = (uintptr_t)phdr;
                po = phdr->pool_order;
            }
            heap_coalesce(dst, blk, pb, po);
        }
    }

    /* Free foreign blocks via coalescing */
    ray_t* fblk = src->foreign;
    while (fblk) {
        ray_t* next = fblk->fl_next;
        int pidx = heap_find_pool(dst, fblk);
        uintptr_t pb;
        uint8_t po;
        if (pidx >= 0) {
            pb = (uintptr_t)dst->pools[pidx].base;
            po = dst->pools[pidx].pool_order;
        } else {
            ray_pool_hdr_t* phdr = ray_pool_of(fblk);
            pb = (uintptr_t)phdr;
            po = phdr->pool_order;
        }
        heap_coalesce(dst, fblk, pb, po);
        fblk = next;
    }
    src->foreign = NULL;

    /* Merge freelists: circular list splice (src chain into dst chain) */
    for (int i = RAY_ORDER_MIN; i < RAY_HEAP_FL_SIZE; i++) {
        if (fl_empty(&src->freelist[i])) continue;

        ray_fl_head_t* src_head = &src->freelist[i];
        ray_fl_head_t* dst_head = &dst->freelist[i];

        /* Splice: src's chain [src_first...src_last] into dst after sentinel */
        ray_t* src_first = src_head->fl_next;
        ray_t* src_last  = src_head->fl_prev;
        ray_t* dst_first = dst_head->fl_next;

        /* src_first goes after dst sentinel */
        dst_head->fl_next = src_first;
        src_first->fl_prev = (ray_t*)dst_head;

        /* src_last connects to old dst_first */
        src_last->fl_next = dst_first;
        dst_first->fl_prev = src_last;

        dst->avail |= (1ULL << i);

        /* Reset src sentinel to empty */
        fl_init(src_head);
    }

    src->avail = 0;

    /* Update pool headers: set heap_id to dst, transfer pool entries.
     * Do NOT rewrite heap_id for pools that can't be tracked — that would
     * make coalescing reference a pool not in dst's pool table. */
    for (uint32_t i = 0; i < src->pool_count; i++) {
        if (dst->pool_count < RAY_MAX_POOLS) {
            ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)src->pools[i].base;
            hdr->heap_id = dst->id;
            dst->pools[dst->pool_count++] = src->pools[i];
        } else {
            /* Pool overflow: only triggers at RAY_MAX_POOLS (512 pools = 16GB+).
             * Assert in debug builds to catch unexpected growth. */
            assert(0 && "ray_heap_merge: pool overflow at RAY_MAX_POOLS");
        }
    }
    src->pool_count = 0;
}

/* --------------------------------------------------------------------------
 * Public foreign-blocks flush
 * -------------------------------------------------------------------------- */

void ray_heap_flush_foreign(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;
    bool safe = (atomic_load_explicit(&ray_parallel_flag,
                                       memory_order_relaxed) == 0);
    heap_flush_foreign(h, safe);
}

/* --------------------------------------------------------------------------
 * Pending-merge queue (lock-free LIFO)
 *
 * Workers that are torn down push their heap onto this queue instead of
 * destroying it immediately. The main thread drains the queue, merging
 * each pending heap into its own and then destroying it.
 * -------------------------------------------------------------------------- */

void ray_heap_push_pending(ray_heap_t* heap) {
    if (!heap) return;
    /* Unregister so no new foreign blocks target this heap */
    ray_heap_registry[heap->id % RAY_HEAP_REGISTRY_SIZE] = NULL;
    /* Lock-free push: CAS loop on global LIFO head */
    heap->pending_next = atomic_load_explicit(&ray_heap_pending_merge, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(
            &ray_heap_pending_merge,
            &heap->pending_next, heap,
            memory_order_release, memory_order_relaxed))
        ;
}

void ray_heap_drain_pending(void) {
    /* Atomically steal the entire pending list */
    ray_heap_t* pending = atomic_exchange_explicit(
        &ray_heap_pending_merge, NULL,
        memory_order_acquire);
    while (pending) {
        ray_heap_t* next = pending->pending_next;
        ray_heap_merge(pending);
        /* Free the heap struct (pools already transferred by merge) */
        uint16_t saved_id = pending->id;
        size_t heap_sz = (sizeof(ray_heap_t) + 4095) & ~(size_t)4095;
        ray_vm_free(pending, heap_sz);
        heap_id_release(saved_id);
        pending = next;
    }
}

/* --------------------------------------------------------------------------
 * Scratch arena: bump allocator backed by buddy-allocated 64KB blocks
 * -------------------------------------------------------------------------- */

void* ray_scratch_arena_push(ray_scratch_arena_t* a, size_t nbytes) {
    /* 16-byte alignment */
    nbytes = (nbytes + 15) & ~(size_t)15;

    if (RAY_LIKELY(a->ptr != NULL && a->ptr + nbytes <= a->end))
        goto bump;

    /* Need a new backing block */
    if (a->n_backing >= RAY_ARENA_MAX_BACKING) return NULL;

    size_t block_data = BSIZEOF(RAY_ARENA_BLOCK_ORDER) - 32;
    /* If request exceeds standard block, allocate exact-fit */
    size_t alloc_size = nbytes > block_data ? nbytes : block_data;
    ray_t* blk = ray_alloc(alloc_size);
    if (!blk) return NULL;
    a->backing[a->n_backing++] = blk;
    a->ptr = (char*)ray_data(blk);
    a->end = (char*)blk + BSIZEOF(blk->order);

bump:;
    void* ret = a->ptr;
    a->ptr += nbytes;
    return ret;
}

void ray_scratch_arena_reset(ray_scratch_arena_t* a) {
    for (int i = 0; i < a->n_backing; i++)
        ray_free(a->backing[i]);
    a->n_backing = 0;
    a->ptr = NULL;
    a->end = NULL;
}

/* --------------------------------------------------------------------------
 * Parallel begin / end
 * -------------------------------------------------------------------------- */

void ray_parallel_begin(void) { atomic_store(&ray_parallel_flag, 1); }
void ray_parallel_end(void) {
    atomic_store(&ray_parallel_flag, 0);
    ray_heap_gc();
}
