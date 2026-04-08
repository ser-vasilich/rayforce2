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

#ifndef RAY_ENV_H
#define RAY_ENV_H

#include <rayforce.h>
#include "lang/eval.h"

/* Create function objects. Name stored inline in nullmap[0..15].
 * The function pointer is in the i64 field. */
ray_t* ray_fn_unary(const char* name, uint8_t fn_attrs, ray_unary_fn fn);
ray_t* ray_fn_binary(const char* name, uint8_t fn_attrs, ray_binary_fn fn);
ray_t* ray_fn_vary(const char* name, uint8_t fn_attrs, ray_vary_fn fn);

/* Read builtin name from nullmap. Binary ops store opcode at [0..1],
 * name at [2..15]. Unary/vary store name at [0..15]. */
static inline const char* ray_fn_name(const ray_t* fn) {
    return (const char*)fn->nullmap + (fn->type == RAY_BINARY ? 2 : 0);
}

/* Global environment: symbol -> function object dict */
ray_err_t ray_env_init(void);
void     ray_env_destroy(void);
ray_t*    ray_env_get(int64_t sym_id);
ray_err_t ray_env_set(int64_t sym_id, ray_t* val);

/* Prefix lookup: scan global env + keywords for names starting with prefix.
 * Fills results[] with pointers to interned name strings (valid until next
 * sym table mutation).  Returns count of matches (up to max_results).
 * Results are sorted alphabetically. */
int64_t ray_env_lookup_prefix(const char* prefix, int64_t len,
                              const char** results, int64_t max_results);

/* Iterate global environment entries.
 * Fills sym_ids[] and vals[] with up to max_entries items.
 * Returns count of entries written. */
int32_t ray_env_list(int64_t* sym_ids, ray_t** vals, int32_t max_entries);

/* Local scope stack for lexical binding (let, do, lambda) */
ray_err_t ray_env_push_scope(void);
void ray_env_pop_scope(void);
ray_err_t ray_env_set_local(int64_t sym_id, ray_t* val);

#endif /* RAY_ENV_H */
