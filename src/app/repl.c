#if !defined(_WIN32)
#define _POSIX_C_SOURCE 199309L
#endif

#include "app/repl.h"
#include "app/term.h"
#include "lang/env.h"
#include "lang/eval.h"
#include "mem/heap.h"
#include "ops/ops.h"
#include "table/sym.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define STDIN_FD 0
#else
#include <unistd.h>
#define STDIN_FD STDIN_FILENO
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

/* Cross-platform monotonic time in nanoseconds */
static int64_t time_now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (int64_t)((double)cnt.QuadPart / (double)freq.QuadPart * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

#ifndef RAYFORCE_VERSION
#define RAYFORCE_VERSION "dev"
#endif
#ifndef RAYFORCE_GIT_COMMIT
#define RAYFORCE_GIT_COMMIT "unknown"
#endif
#ifndef RAYFORCE_BUILD_DATE
#define RAYFORCE_BUILD_DATE "unknown"
#endif

static void get_cpu_name(char* buf, size_t sz) {
#if defined(__linux__)
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char* p = strchr(line, ':');
                if (p) {
                    p++;
                    while (*p == ' ') p++;
                    size_t len = strlen(p);
                    if (len > 0 && p[len - 1] == '\n') p[len - 1] = '\0';
                    snprintf(buf, sz, "%s", p);
                    fclose(f);
                    return;
                }
            }
        }
        fclose(f);
    }
    snprintf(buf, sz, "unknown");
#elif defined(__APPLE__)
    size_t len = sz;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, NULL, 0) != 0)
        snprintf(buf, sz, "unknown");
#elif defined(_WIN32)
    snprintf(buf, sz, "unknown");
#else
    snprintf(buf, sz, "unknown");
#endif
}

static int64_t get_total_mem_mb(void) {
#if defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_sz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_sz > 0)
        return (int64_t)pages * (int64_t)page_sz / (1024 * 1024);
    return 0;
#elif defined(__APPLE__)
    int64_t mem = 0;
    size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    return mem / (1024 * 1024);
#elif defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    return (int64_t)(ms.ullTotalPhys / (1024 * 1024));
#else
    return 0;
#endif
}

static void print_banner(void) {
    char cpu[256];
    get_cpu_name(cpu, sizeof(cpu));
    int64_t mem_mb = get_total_mem_mb();
    int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN);

    fprintf(stdout,
        "Rayforce %s (%s, %s)\n"
        "%s | %d cores | %" PRId64 " MB RAM\n"
        "Apache-2.0 license | type :? for help\n\n",
        RAYFORCE_VERSION, RAYFORCE_GIT_COMMIT, RAYFORCE_BUILD_DATE,
        cpu, ncores, mem_mb);
}

#define PIPE_BUF_SIZE 4096
#define MAX_PRINT_ROWS  40
#define MAX_PRINT_ELEMS 20
#define MAX_COL_WIDTH   40

/* ===== Pretty-print helpers ===== */

