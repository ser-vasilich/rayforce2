# Full-Featured REPL — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the minimal fgets-based REPL with a full-featured terminal interface — raw mode, syntax highlighting, ghost text + dropdown autocomplete, multi-line editing, persistent history, bracket matching. Pure C17, zero dependencies.

**Architecture:** Port Rayforce's `app/term.c`/`repl.c` terminal layer to Teide, adapting types and allocator. Extend with popup suggestion menus and context-aware completion. All rendering via ANSI escape codes on raw fd I/O.

**Tech Stack:** C17, termios (Unix) / Console API (Windows), ANSI escape codes, Teide allocator (`td_alloc`/`td_free`).

**Design doc:** `docs/plans/2026-03-27-repl-design.md`

**Reference code:**
- Rayforce terminal: `/home/hetoku/data/work/rayforce/app/term.h`, `term.c`
- Rayforce REPL: `/home/hetoku/data/work/rayforce/app/repl.h`, `repl.c`

**Build & test commands:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
./build/teide_repl                    # interactive smoke test
echo '(+ 1 2)' | ./build/teide_repl  # piped mode smoke test
```

---

## Phase 1: Raw Terminal & Basic Input

### Task 1.1: Terminal struct and raw mode

**Files:** Create `src/app/term.h`, `src/app/term.c`; Modify `CMakeLists.txt`

- [x] Create `src/app/term.h` with:
  - Key code defines: `KEYCODE_RETURN`, `KEYCODE_BACKSPACE`, `KEYCODE_DELETE`, `KEYCODE_TAB`, `KEYCODE_UP/DOWN/LEFT/RIGHT`, `KEYCODE_HOME/END`, `KEYCODE_ESCAPE`, `KEYCODE_CTRL_A` through `KEYCODE_CTRL_W`, bracket codes
  - `#define TERM_BUF_SIZE 4096`
  - `td_term_t` struct: termios backup, `input[8]` (escape buf), `buf[4096]` (line buf), `buf_len`, `buf_pos`, `term_width`, `term_height`, `prompt_len`, `last_total_rows`
  - API declarations: `td_term_create()`, `td_term_destroy()`, `td_term_getc()`, `td_term_get_size()`
  - Cursor helpers: `td_cursor_move_start/left/right/up/down()`, `td_line_clear/clear_below()`, `td_cursor_hide/show()`
- [x] Create `src/app/term.c` with:
  - `td_term_create()` — `tcgetattr()` to save, set raw mode (no echo, no canonical, char-at-a-time), call `td_term_get_size()`
  - `td_term_destroy()` — `tcsetattr()` to restore original
  - `td_term_getc()` — `read(STDIN_FILENO, &c, 1)`, decode escape sequences (`\033[A` → UP, etc.)
  - `td_term_get_size()` — `ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)`
  - Cursor helpers — single `write()` calls with ANSI sequences
  - `td_term_visual_width()` — count visible chars, skip ANSI escapes, handle UTF-8
  - `td_term_goto_position()` — calculate row/col from position, emit cursor moves
  - Windows stubs (`#if defined(_WIN32)`) — Console API equivalents
