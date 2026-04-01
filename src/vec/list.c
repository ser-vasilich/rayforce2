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

#include "list.h"
#include "mem/heap.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Capacity helpers (same pattern as vec.c)
 * -------------------------------------------------------------------------- */

static int64_t list_capacity(ray_t* list) {
    size_t block_size = (size_t)1 << list->order;
    size_t data_space = block_size - 32;  /* 32B ray_t header */
    return (int64_t)(data_space / sizeof(ray_t*));
}

/* --------------------------------------------------------------------------
 * ray_list_new
 * -------------------------------------------------------------------------- */

ray_t* ray_list_new(int64_t capacity) {
    if (capacity < 0) return ray_error("range", NULL);
    if ((uint64_t)capacity > SIZE_MAX / sizeof(ray_t*))
        return ray_error("oom", NULL);
    size_t data_size = (size_t)capacity * sizeof(ray_t*);

    ray_t* list = ray_alloc(data_size);
    if (!list || RAY_IS_ERR(list)) return list;

    list->type = RAY_LIST;
    list->len = 0;
    list->attrs = 0;
    memset(list->nullmap, 0, 16);

    return list;
}

/* --------------------------------------------------------------------------
 * ray_list_append
 * -------------------------------------------------------------------------- */

ray_t* ray_list_append(ray_t* list, ray_t* item) {
    if (!list || RAY_IS_ERR(list)) return list;

    /* COW if shared */
    list = ray_cow(list);
    if (!list || RAY_IS_ERR(list)) return list;

    int64_t cap = list_capacity(list);

    /* Grow if needed */
    if (list->len >= cap) {
        size_t new_data_size = (size_t)(list->len + 1) * sizeof(ray_t*);
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s = 32;
            while (s < new_data_size) {
                if (s > SIZE_MAX / 2) return ray_error("oom", NULL);
                s *= 2;
            }
            new_data_size = s;
        }
        ray_t* new_list = ray_scratch_realloc(list, new_data_size);
        if (!new_list || RAY_IS_ERR(new_list)) return new_list;
        list = new_list;
    }

    /* Store item pointer and retain it */
    ray_t** slots = (ray_t**)ray_data(list);
    slots[list->len] = item;
    if (item) ray_retain(item);
    list->len++;

    return list;
}

/* --------------------------------------------------------------------------
 * ray_list_get
 * -------------------------------------------------------------------------- */

ray_t* ray_list_get(ray_t* list, int64_t idx) {
    if (!list || RAY_IS_ERR(list)) return NULL;
    if (idx < 0 || idx >= list->len) return NULL;

    ray_t** slots = (ray_t**)ray_data(list);
    return slots[idx];
}

/* --------------------------------------------------------------------------
 * ray_list_set
 * -------------------------------------------------------------------------- */

ray_t* ray_list_set(ray_t* list, int64_t idx, ray_t* item) {
    if (!list || RAY_IS_ERR(list)) return list;
    if (idx < 0 || idx >= list->len)
        return ray_error("range", NULL);

    /* COW if shared */
    list = ray_cow(list);
    if (!list || RAY_IS_ERR(list)) return list;

    ray_t** slots = (ray_t**)ray_data(list);

    /* Release old item */
    ray_t* old = slots[idx];
    if (old) ray_release(old);

    /* Store new item and retain it */
    slots[idx] = item;
    if (item) ray_retain(item);

    return list;
}
