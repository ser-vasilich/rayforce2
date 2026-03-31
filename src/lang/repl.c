#include "app/repl.h"
#include "lang/eval.h"
#include "mem/heap.h"
#include <rayforce.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    ray_heap_init();
    ray_sym_init();
    ray_lang_init();

    int rc = 0;
    int interactive = 0;
    const char* file = NULL;

    /* Parse args: [-i] [file.rfl] */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = 1;
        else
            file = argv[i];
    }

    /* Load script if specified */
    if (file) {
        rc = ray_repl_run_file(file);
        /* Oneshot: file without -i → execute and exit (like Python/rayforce) */
        if (!interactive) goto done;
    }

    /* REPL: interactive TTY, piped stdin, or -i after script */
    {
        ray_repl_t* repl = ray_repl_create();
        if (repl) {
            ray_repl_run(repl);
            ray_repl_destroy(repl);
        }
    }

done:
    ray_lang_destroy();
    ray_sym_destroy();
    ray_heap_destroy();
    return rc;
}
