#ifndef TD_ENV_H
#define TD_ENV_H

#include <teide/td.h>

/* Create function objects. Name is interned as a symbol.
 * The function pointer is stored in the i64 field of the atom. */
td_t* td_fn_unary(const char* name, uint8_t fn_attrs, td_unary_fn fn);
td_t* td_fn_binary(const char* name, uint8_t fn_attrs, td_binary_fn fn);
td_t* td_fn_vary(const char* name, uint8_t fn_attrs, td_vary_fn fn);

/* Global environment: symbol -> function object dict */
td_err_t td_env_init(void);
void     td_env_destroy(void);
td_t*    td_env_get(int64_t sym_id);
td_err_t td_env_set(int64_t sym_id, td_t* val);

/* Prefix lookup: scan global env + keywords for names starting with prefix.
 * Fills results[] with pointers to interned name strings (valid until next
 * sym table mutation).  Returns count of matches (up to max_results).
 * Results are sorted alphabetically. */
int64_t td_env_lookup_prefix(const char* prefix, int64_t len,
                              const char** results, int64_t max_results);

/* Iterate global environment entries.
 * Fills sym_ids[] and vals[] with up to max_entries items.
 * Returns count of entries written. */
int32_t td_env_list(int64_t* sym_ids, td_t** vals, int32_t max_entries);

/* Local scope stack for lexical binding (let, do, lambda) */
td_err_t td_env_push_scope(void);
void td_env_pop_scope(void);
td_err_t td_env_set_local(int64_t sym_id, td_t* val);

#endif /* TD_ENV_H */
