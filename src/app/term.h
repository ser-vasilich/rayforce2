#ifndef TD_TERM_H
#define TD_TERM_H

#include <teide/td.h>

#if defined(_WIN32)
#include <windows.h>
#define KEYCODE_RETURN '\r'
#else
#include <termios.h>
#define KEYCODE_RETURN '\n'
#endif

#define KEYCODE_BACKSPACE '\b'
#define KEYCODE_DELETE    0x7f
#define KEYCODE_TAB       '\t'
#define KEYCODE_UP        'A'
#define KEYCODE_DOWN      'B'
#define KEYCODE_LEFT      'D'
#define KEYCODE_RIGHT     'C'
#define KEYCODE_HOME      'H'
#define KEYCODE_END       'F'
#define KEYCODE_ESCAPE    0x1b
#define KEYCODE_CTRL_A    0x01
#define KEYCODE_CTRL_B    0x02
#define KEYCODE_CTRL_C    0x03
#define KEYCODE_CTRL_D    0x04
#define KEYCODE_CTRL_E    0x05
#define KEYCODE_CTRL_F    0x06
#define KEYCODE_CTRL_K    0x0b
#define KEYCODE_CTRL_N    0x0e
#define KEYCODE_CTRL_P    0x10
#define KEYCODE_CTRL_R    0x12
#define KEYCODE_CTRL_U    0x15
#define KEYCODE_CTRL_W    0x17
#define KEYCODE_LPAREN    '('
#define KEYCODE_RPAREN    ')'
#define KEYCODE_LCURLY    '{'
#define KEYCODE_RCURLY    '}'
#define KEYCODE_SQUOTE    '\''
#define KEYCODE_DQUOTE    '"'
#define KEYCODE_LBRACKET  '['
#define KEYCODE_RBRACKET  ']'

#define TERM_BUF_SIZE 4096
#define HIST_DEFAULT_CAP 256

typedef struct td_hist {
    char**   entries;
    int32_t  count;
    int32_t  capacity;
    int32_t  index;
    int32_t  curr_saved;
    char     curr[TERM_BUF_SIZE];
    int32_t  curr_len;
} td_hist_t;

typedef struct td_term {
    td_t*    _block;
#if defined(_WIN32)
    HANDLE   h_stdin;
    HANDLE   h_stdout;
    DWORD    old_stdin_mode;
    DWORD    old_stdout_mode;
#else
    struct termios oldattr;
    struct termios newattr;
#endif
    int32_t  input_len;
    char     input[8];
    int32_t  buf_len;
    int32_t  buf_pos;
    char     buf[TERM_BUF_SIZE];
    int32_t  term_width;
    int32_t  term_height;
    int32_t  prompt_len;
    int32_t  last_total_rows;
    int32_t  last_cursor_row;
    td_hist_t hist;
    int32_t  search_mode;
    char     search_buf[256];
    int32_t  search_len;
    int32_t  search_match_idx;
    /* Ghost text (inline completion suggestion) */
    char     ghost[TERM_BUF_SIZE];
    int32_t  ghost_len;
    int32_t  ghost_word_start; /* position in buf where the completed word starts */
    int32_t  ghost_word_len;   /* length of the prefix that was matched */
    /* Multi-source completion candidates */
    const char* comp_items[256];  /* borrowed pointers — valid until next collect */
    int32_t     comp_count;
    /* Dropdown popup menu state */
    int32_t     popup_visible;
    int32_t     popup_selected;
    int32_t     popup_count;
    int32_t     popup_scroll;
    const char** popup_items;     /* borrowed from comp_items */
    int32_t     popup_max_visible;
    /* Multi-line input state */
    char        multiline_buf[TERM_BUF_SIZE];
    int32_t     multiline_len;
} td_term_t;

td_term_t* td_term_create(void);
void       td_term_destroy(td_term_t* term);
int64_t    td_term_getc(td_term_t* term);
void       td_term_get_size(td_term_t* term);

void td_cursor_move_start(void);
void td_cursor_move_left(int32_t n);
void td_cursor_move_right(int32_t n);
void td_cursor_move_up(int32_t n);
void td_cursor_move_down(int32_t n);
void td_line_clear(void);
void td_line_clear_below(void);
void td_cursor_hide(void);
void td_cursor_show(void);

int32_t td_term_visual_width(const char* str, int32_t len);
void    td_term_goto_position(td_term_t* term, int32_t from_pos, int32_t to_pos);

td_t*  td_term_read(td_term_t* term);
void   td_term_redraw(td_term_t* term);
void   td_term_prompt(td_term_t* term);

void    td_hist_create(td_hist_t* hist);
void    td_hist_destroy(td_hist_t* hist);
void    td_hist_add(td_hist_t* hist, const char* buf, int32_t len);
int32_t td_hist_prev(td_hist_t* hist, char* buf);
int32_t td_hist_next(td_hist_t* hist, char* buf);
void    td_hist_load(td_hist_t* hist, const char* path);
void    td_hist_save(td_hist_t* hist, const char* path);
int32_t td_hist_search(td_hist_t* hist, const char* needle, int32_t needle_len,
                       int32_t start_idx);

int32_t td_term_find_matching_paren(const char* buf, int32_t buf_len,
                                    int32_t cursor_pos);

/* Collect completion candidates from all sources (env, keywords, columns,
 * history words).  Stores results in term->comp_items / comp_count.
 * prefix/prefix_len is the word being completed. */
void td_term_collect_completions(td_term_t* term, const char* prefix,
                                 int32_t prefix_len);

/* Dropdown popup autocomplete menu */
void td_term_popup_show(td_term_t* term);
void td_term_popup_hide(td_term_t* term);

/* Multi-line input: count unmatched opening brackets in multiline_buf + buf */
int32_t td_term_count_unmatched(td_term_t* term);
void    td_term_continuation_prompt(td_term_t* term);

#define HIST_MAX_ENTRIES 1000
#define HIST_DEFAULT_PATH ".teide_history"

#endif /* TD_TERM_H */
