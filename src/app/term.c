#include "app/term.h"
#include "lang/env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

/* ===== Cursor helpers ===== */

void td_cursor_move_start(void) { putchar('\r'); }
void td_cursor_move_left(int32_t n)  { if (n > 0) printf("\033[%dD", n); }
void td_cursor_move_right(int32_t n) { if (n > 0) printf("\033[%dC", n); }
void td_cursor_move_up(int32_t n)    { if (n > 0) printf("\033[%dA", n); }
void td_cursor_move_down(int32_t n)  { if (n > 0) printf("\033[%dB", n); }
void td_line_clear(void)       { printf("\r\033[K"); }
void td_line_clear_below(void) { printf("\033[J"); }
void td_cursor_hide(void)      { printf("\033[?25l"); }
void td_cursor_show(void)      { printf("\033[?25h"); }

/* ===== Terminal size ===== */

void td_term_get_size(td_term_t* term) {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(term->h_stdout, &csbi)) {
        term->term_width  = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        term->term_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        term->term_width  = 80;
        term->term_height = 24;
    }
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        term->term_width  = w.ws_col;
        term->term_height = w.ws_row;
    } else {
        term->term_width  = 80;
        term->term_height = 24;
    }
#endif
}

/* ===== Visual width ===== */

int32_t td_term_visual_width(const char* str, int32_t len) {
    int32_t width = 0;
    int32_t in_escape = 0;

    for (int32_t i = 0; i < len; i++) {
        if (str[i] == '\033') {
            in_escape = 1;
        } else if (in_escape) {
            if (str[i] == 'm' || str[i] == 'K' || str[i] == 'H' ||
                str[i] == 'A' || str[i] == 'B' || str[i] == 'C' ||
                str[i] == 'D') {
                in_escape = 0;
            }
        } else {
            unsigned char c = (unsigned char)str[i];
            if ((c & 0x80) == 0) {
                width++;
            } else if ((c & 0xE0) == 0xC0) {
                width++;
            } else if ((c & 0xF0) == 0xE0) {
                width++;
            } else if ((c & 0xF8) == 0xF0) {
                width += 2;
            }
        }
    }
    return width;
}

/* ===== Cursor positioning ===== */

void td_term_goto_position(td_term_t* term, int32_t from_pos, int32_t to_pos) {
    if (term->term_width <= 0)
        return;

    int32_t from_total = term->prompt_len + td_term_visual_width(term->buf, from_pos);
    int32_t from_row = from_total / term->term_width;
    int32_t from_col = from_total % term->term_width;

    int32_t to_total = term->prompt_len + td_term_visual_width(term->buf, to_pos);
    int32_t to_row = to_total / term->term_width;
    int32_t to_col = to_total % term->term_width;

    int32_t row_diff = to_row - from_row;
    int32_t col_diff = to_col - from_col;

    if (row_diff < 0) td_cursor_move_up(-row_diff);
    else if (row_diff > 0) td_cursor_move_down(row_diff);

    if (col_diff < 0) td_cursor_move_left(-col_diff);
    else if (col_diff > 0) td_cursor_move_right(col_diff);

    term->last_cursor_row = to_row;
}

/* ===== Terminal create / destroy ===== */

#if defined(_WIN32)

td_term_t* td_term_create(void) {
    td_t* block = td_alloc(sizeof(td_term_t));
    if (!block) return NULL;
    td_term_t* term = (td_term_t*)td_data(block);
    memset(term, 0, sizeof(*term));
    term->_block = block;

    term->h_stdin  = GetStdHandle(STD_INPUT_HANDLE);
    term->h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    SetConsoleOutputCP(CP_UTF8);

    GetConsoleMode(term->h_stdin, &term->old_stdin_mode);
    DWORD mode = term->old_stdin_mode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(term->h_stdin, mode);

    GetConsoleMode(term->h_stdout, &term->old_stdout_mode);
    SetConsoleMode(term->h_stdout, term->old_stdout_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    term->term_width  = 80;
    term->term_height = 24;
    term->last_total_rows = 1;
    td_term_get_size(term);
    td_hist_create(&term->hist);
    td_hist_load(&term->hist, NULL);

    return term;
}

void td_term_destroy(td_term_t* term) {
    if (!term) return;
    td_hist_save(&term->hist, NULL);
    td_hist_destroy(&term->hist);
    SetConsoleMode(term->h_stdin,  term->old_stdin_mode);
    SetConsoleMode(term->h_stdout, term->old_stdout_mode);
    td_free(term->_block);
}

int64_t td_term_getc(td_term_t* term) {
    char c;
    DWORD n;
    if (!ReadFile(term->h_stdin, &c, 1, &n, NULL))
        return -1;
    term->input[term->input_len++ % 8] = c;
    return (int64_t)n;
}

#else /* Unix */

td_term_t* td_term_create(void) {
    td_t* block = td_alloc(sizeof(td_term_t));
    if (!block) return NULL;
    td_term_t* term = (td_term_t*)td_data(block);
    memset(term, 0, sizeof(*term));
    term->_block = block;

    tcgetattr(STDIN_FILENO, &term->oldattr);
    term->newattr = term->oldattr;
    term->newattr.c_lflag &= ~(ICANON | ECHO | ISIG);
    term->newattr.c_cc[VMIN]  = 1;
    term->newattr.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->newattr);

    term->term_width  = 80;
    term->term_height = 24;
    term->last_total_rows = 1;
    td_term_get_size(term);
    td_hist_create(&term->hist);
    td_hist_load(&term->hist, NULL);

    return term;
}

void td_term_destroy(td_term_t* term) {
    if (!term) return;
    td_hist_save(&term->hist, NULL);
    td_hist_destroy(&term->hist);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->oldattr);
    td_free(term->_block);
}

int64_t td_term_getc(td_term_t* term) {
    int64_t sz = (int64_t)read(STDIN_FILENO,
                               term->input + (term->input_len++ % 8), 1);
    return sz;
}

#endif /* _WIN32 */

/* ===== History ===== */

void td_hist_create(td_hist_t* hist) {
    hist->capacity = HIST_DEFAULT_CAP;
    td_t* block = td_alloc((int64_t)(hist->capacity * (int64_t)sizeof(char*)));
    hist->entries = (char**)td_data(block);
    hist->count = 0;
    hist->index = 0;
    hist->curr_saved = 0;
    hist->curr_len = 0;
}

void td_hist_destroy(td_hist_t* hist) {
    if (!hist->entries) return;
    for (int32_t i = 0; i < hist->count; i++) {
        td_t* block = (td_t*)((char*)hist->entries[i] - 32);
        td_free(block);
    }
    td_t* block = (td_t*)((char*)hist->entries - 32);
    td_free(block);
    hist->entries = NULL;
    hist->count = 0;
}

