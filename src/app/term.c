#include "app/term.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
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

    return term;
}

void td_term_destroy(td_term_t* term) {
    if (!term) return;
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

    return term;
}

void td_term_destroy(td_term_t* term) {
    if (!term) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->oldattr);
    td_free(term->_block);
}

int64_t td_term_getc(td_term_t* term) {
    int64_t sz = (int64_t)read(STDIN_FILENO,
                               term->input + (term->input_len++ % 8), 1);
    return sz;
}

#endif /* _WIN32 */

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
        if (key == -KEYCODE_UP || key == -KEYCODE_DOWN) {
            /* History — will be wired in Task 2.1 */
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
