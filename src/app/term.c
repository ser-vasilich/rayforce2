#include "app/term.h"
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

/* ===== Prompt ===== */

#define PROMPT_STR "teide> "
#define PROMPT_LEN 7

void td_term_prompt(td_term_t* term) {
    write(STDOUT_FILENO, PROMPT_STR, PROMPT_LEN);
    term->prompt_len = PROMPT_LEN;
}

/* ===== Redraw ===== */

void td_term_redraw(td_term_t* term) {
    int32_t total_width;

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

    /* Write prompt + buffer */
    write(STDOUT_FILENO, PROMPT_STR, PROMPT_LEN);
    if (term->buf_len > 0)
        write(STDOUT_FILENO, term->buf, (size_t)term->buf_len);

    /* Track rows used */
    total_width = term->prompt_len + td_term_visual_width(term->buf, term->buf_len);
    if (term->term_width > 0) {
        term->last_total_rows = (total_width + term->term_width - 1) / term->term_width;
        if (term->last_total_rows == 0)
            term->last_total_rows = 1;
    }

    /* Position cursor at buf_pos */
    td_term_goto_position(term, term->buf_len, term->buf_pos);

    td_cursor_show();
    fflush(stdout);
}

/* ===== Line editing ===== */

td_t* td_term_read(td_term_t* term) {
    td_term_prompt(term);
    fflush(stdout);
    term->buf_len = 0;
    term->buf_pos = 0;

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
            continue;
        }

    handle:
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
            putchar('\n');
            fflush(stdout);
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

        case KEYCODE_TAB:
            /* Autocomplete — will be wired in Phase 4 */
            continue;

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

                if (term->buf_pos == term->buf_len) {
                    /* Append at end — just write the char */
                    write(STDOUT_FILENO, &key, 1);
                    fflush(stdout);
                } else {
                    td_term_redraw(term);
                }
            }
            continue;
        }
        }
    }
}
