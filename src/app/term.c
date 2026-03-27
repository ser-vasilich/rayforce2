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
