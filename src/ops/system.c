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

#include "lang/eval_internal.h"
#include "lang/env.h"
#include "lang/parse.h"
#include "mem/heap.h"
#include "store/serde.h"
#include "store/splay.h"
#include "store/part.h"
#include "core/ipc.h"
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* ══════════════════════════════════════════
 * Serialization / storage
 * ══════════════════════════════════════════ */

/* (ser val) -> serialize to U8 vector with IPC header */
ray_t* ray_ser_fn(ray_t* val) {
    return ray_ser(val);
}

/* (de bytes) -> deserialize from U8 vector */
ray_t* ray_de_fn(ray_t* val) {
    return ray_de(val);
}

/* Build default sym path: dir/sym. Returns NULL if file does not exist. */
static const char* splay_default_sym(const char* dir, char* buf, size_t bufsz,
                                     bool must_exist) {
    int n = snprintf(buf, bufsz, "%s/sym", dir);
    if (n < 0 || (size_t)n >= bufsz) return NULL;
    if (must_exist && access(buf, F_OK) != 0) return NULL;
    return buf;
}

/* Helper: extract null-terminated path from a STR atom into a stack buffer.
 * Returns pointer to buf on success, NULL on failure. */
static const char* str_to_cpath(ray_t* s, char* buf, size_t bufsz) {
    if (!s || s->type != -RAY_STR) return NULL;
    const char* p = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (!p || len == 0 || len >= bufsz) return NULL;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/* (set-splayed "dir" table) or (set-splayed "dir" table "sym_path") */
ray_t* ray_set_splayed_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("domain", NULL);

    char dir[1024];
    if (!str_to_cpath(args[0], dir, sizeof(dir))) return ray_error("type", NULL);

    ray_t* tbl = args[1];
    if (!tbl || tbl->type != RAY_TABLE) return ray_error("type", NULL);

    char sym[1024];
    const char* sym_path = NULL;
    if (n == 3 && args[2] && args[2]->type == -RAY_STR)
        sym_path = str_to_cpath(args[2], sym, sizeof(sym));
    else
        sym_path = splay_default_sym(dir, sym, sizeof(sym), false);

    ray_err_t err = ray_splay_save(tbl, dir, sym_path);
    if (err != RAY_OK) return ray_error(ray_err_code_str(err), NULL);

    ray_retain(tbl);
    return tbl;
}

/* (get-splayed "dir") or (get-splayed "dir" "sym_path") */
ray_t* ray_get_splayed_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2) return ray_error("domain", NULL);

    char dir[1024];
    if (!str_to_cpath(args[0], dir, sizeof(dir))) return ray_error("type", NULL);

    char sym[1024];
    const char* sym_path = NULL;
    if (n == 2 && args[1] && args[1]->type == -RAY_STR)
        sym_path = str_to_cpath(args[1], sym, sizeof(sym));
    else
        sym_path = splay_default_sym(dir, sym, sizeof(sym), true);

    return ray_splay_load(dir, sym_path);
}

/* (get-parted "db_root" `table_name) -- load partitioned table */
ray_t* ray_get_parted_fn(ray_t** args, int64_t n) {
    if (n != 2) return ray_error("domain", NULL);

    char root[1024];
    if (!str_to_cpath(args[0], root, sizeof(root))) return ray_error("type", NULL);

    /* Table name as symbol atom */
    if (!args[1] || args[1]->type != -RAY_SYM) return ray_error("type", NULL);
    ray_t* name_atom = ray_sym_str(args[1]->i64);
    if (!name_atom) return ray_error("name", NULL);

    char name[256];
    size_t nlen = ray_str_len(name_atom);
    if (nlen == 0 || nlen >= sizeof(name)) return ray_error("domain", NULL);
    memcpy(name, ray_str_ptr(name_atom), nlen);
    name[nlen] = '\0';

    return ray_read_parted(root, name);
}

/* (guid n) -> generate n random GUIDs as GUID vector */
ray_t* ray_guid_fn(ray_t* n_arg) {
    if (!n_arg || !is_numeric(n_arg)) return ray_error("type", NULL);
    int64_t n = as_i64(n_arg);
    if (n < 0) return ray_error("domain", NULL);
    ray_t* result = ray_vec_new(RAY_GUID, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    uint8_t* data = (uint8_t*)ray_data(result);
    for (int64_t i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++)
            data[i * 16 + j] = (uint8_t)(rand() & 0xFF);
        /* Set version 4 and variant bits */
        data[i * 16 + 6] = (data[i * 16 + 6] & 0x0F) | 0x40;
        data[i * 16 + 8] = (data[i * 16 + 8] & 0x3F) | 0x80;
    }
    return result;
}

/* ══════════════════════════════════════════
 * Eval, parse, print, system, env builtins
 * ══════════════════════════════════════════ */