void td_hist_add(td_hist_t* hist, const char* buf, int32_t len) {
    if (len <= 0) return;
    /* Skip if same as last entry */
    if (hist->count > 0) {
        const char* last = hist->entries[hist->count - 1];
        if ((int32_t)strlen(last) == len && memcmp(last, buf, (size_t)len) == 0)
            goto reset;
    }
    /* Grow if needed */
    if (hist->count >= hist->capacity) {
        int32_t new_cap = hist->capacity * 2;
        td_t* new_block = td_alloc((int64_t)(new_cap * (int64_t)sizeof(char*)));
        char** new_entries = (char**)td_data(new_block);
        memcpy(new_entries, hist->entries, (size_t)(hist->count) * sizeof(char*));
        td_t* old_block = (td_t*)((char*)hist->entries - 32);
        td_free(old_block);
        hist->entries = new_entries;
        hist->capacity = new_cap;
    }
    /* Allocate and copy the entry */
    td_t* entry_block = td_alloc((int64_t)(len + 1));
    char* entry = (char*)td_data(entry_block);
    memcpy(entry, buf, (size_t)len);
    entry[len] = '\0';
    hist->entries[hist->count++] = entry;

reset:
    hist->index = hist->count;
    hist->curr_saved = 0;
}

int32_t td_hist_prev(td_hist_t* hist, char* buf) {
    if (hist->count == 0 || hist->index <= 0) return -1;
    /* Save current input on first navigation */
    if (!hist->curr_saved) {
        hist->curr_saved = 1;
        /* caller's buf is the term buf — copy it */
    }
    hist->index--;
    const char* entry = hist->entries[hist->index];
    int32_t len = (int32_t)strlen(entry);
    memcpy(buf, entry, (size_t)len);
    return len;
}

int32_t td_hist_next(td_hist_t* hist, char* buf) {
    if (hist->index >= hist->count) return -1;
    hist->index++;
    if (hist->index >= hist->count) {
        /* Restore saved current input */
        if (hist->curr_saved) {
            memcpy(buf, hist->curr, (size_t)hist->curr_len);
            hist->curr_saved = 0;
            return hist->curr_len;
        }
        return 0; /* empty buffer */
    }
    const char* entry = hist->entries[hist->index];
    int32_t len = (int32_t)strlen(entry);
    memcpy(buf, entry, (size_t)len);
    return len;
}

/* ===== History search ===== */

int32_t td_hist_search(td_hist_t* hist, const char* needle, int32_t needle_len,
                       int32_t start_idx) {
    if (needle_len <= 0 || hist->count == 0) return -1;
    if (start_idx < 0) return -1;
    if (start_idx >= hist->count) start_idx = hist->count - 1;

    for (int32_t i = start_idx; i >= 0; i--) {
        const char* entry = hist->entries[i];
        int32_t elen = (int32_t)strlen(entry);
        if (elen < needle_len) continue;
        /* Substring search */
        for (int32_t j = 0; j <= elen - needle_len; j++) {
            if (memcmp(entry + j, needle, (size_t)needle_len) == 0)
                return i;
        }
    }
    return -1;
}

/* ===== History persistence ===== */

static void hist_build_path(char* out, int32_t out_size) {
    const char* home = getenv("HOME");
#if defined(_WIN32)
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = ".";
    snprintf(out, (size_t)out_size, "%s/%s", home, HIST_DEFAULT_PATH);
}

