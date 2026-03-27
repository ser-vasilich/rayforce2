# Full-Featured REPL — Design Document

**Date:** 2026-03-27
**Goal:** Replace the minimal fgets-based REPL with a full-featured terminal interface — syntax highlighting, ghost text + dropdown autocomplete, multi-line editing, persistent history, bracket matching. Pure C17, zero dependencies.

## Overview

Port Rayforce's `app/term.c`/`repl.c` terminal layer to Teide, adapting types and allocator. Extend beyond Rayforce with popup suggestion menus and context-aware column name completion.

## Architecture

```
Keypress → term_getc() → update buffer → highlight + ghost text → redraw
                                       → Tab → collect completions → popup or accept
                                       → Enter → balanced? → eval or accumulate
                                       → Up/Down → history navigate
```

All rendering is ANSI escape codes written to fd 1 via `write()`. All input is raw bytes read from fd 0 via `read()`. Terminal is put in raw mode via `termios` (Unix) / `SetConsoleMode` (Windows). No libraries.

## Terminal Layer (`src/app/term.h`, `src/app/term.c`)

### Structures

```c
typedef struct td_hist {
    int64_t  fd;                    // history file fd
    char    *entries;               // mmap'd history data
    int64_t  size;                  // total size
    int64_t  pos;                   // write cursor
    int64_t  index;                 // navigation index
    int64_t  search_dir;            // reverse search direction
    int64_t  curr_saved;            // saved current input flag
    int64_t  curr_len;              // saved current input length
    char     curr[4096];            // saved current input
    int64_t  line_count;            // total lines for error traces
    int64_t *line_offsets;          // byte offset of each line start
    int64_t  line_offsets_cap;
} td_hist_t;

typedef struct {
    int64_t entry;                  // completion source index
    int64_t index;                  // position within source
    int64_t sbidx;                  // sub-index for history scan
} td_autocp_idx_t;

typedef struct td_term {
#if defined(_WIN32)
    HANDLE   h_stdin, h_stdout;
    DWORD    old_stdin_mode, old_stdout_mode;
#else
    struct termios oldattr, newattr;
#endif
    int32_t  input_len;
    char     input[8];              // escape sequence buffer
    int32_t  buf_len;
    int32_t  buf_pos;
    char     buf[4096];             // current line buffer
    int32_t  multiline_len;
    char     multiline_buf[4096];   // accumulated multi-line input
    td_autocp_idx_t autocp_idx;
    int32_t  autocp_buf_len;
    int32_t  autocp_buf_pos;
    char     autocp_buf[4096];      // saved buffer for cycling completions
    td_hist_t *hist;
    int32_t  term_width;
    int32_t  term_height;
    int32_t  prompt_len;
    int32_t  last_total_rows;
    int32_t  last_cursor_row;
    int64_t  last_input_line;
    // Popup state
    int32_t  popup_visible;         // 1 if dropdown is showing
    int32_t  popup_selected;        // selected index in popup
    int32_t  popup_count;           // number of candidates
    int32_t  popup_scroll;          // scroll offset for long lists
    char   **popup_items;           // candidate strings (borrowed pointers)
} td_term_t;
```

### Key Codes

Same defines as Rayforce: `KEYCODE_RETURN`, `KEYCODE_BACKSPACE`, `KEYCODE_TAB`, `KEYCODE_UP/DOWN/LEFT/RIGHT`, `KEYCODE_HOME/END`, `KEYCODE_CTRL_A` through `KEYCODE_CTRL_W`, `KEYCODE_ESCAPE`, bracket codes.

### Terminal Operations

- `td_term_create()` — save termios, enable raw mode, load history file
- `td_term_destroy()` — restore termios, save history, free resources
- `td_term_getc()` — `read(0, &c, 1)`, decode escape sequences (arrows, home/end, function keys)
- `td_term_get_size()` — `ioctl(TIOCGWINSZ)` / `GetConsoleScreenBufferInfo`
- Cursor helpers: `cursor_move_start/left/right/up/down`, `line_clear/clear_below`, `cursor_hide/show`
- `term_visual_width()` — count visible characters, skip ANSI escapes, handle UTF-8 multi-byte

## Syntax Highlighting

`term_redraw_into()` walks the input buffer character by character, emitting ANSI color codes:

| Token | Color | ANSI |
|---|---|---|
| Builtins/keywords | Green | `\033[32m` |
| Strings `"..."` | Yellow | `\033[33m` |
| Symbols `'name` | Cyan | `\033[36m` |
| Brackets `()[]{}` | Gray | `\033[90m` |
| Dict keys `key:` | Gray | `\033[90m` |
| Operators `+ - * /` as names | Light blue | `\033[94m` |
| Comments `; ...` | Dark gray | `\033[90m` |
| Numbers | Default | (no code) |

Keyword/builtin detection uses `td_env_lookup_prefix()` — a new function in `env.c` that prefix-matches against all registered function names. This is the same approach as Rayforce's `env_get_internal_function_name()`.