- [x] Add `src/app/term.c` to CMakeLists.txt library sources (either add to `GLOB_RECURSE` path or explicit list)
- [x] Build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build` — compiles clean
- [x] Commit: `feat(repl): terminal layer with raw mode and cursor helpers`

---

### Task 1.2: Basic line editing (insert, backspace, cursor movement)

**Files:** Modify `src/app/term.h`, `src/app/term.c`

- [x] Add `td_term_read()` to `term.h` — reads one complete line, returns `td_t*` string or NULL
- [x] Implement `td_term_read()` in `term.c`:
  - Loop calling `td_term_getc()`
  - Character insert: `memmove` to shift right, insert at `buf_pos`, increment `buf_len` and `buf_pos`
  - Backspace: UTF-8 aware (find prev char start), `memmove` to shift left, decrement
  - Delete: remove char at `buf_pos`
  - Left/Right: move `buf_pos` (with UTF-8 byte skipping)
  - Home/Ctrl-A: `buf_pos = 0`
  - End/Ctrl-E: `buf_pos = buf_len`
  - Ctrl-K: kill to end (`buf_len = buf_pos`)
  - Ctrl-U: kill entire line (`buf_len = buf_pos = 0`)
  - Ctrl-W: kill word backward (scan back past non-alphanum, then past alphanum)
  - Enter: return `td_str(buf, buf_len)`
  - Ctrl-D on empty: return NULL (EOF)
  - Ctrl-C: clear buffer, print newline, show prompt
- [x] Implement `td_term_redraw()` — clear line, rewrite prompt + buffer, position cursor
- [x] Implement `td_term_prompt()` — write `"teide> "` with prompt_len tracking
- [x] Build: compiles clean
- [x] Smoke test: `./build/teide_repl` — type chars, backspace works, arrows work, Enter evals
- [x] Commit: `feat(repl): basic line editing with cursor movement and kill commands`

---

### Task 1.3: Wire REPL to new terminal layer

**Files:** Modify `src/lang/repl.c`; Create `src/app/repl.h`, `src/app/repl.c`

- [x] Create `src/app/repl.h`:
  - `td_repl_t` struct: `td_term_t* term`
  - `td_repl_create()`, `td_repl_destroy()`, `td_repl_run()`, `td_repl_run_file()`
- [x] Create `src/app/repl.c`:
  - `td_repl_create()` — allocate struct, `td_term_create()` if `isatty(STDIN_FILENO)`, NULL otherwise
  - `td_repl_run()` — if terminal: prompt loop with `td_term_read()` → `td_eval_str()` → `td_lang_print()`. If piped: read chunks with `read()`, eval, print
  - `td_repl_run_file()` — read file, eval (keep existing logic from `run_file`)
  - `td_repl_destroy()` — `td_term_destroy()`, free struct
- [x] Update `src/lang/repl.c` (main binary):
  - Strip out `run_repl()` and `run_file()` implementations
  - Replace with: `td_repl_create()` → `td_repl_run()` or `td_repl_run_file()` → `td_repl_destroy()`
- [x] Build and smoke test: `./build/teide_repl` — interactive editing works, `echo '(+ 1 2)' | ./build/teide_repl` → `3`
- [x] Commit: `feat(repl): wire REPL to new terminal layer, separate app/repl module`

---

## Phase 2: History

### Task 2.1: History ring buffer with navigation

**Files:** Modify `src/app/term.h`, `src/app/term.c`

- [x] Add `td_hist_t` struct to `term.h`:
  - `char** entries` — array of strings (td_alloc'd)
  - `int32_t count`, `capacity`, `index` (navigation pos)
  - `int32_t curr_saved` — flag for saved current input
  - `char curr[TERM_BUF_SIZE]` — saved current input before navigating
  - `int32_t curr_len`
- [x] Add `hist` field to `td_term_t`
- [x] Implement in `term.c`:
  - `td_hist_create()` — allocate entry array
  - `td_hist_destroy()` — free all entries
  - `td_hist_add(hist, buf, len)` — copy entry to array (skip if same as last entry)
  - `td_hist_prev(hist, buf)` — save current if first time, copy prev entry to buf, return len
  - `td_hist_next(hist, buf)` — copy next entry (or restore saved current), return len
- [x] Wire into `td_term_read()`:
  - Enter: call `td_hist_add()` before returning
  - Up/Ctrl-P: `td_hist_prev()`, update buf/buf_len/buf_pos, redraw
  - Down/Ctrl-N: `td_hist_next()`, update, redraw
- [x] Smoke test: type expressions, Up recalls them, Down goes back to current
- [x] Commit: `feat(repl): history navigation with Up/Down`

---

### Task 2.2: Persistent history file

**Files:** Modify `src/app/term.c`

- [x] Implement `td_hist_load(path)`:
  - Open `~/.teide_history` (or `$HOME/.teide_history`)
  - Read line by line, add each to history entries
  - Multi-line entries delimited by `\x00` (null byte between entries, newlines within)
- [x] Implement `td_hist_save(hist, path)`:
  - Write all entries to file, `\x00` delimited
  - Truncate to last 1000 entries if exceeds
- [x] Wire into `td_term_create()` → `td_hist_load()` and `td_term_destroy()` → `td_hist_save()`
- [x] Smoke test: type expressions, exit, restart, Up recalls previous session's history
- [x] Commit: `feat(repl): persistent history file (~/.teide_history)`

---

### Task 2.3: Reverse incremental search (Ctrl-R)

**Files:** Modify `src/app/term.c`

- [ ] Implement search mode state in `td_term_t`: `int32_t search_mode`, `char search_buf[256]`, `int32_t search_len`
- [ ] Ctrl-R enters search mode:
  - Prompt changes to `(search) `
  - Each keystroke appends to `search_buf` and scans history for substring match
  - Display matching entry with highlighted match
  - Enter accepts match into buffer, exits search mode
  - Escape cancels search
  - Ctrl-R again goes to next match (further back)
- [ ] Smoke test: Ctrl-R, type partial text, matching history entry appears
- [ ] Commit: `feat(repl): reverse incremental history search (Ctrl-R)`

---

## Phase 3: Syntax Highlighting

### Task 3.1: Colorized redraw

**Files:** Modify `src/app/term.c`

Reference: `/home/hetoku/data/work/rayforce/app/term.c` lines 680-784 (`term_redraw_into`)

- [ ] Define ANSI color constants:
  - `GREEN "\033[32m"`, `YELLOW "\033[33m"`, `CYAN "\033[36m"`, `GRAY "\033[90m"`, `LIGHT_BLUE "\033[94m"`, `RESET "\033[0m"`, `BOLD "\033[1m"`, `BACK_CYAN "\033[46m"`
- [ ] Implement `term_redraw_highlighted()` — replaces plain `td_term_redraw()`:
  - Walk `buf` char by char
  - Brackets `()[]{}` → Gray
  - `:` at word boundary → Gray (dict key)
  - `"..."` strings → Yellow (scan to closing `"`, handle `\"` escapes)
  - `'name` symbols → Cyan (scan to non-alphanum)
  - `;` comments → Gray to end of line
  - Word at word boundary → check `td_env_lookup_prefix()` for exact match → Green if builtin
  - Operator chars (`+ - * / % < > = ! & |`) standing alone → Light Blue
  - Numbers → Default (no color)
  - All other → Default
