#ifndef RAY_APP_REPL_H
#define RAY_APP_REPL_H

#include <rayforce.h>

typedef struct ray_term ray_term_t;

typedef struct ray_repl {
    ray_t*        _block;
    ray_term_t*   term;    /* NULL if piped/non-tty */
    bool   timeit;  /* :timeit toggle — print expression timing */
} ray_repl_t;

ray_repl_t* ray_repl_create(void);
void       ray_repl_destroy(ray_repl_t* repl);
void       ray_repl_run(ray_repl_t* repl);
int        ray_repl_run_file(const char* path);

#endif /* RAY_APP_REPL_H */
