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

#include "cow.h"
#include <stdatomic.h>

/* --------------------------------------------------------------------------
 * ray_retain
 * -------------------------------------------------------------------------- */

void ray_retain(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    if (v->attrs & RAY_ATTR_ARENA) return;  /* arena-owned, no-op */
    /* conc-L3: Relaxed ordering is sufficient for retain — the caller already
     * holds a valid reference, so no inter-thread synchronization is needed
     * for the increment itself. Release synchronizes via ray_release's acq_rel. */
    atomic_fetch_add_explicit((_Atomic(uint32_t)*)&v->rc, 1, memory_order_relaxed);
}

/* --------------------------------------------------------------------------
 * ray_release
 * -------------------------------------------------------------------------- */

void ray_release(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    if (v->attrs & RAY_ATTR_ARENA) return;  /* arena-owned, no-op */
    uint32_t prev = atomic_fetch_sub_explicit((_Atomic(uint32_t)*)&v->rc, 1, memory_order_acq_rel);
    if (prev == 1) ray_free(v);
}

/* --------------------------------------------------------------------------
 * ray_cow
 * -------------------------------------------------------------------------- */

ray_t* ray_cow(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    if (v->attrs & RAY_ATTR_ARENA) return v;  /* arena-owned, no-op */
    /* Caller must hold exclusive logical ownership — no concurrent
       ray_retain/ray_release allowed. The acquire load ensures visibility
       of prior writes by threads that have released their reference. */
    uint32_t rc = atomic_load_explicit((_Atomic(uint32_t)*)&v->rc, memory_order_acquire);
    if (rc == 1) return v;  /* sole owner -- mutate in place */
    ray_t* copy = ray_alloc_copy(v);
    if (!copy || RAY_IS_ERR(copy)) return copy;
    /* L3: ray_alloc_copy() already sets copy->rc = 1, so no redundant store needed. */
    ray_release(v);
    return copy;
}
