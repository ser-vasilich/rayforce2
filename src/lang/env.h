#ifndef RAY_ENV_H
#define RAY_ENV_H

#include <rayforce.h>

/* Create function objects. Name is interned as a symbol.
 * The function pointer is stored in the i64 field of the atom. */
ray_t* ray_fn_unary(const char* name, uint8_t fn_attrs, ray_unary_fn fn);
ray_t* ray_fn_binary(const char* name, uint8_t fn_attrs, ray_binary_fn fn);
ray_t* ray_fn_vary(const char* name, uint8_t fn_attrs, ray_vary_fn fn);

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
