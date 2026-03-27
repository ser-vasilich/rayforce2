#include "app/repl.h"
#include "app/term.h"
#include "lang/eval.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define isatty _isatty
#define STDIN_FD 0
#else
#include <unistd.h>
#define STDIN_FD STDIN_FILENO
#endif

#define PIPE_BUF_SIZE 4096

td_repl_t* td_repl_create(void) {
    td_t* block = td_alloc(sizeof(td_repl_t));
    if (!block) return NULL;
    td_repl_t* repl = (td_repl_t*)td_data(block);
    memset(repl, 0, sizeof(*repl));
    repl->_block = block;

    if (isatty(STDIN_FD)) {
        repl->term = td_term_create();
    }
    return repl;
}

void td_repl_destroy(td_repl_t* repl) {
    if (!repl) return;
    if (repl->term) {
        td_term_destroy((td_term_t*)repl->term);
    }
    td_free(repl->_block);
}

static void eval_and_print(const char* input) {
    td_t* result = td_eval_str(input);
    if (result && !TD_IS_ERR(result)) {
        td_lang_print(stdout, result);
        fputc('\n', stdout);
        fflush(stdout);
        td_release(result);
    } else if (TD_IS_ERR(result)) {
        fprintf(stderr, "error\n");
    }
}

static void run_interactive(td_repl_t* repl) {
    td_term_t* term = (td_term_t*)repl->term;

    for (;;) {
        td_t* line = td_term_read(term);
        if (!line) break; /* EOF / Ctrl-D */

        const char* str = td_str_ptr(line);
        size_t len = td_str_len(line);

        if (len == 0) {
            td_release(line);
            continue;
        }

        /* Exit commands */
        if ((len == 2 && memcmp(str, "\\\\", 2) == 0) ||
            (len == 4 && memcmp(str, "exit", 4) == 0)) {
            td_release(line);
            break;
        }

        eval_and_print(str);
        td_release(line);
    }
}

static void run_piped(void) {
    char buf[PIPE_BUF_SIZE];
    fprintf(stdout, "teide> ");
    fflush(stdout);

    while (fgets(buf, PIPE_BUF_SIZE, stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (buf[0] == '\0') {
            fprintf(stdout, "teide> ");
            fflush(stdout);
            continue;
        }
        if (strcmp(buf, "\\\\") == 0 || strcmp(buf, "exit") == 0) break;

        eval_and_print(buf);
        fprintf(stdout, "teide> ");
        fflush(stdout);
    }
}

void td_repl_run(td_repl_t* repl) {
    if (repl->term) {
        run_interactive(repl);
    } else {
        run_piped();
    }
}

int td_repl_run_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) {
        fclose(f);
        fprintf(stderr, "error: cannot determine size of '%s'\n", path);
        return 1;
    }

    td_t* block = td_alloc((int64_t)flen + 1);
    if (!block) {
        fclose(f);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    char* buf = (char*)td_data(block);
    size_t nread = fread(buf, 1, (size_t)flen, f);
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