/* Check if vector element at idx is null */
static bool vec_is_null(ray_t* vec, int64_t idx) {
    if (!(vec->attrs & RAY_ATTR_HAS_NULLS)) return false;
    const uint8_t* bm;
    if (vec->attrs & RAY_ATTR_NULLMAP_EXT) {
        ray_t* ext = vec->ext_nullmap;
        if (!ext) return false;
        bm = (const uint8_t*)ray_data(ext);
    } else {
        if (idx >= 128) return false;
        bm = vec->nullmap;
    }
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

/* Format a single vector element into buf, return chars written */
static int fmt_vec_elem(ray_t* vec, int64_t idx, char* out, int max) {
    if (vec_is_null(vec, idx))
        return snprintf(out, (size_t)max, "null");

    switch (vec->type) {
    case RAY_BOOL: {
        bool* data = (bool*)ray_data(vec);
        return snprintf(out, (size_t)max, "%s", data[idx] ? "true" : "false");
    }
    case RAY_I64: {
        int64_t* data = (int64_t*)ray_data(vec);
        return snprintf(out, (size_t)max, "%" PRId64, data[idx]);
    }
    case RAY_F64: {
        double* data = (double*)ray_data(vec);
        return snprintf(out, (size_t)max, "%g", data[idx]);
    }
    case RAY_I32: {
        int32_t* data = (int32_t*)ray_data(vec);
        return snprintf(out, (size_t)max, "%d", data[idx]);
    }
    case RAY_I16: {
        int16_t* data = (int16_t*)ray_data(vec);
        return snprintf(out, (size_t)max, "%d", (int)data[idx]);
    }
    case RAY_SYM: {
        uint8_t esz = (uint8_t)RAY_SYM_ELEM(vec->attrs);
        const uint8_t* base = (const uint8_t*)ray_data(vec);
        int64_t sym_id = 0;
        memcpy(&sym_id, base + idx * esz, esz);
        ray_t* s = ray_sym_str(sym_id);
        if (s) return snprintf(out, (size_t)max, "%.*s",
                               (int)ray_str_len(s), ray_str_ptr(s));
        return snprintf(out, (size_t)max, "?sym%" PRId64, sym_id);
    }
    case RAY_STR: {
        size_t slen = 0;
        const char* s = ray_str_vec_get(vec, idx, &slen);
        if (s) return snprintf(out, (size_t)max, "%.*s", (int)slen, s);
        return snprintf(out, (size_t)max, "null");
    }
    case RAY_DATE: {
        int64_t* data = (int64_t*)ray_data(vec);
        int64_t d = data[idx];
        /* Rayforce date = days since 2000-01-01 */
        return snprintf(out, (size_t)max, "%" PRId64, d);
    }
    default:
        return snprintf(out, (size_t)max, "?");
    }
}

/* Print a vector in [1 2 3 ...] format */
static void print_vector(FILE* fp, ray_t* val) {
    int64_t len = ray_len(val);
    ray_t** elems = (ray_t**)ray_data(val);
    bool is_dict = (val->attrs & RAY_ATTR_DICT) != 0;

    if (is_dict) {
        int64_t npairs = len / 2;
        int64_t show = npairs > MAX_PRINT_ELEMS ? MAX_PRINT_ELEMS : npairs;
        fprintf(fp, "{");
        for (int64_t i = 0; i < show; i++) {
            if (i > 0) fprintf(fp, " ");
            ray_lang_print(fp, elems[i * 2]);
            fprintf(fp, ": ");
            ray_lang_print(fp, elems[i * 2 + 1]);
        }
        if (npairs > show) fprintf(fp, " ...(+%" PRId64 ")", npairs - show);
        fprintf(fp, "}");
        return;
    }

    int64_t show = len > MAX_PRINT_ELEMS ? MAX_PRINT_ELEMS : len;
    fprintf(fp, "[");
    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fprintf(fp, " ");
        ray_lang_print(fp, elems[i]);
    }
    if (len > show) fprintf(fp, " ...(+%" PRId64 ")", len - show);
    fprintf(fp, "]");
}

/* Print a typed vector (RAY_I64, RAY_F64, RAY_SYM, RAY_STR, etc.) */
static void print_typed_vector(FILE* fp, ray_t* vec) {
    int64_t len = ray_len(vec);
    char elem_buf[256];
    fprintf(fp, "[");
    int64_t show = len > MAX_PRINT_ELEMS ? MAX_PRINT_ELEMS : len;
    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fprintf(fp, " ");
        fmt_vec_elem(vec, i, elem_buf, (int)sizeof(elem_buf));
        fprintf(fp, "%s", elem_buf);
    }
    if (len > show) fprintf(fp, " ...(+%" PRId64 ")", len - show);
    fprintf(fp, "]");
}

