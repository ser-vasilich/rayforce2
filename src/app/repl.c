#if !defined(_WIN32)
#define _POSIX_C_SOURCE 199309L
#endif

#include "app/repl.h"
#include "app/term.h"
#include "lang/env.h"
#include "lang/eval.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#define isatty _isatty
#define STDIN_FD 0
#else
#include <unistd.h>
#define STDIN_FD STDIN_FILENO
#endif

#define PIPE_BUF_SIZE 4096
#define MAX_PRINT_ROWS 40
#define MAX_COL_WIDTH 40

/* ===== Pretty-print helpers ===== */

/* Check if vector element at idx is null */
static bool vec_is_null(td_t* vec, int64_t idx) {
    if (!(vec->attrs & TD_ATTR_HAS_NULLS)) return false;
    const uint8_t* bm;
    if (vec->attrs & TD_ATTR_NULLMAP_EXT) {
        td_t* ext = vec->ext_nullmap;
        if (!ext) return false;
        bm = (const uint8_t*)td_data(ext);
    } else {
        if (idx >= 128) return false;
        bm = vec->nullmap;
    }
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

/* Format a single vector element into buf, return chars written */
static int fmt_vec_elem(td_t* vec, int64_t idx, char* out, int max) {
    if (vec_is_null(vec, idx))
        return snprintf(out, (size_t)max, "null");

    switch (vec->type) {
    case TD_BOOL: {
        bool* data = (bool*)td_data(vec);
        return snprintf(out, (size_t)max, "%s", data[idx] ? "true" : "false");
    }
    case TD_I64: {
        int64_t* data = (int64_t*)td_data(vec);
        return snprintf(out, (size_t)max, "%ld", (long)data[idx]);
    }
    case TD_F64: {
        double* data = (double*)td_data(vec);
        return snprintf(out, (size_t)max, "%g", data[idx]);
    }
    case TD_I32: {
        int32_t* data = (int32_t*)td_data(vec);
        return snprintf(out, (size_t)max, "%d", data[idx]);
    }
    case TD_I16: {
        int16_t* data = (int16_t*)td_data(vec);
        return snprintf(out, (size_t)max, "%d", (int)data[idx]);
    }
    case TD_SYM: {
        uint8_t esz = (uint8_t)TD_SYM_ELEM(vec->attrs);
        const uint8_t* base = (const uint8_t*)td_data(vec);
        int64_t sym_id = 0;
        memcpy(&sym_id, base + idx * esz, esz);
        td_t* s = td_sym_str(sym_id);
        if (s) return snprintf(out, (size_t)max, "%.*s",
                               (int)td_str_len(s), td_str_ptr(s));
        return snprintf(out, (size_t)max, "?sym%ld", (long)sym_id);
    }
    case TD_STR: {
        size_t slen = 0;
        const char* s = td_str_vec_get(vec, idx, &slen);
        if (s) return snprintf(out, (size_t)max, "%.*s", (int)slen, s);
        return snprintf(out, (size_t)max, "null");
    }
    case TD_DATE: {
        int64_t* data = (int64_t*)td_data(vec);
        int64_t d = data[idx];
        /* Teide date = days since 2000-01-01 */
        return snprintf(out, (size_t)max, "%ld", (long)d);
    }
    default:
        return snprintf(out, (size_t)max, "?");
    }
}

/* Print a vector in [1 2 3 ...] format */
static void print_vector(FILE* fp, td_t* val) {
    int64_t len = td_len(val);
    td_t** elems = (td_t**)td_data(val);
    bool is_dict = (val->attrs & TD_ATTR_DICT) != 0;

    if (is_dict) {
        fprintf(fp, "{");
        for (int64_t i = 0; i + 1 < len; i += 2) {
            if (i > 0) fprintf(fp, " ");
            td_lang_print(fp, elems[i]);
            fprintf(fp, ": ");
            td_lang_print(fp, elems[i + 1]);
        }
        fprintf(fp, "}");
        return;
    }

    fprintf(fp, "[");
    for (int64_t i = 0; i < len; i++) {
        if (i > 0) fprintf(fp, " ");
        td_lang_print(fp, elems[i]);
    }
    fprintf(fp, "]");
}

/* Print a typed vector (TD_I64, TD_F64, TD_SYM, TD_STR, etc.) */
static void print_typed_vector(FILE* fp, td_t* vec) {
    int64_t len = td_len(vec);
    char elem_buf[256];
    fprintf(fp, "[");
    int64_t show = len > 100 ? 100 : len;
    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fprintf(fp, " ");
        fmt_vec_elem(vec, i, elem_buf, (int)sizeof(elem_buf));
        fprintf(fp, "%s", elem_buf);
    }
    if (len > show) fprintf(fp, " ...(+%ld)", (long)(len - show));
    fprintf(fp, "]");
}