void td_hist_load(td_hist_t* hist, const char* path) {
    char pathbuf[1024];
    if (!path) {
        hist_build_path(pathbuf, (int32_t)sizeof(pathbuf));
        path = pathbuf;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return;
    }

    /* Read entire file into a temp buffer */
    int64_t fsize = st.st_size;
    if (fsize > TERM_BUF_SIZE * 100) fsize = TERM_BUF_SIZE * 100; /* sanity cap */
    td_t* fbuf_block = td_alloc(fsize + 1);
    if (!fbuf_block) { close(fd); return; }
    char* fbuf = (char*)td_data(fbuf_block);

    int64_t total = 0;
    while (total < fsize) {
        ssize_t n = read(fd, fbuf + total, (size_t)(fsize - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    /* Parse null-byte delimited entries */
    char* p = fbuf;
    char* end = fbuf + total;
    while (p < end) {
        char* entry_start = p;
        /* Find next null byte or end */
        while (p < end && *p != '\0') p++;
        int32_t len = (int32_t)(p - entry_start);
        if (len > 0)
            td_hist_add(hist, entry_start, len);
        if (p < end) p++; /* skip null delimiter */
    }

    td_free(fbuf_block);
}

void td_hist_save(td_hist_t* hist, const char* path) {
    char pathbuf[1024];
    if (!path) {
        hist_build_path(pathbuf, (int32_t)sizeof(pathbuf));
        path = pathbuf;
    }

    if (hist->count == 0) return;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;

    /* Only save last HIST_MAX_ENTRIES entries */
    int32_t start = 0;
    if (hist->count > HIST_MAX_ENTRIES)
        start = hist->count - HIST_MAX_ENTRIES;

    for (int32_t i = start; i < hist->count; i++) {
        const char* entry = hist->entries[i];
        int32_t len = (int32_t)strlen(entry);
        write(fd, entry, (size_t)len);
        if (i < hist->count - 1) {
            write(fd, "\0", 1);
        }
    }
    /* Write trailing null so load knows where last entry ends */
    write(fd, "\0", 1);

    close(fd);
}

/* ===== UTF-8 helpers ===== */

static int32_t find_prev_utf8(const char* buf, int32_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static int32_t find_next_utf8(const char* buf, int32_t pos, int32_t len) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)buf[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

/* ===== ANSI color constants ===== */

#define CLR_GREEN      "\033[32m"
#define CLR_YELLOW     "\033[33m"
#define CLR_CYAN       "\033[36m"
#define CLR_GRAY       "\033[90m"
#define CLR_LIGHT_BLUE "\033[94m"
#define CLR_RESET      "\033[0m"
#define CLR_BOLD       "\033[1m"
#define CLR_BACK_CYAN  "\033[46m"

/* ===== Syntax highlighting helpers ===== */

static int is_alphanum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '?' || c == '!';
}

static int is_op_char(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
           c == '<' || c == '>' || c == '=' || c == '!' || c == '&' || c == '|';
}

/* ===== Bracket matching ===== */

static int is_open_bracket(char c) {
    return c == '(' || c == '[' || c == '{';
}

static int is_close_bracket(char c) {
    return c == ')' || c == ']' || c == '}';
}

static char opposite_bracket(char c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    default:  return 0;
    }
}

static int in_string_at(const char* buf, int32_t pos) {
    int in_str = 0;
    for (int32_t i = 0; i < pos; i++) {
        if (buf[i] == '"' && (i == 0 || buf[i - 1] != '\\'))
            in_str = !in_str;
    }
    return in_str;
}

int32_t td_term_find_matching_paren(const char* buf, int32_t buf_len,
                                    int32_t cursor_pos) {
    if (cursor_pos < 0 || cursor_pos >= buf_len)
        return -1;

    char c = buf[cursor_pos];
    if (!is_open_bracket(c) && !is_close_bracket(c))
        return -1;

    if (in_string_at(buf, cursor_pos))
        return -1;

    char target = opposite_bracket(c);
    int depth = 0;

    if (is_open_bracket(c)) {
        /* Scan forward */
        for (int32_t i = cursor_pos; i < buf_len; i++) {
            if (in_string_at(buf, i)) continue;
            if (buf[i] == c) depth++;
            else if (buf[i] == target) {
                depth--;
                if (depth == 0) return i;
            }
        }
    } else {
        /* Scan backward */
        for (int32_t i = cursor_pos; i >= 0; i--) {
            if (in_string_at(buf, i)) continue;
            if (buf[i] == c) depth++;
            else if (buf[i] == target) {
                depth--;
                if (depth == 0) return i;
            }
        }
    }

    return -1;
}

/* Write highlighted buffer content into dst. Returns bytes written. */
static int32_t term_highlight_into(char* dst, int32_t dst_cap,
                                   const char* buf, int32_t buf_len,
                                   int32_t match_pos1, int32_t match_pos2) {
    int32_t n = 0;

#define HL_APPEND(s, slen) do { \
    if (n + (slen) < dst_cap) { memcpy(dst + n, (s), (size_t)(slen)); n += (slen); } \
} while (0)
#define HL_LIT(s) HL_APPEND((s), (int32_t)strlen(s))

    for (int32_t i = 0; i < buf_len; i++) {
        char c = buf[i];
        int colored = 0;

        switch (c) {
        case '(': case ')': case '[': case ']': case '{': case '}':
            if (i == match_pos1 || i == match_pos2) {
                HL_LIT(CLR_BACK_CYAN);
            } else {
                HL_LIT(CLR_GRAY);
            }
            HL_APPEND(&c, 1);
            HL_LIT(CLR_RESET);
            colored = 1;
            break;

        case ':':
            /* Dict key colon */
            HL_LIT(CLR_GRAY);
            HL_APPEND(&c, 1);
            HL_LIT(CLR_RESET);
            colored = 1;
            break;

        case '"': {
            /* String literal */
            int32_t j = i + 1;
            while (j < buf_len) {
                if (buf[j] == '"' && (j == i + 1 || buf[j - 1] != '\\')) {
                    j++;
                    break;
                }
                j++;
            }
            HL_LIT(CLR_YELLOW);
            HL_APPEND(buf + i, j - i);
            HL_LIT(CLR_RESET);
            i = j - 1;
            colored = 1;
            break;
        }

        case '\'': {
            /* Quoted symbol: 'name */
            int32_t j = i + 1;
            while (j < buf_len && is_alphanum(buf[j])) j++;
            if (j > i + 1) {
                HL_LIT(CLR_CYAN);
                HL_APPEND(buf + i, j - i);
                HL_LIT(CLR_RESET);
                i = j - 1;
                colored = 1;
            }
            break;
        }

        case ';': {
            /* Comment to end of line */
            int32_t j = i;
            while (j < buf_len && buf[j] != '\n') j++;
            HL_LIT(CLR_GRAY);
            HL_APPEND(buf + i, j - i);
            HL_LIT(CLR_RESET);
            i = j - 1;
            colored = 1;
            break;
        }

        default:
            /* Check for word at word boundary */
            if ((i == 0 || !is_alphanum(buf[i - 1])) && is_alphanum(c)) {
                int32_t j = i + 1;
                while (j < buf_len && is_alphanum(buf[j])) j++;
                int32_t wlen = j - i;

                const char* match = NULL;
                int64_t nmatches = td_env_lookup_prefix(buf + i, wlen,
                                                         &match, 1);
                if (nmatches == 1 && (int32_t)strlen(match) == wlen) {
                    HL_LIT(CLR_GREEN);
                    HL_APPEND(buf + i, wlen);
                    HL_LIT(CLR_RESET);
                    i = j - 1;
                    colored = 1;
                } else {
                    /* Not a builtin — emit plain */
                    HL_APPEND(buf + i, wlen);
                    i = j - 1;
                    colored = 1;
                }
            } else if (is_op_char(c)) {
                /* Check operator is standing alone (not part of a word) */
                int prev_alnum = (i > 0 && is_alphanum(buf[i - 1]));
                int next_alnum = (i + 1 < buf_len && is_alphanum(buf[i + 1]));
                if (!prev_alnum && !next_alnum) {
                    HL_LIT(CLR_LIGHT_BLUE);
                    HL_APPEND(&c, 1);
                    HL_LIT(CLR_RESET);
                    colored = 1;
                }
            }
            break;
        }

        if (!colored) {
            HL_APPEND(&c, 1);
        }
    }

#undef HL_LIT
#undef HL_APPEND

    return n;
}

/* ===== Ghost text (inline completion) ===== */

/* Find the start of the word at/before the cursor */
static int32_t find_word_start(const char* buf, int32_t pos) {
    int32_t i = pos;
    while (i > 0 && is_alphanum(buf[i - 1]))
        i--;
    return i;
}

/* Compute ghost text suggestion based on the word at cursor.
 * Sets term->ghost, ghost_len, ghost_word_start, ghost_word_len.
 * Ghost text is the REMAINING part of the best match (after the typed prefix).
 * Also populates term->comp_items/comp_count via collect_completions. */
static void td_term_update_ghost(td_term_t* term) {
    term->ghost_len = 0;

    /* Only show ghost at end of buffer or end of a word */
    if (term->buf_pos != term->buf_len)
        return;
    if (term->buf_len == 0)
        return;

    /* Extract word before cursor */
    int32_t ws = find_word_start(term->buf, term->buf_pos);
    int32_t wlen = term->buf_pos - ws;
    if (wlen <= 0)
        return;

    /* Collect completions from all sources */
    td_term_collect_completions(term, term->buf + ws, wlen);
    if (term->comp_count <= 0)
        return;

    /* Use the first (alphabetically) match */
    const char* match = term->comp_items[0];
    int32_t mlen = (int32_t)strlen(match);
    if (mlen <= wlen)
        return; /* already fully typed */

    /* Ghost = the remaining characters */
    int32_t remaining = mlen - wlen;
    if (remaining >= TERM_BUF_SIZE)
        remaining = TERM_BUF_SIZE - 1;
    memcpy(term->ghost, match + wlen, (size_t)remaining);
    term->ghost_len = remaining;
    term->ghost_word_start = ws;
    term->ghost_word_len = wlen;
}

/* Accept ghost text into the buffer */
static void td_term_accept_ghost(td_term_t* term) {
    if (term->ghost_len <= 0)
        return;
    if (term->buf_len + term->ghost_len >= TERM_BUF_SIZE)
        return;

    memcpy(term->buf + term->buf_len, term->ghost, (size_t)term->ghost_len);
    term->buf_len += term->ghost_len;
    term->buf_pos = term->buf_len;
    term->ghost_len = 0;
}

/* ===== Multi-source completion collection ===== */

/* Max completion candidates stored in td_term_t::comp_items */
#define COMP_MAX 256

static int comp_cmp_str(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

/* Check if name is already in results[0..count) */
static int comp_has(const char** results, int32_t count, const char* name) {
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(results[i], name) == 0) return 1;
    }
    return 0;
}

