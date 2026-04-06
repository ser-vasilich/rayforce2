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
#include "mem/heap.h"
#include "mem/sys.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Forward-declare lang init/destroy to avoid eval.h ray_vm_t conflict */
extern ray_err_t ray_lang_init(void);
extern void      ray_lang_destroy(void);

/* ===== Global state ===== */

ray_runtime_t *__RUNTIME = NULL;
_Thread_local ray_vm_t *__VM = NULL;

/* Static null singleton — type RAY_NULL, ARENA flag makes retain/release no-ops */
ray_t __ray_null = { .type = RAY_NULL, .attrs = RAY_ATTR_ARENA, .rc = 0, .len = 0 };

/* ===== Error code to string ===== */

const char* ray_err_code_str(ray_err_t e) {
    static const char* codes[] = {
        [RAY_OK]          = "ok",
        [RAY_ERR_OOM]     = "oom",
        [RAY_ERR_TYPE]    = "type",
        [RAY_ERR_RANGE]   = "range",
        [RAY_ERR_LENGTH]  = "length",
        [RAY_ERR_RANK]    = "rank",
        [RAY_ERR_DOMAIN]  = "domain",
        [RAY_ERR_NYI]     = "nyi",
        [RAY_ERR_IO]      = "io",
        [RAY_ERR_SCHEMA]  = "schema",
        [RAY_ERR_CORRUPT] = "corrupt",
        [RAY_ERR_CANCEL]  = "cancel",
        [RAY_ERR_PARSE]   = "parse",
        [RAY_ERR_NAME]    = "name",
        [RAY_ERR_LIMIT]   = "limit",
    };
    if ((unsigned)e >= sizeof(codes)/sizeof(codes[0])) return "error";
    return codes[e];
}

ray_err_t ray_err_from_obj(ray_t* err) {
    if (!err || err->type != RAY_ERROR) return RAY_ERR_DOMAIN;
    const char* s = err->sdata;
    int n = err->slen;
    static const struct { const char* s; int len; ray_err_t e; } map[] = {
        {"oom",     3, RAY_ERR_OOM},     {"type",    4, RAY_ERR_TYPE},
        {"range",   5, RAY_ERR_RANGE},   {"length",  6, RAY_ERR_LENGTH},
        {"rank",    4, RAY_ERR_RANK},    {"domain",  6, RAY_ERR_DOMAIN},
        {"nyi",     3, RAY_ERR_NYI},     {"io",      2, RAY_ERR_IO},
        {"schema",  6, RAY_ERR_SCHEMA},  {"corrupt", 7, RAY_ERR_CORRUPT},
        {"cancel",  6, RAY_ERR_CANCEL},  {"parse",   5, RAY_ERR_PARSE},
        {"name",    4, RAY_ERR_NAME},    {"limit",   5, RAY_ERR_LIMIT},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if (n == map[i].len && memcmp(s, map[i].s, n) == 0) return map[i].e;
    return RAY_ERR_DOMAIN;
}

/* ===== Error API ===== */

static ray_t* ray_verror(const char* code, const char* fmt, va_list ap) {
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
    /* sdata is 7 bytes and may not be null-terminated when full */
    static _Thread_local char buf[8];
    memcpy(buf, err->sdata, err->slen);
    buf[err->slen] = '\0';
    return buf;
}

const char* ray_error_msg(void) {
    if (!__VM || !__VM->err.msg[0]) return NULL;
    return __VM->err.msg;
}

void ray_error_clear(void) {
    if (__VM) __VM->err.msg[0] = '\0';
}

/* ===== Lifecycle ===== */

ray_runtime_t* ray_runtime_create(int argc, char** argv) {
    (void)argc; (void)argv;

    /* Init subsystems */
    ray_heap_init();
    ray_sym_init();

    /* Allocate runtime via system allocator */
    ray_runtime_t* rt = (ray_runtime_t*)ray_sys_alloc(sizeof(ray_runtime_t));
    if (!rt) return NULL;
    memset(rt, 0, sizeof(*rt));

    /* Create main VM (id=0) */
    rt->n_vms = 1;
    rt->vms = (ray_vm_t**)ray_sys_alloc(sizeof(ray_vm_t*));
    if (!rt->vms) { ray_sys_free(rt); return NULL; }
    rt->vms[0] = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    if (!rt->vms[0]) { ray_sys_free(rt->vms); ray_sys_free(rt); return NULL; }
    memset(rt->vms[0], 0, sizeof(ray_vm_t));
    rt->vms[0]->id = 0;
    __VM = rt->vms[0];

    /* Init language (env + builtins) — must be after __VM is set */
    ray_lang_init();

    __RUNTIME = rt;
    return rt;
}

void ray_runtime_destroy(ray_runtime_t* rt) {
    if (!rt) return;

    ray_lang_destroy();

    /* Free VMs */
    for (int32_t i = 0; i < rt->n_vms; i++) {
        ray_vm_t* vm = rt->vms[i];
        if (vm->raise_val) ray_release(vm->raise_val);
        if (vm->trace) { ray_release(vm->trace); vm->trace = NULL; }
        ray_sys_free(vm);
    }
    ray_sys_free(rt->vms);

    __VM = NULL;
    __RUNTIME = NULL;

    ray_sym_destroy();
    ray_heap_destroy();

    ray_sys_free(rt);
}
