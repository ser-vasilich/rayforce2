#ifndef TD_EVAL_H
#define TD_EVAL_H

#include <teide/td.h>

/* Initialize the Rayfall runtime: symbols, environment, builtins. */
td_err_t td_lang_init(void);
void     td_lang_destroy(void);

/* Evaluate a parsed td_t object tree. */
td_t* td_eval(td_t* obj);

/* Parse + eval convenience. */
td_t* td_eval_str(const char* source);

#endif /* TD_EVAL_H */
