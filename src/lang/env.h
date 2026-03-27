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
void     td_env_set(int64_t sym_id, td_t* val);

/* Local scope stack for lexical binding (let, do, lambda) */
void td_env_push_scope(void);
void td_env_pop_scope(void);
void td_env_set_local(int64_t sym_id, td_t* val);

#endif /* TD_ENV_H */
