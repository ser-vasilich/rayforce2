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

#include <rayforce.h>
#include "mem/heap.h"
#include "store/col.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ray_heap_init();
    assert(ray_sym_init() == RAY_OK);

    char path[] = "/tmp/fuzz_col_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { ray_sym_destroy(); ray_heap_destroy(); return 0; }
    write(fd, data, size);
    close(fd);

    ray_t* result = ray_col_load(path);
    if (result && !RAY_IS_ERR(result))
        ray_release(result);

    unlink(path);
    ray_sym_destroy();
    ray_heap_destroy();
    return 0;
}