**Bracket matching:** When cursor is on a paren/bracket, scan for the matching pair (stack-based) and render both with bold/underline highlight.

## Autocomplete & Suggestions

### Completion Sources (checked in order)

1. **Environment** — all builtins + user variables from `td_env`. `td_env_lookup_prefix(prefix, len, results, max)` returns matching names.
2. **Keywords** — `fn`, `do`, `if`, `let`, `set`, `true`, `false`, `from`, `where`, `by`. Static sorted array.
3. **Column names** — when cursor is inside `(select {from: <table> ...})`, extract the table variable name, resolve it from env, get column names. Lightweight buffer scan to detect context.
4. **History** — scan history entries for words starting with prefix. Deduplicate against sources 1-3.

### Ghost Text (passive)

After every keystroke, find the top completion match. Render it grayed out (`\033[90m`) after the cursor position. The ghost text is part of the redraw — it does not modify the buffer.

- Tab with exactly 1 match: accept ghost text into buffer
- Any other keystroke: ghost text updates or disappears

### Dropdown Popup (active, on Tab with 2+ matches)

When Tab is pressed and there are multiple matches:

1. Save cursor position
2. Render a bordered box below the current line:
   ```
   ┌──────────────┐
   │ select       │  ← highlighted (selected)
   │ set          │
   │ sum          │
   │ scan         │
   └──────────────┘
   ```
3. Arrow up/down to navigate, scroll if list exceeds terminal height - 3
4. Enter to accept selected item into buffer
5. Escape or any non-navigation key to dismiss
6. On dismiss: clear popup area with `\033[J`, redraw prompt line

**Popup rendering:** Calculate box position from cursor row/col + terminal dimensions. If not enough space below, render above. Box width = max candidate length + 4 (borders + padding). Render with `write()` to fd 1 — move cursor down, print each line, move cursor back.

## Multi-line Input

After Enter, count unmatched open parens/brackets/braces in `buf + multiline_buf`:

- If balanced (count == 0): concatenate multiline_buf + buf, submit to eval
- If unbalanced (count > 0): append buf + newline to multiline_buf, show continuation prompt `  ··· `, reset buf

The continuation prompt is shorter than the main prompt, matching Rayforce's pattern.

## History

- **Storage:** `~/.teide_history`, mmap'd for fast access, append-only during session
- **Navigation:** Up/Down arrows. Current input saved before entering history, restored on Down past newest.
- **Persistence:** Save on `td_term_destroy()`, load on `td_term_create()`
- **Search:** Ctrl-R enters reverse incremental search mode — type to filter, show matching history entry with highlight
- **Multi-line entries:** Stored as single entries with embedded newlines

## Key Bindings

| Key | Action |
|---|---|
| Enter | Submit or continue (if unbalanced parens) |
| Tab | Accept ghost text / open dropdown popup |
| Shift-Tab | Previous completion in popup |
| Up / Ctrl-P | History previous (or popup up) |
| Down / Ctrl-N | History next (or popup down) |
| Left / Ctrl-B | Cursor left |
| Right / Ctrl-F | Cursor right (or accept ghost text if at end) |
| Ctrl-A / Home | Start of line |
| Ctrl-E / End | End of line |
| Ctrl-K | Kill to end of line |
| Ctrl-U | Kill entire line |
| Ctrl-W | Kill word backward |
| Ctrl-D | EOF (exit if buffer empty) |
| Ctrl-C | Cancel current input / dismiss popup |
| Ctrl-R | Reverse history search |
| Escape | Dismiss popup / cancel search |
| Backspace / Del | Delete char |

## File Layout

```
src/app/
├── term.h       # td_term_t, td_hist_t, key codes, cursor helpers
├── term.c       # Raw terminal, input, highlighting, autocomplete, popup
├── repl.h       # td_repl_t lifecycle
├── repl.c       # Read-eval-print loop, result formatting

src/lang/
├── repl.c       # main() binary — creates td_repl_t, runs it (updated)
```

**Changes to existing files:**
- `src/lang/env.c` — add `td_env_lookup_prefix()` for completion matching
- `src/lang/env.h` — declare `td_env_lookup_prefix()`
- `CMakeLists.txt` — add `src/app/*.c` to libteide sources

## API

```c
// term.h — terminal layer
td_term_t* td_term_create(void);
void       td_term_destroy(td_term_t* term);
int64_t    td_term_getc(td_term_t* term);
td_t*      td_term_read(td_term_t* term);    // full line editing, returns string or NULL
void       td_term_prompt(td_term_t* term);
void       td_term_redraw(td_term_t* term);

// repl.h — REPL lifecycle
td_repl_t* td_repl_create(void);
void       td_repl_destroy(td_repl_t* repl);
int        td_repl_run(td_repl_t* repl);
int        td_repl_run_file(td_repl_t* repl, const char* path);

// env.h — new completion API
int64_t    td_env_lookup_prefix(const char* prefix, int64_t len,
                                 const char** results, int64_t max_results);
```

No new dependencies. Pure C17, raw fd I/O, ANSI escape codes, termios/Console API.