/* Print a table in columnar format */
static void print_table(FILE* fp, ray_t* tbl) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    if (ncols == 0) {
        fprintf(fp, "(empty table)\n");
        return;
    }

    /* Collect column names and vectors */
    ray_t* cols[256];
    const char* names[256];
    int name_lens[256];
    int col_widths[256];
    int actual_ncols = ncols > 256 ? 256 : (int)ncols;

    for (int c = 0; c < actual_ncols; c++) {
        cols[c] = ray_table_get_col_idx(tbl, c);
        int64_t name_id = ray_table_col_name(tbl, c);
        ray_t* ns = ray_sym_str(name_id);
        if (ns) {
            names[c] = ray_str_ptr(ns);
            name_lens[c] = (int)ray_str_len(ns);
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
        fprintf(fp, "... %" PRId64 " more rows\n", nrows - sample);

    fprintf(fp, "(%" PRId64 " row%s)\n", nrows, nrows == 1 ? "" : "s");
}

/* Pretty-print a result value */
static void repl_print_result(FILE* fp, ray_t* val, bool use_color) {
    if (!val) return;
    if (RAY_IS_ERR(val)) {
        ray_err_t code = RAY_ERR_CODE(val);
        if (use_color) fprintf(fp, "\033[31m");
        fprintf(fp, "error: %s", ray_err_str(code));
        if (use_color) fprintf(fp, "\033[0m");
        fprintf(fp, "\n");
        return;
    }

    switch (val->type) {
    case RAY_TABLE:
        print_table(fp, val);
        break;
    case RAY_LIST:
        print_vector(fp, val);
        fprintf(fp, "\n");
        break;
    default:
        if (ray_is_atom(val)) {
            ray_lang_print(fp, val);
            fprintf(fp, "\n");
        } else if (ray_is_vec(val)) {
            print_typed_vector(fp, val);
            fprintf(fp, "\n");
        } else {
            ray_lang_print(fp, val);
            fprintf(fp, "\n");
        }
        break;
    }
}

ray_repl_t* ray_repl_create(void) {
    ray_t* block = ray_alloc(sizeof(ray_repl_t));
    if (!block) return NULL;
    ray_repl_t* repl = (ray_repl_t*)ray_data(block);
    memset(repl, 0, sizeof(*repl));
    repl->_block = block;

    if (isatty(STDIN_FD)) {
        repl->term = ray_term_create();
        if (repl->term)
            ray_term_install_signals(repl->term);
    }
    return repl;
}

void ray_repl_destroy(ray_repl_t* repl) {
    if (!repl) return;
    if (repl->term) {
        ray_term_destroy(repl->term);
    }
    ray_free(repl->_block);
}

static void eval_and_print(ray_term_t* term, const char* input,
                           bool use_color, bool timeit) {
    int64_t t0 = 0, t1 = 0;
    if (timeit) t0 = time_now_ns();

    ray_term_clear_interrupt();
    ray_eval_clear_interrupt();
    if (term) ray_term_eval_begin(term);
    ray_t* result = ray_eval_str(input);
    if (term) ray_term_eval_end(term);

    /* Materialize lazy handles before printing */
    if (ray_is_lazy(result))
        result = ray_lazy_materialize(result);

    if (timeit) t1 = time_now_ns();

    if (ray_term_interrupted()) {
        ray_term_clear_interrupt();
        ray_eval_clear_interrupt();
        fprintf(stdout, "\n^C\n");
        fflush(stdout);
        if (result && !RAY_IS_ERR(result)) ray_release(result);
        return;
    }

    if (RAY_IS_ERR(result)) {
        repl_print_result(stdout, result, use_color);
        fflush(stdout);
    } else if (result) {
        repl_print_result(stdout, result, use_color);
        fflush(stdout);
        ray_release(result);
    }

    if (timeit) {
        double ms = (double)(t1 - t0) / 1e6;
        if (use_color) fprintf(stdout, "\033[90m");
        fprintf(stdout, "%.3f ms\n", ms);
        if (use_color) fprintf(stdout, "\033[0m");
        fflush(stdout);
    }
}

static const char* type_label(ray_t* val) {
    if (!val) return "nil";
    switch (val->type) {
    case RAY_UNARY:  return "builtin/1";
    case RAY_BINARY: return "builtin/2";
    case RAY_VARY:   return "builtin/n";
    case RAY_LAMBDA: return "lambda";
    case RAY_TABLE:       return "table";
    case RAY_LIST:        return "list";
    default:
        if (ray_is_vec(val)) return "vector";
        if (ray_is_atom(val)) return "atom";
        return "?";
    }
}

static bool cmd_match(const char* cmd, size_t clen,
                      const char* name, size_t nlen,
                      const char** arg, size_t* arg_len) {
    if (clen < nlen) return false;
    if (memcmp(cmd, name, nlen) != 0) return false;
    if (clen == nlen) { *arg = NULL; *arg_len = 0; return true; }
    if (cmd[nlen] != ' ') return false;
    /* Skip spaces after command name */
    size_t off = nlen + 1;
    while (off < clen && cmd[off] == ' ') off++;
    *arg = cmd + off;
    *arg_len = clen - off;
    return true;
}

static bool handle_command(ray_repl_t* repl, const char* str, size_t len) {
    if (len == 0 || str[0] != ':') return false;

    const char* cmd = str + 1;
    size_t clen = len - 1;
    const char* arg = NULL;
    size_t arg_len = 0;

    if (cmd_match(cmd, clen, "?", 1, &arg, &arg_len) ||
        cmd_match(cmd, clen, "help", 4, &arg, &arg_len)) {
        fprintf(stdout,
            "Commands:\n"
            "  :help, :?       Show this help\n"
            "  :t, :timeit     Toggle expression timing\n"
            "  :t <expr>       Time a single expression\n"
            "  :env            List defined variables\n"
            "  :clear          Clear screen\n"
            "  :quit, :q       Exit REPL\n");
        return true;
    }

    if (cmd_match(cmd, clen, "t", 1, &arg, &arg_len) ||
        cmd_match(cmd, clen, "timeit", 6, &arg, &arg_len)) {
        if (arg && arg_len > 0) {
            /* :t <expr> — time a single expression */
            eval_and_print(repl->term, arg, repl->term != NULL, true);
        } else {
            repl->timeit = !repl->timeit;
            fprintf(stdout, "timing %s\n", repl->timeit ? "on" : "off");
        }
        return true;
    }

    if (cmd_match(cmd, clen, "env", 3, &arg, &arg_len)) {
        int64_t sym_ids[512];
        ray_t* vals[512];
        int32_t n = ray_env_list(sym_ids, vals, 512);
        for (int32_t i = 0; i < n; i++) {
            ray_t* s = ray_sym_str(sym_ids[i]);
            const char* name = s ? ray_str_ptr(s) : "?";
            fprintf(stdout, "  %-20s %s\n", name, type_label(vals[i]));
        }
        fprintf(stdout, "(%d entries)\n", n);
        return true;
    }

    if (cmd_match(cmd, clen, "clear", 5, &arg, &arg_len)) {
        fprintf(stdout, "\033[2J\033[H");
        fflush(stdout);
        return true;
    }

    fprintf(stdout, "unknown command: %.*s\n", (int)len, str);
    fprintf(stdout, "type :? for help\n");
    return true;
}

static void run_interactive(ray_repl_t* repl) {
    ray_term_t* term = repl->term;
    print_banner();

    for (;;) {
        ray_t* line = ray_term_read(term);
        if (!line) break; /* EOF / Ctrl-D */

        const char* str = ray_str_ptr(line);
        size_t len = ray_str_len(line);

        if (len == 0) {
            ray_release(line);
            continue;
        }

        /* Exit commands */
        if ((len == 2 && memcmp(str, "\\\\", 2) == 0) ||
            (len == 4 && memcmp(str, "exit", 4) == 0)) {
            ray_release(line);
            break;
        }

        /* REPL commands starting with ':' */
        if (str[0] == ':') {
            /* :q / :quit need special handling — they signal exit */
            size_t clen = len - 1;
            const char* cmd = str + 1;
            if ((clen == 1 && cmd[0] == 'q') ||
                (clen == 4 && memcmp(cmd, "quit", 4) == 0)) {
                ray_release(line);
                break;
            }
            handle_command(repl, str, len);
            ray_release(line);
            continue;
        }

        eval_and_print(repl->term, str, true, repl->timeit);
        ray_release(line);
    }
}

/* Parse state for bracket_delta, preserved across chunk boundaries. */
typedef struct {
    int in_string;   /* inside a "..." literal */
    int in_comment;  /* inside a ;-comment (until newline) */
} bracket_state_t;

/* Compute the net bracket delta for a string, skipping string literals
 * and ;-comments.  Result can be negative (more closers than openers).
 * If `state` is non-NULL, string/comment parse state is carried across
 * calls (required for chunked overflow recovery in piped mode). */
static int32_t bracket_delta_s(const char* s, size_t len,
                               bracket_state_t* state) {
    int32_t depth = 0;
    int in_string  = state ? state->in_string  : 0;
    int in_comment = state ? state->in_comment : 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_comment) {
            if (c == '\n') in_comment = 0;
            continue;
        }
        if (in_string) {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == ';') {
            in_comment = 1;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
    }
    if (state) {
        state->in_string  = in_string;
        state->in_comment = in_comment;
    }
    return depth;
}

