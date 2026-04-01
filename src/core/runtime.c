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

#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* ===== Global state ===== */

ray_runtime_t *__RUNTIME = NULL;
_Thread_local ray_vm_t *__VM = NULL;

/* ===== Error API ===== */

ray_t* ray_verror(const char* code, const char* fmt, va_list ap) {
    ray_t* err = ray_alloc(0);
    if (!err) return NULL;
    err->type = RAY_ERROR;
    err->slen = 0;
    memset(err->sdata, 0, 7);
    if (code) {
        size_t len = strlen(code);
        if (len > 7) len = 7;
        memcpy(err->sdata, code, len);
        err->slen = (uint8_t)len;
    }
    if (__VM && fmt) {
        vsnprintf(__VM->err.msg, sizeof(__VM->err.msg), fmt, ap);
    } else if (__VM) {
        __VM->err.msg[0] = '\0';
    }
    return err;
}

ray_t* ray_error(const char* code, const char* fmt, ...) {
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        ray_t* err = ray_verror(code, fmt, ap);
        va_end(ap);
        return err;
    }
    /* No format string — skip va_list entirely for portability */
    ray_t* err = ray_alloc(0);
    if (!err) return NULL;
    err->type = RAY_ERROR;
    err->slen = 0;
    memset(err->sdata, 0, 7);
    if (code) {
        size_t len = strlen(code);
        if (len > 7) len = 7;
        memcpy(err->sdata, code, len);
        err->slen = (uint8_t)len;
    }
    if (__VM) __VM->err.msg[0] = '\0';
    return err;
}

const char* ray_err_code(ray_t* err) {
    if (!err || err->type != RAY_ERROR) return NULL;
    return err->sdata;
}

const char* ray_error_msg(void) {
    if (!__VM || !__VM->err.msg[0]) return NULL;
    return __VM->err.msg;
}

void ray_error_clear(void) {
    if (__VM) __VM->err.msg[0] = '\0';
}

/* ===== Lifecycle stubs (filled in Task 8) ===== */

ray_runtime_t* ray_runtime_create(int argc, char** argv) {
    (void)argc; (void)argv;
    return NULL;
}

void ray_runtime_destroy(ray_runtime_t* rt) {
    (void)rt;
}
