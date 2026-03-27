#include "lang/eval.h"
#include <teide/td.h>
#include <stdio.h>
#include <string.h>

#define REPL_BUF_SIZE 4096

static int run_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    td_t* block = td_alloc((int64_t)len + 1);
    if (!block) {
        fclose(f);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    char* buf = (char*)td_data(block);
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';

    td_t* result = td_eval_str(buf);
    td_release(block);
    if (result && !TD_IS_ERR(result)) {
        td_lang_print(stdout, result);
        fputc('\n', stdout);
        td_release(result);
    } else if (TD_IS_ERR(result)) {
        fprintf(stderr, "error: evaluation failed\n");
        return 1;
    }
    return 0;
}

static void run_repl(void) {
    char buf[REPL_BUF_SIZE];
    fprintf(stdout, "teide> ");
    fflush(stdout);
    while (fgets(buf, REPL_BUF_SIZE, stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (buf[0] == '\0') {
            fprintf(stdout, "teide> ");
            fflush(stdout);
            continue;
        }
        if (strcmp(buf, "\\\\") == 0 || strcmp(buf, "exit") == 0) break;

        td_t* result = td_eval_str(buf);
        if (result && !TD_IS_ERR(result)) {
            td_lang_print(stdout, result);
            fputc('\n', stdout);
            td_release(result);
        } else if (TD_IS_ERR(result)) {
            fprintf(stderr, "error\n");
        }
        fprintf(stdout, "teide> ");
        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    td_heap_init();
    td_sym_init();
    td_lang_init();

    int rc = 0;
    if (argc > 1) {
        rc = run_file(argv[1]);
    } else {
        run_repl();
    }

    td_lang_destroy();
    td_sym_destroy();
    td_heap_destroy();
    return rc;
}