- [ ] Emit into a temporary buffer, then single `write()` to stdout (avoid flicker)
- [ ] Call from `td_term_redraw()` instead of plain rewrite
- [ ] Smoke test: type `(select {from: t where: (> salary 50000)})` — parens gray, `select` green, string would be yellow, `'sym` cyan
- [ ] Commit: `feat(repl): syntax highlighting with ANSI colors`

---

### Task 3.2: Bracket matching

**Files:** Modify `src/app/term.c`

Reference: `/home/hetoku/data/work/rayforce/app/term.c` lines 930-1055 (`term_highlight_pos`, `term_find_open_paren`)

- [ ] Implement `td_term_find_matching_paren()`:
  - Stack-based scan from cursor position
  - If cursor is on opening bracket: scan forward for matching close
  - If cursor is on closing bracket: scan backward for matching open
  - Handle multiline buffer too (scan `multiline_buf` first for context)
  - Skip brackets inside strings (`"..."`)
  - Return position of match, or -1
- [ ] In `term_redraw_highlighted()`: if cursor is on a bracket and match found, render both with `BACK_CYAN` (highlight background)
- [ ] Smoke test: type `(+ (* 2 3) 4)`, cursor on `(` highlights matching `)`
- [ ] Commit: `feat(repl): bracket matching with highlight`

---

### Task 3.3: Environment prefix lookup for highlighting

**Files:** Modify `src/lang/env.h`, `src/lang/env.c`

- [ ] Add to `env.h`:
  ```c
  int64_t td_env_lookup_prefix(const char* prefix, int64_t len,
                                const char** results, int64_t max_results);
  ```
  Returns count of matches. Fills `results[]` with pointers to interned name strings.
- [ ] Implement in `env.c`:
  - Scan `g_env` keys, resolve each symbol ID to string via `td_sym_str()`
  - Compare prefix with `strncmp`
  - Also scan a static keyword list: `fn`, `do`, `if`, `let`, `set`, `true`, `false`
  - Return matches sorted alphabetically