/* Try to extract a table variable name from a `(select {from: NAME ...` pattern
 * in the current buffer.  Returns the env value (a table) or NULL. */
static td_t* comp_find_from_table(const char* buf, int32_t buf_len) {
    /* Scan for "from:" followed by a name */
    for (int32_t i = 0; i + 5 < buf_len; i++) {
        if (memcmp(buf + i, "from:", 5) != 0) continue;
        int32_t j = i + 5;
        /* skip whitespace */
        while (j < buf_len && (buf[j] == ' ' || buf[j] == '\t')) j++;
        if (j >= buf_len || !is_alphanum(buf[j])) continue;
        int32_t start = j;
        while (j < buf_len && is_alphanum(buf[j])) j++;
        int32_t nlen = j - start;
        /* Intern the name and look it up in env */
        int64_t sym = td_sym_intern(buf + start, (size_t)nlen);
        if (sym < 0) continue;
        td_t* val = td_env_get(sym);
        if (val && val->type == TD_TABLE) return val;
    }
    return NULL;
}

void td_term_collect_completions(td_term_t* term, const char* prefix,
                                 int32_t prefix_len) {
    term->comp_count = 0;
    if (prefix_len <= 0) return;

    const char** out = term->comp_items;
    int32_t cap = COMP_MAX;
    int32_t n = 0;

    /* Source 1: env builtins + user variables (already sorted) */
    {
        const char* env_results[128];
        int64_t ec = td_env_lookup_prefix(prefix, (int64_t)prefix_len,
                                           env_results, 128);
        for (int64_t i = 0; i < ec && n < cap; i++) {
            out[n++] = env_results[i];
        }
    }

    /* Source 2: static keywords (s_keywords[] is in env.c and already scanned
     * by td_env_lookup_prefix, so nothing extra needed here — they are included
     * in source 1).  This is a no-op; the plan's "binary search on prefix" is
     * satisfied by env_lookup_prefix which already scans the keyword list. */

    /* Source 3: column names from a table referenced in the buffer */
    {
        td_t* tbl = comp_find_from_table(term->buf, term->buf_len);
        if (tbl) {
            int64_t ncols = td_table_ncols(tbl);
            for (int64_t ci = 0; ci < ncols && n < cap; ci++) {
                int64_t sym = td_table_col_name(tbl, ci);
                if (sym < 0) continue;
                td_t* s = td_sym_str(sym);
                if (!s) continue;
                const char* cname = td_str_ptr(s);
                if (!cname) continue;
                int64_t clen = (int64_t)strlen(cname);
                if (clen >= prefix_len &&
                    strncmp(cname, prefix, (size_t)prefix_len) == 0 &&
                    !comp_has(out, n, cname)) {
                    out[n++] = cname;
                }
            }
        }
    }

    /* Source 4: history words — tokenize history entries, match prefix.
     * We intern matching words via td_sym_intern to get stable null-terminated
     * pointers (sym table strings live until program exit). */
    {
        td_hist_t* hist = &term->hist;
        for (int32_t hi = hist->count - 1; hi >= 0 && n < cap; hi--) {
            const char* entry = hist->entries[hi];
            int32_t elen = (int32_t)strlen(entry);
            int32_t wi = 0;
            while (wi < elen && n < cap) {
                /* skip non-alphanum */
                while (wi < elen && !is_alphanum(entry[wi])) wi++;
                if (wi >= elen) break;
                int32_t ws = wi;
                while (wi < elen && is_alphanum(entry[wi])) wi++;
                int32_t wlen = wi - ws;
                /* Must be longer than what's typed (otherwise not a useful completion) */
                if (wlen > prefix_len &&
                    strncmp(entry + ws, prefix, (size_t)prefix_len) == 0) {
                    /* Length-aware dedup against existing candidates */
                    int dup = 0;
                    for (int32_t di = 0; di < n; di++) {
                        int32_t olen = (int32_t)strlen(out[di]);
                        if (olen == wlen &&
                            strncmp(out[di], entry + ws, (size_t)wlen) == 0) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup) {
                        /* Intern to get a stable null-terminated pointer */
                        int64_t sym = td_sym_intern(entry + ws, (size_t)wlen);
                        if (sym >= 0) {
                            td_t* s = td_sym_str(sym);
                            if (s) {
                                const char* word = td_str_ptr(s);
                                if (word) out[n++] = word;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Sort the merged results alphabetically */
    if (n > 1) {
        qsort((void*)out, (size_t)n, sizeof(const char*), comp_cmp_str);
    }

    term->comp_count = n;
}

/* ===== Dropdown popup menu ===== */

/* Maximum items visible in popup at once */
#define POPUP_MAX_VIS 10

void td_term_popup_show(td_term_t* term) {
    if (term->popup_count <= 0) return;

    /* Calculate max visible items */
    int32_t max_vis = term->term_height - 3;
    if (max_vis > POPUP_MAX_VIS) max_vis = POPUP_MAX_VIS;
    if (max_vis < 1) max_vis = 1;
    if (max_vis > term->popup_count) max_vis = term->popup_count;
    term->popup_max_visible = max_vis;

    /* Adjust scroll so selected item is visible */
    if (term->popup_selected < term->popup_scroll)
        term->popup_scroll = term->popup_selected;
    if (term->popup_selected >= term->popup_scroll + max_vis)
        term->popup_scroll = term->popup_selected - max_vis + 1;

    /* Calculate box width from longest item */
    int32_t max_item_len = 0;
    for (int32_t i = 0; i < term->popup_count; i++) {
        int32_t len = (int32_t)strlen(term->popup_items[i]);
        if (len > max_item_len) max_item_len = len;
    }
    int32_t box_width = max_item_len + 4; /* border + padding */
    if (box_width > term->term_width - 2) box_width = term->term_width - 2;
    int32_t inner_width = box_width - 2; /* inside the vertical bars */

    /* Save cursor position */
    printf("\033[s");

    /* Move cursor to line below the current input */
    /* We're at the cursor position in the input — move to end of content first,
     * then down one line, then to start */
    int32_t total_width = term->prompt_len + td_term_visual_width(term->buf, term->buf_len);
    int32_t ghost_vis = (term->ghost_len > 0 && term->buf_pos == term->buf_len)
                        ? term->ghost_len : 0;
    total_width += ghost_vis;
    int32_t content_rows = 1;
    if (term->term_width > 0)
        content_rows = (total_width + term->term_width - 1) / term->term_width;
    if (content_rows < 1) content_rows = 1;

    /* We need to position below the content. Move to start of line, then down. */
    printf("\r");
    /* Move down from current cursor row to content bottom + 1 */
    int32_t cursor_total = term->prompt_len + td_term_visual_width(term->buf, term->buf_pos);
    int32_t cursor_row = 0;
    if (term->term_width > 0)
        cursor_row = cursor_total / term->term_width;
    int32_t rows_down = content_rows - cursor_row;
    if (rows_down > 0)
        td_cursor_move_down(rows_down);

    /* Calculate horizontal offset: align with word start */
    int32_t word_col = term->prompt_len + td_term_visual_width(term->buf, term->ghost_word_start);
    int32_t popup_col = word_col;
    /* Clamp so popup doesn't go off right edge */
    if (popup_col + box_width > term->term_width)
        popup_col = term->term_width - box_width;
    if (popup_col < 0) popup_col = 0;

    /* Render top border */
    printf("\r");
    if (popup_col > 0) td_cursor_move_right(popup_col);
    printf("\xe2\x94\x8c"); /* U+250C: box drawings light down and right */
    for (int32_t i = 0; i < inner_width; i++)
        printf("\xe2\x94\x80"); /* U+2500: box drawings light horizontal */
    printf("\xe2\x94\x90"); /* U+2510: box drawings light down and left */

    /* Render visible items */
    for (int32_t i = 0; i < max_vis; i++) {
        int32_t idx = term->popup_scroll + i;
        const char* item = term->popup_items[idx];
        int32_t ilen = (int32_t)strlen(item);
        int32_t pad = inner_width - 1 - ilen; /* 1 for leading space */
        if (pad < 0) { ilen = inner_width - 1; pad = 0; }

        printf("\n\r");
        if (popup_col > 0) td_cursor_move_right(popup_col);
        printf("\xe2\x94\x82"); /* U+2502: box drawings light vertical */
        if (idx == term->popup_selected)
            printf("\033[7m"); /* reverse video */
        printf(" ");
        fwrite(item, 1, (size_t)ilen, stdout);
        for (int32_t p = 0; p < pad; p++) putchar(' ');
        if (idx == term->popup_selected)
            printf("\033[0m");
        printf("\xe2\x94\x82");
    }

    /* Render bottom border */
    printf("\n\r");
    if (popup_col > 0) td_cursor_move_right(popup_col);
    printf("\xe2\x94\x94"); /* U+2514: box drawings light up and right */
    for (int32_t i = 0; i < inner_width; i++)
        printf("\xe2\x94\x80");
    printf("\xe2\x94\x98"); /* U+2518: box drawings light up and left */

    /* Restore cursor position */
    printf("\033[u");
    fflush(stdout);
}

void td_term_popup_hide(td_term_t* term) {
    if (!term->popup_visible) return;
    term->popup_visible = 0;

    /* Save cursor, move below content, clear everything below, restore cursor */
    printf("\033[s");

    /* Move to end of content area */
    int32_t total_width = term->prompt_len + td_term_visual_width(term->buf, term->buf_len);
    int32_t ghost_vis = (term->ghost_len > 0 && term->buf_pos == term->buf_len)
                        ? term->ghost_len : 0;
    total_width += ghost_vis;
    int32_t content_rows = 1;
    if (term->term_width > 0)
        content_rows = (total_width + term->term_width - 1) / term->term_width;
    if (content_rows < 1) content_rows = 1;

    int32_t cursor_total = term->prompt_len + td_term_visual_width(term->buf, term->buf_pos);
    int32_t cursor_row = 0;
    if (term->term_width > 0)
        cursor_row = cursor_total / term->term_width;
    int32_t rows_down = content_rows - cursor_row;
    if (rows_down > 0)
        td_cursor_move_down(rows_down);

    /* Clear from here to end of screen */
    printf("\n\033[J");

    /* Restore cursor */
    printf("\033[u");
    fflush(stdout);
}

/* ===== Multi-line input ===== */

int32_t td_term_count_unmatched(td_term_t* term) {
    int32_t depth = 0;
    int32_t in_string = 0;

    /* Scan multiline_buf first */
    for (int32_t i = 0; i < term->multiline_len; i++) {
        char c = term->multiline_buf[i];
        if (in_string) {
            if (c == '\\' && i + 1 < term->multiline_len) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
    }

    /* Then scan current buf */
    for (int32_t i = 0; i < term->buf_len; i++) {
        char c = term->buf[i];
        if (in_string) {
            if (c == '\\' && i + 1 < term->buf_len) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
    }

    return depth;
}

/* ===== Prompt ===== */

#define PROMPT_STR "teide> "
#define PROMPT_LEN 7
#define CONT_PROMPT_STR "  \xc2\xb7\xc2\xb7\xc2\xb7 "  /* "  ··· " in UTF-8 */
#define CONT_PROMPT_LEN 8  /* 2 spaces + 3x2-byte dots + 1 space = 8 bytes */
#define CONT_PROMPT_VISUAL 8  /* 2 + 3*1 + 1 space + 2 trailing = visual width matches */

void td_term_prompt(td_term_t* term) {
    write(STDOUT_FILENO, PROMPT_STR, PROMPT_LEN);
    term->prompt_len = PROMPT_LEN;
}

void td_term_continuation_prompt(td_term_t* term) {
    write(STDOUT_FILENO, CONT_PROMPT_STR, CONT_PROMPT_LEN);
    term->prompt_len = 6; /* visual width: "  ··· " = 2+3+1 = 6 chars */
}

/* ===== Redraw ===== */

void td_term_redraw(td_term_t* term) {
    int32_t total_width;

    /* Recompute ghost text on every redraw */
    td_term_update_ghost(term);

    td_cursor_hide();
    td_term_get_size(term);

    /* Move to start of first line */
    printf("\r");
    if (term->last_total_rows > 1) {
        for (int32_t i = 1; i < term->last_total_rows; i++) {
            td_cursor_move_up(1);
            printf("\r");
        }
    }

    /* Clear from cursor to end of screen */
    printf("\033[J");

    /* Write prompt + highlighted buffer into temp buf, then single write */
    {
        /* 8K should be enough for 4K buf + ANSI escapes */
        char hlbuf[8192];
        int32_t hlen = 0;
        if (term->multiline_len > 0) {
            memcpy(hlbuf, CONT_PROMPT_STR, CONT_PROMPT_LEN);
            hlen = CONT_PROMPT_LEN;
        } else {
            memcpy(hlbuf, PROMPT_STR, PROMPT_LEN);
            hlen = PROMPT_LEN;
        }
        if (term->buf_len > 0) {
            /* Find bracket match at cursor */
            int32_t match_pos1 = -1, match_pos2 = -1;
            int32_t cursor = term->buf_pos;
            /* Check char at cursor, or char before cursor */
            if (cursor < term->buf_len) {
                int32_t m = td_term_find_matching_paren(term->buf, term->buf_len, cursor);
                if (m >= 0) { match_pos1 = cursor; match_pos2 = m; }
            }
            if (match_pos1 < 0 && cursor > 0) {
                int32_t m = td_term_find_matching_paren(term->buf, term->buf_len, cursor - 1);
                if (m >= 0) { match_pos1 = cursor - 1; match_pos2 = m; }
            }
            hlen += term_highlight_into(hlbuf + hlen,
                                        (int32_t)sizeof(hlbuf) - hlen,
                                        term->buf, term->buf_len,
                                        match_pos1, match_pos2);
        }
        /* Append ghost text in gray after buffer content */
        if (term->ghost_len > 0 && term->buf_pos == term->buf_len) {
            const char* g_pre = CLR_GRAY;
            const char* g_post = CLR_RESET;
            int32_t g_pre_len = (int32_t)strlen(g_pre);
            int32_t g_post_len = (int32_t)strlen(g_post);
            if (hlen + g_pre_len + term->ghost_len + g_post_len < (int32_t)sizeof(hlbuf)) {
                memcpy(hlbuf + hlen, g_pre, (size_t)g_pre_len);
                hlen += g_pre_len;
                memcpy(hlbuf + hlen, term->ghost, (size_t)term->ghost_len);
                hlen += term->ghost_len;
                memcpy(hlbuf + hlen, g_post, (size_t)g_post_len);
                hlen += g_post_len;
            }
        }
        write(STDOUT_FILENO, hlbuf, (size_t)hlen);
    }

    /* Track rows used — include ghost text width for row calculation */
    int32_t ghost_vis = (term->ghost_len > 0 && term->buf_pos == term->buf_len)
                        ? term->ghost_len : 0;
    total_width = term->prompt_len + td_term_visual_width(term->buf, term->buf_len) + ghost_vis;
    if (term->term_width > 0) {
        term->last_total_rows = (total_width + term->term_width - 1) / term->term_width;
        if (term->last_total_rows == 0)
            term->last_total_rows = 1;
    }

    /* Position cursor at buf_pos */
    td_term_goto_position(term, term->buf_len, term->buf_pos);

    /* Redraw popup if visible */
    if (term->popup_visible)
        td_term_popup_show(term);

    td_cursor_show();
    fflush(stdout);
}

/* ===== Search mode redraw ===== */

#define SEARCH_PROMPT     "(search) `"
#define SEARCH_PROMPT_LEN 10
#define SEARCH_HIGHLIGHT  "\033[7m"
#define SEARCH_RESET      "\033[0m"

static void td_term_search_redraw(td_term_t* term) {
    td_cursor_hide();

    /* Move to start */
    printf("\r");
    if (term->last_total_rows > 1) {
        for (int32_t i = 1; i < term->last_total_rows; i++) {
            td_cursor_move_up(1);
            printf("\r");
        }
    }
    printf("\033[J");

    /* Write search prompt: (search) `query`: matched_entry */
    write(STDOUT_FILENO, SEARCH_PROMPT, SEARCH_PROMPT_LEN);
    if (term->search_len > 0)
        write(STDOUT_FILENO, term->search_buf, (size_t)term->search_len);
    write(STDOUT_FILENO, "': ", 3);

    /* Show matching entry with highlighted match substring */
    if (term->search_match_idx >= 0) {
        const char* entry = term->hist.entries[term->search_match_idx];
        int32_t elen = (int32_t)strlen(entry);

        /* Find match position within entry */
        int32_t match_pos = -1;
        if (term->search_len > 0) {
            for (int32_t j = 0; j <= elen - term->search_len; j++) {
                if (memcmp(entry + j, term->search_buf, (size_t)term->search_len) == 0) {
                    match_pos = j;
                    break;
                }
            }
        }

        if (match_pos >= 0) {
            /* Before match */
            if (match_pos > 0)
                write(STDOUT_FILENO, entry, (size_t)match_pos);
            /* Highlighted match */
            write(STDOUT_FILENO, SEARCH_HIGHLIGHT, 4);
            write(STDOUT_FILENO, entry + match_pos, (size_t)term->search_len);
            write(STDOUT_FILENO, SEARCH_RESET, 4);
            /* After match */
            int32_t after = match_pos + term->search_len;
            if (after < elen)
                write(STDOUT_FILENO, entry + after, (size_t)(elen - after));
        } else {
            write(STDOUT_FILENO, entry, (size_t)elen);
        }
    }

    /* Update row tracking */
    int32_t total_vis = SEARCH_PROMPT_LEN + term->search_len + 3;
    if (term->search_match_idx >= 0)
        total_vis += (int32_t)strlen(term->hist.entries[term->search_match_idx]);
    if (term->term_width > 0) {
        term->last_total_rows = (total_vis + term->term_width - 1) / term->term_width;
        if (term->last_total_rows == 0) term->last_total_rows = 1;
    }

    /* Position cursor right after the search query closing tick */
    int32_t cursor_col = SEARCH_PROMPT_LEN + term->search_len;
    int32_t end_col = total_vis;
    int32_t diff = end_col - cursor_col;
    if (diff > 0) td_cursor_move_left(diff);

    td_cursor_show();
    fflush(stdout);
}

/* ===== Line editing ===== */

td_t* td_term_read(td_term_t* term) {
    td_term_prompt(term);
    fflush(stdout);
    term->buf_len = 0;
    term->buf_pos = 0;
    term->multiline_len = 0;

    for (;;) {
        int64_t sz = td_term_getc(term);
        if (sz <= 0) return NULL;

        char key = term->input[0];

        if (key == KEYCODE_ESCAPE) {
            /* Read escape sequence */
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
                switch (seq[1]) {
                    case 'A': key = -KEYCODE_UP;    goto handle; /* Up */
                    case 'B': key = -KEYCODE_DOWN;  goto handle; /* Down */
                    case 'C': key = -KEYCODE_RIGHT; goto handle; /* Right */
                    case 'D': key = -KEYCODE_LEFT;  goto handle; /* Left */
                    case 'H': key = -KEYCODE_HOME;  goto handle; /* Home */
                    case 'F': key = -KEYCODE_END;   goto handle; /* End */
                    case '3': /* Delete key: \033[3~ */
                        if (read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') {
                            if (term->buf_pos < term->buf_len) {
                                int32_t next = find_next_utf8(term->buf, term->buf_pos, term->buf_len);
                                int32_t bytes = next - term->buf_pos;
                                memmove(term->buf + term->buf_pos,
                                        term->buf + term->buf_pos + bytes,
                                        (size_t)(term->buf_len - term->buf_pos - bytes));
                                term->buf_len -= bytes;
                                td_term_redraw(term);
                            }
                        }
                        continue;
                    default: continue;
                }
            } else if (seq[0] == 'O') {
                if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
                switch (seq[1]) {
                    case 'H': key = -KEYCODE_HOME; goto handle;
                    case 'F': key = -KEYCODE_END;  goto handle;
                    default: continue;
                }
            }
            /* Unrecognized escape — if popup is visible, dismiss it */
            if (term->popup_visible) {
                td_term_popup_hide(term);
                td_term_redraw(term);
            }
            continue;
        }

    handle:
        /* ---- Popup navigation mode ---- */
        if (term->popup_visible) {
            if (key == -KEYCODE_UP || key == KEYCODE_CTRL_P) {
                if (term->popup_selected > 0) {
                    term->popup_selected--;
                    td_term_popup_show(term);
                }
                continue;
            }
            if (key == -KEYCODE_DOWN || key == KEYCODE_CTRL_N) {
                if (term->popup_selected < term->popup_count - 1) {
                    term->popup_selected++;
                    td_term_popup_show(term);
                }
                continue;
            }
            if (key == KEYCODE_TAB) {
                /* Cycle to next item */
                term->popup_selected++;
                if (term->popup_selected >= term->popup_count)
                    term->popup_selected = 0;
                td_term_popup_show(term);
                continue;
            }
            if (key == KEYCODE_RETURN) {
                /* Accept selected item into buffer */
                if (term->popup_selected >= 0 && term->popup_selected < term->popup_count) {
                    const char* item = term->popup_items[term->popup_selected];
                    int32_t ilen = (int32_t)strlen(item);
                    /* Replace the word being completed */
                    int32_t ws = term->ghost_word_start;
                    int32_t wlen = term->ghost_word_len;
                    int32_t tail = term->buf_len - (ws + wlen);
                    if (ws + ilen + tail < TERM_BUF_SIZE) {
                        memmove(term->buf + ws + ilen,
                                term->buf + ws + wlen,
                                (size_t)tail);
                        memcpy(term->buf + ws, item, (size_t)ilen);
                        term->buf_len = ws + ilen + tail;
                        term->buf_pos = ws + ilen;
                    }
                }
                td_term_popup_hide(term);
                term->ghost_len = 0;
                td_term_redraw(term);
                continue;
            }
            /* Any other key: hide popup, fall through to normal handling */
            td_term_popup_hide(term);
            /* Fall through */
        }

        /* Arrow keys are encoded as negative to distinguish from printable chars */
        if (key == -KEYCODE_UP || key == KEYCODE_CTRL_P) {
            /* Save current input before first navigation */
            if (!term->hist.curr_saved) {
                memcpy(term->hist.curr, term->buf, (size_t)term->buf_len);
                term->hist.curr_len = term->buf_len;
            }
            int32_t len = td_hist_prev(&term->hist, term->buf);
            if (len >= 0) {
                term->buf_len = len;
                term->buf_pos = len;
                td_term_redraw(term);
            }
            continue;
        }

        if (key == -KEYCODE_DOWN || key == KEYCODE_CTRL_N) {
            int32_t len = td_hist_next(&term->hist, term->buf);
            if (len >= 0) {
                term->buf_len = len;
                term->buf_pos = len;
                td_term_redraw(term);
            }
            continue;
        }

        if (key == -KEYCODE_LEFT) {
            if (term->buf_pos > 0) {
                int32_t prev = find_prev_utf8(term->buf, term->buf_pos);
                td_term_goto_position(term, term->buf_pos, prev);
                term->buf_pos = prev;
                fflush(stdout);
            }
            continue;
        }

        if (key == -KEYCODE_RIGHT) {
            if (term->buf_pos < term->buf_len) {
                int32_t next = find_next_utf8(term->buf, term->buf_pos, term->buf_len);
                td_term_goto_position(term, term->buf_pos, next);
                term->buf_pos = next;
                fflush(stdout);
            } else if (term->ghost_len > 0) {
                /* Accept ghost text at end of line */
                td_term_accept_ghost(term);
                td_term_redraw(term);
            }
            continue;
        }

        if (key == -KEYCODE_HOME || key == KEYCODE_CTRL_A) {
            td_term_goto_position(term, term->buf_pos, 0);
            term->buf_pos = 0;
            fflush(stdout);
            continue;
        }

        if (key == -KEYCODE_END || key == KEYCODE_CTRL_E) {
            td_term_goto_position(term, term->buf_pos, term->buf_len);
            term->buf_pos = term->buf_len;
            fflush(stdout);
            continue;
        }

        switch (key) {
        case KEYCODE_RETURN: {
            term->buf[term->buf_len] = '\0';
            int32_t unmatched = td_term_count_unmatched(term);
            if (unmatched > 0) {
                /* Append buf + newline to multiline_buf, show continuation */
                if (term->multiline_len + term->buf_len + 1 < TERM_BUF_SIZE) {
                    memcpy(term->multiline_buf + term->multiline_len,
                           term->buf, (size_t)term->buf_len);
                    term->multiline_len += term->buf_len;
                    term->multiline_buf[term->multiline_len++] = '\n';
                }
                term->buf_len = 0;
                term->buf_pos = 0;
                putchar('\n');
                fflush(stdout);
                td_term_continuation_prompt(term);
                fflush(stdout);
                continue;
            }
            putchar('\n');
            fflush(stdout);
            if (term->multiline_len > 0) {
                /* Concatenate multiline_buf + buf */
                if (term->multiline_len + term->buf_len < TERM_BUF_SIZE) {
                    memcpy(term->multiline_buf + term->multiline_len,
                           term->buf, (size_t)term->buf_len);
                    term->multiline_len += term->buf_len;
                }
                td_hist_add(&term->hist, term->multiline_buf, term->multiline_len);
                td_t* result = td_str(term->multiline_buf, (size_t)term->multiline_len);
                term->multiline_len = 0;
                return result;
            }
            td_hist_add(&term->hist, term->buf, term->buf_len);
            if (term->buf_len == 0) return td_str("", 0);
            return td_str(term->buf, (size_t)term->buf_len);
        }

        case KEYCODE_CTRL_D: {
            if (term->buf_len == 0) {
                putchar('\n');
                fflush(stdout);
                return NULL;
            }
            /* Delete char at cursor (like Delete key) */
            if (term->buf_pos < term->buf_len) {
                int32_t next = find_next_utf8(term->buf, term->buf_pos, term->buf_len);
                int32_t bytes = next - term->buf_pos;
                memmove(term->buf + term->buf_pos,
                        term->buf + term->buf_pos + bytes,
                        (size_t)(term->buf_len - term->buf_pos - bytes));
                term->buf_len -= bytes;
                td_term_redraw(term);
            }
            continue;
        }

        case KEYCODE_CTRL_C: {
            term->buf_len = 0;
            term->buf_pos = 0;
            term->multiline_len = 0;
            write(STDOUT_FILENO, "^C\n", 3);
            td_term_prompt(term);
            fflush(stdout);
            continue;
        }

        case KEYCODE_BACKSPACE:
        case KEYCODE_DELETE: {
            if (term->buf_pos > 0) {
                int32_t prev = find_prev_utf8(term->buf, term->buf_pos);
                int32_t bytes = term->buf_pos - prev;
                memmove(term->buf + prev,
                        term->buf + term->buf_pos,
                        (size_t)(term->buf_len - term->buf_pos));
                term->buf_len -= bytes;
                term->buf_pos = prev;
                td_term_redraw(term);
            }
            continue;
        }

        case KEYCODE_CTRL_K: {
            term->buf_len = term->buf_pos;
            td_term_redraw(term);
            continue;
        }

        case KEYCODE_CTRL_U: {
            term->buf_len = 0;
            term->buf_pos = 0;
            td_term_redraw(term);
            continue;
        }

        case KEYCODE_CTRL_W: {
            if (term->buf_pos > 0) {
                int32_t end = term->buf_pos;
                /* Skip non-alphanum */
                while (term->buf_pos > 0 && !((term->buf[term->buf_pos - 1] >= 'a' && term->buf[term->buf_pos - 1] <= 'z') ||
                       (term->buf[term->buf_pos - 1] >= 'A' && term->buf[term->buf_pos - 1] <= 'Z') ||
                       (term->buf[term->buf_pos - 1] >= '0' && term->buf[term->buf_pos - 1] <= '9') ||
                       term->buf[term->buf_pos - 1] == '_' || term->buf[term->buf_pos - 1] == '-'))
                    term->buf_pos--;
                /* Skip alphanum */
                while (term->buf_pos > 0 && ((term->buf[term->buf_pos - 1] >= 'a' && term->buf[term->buf_pos - 1] <= 'z') ||
                       (term->buf[term->buf_pos - 1] >= 'A' && term->buf[term->buf_pos - 1] <= 'Z') ||
                       (term->buf[term->buf_pos - 1] >= '0' && term->buf[term->buf_pos - 1] <= '9') ||
                       term->buf[term->buf_pos - 1] == '_' || term->buf[term->buf_pos - 1] == '-'))
                    term->buf_pos--;
                memmove(term->buf + term->buf_pos,
                        term->buf + end,
                        (size_t)(term->buf_len - end));
                term->buf_len -= (end - term->buf_pos);
                td_term_redraw(term);
            }
            continue;
        }

        case KEYCODE_CTRL_R: {
            /* Enter reverse incremental search mode */
            term->search_mode = 1;
            term->search_len = 0;
            term->search_match_idx = -1;
            td_term_search_redraw(term);

            for (;;) {
                int64_t ssz = td_term_getc(term);
                if (ssz <= 0) {
                    term->search_mode = 0;
                    td_term_redraw(term);
                    break;
                }

                char skey = term->input[0];

                if (skey == KEYCODE_RETURN) {
                    /* Accept match into buffer */
                    if (term->search_match_idx >= 0) {
                        const char* entry = term->hist.entries[term->search_match_idx];
                        int32_t len = (int32_t)strlen(entry);
                        memcpy(term->buf, entry, (size_t)len);
                        term->buf_len = len;
                        term->buf_pos = len;
                    }
                    term->search_mode = 0;
                    putchar('\n');
                    fflush(stdout);
                    term->buf[term->buf_len] = '\0';
                    td_hist_add(&term->hist, term->buf, term->buf_len);
                    if (term->buf_len == 0) return td_str("", 0);
                    return td_str(term->buf, (size_t)term->buf_len);
                }

                if (skey == KEYCODE_ESCAPE) {
                    /* Read and discard potential escape sequence */
                    /* Use non-blocking check: set a short timeout */
                    /* For simplicity, just cancel search */
                    term->search_mode = 0;
                    td_term_redraw(term);
                    break;
                }

                if (skey == KEYCODE_CTRL_C) {
                    /* Cancel search, clear buffer */
                    term->search_mode = 0;
                    term->buf_len = 0;
                    term->buf_pos = 0;
                    write(STDOUT_FILENO, "^C\n", 3);
                    td_term_prompt(term);
                    fflush(stdout);
                    break;
                }

                if (skey == KEYCODE_CTRL_R) {
                    /* Search further back */
                    if (term->search_match_idx > 0 && term->search_len > 0) {
                        int32_t idx = td_hist_search(&term->hist,
                                                     term->search_buf,
                                                     term->search_len,
                                                     term->search_match_idx - 1);
                        if (idx >= 0)
                            term->search_match_idx = idx;
                    }
                    td_term_search_redraw(term);
                    continue;
                }

                if (skey == KEYCODE_BACKSPACE || skey == KEYCODE_DELETE) {
                    /* Remove last char from search query */
                    if (term->search_len > 0) {
                        term->search_len--;
                        if (term->search_len > 0) {
                            term->search_match_idx = td_hist_search(
                                &term->hist, term->search_buf,
                                term->search_len, term->hist.count - 1);
                        } else {
                            term->search_match_idx = -1;
                        }
                    }
                    td_term_search_redraw(term);
                    continue;
                }

                /* Printable character — append to search and search */
                if ((unsigned char)skey >= 0x20 && term->search_len < 255) {
                    term->search_buf[term->search_len++] = skey;
                    /* Search from current match position or from end */
                    int32_t start = (term->search_match_idx >= 0)
                                    ? term->search_match_idx
                                    : term->hist.count - 1;
                    term->search_match_idx = td_hist_search(
                        &term->hist, term->search_buf,
                        term->search_len, start);
                    td_term_search_redraw(term);
                    continue;
                }
            }
            continue;
        }

        case KEYCODE_TAB: {
            if (term->ghost_len > 0 && term->comp_count == 1) {
                /* Single match — accept ghost text directly */
                td_term_accept_ghost(term);
                td_term_update_ghost(term);
                td_term_redraw(term);
            } else if (term->comp_count >= 2) {
                /* Multiple matches — show popup */
                term->popup_visible = 1;
                term->popup_items = term->comp_items;
                term->popup_count = term->comp_count;
                term->popup_selected = 0;
                term->popup_scroll = 0;
                td_term_popup_show(term);
            } else if (term->ghost_len > 0) {
                /* Accept whatever ghost we have */
                td_term_accept_ghost(term);
                td_term_update_ghost(term);
                td_term_redraw(term);
            }
            continue;
        }

        default: {
            /* Printable character insert */
            if ((unsigned char)key >= 0x20 && term->buf_len < TERM_BUF_SIZE - 1) {
                if (term->buf_pos < term->buf_len) {
                    memmove(term->buf + term->buf_pos + 1,
                            term->buf + term->buf_pos,
                            (size_t)(term->buf_len - term->buf_pos));
                }
                term->buf[term->buf_pos] = key;
                term->buf_pos++;
                term->buf_len++;

                /* Always do full redraw to show ghost text */
                td_term_redraw(term);
            }
            continue;
        }
        }
    }
}