/* (eval expr) -- evaluate a parsed expression */
ray_t* ray_eval_builtin(ray_t* x) {
    return ray_eval(x);
}

/* (parse str) -- parse a string into an AST */
ray_t* ray_parse_builtin(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "parse expects a string");
    const char* src = ray_str_ptr(x);
    if (!src) return ray_error("domain", NULL);
    ray_t* parsed = ray_parse(src);
    return parsed ? parsed : ray_error("parse", NULL);
}

/* (print val) -- print without newline, return the value */
ray_t* ray_print_fn(ray_t* x) {
    ray_fmt_print(stdout, x, 0);
    fflush(stdout);
    return x;
}

/* (meta x) -- return metadata about an object as a dict */
ray_t* ray_meta_fn(ray_t* x) {
    if (!x) return ray_error("type", NULL);

    const char* tname = type_sym_name(x->type);
    int64_t type_sym = ray_sym_intern("type", 4);
    int64_t type_id  = ray_sym_intern(tname, strlen(tname));

    if (ray_is_atom(x)) {
        /* Atom: return {type: <typename>} */
        ray_t* dict = ray_list_new(2);
        if (RAY_IS_ERR(dict)) return dict;
        dict->attrs |= RAY_ATTR_DICT;
        ray_t* k = ray_sym(type_sym);
        dict = ray_list_append(dict, k); ray_release(k);
        ray_t* tv = ray_sym(type_id);
        dict = ray_list_append(dict, tv); ray_release(tv);
        return dict;
    }

    /* Vector/table/list: return {type: <typename>, len: <n>} */
    ray_t* dict = ray_list_new(4);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    ray_t* k1 = ray_sym(type_sym);
    dict = ray_list_append(dict, k1); ray_release(k1);
    if (x->type == RAY_LIST && (x->attrs & RAY_ATTR_DICT)) {
        int64_t did = ray_sym_intern("DICT", 4);
        ray_t* tv = ray_sym(did);
        dict = ray_list_append(dict, tv); ray_release(tv);
    } else {
        ray_t* tv = ray_sym(type_id);
        dict = ray_list_append(dict, tv); ray_release(tv);
    }

    int64_t len_sym = ray_sym_intern("len", 3);
    ray_t* k2 = ray_sym(len_sym);
    dict = ray_list_append(dict, k2); ray_release(k2);
    ray_t* lv = make_i64(x->len);
    dict = ray_list_append(dict, lv); ray_release(lv);

    return dict;
}

/* (gc) -- no-op garbage collection trigger, return 0 */
ray_t* ray_gc_fn(ray_t* x) { (void)x; return ray_i64(0); }

/* (system cmd) -- run shell command, return exit code */
ray_t* ray_system_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "system expects a string");
    const char* cmd = ray_str_ptr(x);
    if (!cmd) return ray_error("domain", NULL);
    int rc = system(cmd);
    return make_i64(rc);
}

/* (getenv name) -- get environment variable */
ray_t* ray_getenv_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "getenv expects a string");
    const char* name = ray_str_ptr(x);
    if (!name) return ray_error("domain", NULL);
    const char* val = getenv(name);
    return val ? ray_str(val, strlen(val)) : ray_str("", 0);
}

/* (setenv name val) -- set environment variable */
#if !defined(_WIN32)
extern int setenv(const char*, const char*, int);
#endif
ray_t* ray_setenv_fn(ray_t* name, ray_t* val) {
    if (name->type != -RAY_STR || val->type != -RAY_STR)
        return ray_error("type", "setenv expects two strings");
    const char* n = ray_str_ptr(name);
    const char* v = ray_str_ptr(val);
    if (!n || !v) return ray_error("domain", NULL);
#if defined(_WIN32)
    _putenv_s(n, v);
#else
    setenv(n, v, 1);
#endif
    return val;
}

/* ══════════════════════════════════════════
 * Quote, return, args, rc, diverse, get, remove,
 * timer, env, internals, memstat, sysinfo
 * ══════════════════════════════════════════ */

/* (quote expr) -- special form, returns argument unevaluated */
ray_t* ray_quote_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", "quote expects 1 argument");
    ray_retain(args[0]);
    return args[0];
}

/* (return x) -- early return from function (identity in Rayfall) */
ray_t* ray_return_fn(ray_t* x) {
    ray_retain(x);
    return x;
}

/* (args) -- return command-line arguments as a list of strings */
ray_t* ray_args_fn(ray_t* x) {
    (void)x;
    /* Return empty list -- CLI args not wired into eval context */
    ray_t* list = ray_list_new(0);
    if (!list) return ray_error("oom", NULL);
    return list;
}

/* (rc x) -- return reference count of object */
ray_t* ray_rc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return make_i64(0);
    return make_i64((int64_t)x->rc);
}

