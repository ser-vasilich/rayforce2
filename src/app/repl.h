#ifndef TD_APP_REPL_H
#define TD_APP_REPL_H

#include <teide/td.h>

typedef struct td_repl {
    td_t*  _block;
    void*  term;    /* td_term_t* — NULL if piped/non-tty */
    bool   timeit;  /* :timeit toggle — print expression timing */
} td_repl_t;

td_repl_t* td_repl_create(void);
void       td_repl_destroy(td_repl_t* repl);
void       td_repl_run(td_repl_t* repl);
int        td_repl_run_file(const char* path);

#endif /* TD_APP_REPL_H */