- [ ] Wire into `term_redraw_highlighted()` for exact-match coloring (1 result, len == word len)
- [ ] Build and test: highlighting uses live env data
- [ ] Commit: `feat(repl): environment prefix lookup for highlighting and completion`

---

## Phase 4: Autocomplete

### Task 4.1: Ghost text (inline suggestion)

**Files:** Modify `src/app/term.c`

- [ ] After each keystroke (in `td_term_read` loop, after redraw):
  - Extract word under/before cursor (scan backward for word start)
  - Call `td_env_lookup_prefix(word, word_len, results, 64)`
  - Also scan keywords, column names (if in select context), history words
  - If exactly 1 match and it's longer than what's typed: render remainder as gray ghost text after cursor
  - If 0 matches: no ghost text
  - If 2+ matches: show ghost text for first match
- [ ] Ghost text rendering: append `"\033[90m<rest>\033[0m"` in the redraw output after cursor position, but do NOT modify `buf`
- [ ] Right arrow at end of line: accept ghost text (copy remainder into buf)
- [ ] Tab with 1 match: accept ghost text into buf
- [ ] Any other keystroke: ghost text recalculates
- [ ] Smoke test: type `sel` → ghost shows `ect` grayed out, Tab completes to `select`
- [ ] Commit: `feat(repl): inline ghost text suggestions`

---

### Task 4.2: Completion candidate collection

**Files:** Modify `src/app/term.c`

- [ ] Implement `td_term_collect_completions()`:
  - Source 1: `td_env_lookup_prefix()` — builtins + user variables
  - Source 2: static keyword array (binary search on prefix)
  - Source 3: column names — lightweight scan of current buffer for `(select {from: <name>`, resolve `<name>` from env, if it's a table, get column name symbols
  - Source 4: history words — scan history entries, tokenize by whitespace/parens, match prefix, deduplicate against sources 1-3
  - Merge all into a single sorted deduplicated array
  - Return count and array of `const char*` pointers
- [ ] Store results in `td_term_t` for popup use
- [ ] Smoke test: verify completion sources work (use ghost text to validate — type prefix, see correct suggestion)
- [ ] Commit: `feat(repl): multi-source completion candidate collection`

---

### Task 4.3: Dropdown popup menu

**Files:** Modify `src/app/term.c`, `src/app/term.h`

- [ ] Add popup state to `td_term_t`:
  - `popup_visible` (bool), `popup_selected` (int), `popup_count`, `popup_scroll`
  - `popup_items` (const char** — borrowed pointers from completion results)
  - `popup_max_visible` — min of candidate count and (term_height - 3)
- [ ] Implement `td_term_popup_show()`:
  - Save cursor position
  - Calculate box position (below current line, or above if not enough space)
  - Box width = max item length + 4 (border + padding)
  - Render top border: `┌──...──┐`
  - Render each visible item: `│ item  │` (selected item gets reverse video `\033[7m`)
  - Render bottom border: `└──...──┘`
  - Restore cursor position
- [ ] Implement `td_term_popup_hide()`:
  - Move cursor below prompt line, clear with `\033[J`, restore cursor
- [ ] Implement popup navigation in `td_term_read()`:
  - When `popup_visible`:
    - Up: decrement `popup_selected` (scroll if needed), redraw popup
    - Down: increment `popup_selected`, redraw popup
    - Enter: accept `popup_items[popup_selected]` into buf, hide popup
    - Escape: hide popup, continue editing
    - Tab: cycle to next item
    - Any printable char: hide popup, insert char normally
- [ ] Wire into Tab handling:
  - Tab pressed, 1 match → accept directly
  - Tab pressed, 2+ matches → show popup
- [ ] Smoke test: type `s`, Tab → popup shows `scan`, `select`, `set`, `sum`, etc. Arrow to navigate, Enter to accept
- [ ] Commit: `feat(repl): dropdown popup autocomplete menu`

---

## Phase 5: Multi-line Input

### Task 5.1: Paren balancing and continuation prompt

**Files:** Modify `src/app/term.c`, `src/app/term.h`

- [ ] Add `multiline_buf[TERM_BUF_SIZE]` and `multiline_len` to `td_term_t` (already in struct from design)
- [ ] Implement `td_term_count_unmatched()`:
  - Scan `multiline_buf + buf` for unmatched `(`, `[`, `{`
  - Skip brackets inside strings (`"..."`)
  - Return count of unmatched openers
