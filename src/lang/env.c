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

/* ---- Local scope stack ---- */

#define SCOPE_CAP  64
#define FRAME_CAP  64

typedef struct {
    int64_t keys[FRAME_CAP];
    td_t*   vals[FRAME_CAP];
    int32_t count;
} td_scope_frame_t;

static td_scope_frame_t scope_stack[SCOPE_CAP];
static int32_t scope_depth = 0;

td_err_t td_env_init(void) {
    memset(&g_env, 0, sizeof(g_env));
    scope_depth = 0;
    return TD_OK;
}

void td_env_destroy(void) {
    /* Pop any remaining scopes */
    while (scope_depth > 0) td_env_pop_scope();
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.vals[i]) td_release(g_env.vals[i]);
    }
    memset(&g_env, 0, sizeof(g_env));
}

td_t* td_env_get(int64_t sym_id) {
    /* Search local scopes top-down first */
    for (int32_t d = scope_depth - 1; d >= 0; d--) {
        td_scope_frame_t* f = &scope_stack[d];
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

void td_env_push_scope(void) {
    if (scope_depth < SCOPE_CAP) {
        scope_stack[scope_depth].count = 0;
        scope_depth++;
    }
}

void td_env_pop_scope(void) {
    if (scope_depth <= 0) return;
    scope_depth--;
    td_scope_frame_t* f = &scope_stack[scope_depth];
    for (int32_t i = 0; i < f->count; i++) {
        if (f->vals[i]) td_release(f->vals[i]);
    }
    f->count = 0;
}

void td_env_set_local(int64_t sym_id, td_t* val) {
    if (scope_depth <= 0) { td_env_set(sym_id, val); return; }
    td_scope_frame_t* f = &scope_stack[scope_depth - 1];
    /* Update existing in this frame */
    for (int32_t i = 0; i < f->count; i++) {
        if (f->keys[i] == sym_id) {
            if (f->vals[i]) td_release(f->vals[i]);
            td_retain(val);
            f->vals[i] = val;
            return;
        }
    }
    if (f->count < FRAME_CAP) {
        f->keys[f->count] = sym_id;
        td_retain(val);
        f->vals[f->count] = val;
        f->count++;
    }
}
