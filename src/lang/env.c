#include "lang/env.h"
#include <string.h>

/* ---- Function constructors ---- */

td_t* td_fn_unary(const char* name, uint8_t fn_attrs, td_unary_fn fn) {
    td_t* obj = td_alloc(0);  /* atom, no data beyond header */
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_UNARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    (void)name;
    return obj;
}

td_t* td_fn_binary(const char* name, uint8_t fn_attrs, td_binary_fn fn) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_BINARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    (void)name;
    return obj;
}

td_t* td_fn_vary(const char* name, uint8_t fn_attrs, td_vary_fn fn) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_VARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    (void)name;
    return obj;
}

/* ---- Global environment ---- */

#define ENV_CAP 512

static struct {
    int64_t keys[ENV_CAP];
    td_t*   vals[ENV_CAP];
    int32_t count;
} g_env;

td_err_t td_env_init(void) {
    memset(&g_env, 0, sizeof(g_env));
    return TD_OK;
}

void td_env_destroy(void) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.vals[i]) td_release(g_env.vals[i]);
    }
    memset(&g_env, 0, sizeof(g_env));
}

td_t* td_env_get(int64_t sym_id) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) return g_env.vals[i];
    }
    return NULL;
}

void td_env_set(int64_t sym_id, td_t* val) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) {
            if (g_env.vals[i]) td_release(g_env.vals[i]);
            td_retain(val);
            g_env.vals[i] = val;
            return;
        }
    }
    if (g_env.count < ENV_CAP) {
        g_env.keys[g_env.count] = sym_id;
        td_retain(val);
        g_env.vals[g_env.count] = val;
        g_env.count++;
    }
}
