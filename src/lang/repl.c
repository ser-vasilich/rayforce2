#include "app/repl.h"
#include "lang/eval.h"
#include "mem/heap.h"
#include <rayforce.h>

int main(int argc, char** argv) {
    ray_heap_init();
    ray_sym_init();
    ray_lang_init();

    int rc = 0;
    if (argc > 1) {
        rc = ray_repl_run_file(argv[1]);
    } else {
        ray_repl_t* repl = ray_repl_create();
        if (repl) {
            ray_repl_run(repl);
            ray_repl_destroy(repl);
        }
    }

    ray_lang_destroy();
    ray_sym_destroy();
    ray_heap_destroy();
    return rc;
}