/* Print a table in columnar format */
static void print_table(FILE* fp, td_t* tbl) {
    int64_t ncols = td_table_ncols(tbl);
    int64_t nrows = td_table_nrows(tbl);

    if (ncols == 0) {
        fprintf(fp, "(empty table)\n");
        return;
    }

    /* Collect column names and vectors */
    td_t* cols[256];
    const char* names[256];
    int name_lens[256];
    int col_widths[256];
    int actual_ncols = ncols > 256 ? 256 : (int)ncols;

    for (int c = 0; c < actual_ncols; c++) {
        cols[c] = td_table_get_col_idx(tbl, c);
        int64_t name_id = td_table_col_name(tbl, c);
        td_t* ns = td_sym_str(name_id);
        if (ns) {
            names[c] = td_str_ptr(ns);
            name_lens[c] = (int)td_str_len(ns);
        } else {
            names[c] = "?";
            name_lens[c] = 1;
        }
        col_widths[c] = name_lens[c];
    }

    /* Determine column widths by sampling rows */
    int64_t sample = nrows > MAX_PRINT_ROWS ? MAX_PRINT_ROWS : nrows;
    char elem_buf[256];
    for (int64_t r = 0; r < sample; r++) {
        for (int c = 0; c < actual_ncols; c++) {
            if (!cols[c]) continue;
            int w = fmt_vec_elem(cols[c], r, elem_buf, (int)sizeof(elem_buf));
            if (w > MAX_COL_WIDTH) w = MAX_COL_WIDTH;
            if (w > col_widths[c]) col_widths[c] = w;
        }
    }

    /* Print header */
    for (int c = 0; c < actual_ncols; c++) {
        if (c > 0) fprintf(fp, " | ");
        fprintf(fp, "%-*.*s", col_widths[c], name_lens[c], names[c]);
    }
    fprintf(fp, "\n");

    /* Print separator */
    for (int c = 0; c < actual_ncols; c++) {
        if (c > 0) fprintf(fp, "-+-");
        for (int i = 0; i < col_widths[c]; i++) fputc('-', fp);
    }
    fprintf(fp, "\n");

    /* Print rows */
    for (int64_t r = 0; r < sample; r++) {
        for (int c = 0; c < actual_ncols; c++) {
            if (c > 0) fprintf(fp, " | ");
            if (cols[c]) {
                int w = fmt_vec_elem(cols[c], r, elem_buf, (int)sizeof(elem_buf));
                (void)w;
                fprintf(fp, "%-*s", col_widths[c], elem_buf);
            } else {
                fprintf(fp, "%-*s", col_widths[c], "null");
            }
        }
        fprintf(fp, "\n");
    }

    if (nrows > sample)
        fprintf(fp, "... %ld more rows\n", (long)(nrows - sample));

    fprintf(fp, "(%ld row%s)\n", (long)nrows, nrows == 1 ? "" : "s");
}

/* Pretty-print a result value */
static void repl_print_result(FILE* fp, td_t* val, bool use_color) {
    if (!val) return;
    if (TD_IS_ERR(val)) {
        td_err_t code = TD_ERR_CODE(val);
        if (use_color) fprintf(fp, "\033[31m");
        fprintf(fp, "error: %s", td_err_str(code));
        if (use_color) fprintf(fp, "\033[0m");
        fprintf(fp, "\n");
        return;
    }

    switch (val->type) {
    case TD_TABLE:
        print_table(fp, val);
        break;
    case TD_LIST:
        print_vector(fp, val);
        fprintf(fp, "\n");
        break;
    default:
        if (td_is_atom(val)) {
            td_lang_print(fp, val);
            fprintf(fp, "\n");
        } else if (td_is_vec(val)) {
            print_typed_vector(fp, val);
            fprintf(fp, "\n");
        } else {
            td_lang_print(fp, val);
            fprintf(fp, "\n");
        }
        break;
    }
}

td_repl_t* td_repl_create(void) {
    td_t* block = td_alloc(sizeof(td_repl_t));
    if (!block) return NULL;
    td_repl_t* repl = (td_repl_t*)td_data(block);
    memset(repl, 0, sizeof(*repl));
    repl->_block = block;

    if (isatty(STDIN_FD)) {
        repl->term = td_term_create();
        if (repl->term)
            td_term_install_signals(repl->term);
    }
    return repl;
}

void td_repl_destroy(td_repl_t* repl) {
    if (!repl) return;
    if (repl->term) {
        td_term_destroy(repl->term);
    }
    td_free(repl->_block);
}

