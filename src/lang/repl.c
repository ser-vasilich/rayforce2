#include "app/repl.h"
#include "lang/eval.h"
#include <teide/td.h>

int main(int argc, char** argv) {
    td_heap_init();
    td_sym_init();
    td_lang_init();

    int rc = 0;
    if (argc > 1) {
        rc = td_repl_run_file(argv[1]);
    } else {
        td_repl_t* repl = td_repl_create();
        if (repl) {
            td_repl_run(repl);
            td_repl_destroy(repl);
        }
    }

    td_lang_destroy();
    td_sym_destroy();
    td_heap_destroy();
    return rc;
}