/* (diverse x) -- check if all elements in a collection are unique */
ray_t* ray_diverse_fn(ray_t* x) {
    if (ray_is_atom(x)) return make_bool(1);
    if (!is_collection(x)) return ray_error("type", "diverse expects a collection");

    int64_t n = ray_len(x);
    if (n <= 1) return make_bool(1);

    ray_t* d = ray_distinct_fn(x);
    if (RAY_IS_ERR(d)) return d;
    int64_t dn = ray_len(d);
    ray_release(d);
    return make_bool(dn == n ? 1 : 0);
}

/* (get dict key) -- dictionary/table lookup (alias for at) */
ray_t* ray_get_fn(ray_t* dict, ray_t* key) {
    return ray_at(dict, key);
}

/* (remove dict key) -- remove key from dict, return new dict */
ray_t* ray_remove_fn(ray_t* dict, ray_t* key) {
    if (dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return ray_error("type", "remove expects a dict");
    if (key->type != -RAY_SYM)
        return ray_error("type", "remove key must be a symbol");

    ray_t** items = (ray_t**)ray_data(dict);
    int64_t n = dict->len;
    ray_t* result = ray_list_new(0);
    if (RAY_IS_ERR(result)) return result;
    result->attrs |= RAY_ATTR_DICT;

    for (int64_t i = 0; i < n; i += 2) {
        if (items[i]->type == -RAY_SYM && items[i]->i64 == key->i64)
            continue; /* skip this key-value pair */
        result = ray_list_append(result, items[i]);
        if (RAY_IS_ERR(result)) return result;
        if (i + 1 < n) {
            result = ray_list_append(result, items[i + 1]);
            if (RAY_IS_ERR(result)) return result;
        }
    }
    return result;
}

/* (timer) -- return high-res timestamp in nanoseconds for benchmarking */
ray_t* ray_timer_fn(ray_t* x) {
    (void)x;
    clock_t t = clock();
    int64_t nanos = (int64_t)((double)t / (double)CLOCKS_PER_SEC * 1e9);
    return make_i64(nanos);
}

/* (env) -- return dict of all global environment bindings */
ray_t* ray_env_fn(ray_t* x) {
    (void)x;
    int64_t sym_ids[1024];
    ray_t* vals[1024];
    int32_t count = ray_env_list(sym_ids, vals, 1024);

    ray_t* dict = ray_list_new(0);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    for (int32_t i = 0; i < count; i++) {
        ray_t* k = ray_sym(sym_ids[i]);
        if (RAY_IS_ERR(k)) { ray_release(dict); return k; }
        dict = ray_list_append(dict, k);
        ray_release(k);
        if (RAY_IS_ERR(dict)) return dict;
        ray_retain(vals[i]);
        dict = ray_list_append(dict, vals[i]);
        if (RAY_IS_ERR(dict)) { ray_release(vals[i]); return dict; }
    }
    return dict;
}

/* (internals) -- return dict with internal build information */
ray_t* ray_internals_fn(ray_t* x) {
    (void)x;
    ray_t* dict = ray_list_new(4);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    int64_t ver_sym = ray_sym_intern("version", 7);
    ray_t* k1 = ray_sym(ver_sym);
    dict = ray_list_append(dict, k1); ray_release(k1);
#ifdef RAYFORCE_VERSION
    ray_t* v1 = ray_str(RAYFORCE_VERSION, strlen(RAYFORCE_VERSION));
#else
    ray_t* v1 = ray_str("unknown", 7);
#endif
    dict = ray_list_append(dict, v1); ray_release(v1);

    int64_t date_sym = ray_sym_intern("build-date", 10);
    ray_t* k2 = ray_sym(date_sym);
    dict = ray_list_append(dict, k2); ray_release(k2);
#ifdef RAYFORCE_BUILD_DATE
    ray_t* v2 = ray_str(RAYFORCE_BUILD_DATE, strlen(RAYFORCE_BUILD_DATE));
#else
    ray_t* v2 = ray_str("unknown", 7);
#endif
    dict = ray_list_append(dict, v2); ray_release(v2);

    return dict;
}

/* (memstat) -- return dict with memory allocator statistics */
ray_t* ray_memstat_fn(ray_t* x) {
    (void)x;
    ray_mem_stats_t st;
    ray_mem_stats(&st);

    ray_t* dict = ray_list_new(10);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    /* alloc-count */
    int64_t s1 = ray_sym_intern("alloc-count", 11);
    ray_t* k1 = ray_sym(s1); dict = ray_list_append(dict, k1); ray_release(k1);
    ray_t* v1 = make_i64((int64_t)st.alloc_count);
    dict = ray_list_append(dict, v1); ray_release(v1);

    /* bytes-allocated */
    int64_t s2 = ray_sym_intern("bytes-allocated", 15);
    ray_t* k2 = ray_sym(s2); dict = ray_list_append(dict, k2); ray_release(k2);
    ray_t* v2 = make_i64((int64_t)st.bytes_allocated);
    dict = ray_list_append(dict, v2); ray_release(v2);

    /* peak-bytes */
    int64_t s3 = ray_sym_intern("peak-bytes", 10);
    ray_t* k3 = ray_sym(s3); dict = ray_list_append(dict, k3); ray_release(k3);
    ray_t* v3 = make_i64((int64_t)st.peak_bytes);
    dict = ray_list_append(dict, v3); ray_release(v3);

    /* slab-hits */
    int64_t s4 = ray_sym_intern("slab-hits", 9);
    ray_t* k4 = ray_sym(s4); dict = ray_list_append(dict, k4); ray_release(k4);
    ray_t* v4 = make_i64((int64_t)st.slab_hits);
    dict = ray_list_append(dict, v4); ray_release(v4);

    /* sys-current */
    int64_t s5 = ray_sym_intern("sys-current", 11);
    ray_t* k5 = ray_sym(s5); dict = ray_list_append(dict, k5); ray_release(k5);
    ray_t* v5 = make_i64((int64_t)st.sys_current);
    dict = ray_list_append(dict, v5); ray_release(v5);

    return dict;
}

ray_t* ray_sysinfo_fn(ray_t* x) {
    (void)x;
    ray_t* dict = ray_list_new(6);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

#if !defined(_WIN32)
    int64_t s1 = ray_sym_intern("cores", 5);
    ray_t* k1 = ray_sym(s1); dict = ray_list_append(dict, k1); ray_release(k1);
    ray_t* v1 = make_i64(sysconf(_SC_NPROCESSORS_ONLN));
    dict = ray_list_append(dict, v1); ray_release(v1);

    int64_t s2 = ray_sym_intern("page-size", 9);
    ray_t* k2 = ray_sym(s2); dict = ray_list_append(dict, k2); ray_release(k2);
    ray_t* v2 = make_i64(sysconf(_SC_PAGESIZE));
    dict = ray_list_append(dict, v2); ray_release(v2);

    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    int64_t s3 = ray_sym_intern("total-mem", 9);
    ray_t* k3 = ray_sym(s3); dict = ray_list_append(dict, k3); ray_release(k3);
    ray_t* v3 = make_i64((int64_t)pages * (int64_t)psize);
    dict = ray_list_append(dict, v3); ray_release(v3);
#else
    int64_t s1 = ray_sym_intern("cores", 5);
    ray_t* k1 = ray_sym(s1); dict = ray_list_append(dict, k1); ray_release(k1);
    ray_t* v1 = make_i64(1);
    dict = ray_list_append(dict, v1); ray_release(v1);
#endif

    return dict;
}

/* ══════════════════════════════════════════
 * IPC builtins
 * ══════════════════════════════════════════ */

/* (hopen "host:port") → i64 handle */
ray_t* ray_hopen_fn(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_STR)
        return ray_error("type", NULL);

    const char* s = ray_str_ptr(x);
    size_t slen = ray_str_len(x);

    /* Parse "host:port" — find last colon for port */
    const char* colon = NULL;
    for (size_t i = 0; i < slen; i++)
        if (s[i] == ':') colon = s + i;
    if (!colon) return ray_error("domain", NULL);

    /* Extract host (need null-terminated copy) */
    size_t host_len = (size_t)(colon - s);
    char host[256];
    if (host_len >= sizeof(host)) return ray_error("domain", NULL);
    memcpy(host, s, host_len);
    host[host_len] = '\0';

    /* Parse port */
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return ray_error("domain", NULL);

    int64_t h = ray_ipc_connect(host, (uint16_t)port);
    if (h < 0) return ray_error("io", "connection refused: %s:%d", host, port);

    return make_i64(h);
}

/* (hclose handle) → null */
ray_t* ray_hclose_fn(ray_t* x) {
    if (!ray_is_atom(x) || (x->type != -RAY_I64 && x->type != -RAY_I32))
        return ray_error("type", NULL);
    int64_t h = (x->type == -RAY_I64) ? x->i64 : x->i32;
    ray_ipc_close(h);
    return RAY_NULL_OBJ;
}

/* (hsend handle msg) → result */
ray_t* ray_hsend_fn(ray_t* handle, ray_t* msg) {
    if (!ray_is_atom(handle) || (handle->type != -RAY_I64 && handle->type != -RAY_I32))
        return ray_error("type", NULL);
    int64_t h = (handle->type == -RAY_I64) ? handle->i64 : handle->i32;
    /* Validate message is serializable (reject builtins, etc.) */
    if (ray_serde_size(msg) <= 0)
        return ray_error("type", "message not serializable");
    return ray_ipc_send(h, msg);
}