/* Convenience wrapper with no persistent state (single-buffer calls). */
static int32_t bracket_delta(const char* s, size_t len) {
    return bracket_delta_s(s, len, NULL);
}

/* Count unmatched opening brackets (clamped >= 0). */
static int32_t count_unmatched(const char* s, size_t len) {
    int32_t d = bracket_delta(s, len);
    return d > 0 ? d : 0;
}

static void run_piped(ray_repl_t* repl) {
    char line[PIPE_BUF_SIZE];
    char accum[PIPE_BUF_SIZE];
    size_t accum_len = 0;
    bool mid_line = false; /* true when fgets returned without a newline */

    while (fgets(line, PIPE_BUF_SIZE, stdin)) {
        size_t len = strlen(line);
        bool had_newline = (len > 0 && line[len - 1] == '\n');
        if (had_newline) line[--len] = '\0';

        /* Skip empty lines when no accumulation in progress */
        if (len == 0 && accum_len == 0 && !mid_line)
            continue;

        if (accum_len == 0 && !mid_line) {
            if (strcmp(line, "\\\\") == 0 || strcmp(line, "exit") == 0) break;

            /* REPL commands */
            if (line[0] == ':') {
                size_t clen = len - 1;
                const char* cmd = line + 1;
                if ((clen == 1 && cmd[0] == 'q') ||
                    (clen == 4 && memcmp(cmd, "quit", 4) == 0))
                    break;
                handle_command(repl, line, len);
                continue;
            }
        }

        /* Append chunk to accumulator */
        size_t needed = accum_len + len + (accum_len > 0 && !mid_line ? 1 : 0);
        if (needed < PIPE_BUF_SIZE) {
            if (accum_len > 0 && !mid_line) accum[accum_len++] = '\n';
            memcpy(accum + accum_len, line, len);
            accum_len += len;
            accum[accum_len] = '\0';
        } else {
            fprintf(stderr, "error: input too large (max %d bytes)\n",
                    PIPE_BUF_SIZE - 1);
            /* Compute bracket depth of the accumulated text plus the
               current chunk that triggered the overflow.  Use stateful
               parsing so string/comment context survives chunk splits. */
            bracket_state_t bs = {0, 0};
            int32_t depth = bracket_delta_s(accum, accum_len, &bs);
            /* A logical newline separates accum from line (the normal
               append path inserts one at line 519).  Reset comment state
               so a trailing ;-comment in accum doesn't bleed into line. */
            if (accum_len > 0 && !mid_line) bs.in_comment = 0;
            depth += bracket_delta_s(line, len, &bs);
            accum_len = 0;
            mid_line = false;
            /* Drain the rest of the oversized physical line. */
            while (!had_newline) {
                if (!fgets(line, PIPE_BUF_SIZE, stdin)) break;
                len = strlen(line);
                had_newline = (len > 0 && line[len - 1] == '\n');
                if (had_newline) {
                    line[--len] = '\0';
                    bs.in_comment = 0; /* newline ends ; comment */
                }
                depth += bracket_delta_s(line, len, &bs);
            }
            /* If we were inside an unmatched multi-line form, keep
               draining lines until brackets balance or EOF. */
            while (depth > 0) {
                if (!fgets(line, PIPE_BUF_SIZE, stdin)) break;
                len = strlen(line);
                had_newline = (len > 0 && line[len - 1] == '\n');
                if (had_newline) {
                    line[--len] = '\0';
                    bs.in_comment = 0; /* newline ends ; comment */
                }
                depth += bracket_delta_s(line, len, &bs);
                /* If fgets didn't see a newline, drain the rest of
                   this physical line before counting brackets. */
                while (!had_newline) {
                    if (!fgets(line, PIPE_BUF_SIZE, stdin)) break;
                    len = strlen(line);
                    had_newline = (len > 0 && line[len - 1] == '\n');
                    if (had_newline) {
                        line[--len] = '\0';
                        bs.in_comment = 0; /* newline ends ; comment */
                    }
                    depth += bracket_delta_s(line, len, &bs);
                }
            }
            continue;
        }

        /* Track whether we're in the middle of a physical line */
        if (!had_newline) {
            mid_line = true;
            continue; /* keep accumulating until the line ends */
        }
        mid_line = false;

        /* Evaluate when brackets are balanced */
        if (count_unmatched(accum, accum_len) == 0) {
            eval_and_print(NULL, accum, false, repl->timeit);
            accum_len = 0;
        }
    }

    /* Evaluate any remaining accumulated input */
    if (accum_len > 0) {
        accum[accum_len] = '\0';
        eval_and_print(NULL, accum, false, repl->timeit);
    }
}

void ray_repl_run(ray_repl_t* repl) {
    if (repl->term) {
        run_interactive(repl);
    } else {
        run_piped(repl);
    }
}

int ray_repl_run_file(const char* path) {
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

    ray_t* block = ray_alloc((int64_t)flen + 1);
    if (!block) {
        fclose(f);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    char* buf = (char*)ray_data(block);
    size_t nread = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    buf[nread] = '\0';

    ray_t* result = ray_eval_str(buf);
    ray_release(block);
    /* Materialize lazy handles before printing */
    if (ray_is_lazy(result))
        result = ray_lazy_materialize(result);
    if (RAY_IS_ERR(result)) {
        repl_print_result(stderr, result, false);
        return 1;
    } else if (result) {
        repl_print_result(stdout, result, false);
        ray_release(result);
    }
    return 0;
}