static void eval_and_print(const char* input, bool use_color, bool timeit) {
    struct timespec t0, t1;
    if (timeit) clock_gettime(CLOCK_MONOTONIC, &t0);

    td_term_clear_interrupt();
    td_t* result = td_eval_str(input);

    if (timeit) clock_gettime(CLOCK_MONOTONIC, &t1);

    if (td_term_interrupted()) {
        td_term_clear_interrupt();
        fprintf(stdout, "\n^C\n");
        fflush(stdout);
        if (result && !TD_IS_ERR(result)) td_release(result);
        return;
    }

    if (TD_IS_ERR(result)) {
        repl_print_result(stdout, result, use_color);
    } else if (result) {
        repl_print_result(stdout, result, use_color);
        fflush(stdout);
        td_release(result);
    }

    if (timeit) {
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0
                   + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (use_color) fprintf(stdout, "\033[90m");
        fprintf(stdout, "%.3f ms\n", ms);
        if (use_color) fprintf(stdout, "\033[0m");
        fflush(stdout);
    }
}

static const char* type_label(td_t* val) {
    if (!val) return "nil";
    switch (val->type) {
    case TD_ATOM_UNARY:  return "builtin/1";
    case TD_ATOM_BINARY: return "builtin/2";
    case TD_ATOM_VARY:   return "builtin/n";
    case TD_ATOM_LAMBDA: return "lambda";
    case TD_TABLE:       return "table";
    case TD_LIST:        return "list";
    default:
        if (td_is_vec(val)) return "vector";
        if (td_is_atom(val)) return "atom";
        return "?";
    }
}

static bool handle_command(td_repl_t* repl, const char* str, size_t len) {
    if (len == 0 || str[0] != ':') return false;

    const char* cmd = str + 1;
    size_t clen = len - 1;

    if ((clen == 1 && cmd[0] == '?') ||
        (clen == 4 && memcmp(cmd, "help", 4) == 0)) {
        fprintf(stdout,
            "Commands:\n"
            "  :help, :?     Show this help\n"
            "  :timeit, :t   Toggle expression timing\n"
            "  :env          List defined variables\n"
            "  :clear        Clear screen\n"
            "  :quit, :q     Exit REPL\n");
        return true;
    }

    if ((clen == 1 && cmd[0] == 't') ||
        (clen == 6 && memcmp(cmd, "timeit", 6) == 0)) {
        repl->timeit = !repl->timeit;
        fprintf(stdout, "timing %s\n", repl->timeit ? "on" : "off");
        return true;
    }

    if (clen == 3 && memcmp(cmd, "env", 3) == 0) {
        int64_t sym_ids[512];
        td_t* vals[512];
        int32_t n = td_env_list(sym_ids, vals, 512);
        for (int32_t i = 0; i < n; i++) {
            td_t* s = td_sym_str(sym_ids[i]);
            const char* name = s ? td_str_ptr(s) : "?";
            fprintf(stdout, "  %-20s %s\n", name, type_label(vals[i]));
        }
        fprintf(stdout, "(%d entries)\n", n);
        return true;
    }

    if (clen == 5 && memcmp(cmd, "clear", 5) == 0) {
        fprintf(stdout, "\033[2J\033[H");
        fflush(stdout);
        return true;
    }

    fprintf(stdout, "unknown command: %.*s\n", (int)len, str);
    fprintf(stdout, "type :? for help\n");
    return true;
}

static void run_interactive(td_repl_t* repl) {
    td_term_t* term = repl->term;

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

        /* REPL commands starting with ':' */
        if (str[0] == ':') {
            /* :q / :quit need special handling — they signal exit */
            size_t clen = len - 1;
            const char* cmd = str + 1;
            if ((clen == 1 && cmd[0] == 'q') ||
                (clen == 4 && memcmp(cmd, "quit", 4) == 0)) {
                td_release(line);
                break;
            }
            handle_command(repl, str, len);
            td_release(line);
            continue;
        }

        eval_and_print(str, true, repl->timeit);
        td_release(line);
    }
}

static void run_piped(td_repl_t* repl) {
    char buf[PIPE_BUF_SIZE];
    fprintf(stdout, "teide> ");
    fflush(stdout);

    while (fgets(buf, PIPE_BUF_SIZE, stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len == 0) {
            fprintf(stdout, "teide> ");
            fflush(stdout);
            continue;
        }
        if (strcmp(buf, "\\\\") == 0 || strcmp(buf, "exit") == 0) break;

        /* REPL commands */
        if (buf[0] == ':') {
            size_t clen = len - 1;
            const char* cmd = buf + 1;
            if ((clen == 1 && cmd[0] == 'q') ||
                (clen == 4 && memcmp(cmd, "quit", 4) == 0))
                break;
            handle_command(repl, buf, len);
            fprintf(stdout, "teide> ");
            fflush(stdout);
            continue;
        }

        eval_and_print(buf, false, repl->timeit);
        fprintf(stdout, "teide> ");
        fflush(stdout);
    }
}

void td_repl_run(td_repl_t* repl) {
    if (repl->term) {
        run_interactive(repl);
    } else {
        run_piped(repl);
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
    if (TD_IS_ERR(result)) {
        repl_print_result(stderr, result, false);
        return 1;
    } else if (result) {
        repl_print_result(stdout, result, false);
        td_release(result);
    }
    return 0;
}
