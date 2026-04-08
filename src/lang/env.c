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

#include "lang/env.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---- Function constructors ---- */

/* Builtin name stored inline in nullmap.
 * Binary ops: nullmap[0..1] = opcode (set later by RAY_FN_SET_OPCODE),
 *             nullmap[2..15] = name (max 13 chars + null).
 * Unary/vary: nullmap[0..15] = name (max 15 chars + null). No opcode. */
static void fn_set_name(ray_t* obj, const char* name) {
    int off = (obj->type == RAY_BINARY) ? 2 : 0;
    int max = 16 - off - 1;
    size_t len = strlen(name);
    if ((int)len > max) len = (size_t)max;
    memcpy(obj->nullmap + off, name, len);
    obj->nullmap[off + len] = 0;
}

ray_t* ray_fn_unary(const char* name, uint8_t fn_attrs, ray_unary_fn fn) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = RAY_UNARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    fn_set_name(obj, name);
    return obj;
}

ray_t* ray_fn_binary(const char* name, uint8_t fn_attrs, ray_binary_fn fn) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = RAY_BINARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    fn_set_name(obj, name);
    return obj;
}

ray_t* ray_fn_vary(const char* name, uint8_t fn_attrs, ray_vary_fn fn) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = RAY_VARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    fn_set_name(obj, name);
    return obj;
}

/* ---- Global environment ---- */

/* Spinlock protecting g_env mutations in ray_env_set */
static _Atomic(int) g_env_lock = 0;
static inline void env_lock(void) {
    while (atomic_exchange_explicit(&g_env_lock, 1, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}
static inline void env_unlock(void) {
    atomic_store_explicit(&g_env_lock, 0, memory_order_release);
}

#define ENV_CAP 512

static struct {
    int64_t keys[ENV_CAP];
    ray_t*   vals[ENV_CAP];
    int32_t count;
} g_env;

/* ---- Local scope stack ---- */

#define SCOPE_CAP  64
#define FRAME_CAP  64

typedef struct {
    int64_t keys[FRAME_CAP];
    ray_t*   vals[FRAME_CAP];
    int32_t count;
} ray_scope_frame_t;

static _Thread_local ray_scope_frame_t scope_stack[SCOPE_CAP];
static _Thread_local int32_t scope_depth = 0;

int32_t ray_env_scope_depth(void) { return scope_depth; }
int32_t ray_env_global_count(void) { return g_env.count; }

ray_err_t ray_env_init(void) {
    memset(&g_env, 0, sizeof(g_env));
    scope_depth = 0;
    return RAY_OK;
}

void ray_env_destroy(void) {
    /* Pop any remaining scopes */
    while (scope_depth > 0) ray_env_pop_scope();
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.vals[i]) ray_release(g_env.vals[i]);
    }
    memset(&g_env, 0, sizeof(g_env));
}

ray_t* ray_env_get(int64_t sym_id) {
    /* Search local scopes top-down first */
    for (int32_t d = scope_depth - 1; d >= 0; d--) {
        ray_scope_frame_t* f = &scope_stack[d];
        for (int32_t i = 0; i < f->count; i++) {
            if (f->keys[i] == sym_id) return f->vals[i];
        }
    }
    /* Fall through to global */
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) return g_env.vals[i];
    }
    return NULL;
}

ray_err_t ray_env_set(int64_t sym_id, ray_t* val) {
    env_lock();
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) {
            if (g_env.vals[i]) ray_release(g_env.vals[i]);
            ray_retain(val);
            g_env.vals[i] = val;
            env_unlock();
            return RAY_OK;
        }
    }
    if (g_env.count >= ENV_CAP) {
        env_unlock();
        return RAY_ERR_OOM;
    }
    g_env.keys[g_env.count] = sym_id;
    ray_retain(val);
    g_env.vals[g_env.count] = val;
    g_env.count++;
    env_unlock();
    return RAY_OK;
}

ray_err_t ray_env_push_scope(void) {
    if (scope_depth >= SCOPE_CAP) return RAY_ERR_OOM;
    scope_stack[scope_depth].count = 0;
    scope_depth++;
    return RAY_OK;
}

void ray_env_pop_scope(void) {
    if (scope_depth <= 0) return;
    scope_depth--;
    ray_scope_frame_t* f = &scope_stack[scope_depth];
    for (int32_t i = 0; i < f->count; i++) {
        if (f->vals[i]) ray_release(f->vals[i]);
    }
    f->count = 0;
}

/* ---- Iteration ---- */

int32_t ray_env_list(int64_t* sym_ids, ray_t** vals, int32_t max_entries) {
    int32_t n = g_env.count < max_entries ? g_env.count : max_entries;
    for (int32_t i = 0; i < n; i++) {
        sym_ids[i] = g_env.keys[i];
        vals[i] = g_env.vals[i];
    }
    return n;
}

/* ---- Prefix lookup ---- */

static const char* s_keywords[] = {
    "def", "do", "false", "fn", "if", "let", "set", "true", NULL
};

/* Compare helper for qsort on const char* */
static int cmp_str_ptr(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

int64_t ray_env_lookup_prefix(const char* prefix, int64_t len,
                              const char** results, int64_t max_results) {
    int64_t count = 0;

    /* Scan global env keys */
    for (int32_t i = 0; i < g_env.count && count < max_results; i++) {
        ray_t* s = ray_sym_str(g_env.keys[i]);
        if (!s) continue;
        const char* name = ray_str_ptr(s);
        if (!name) continue;
        int64_t nlen = (int64_t)strlen(name);
        if (nlen >= len && strncmp(name, prefix, (size_t)len) == 0) {
            /* Deduplicate against what we already have */
            int dup = 0;
            for (int64_t j = 0; j < count; j++) {
                if (strcmp(results[j], name) == 0) { dup = 1; break; }
            }
            if (!dup) results[count++] = name;
        }
    }

    /* Scan static keyword list */
    for (const char** kw = s_keywords; *kw && count < max_results; kw++) {
        int64_t klen = (int64_t)strlen(*kw);
        if (klen >= len && strncmp(*kw, prefix, (size_t)len) == 0) {
            int dup = 0;
            for (int64_t j = 0; j < count; j++) {
                if (strcmp(results[j], *kw) == 0) { dup = 1; break; }
            }
            if (!dup) results[count++] = *kw;
        }
    }

    /* Sort alphabetically */
    if (count > 1) {
        qsort((void*)results, (size_t)count, sizeof(const char*), cmp_str_ptr);
    }
    return count;
}

ray_err_t ray_env_set_local(int64_t sym_id, ray_t* val) {
    if (scope_depth <= 0) return ray_env_set(sym_id, val);
    ray_scope_frame_t* f = &scope_stack[scope_depth - 1];
    /* Update existing in this frame */
    for (int32_t i = 0; i < f->count; i++) {
        if (f->keys[i] == sym_id) {
            if (f->vals[i]) ray_release(f->vals[i]);
            ray_retain(val);
            f->vals[i] = val;
            return RAY_OK;
        }
    }
    if (f->count >= FRAME_CAP) return RAY_ERR_OOM;
    f->keys[f->count] = sym_id;
    ray_retain(val);
    f->vals[f->count] = val;
    f->count++;
    return RAY_OK;
}