- [ ] Modify Enter handling in `td_term_read()`:
  - Call `td_term_count_unmatched()`
  - If count > 0: append `buf + \n` to `multiline_buf`, clear buf, show continuation prompt `  ··· `, continue reading
  - If count == 0 and `multiline_len > 0`: concatenate `multiline_buf + buf`, reset multiline state, return full string
  - If count == 0 and `multiline_len == 0`: return buf as normal
- [ ] Implement `td_term_continuation_prompt()` — writes `"  ··· "` with correct prompt_len
- [ ] Smoke test: type `(select {` Enter → continuation prompt → `from: t` Enter → continuation prompt → `})` Enter → evaluates full expression
- [ ] Commit: `feat(repl): multi-line input with paren balancing and continuation prompt`

---

## Phase 6: Polish

### Task 6.1: Pretty-print output (tables, vectors)

**Files:** Modify `src/app/repl.c`

- [ ] Implement `td_repl_print_result()`:
  - Scalars: print value directly
  - Vectors: `[1 2 3 4 5]` format, truncate at terminal width
  - Tables: columnar format with headers, aligned columns, row count
    ```
    name   | salary
    -------+-------
    Alice  | 50000
    Bob    | 70000
    (2 rows)
    ```
  - Dicts: `{key: val ...}` format
  - Errors: red color `\033[31m`
- [ ] Wire into REPL eval loop instead of raw `td_lang_print()`
- [ ] Smoke test: eval a table expression, see formatted output
- [ ] Commit: `feat(repl): pretty-print tables, vectors, and errors`

---

### Task 6.2: REPL commands

**Files:** Modify `src/app/repl.c`

- [ ] Implement command detection: if line starts with `:`, handle as command
  - `:?` or `:help` — print help text
  - `:t` or `:timeit` — toggle expression timing
  - `:q` or `:quit` — exit with code 0
  - `:env` — list all defined variables
  - `:clear` — clear screen (`\033[2J\033[H`)
- [ ] Smoke test: type `:?` → see help, `:q` → exits
- [ ] Commit: `feat(repl): REPL commands (:help, :timeit, :quit, :env, :clear)`

---

### Task 6.3: Signal handling (Ctrl-C interrupt)

**Files:** Modify `src/app/repl.c`, `src/app/term.c`

- [ ] Install `SIGINT` handler that sets a flag instead of terminating
- [ ] In eval loop: check flag after `td_eval_str()`, if set → print `^C`, clear flag, show prompt
- [ ] In `td_term_read()`: Ctrl-C clears current buffer, prints `^C\n`, shows fresh prompt
- [ ] Ensure terminal is restored on any exit path (SIGTERM, SIGQUIT)
- [ ] Install `atexit()` handler that calls `td_term_destroy()` to restore termios
- [ ] Smoke test: Ctrl-C during input clears line, Ctrl-C during long eval interrupts
- [ ] Commit: `feat(repl): signal handling for clean Ctrl-C interrupt`

---

## Dependency Graph

```
Phase 1: Raw Terminal (sequential)
  1.1 Term struct + raw mode ──→ 1.2 Line editing ──→ 1.3 Wire to REPL

Phase 2: History (sequential, after 1.3)
  2.1 History navigation ──→ 2.2 Persistent file ──→ 2.3 Ctrl-R search

Phase 3: Highlighting (after 1.3)
  3.3 Env prefix lookup ──→ 3.1 Colorized redraw ──→ 3.2 Bracket matching

Phase 4: Autocomplete (after 3.1 + 3.3)
  4.1 Ghost text ──→ 4.2 Candidate collection ──→ 4.3 Dropdown popup

Phase 5: Multi-line (after 1.3)
  5.1 Paren balancing + continuation

Phase 6: Polish (after all above)
  6.1 Pretty-print ──→ 6.2 Commands ──→ 6.3 Signal handling
```

**Critical path:** 1.1 → 1.2 → 1.3 → 3.3 → 3.1 → 4.1 → 4.2 → 4.3

**Parallelizable after 1.3:** Phase 2 (history), Phase 5 (multi-line), Task 3.3 (env lookup) can all start independently.
