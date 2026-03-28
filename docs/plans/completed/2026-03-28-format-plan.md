# Rayforce-Compatible Format System — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace REPL formatting with a standalone format module that matches Rayforce output exactly — 0N nulls, box-drawing tables, configurable float precision.

**Architecture:** New `src/app/format.h/c` module. Per-type atom/vector formatters write into a growable char buffer. Table formatter uses box-drawing glyphs. REPL becomes a thin wrapper. Date/time conversions ported from Rayforce.

**Tech Stack:** C17, no external deps. Uses `snprintf` for number formatting, UTF-8 box-drawing chars.

**Design doc:** `docs/plans/2026-03-28-format-design.md`

**Build & test:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
```

**Reference files:**
- `src/app/repl.c:135-359` — current formatting (to be replaced)
- `/home/hetoku/data/work/rayforce/core/format.c` — reference implementation
- `/home/hetoku/data/work/rayforce/core/date.c` — date conversion
- `/home/hetoku/data/work/rayforce/core/time.c` — time conversion
- `/home/hetoku/data/work/rayforce/core/timestamp.c` — timestamp conversion

---

## Task 1: Create format.h/c skeleton with buffer helper

**Files:** Create `src/app/format.h`, `src/app/format.c`

- [ ] Create `src/app/format.h` with:
  ```c
  #ifndef RAY_FORMAT_H
  #define RAY_FORMAT_H

  #include <rayforce.h>
  #include <stdio.h>

  #define FMT_TABLE_MAX_WIDTH   10
  #define FMT_TABLE_MAX_HEIGHT  20
  #define FMT_LIST_MAX_HEIGHT   50
  #define FMT_DEFAULT_ROW_WIDTH 80
  #define FMT_DEFAULT_PRECISION  2

  /* Format a ray_t value into a new ray_t string (RAY_STR atom).
   * mode: 0 = compact, 1 = full (REPL), 2 = show (no limits) */
  ray_t* ray_fmt(ray_t* obj, int mode);

  /* Format and write to FILE* */
  void ray_fmt_print(FILE* fp, ray_t* obj, int mode);

  /* Display settings */
  void ray_fmt_set_precision(int digits);
  void ray_fmt_set_width(int cols);

  /* Type name string (e.g. RAY_I64 -> "i64") */
  const char* ray_type_name(int8_t type);

  #endif
  ```

- [ ] Create `src/app/format.c` with:
  - Internal growable buffer type:
    ```c
    typedef struct {
        char*   buf;
        int32_t len;
        int32_t cap;
    } fmt_buf_t;
    ```
  - `fmt_init`, `fmt_destroy`, `fmt_printf(buf, fmt, ...)` — writes formatted text into buffer, grows as needed using `ray_alloc`/`ray_free`
  - `fmt_puts(buf, str)` — append literal string
  - Static globals: `g_precision = FMT_DEFAULT_PRECISION`, `g_row_width = FMT_DEFAULT_ROW_WIDTH`
  - Implement `ray_fmt_set_precision`, `ray_fmt_set_width`
  - Implement `ray_type_name` — switch on type returning: `"list"`, `"b8"`, `"u8"`, `"c8"`, `"i16"`, `"i32"`, `"i64"`, `"f64"`, `"f32"`, `"date"`, `"time"`, `"timestamp"`, `"guid"`, `"sym"`, `"str"`
  - Stub `ray_fmt` returning empty string
  - Stub `ray_fmt_print` printing `"<todo>"`

- [ ] Build: compiles clean
- [ ] Commit: `feat(format): create format module skeleton with buffer helper`

---

## Task 2: Atom formatters

**Files:** Modify `src/app/format.c`

Implement per-type atom formatting into `fmt_buf_t`. Each takes a `fmt_buf_t*` and the atom value.

- [ ] `fmt_bool(buf, b8)` — `"true"` / `"false"`
- [ ] `fmt_u8(buf, u8)` — `"0x%02x"`
- [ ] `fmt_char(buf, c8)` — `"'c'"` with escapes: `'\n'`, `'\t'`, `'\r'`, `'\"'`, `''` for null char
- [ ] `fmt_i16(buf, i16)` — `"%d"` or `"0Nh"` if `val == INT16_MIN`
- [ ] `fmt_i32(buf, i32)` — `"%d"` or `"0Ni"` if `val == INT32_MIN`
- [ ] `fmt_i64(buf, i64)` — `"%" PRId64` or `"0Nl"` if `val == INT64_MIN`
- [ ] `fmt_f64(buf, f64)` — configurable precision, scientific for large/small values, `"0Nf"` for NaN, `"0.00"` for `-0.0`. Logic:
  ```c
  if (isnan(val)) return "0Nf";
  if (val == -0.0) { snprintf(... "%.*f", g_precision, 0.0); return; }
  double order = log10(fabs(val));
  if (val != 0.0 && (order > 6 || order < -1))
      snprintf(... "%.*e", g_precision, val);
  else
      snprintf(... "%.*f", g_precision, val);
  ```
- [ ] `fmt_sym(buf, sym_id)` — resolve via `ray_sym_str(id)`, print string. `"0Ns"` if `id == INT64_MIN`
- [ ] Wire into `ray_fmt` — dispatch by `obj->type` for negative types (atoms)
- [ ] Build and test: `echo '42' | ./build/rayforce` → `42`, `echo '3.14159' | ./build/rayforce` → `3.14`
- [ ] Commit: `feat(format): atom formatters — int, float, bool, char, symbol`

---

## Task 3: Date/time/timestamp formatters

**Files:** Modify `src/app/format.c`

Port date/time conversion logic from Rayforce. Our dates are days since 2000-01-01 (i32), times are milliseconds since midnight (i32), timestamps are nanoseconds since 2000-01-01 (i64).

- [ ] Implement `date_to_ymd(int32_t days, int* y, int* m, int* d)`:
  - Port from `/home/hetoku/data/work/rayforce/core/date.c:34` (`date_from_i32`)
  - Epoch: 2000-01-01
- [ ] Implement `time_to_hms(int32_t ms, int* h, int* min, int* s, int* ms_out)`:
  - Port from `/home/hetoku/data/work/rayforce/core/time.c:31` (`time_from_i32`)
- [ ] Implement `timestamp_to_parts(int64_t ns, int* y, int* mo, int* d, int* h, int* mi, int* s, int* nanos)`:
  - Port from `/home/hetoku/data/work/rayforce/core/timestamp.c:65` (`timestamp_from_i64`)
- [ ] `fmt_date(buf, i32)` — `"YYYY.MM.DD"` or `"0Nd"` if null
- [ ] `fmt_time(buf, i32)` — `"HH:MM:SS.mmm"` or `"0Nt"` if null. Negative times: `"-HH:MM:SS.mmm"`
- [ ] `fmt_timestamp(buf, i64)` — `"YYYY.MM.DDDHH:MM:SS.nnnnnnnnn"` or `"0Np"` if null
- [ ] Wire into atom dispatch
- [ ] Build and smoke test
- [ ] Commit: `feat(format): date, time, timestamp formatters`

---

## Task 4: Vector and list formatters

**Files:** Modify `src/app/format.c`

- [ ] Implement `fmt_raw_elem(buf, vec, idx)` — format single vector element at index:
  - Check `ray_vec_is_null(vec, idx)` → print `0N*` for the type
  - Otherwise dispatch by `vec->type` to the appropriate atom formatter, reading from `ray_data(vec)` at index
  - Handle all types: BOOL, U8, CHAR, I16, I32, I64, F64, DATE, TIME, TIMESTAMP, GUID, SYM (adaptive width via `ray_read_sym`), STR (via `ray_str_vec_get`)
  - **Include sym.h and str.h** for SYM/STR element access

- [ ] Implement `fmt_vector(buf, vec, limit)` — typed vector:
  - `"[]"` for empty
  - `"["` + space-separated elements + `"]"`
  - Width-limited: if output exceeds `limit`, truncate with `"..]"`

- [ ] Implement `fmt_list(buf, list, mode)` — boxed list (RAY_LIST):
  - `"()"` for empty
  - `"("` + space-separated `ray_fmt` of each element + `")"`
  - Height-limited at `FMT_LIST_MAX_HEIGHT` (mode 1), unlimited (mode 2)
  - Truncated with `".."`

- [ ] Implement `fmt_dict(buf, dict, mode)` — RAY_LIST with RAY_ATTR_DICT:
  - Keys at even indices, values at odd
  - Compact: `"{key1: val1 key2: val2}"`
  - Full: multi-line with indentation

- [ ] Wire into `ray_fmt` for positive types (vectors) and RAY_LIST
- [ ] Build and test: `echo '(til 5)' | ./build/rayforce` → `[0 1 2 3 4]`
- [ ] Commit: `feat(format): vector, list, dict formatters`

---

## Task 5: Table formatter with box-drawing

**Files:** Modify `src/app/format.c`

This is the largest task. Match Rayforce's table layout exactly.

- [ ] Implement `fmt_table(buf, tbl, mode)`:
  - **Compact mode (0):** `"(table [col1 col2]…)"`
  - **Full mode (1):**
    1. Collect column names, types, vectors
    2. Apply limits: `FMT_TABLE_MAX_WIDTH` cols, `FMT_TABLE_MAX_HEIGHT` rows
    3. Pre-format all cells into strings, calculate column widths (max of name, type, elements + 2 padding)
    4. Head+tail display: for large tables, show first N/2 + last N/2 rows
    5. Render:
       - Top border: `┌──┬──┐`
       - Header row: `│ name │` (centered)
       - Type row: `│ i64  │` (centered)
       - Separator: `├──┼──┤`
       - Data rows: `│ val  │`
       - If truncated: `│  …   │` row
       - Bottom border: `└──┴──┘`
       - Footer: ` N rows (M shown) C columns (K shown)`
    6. Hidden columns: `… │` indicator
  - **Show mode (2):** Same as full but no limits

- [ ] Box-drawing helpers:
  ```c
  #define GLYPH_TL "┌"
  #define GLYPH_TR "┐"
  #define GLYPH_BL "└"
  #define GLYPH_BR "┘"
  #define GLYPH_H  "─"
  #define GLYPH_V  "│"
  #define GLYPH_TT "┬"
  #define GLYPH_BT "┴"
  #define GLYPH_LT "├"
  #define GLYPH_RT "┤"
  #define GLYPH_X  "┼"
  #define GLYPH_HDOTS "…"
  ```

- [ ] Build and test with a table expression in REPL
- [ ] Commit: `feat(format): box-drawing table formatter`

---

## Task 6: Error, function, and lazy formatters

**Files:** Modify `src/app/format.c`

- [ ] `fmt_error(buf, err_code)` — `"error: <message>"`
- [ ] Function types (`RAY_UNARY`, `RAY_BINARY`, `RAY_VARY`) — print function name or `"builtin/<arity>"`
- [ ] `RAY_LAMBDA` — `"lambda"`
- [ ] Lazy handles — materialize first, then format the result
- [ ] Wire all into `ray_fmt` dispatch
- [ ] Commit: `feat(format): error, function, lazy formatters`

---

## Task 7: Wire into REPL, remove old formatting

**Files:** Modify `src/app/repl.c`

- [ ] Add `#include "app/format.h"` to repl.c
- [ ] Replace `repl_print_result` body with:
  ```c
  static void repl_print_result(FILE* fp, ray_t* val, bool use_color) {
      if (!val) return;
      if (RAY_IS_ERR(val)) {
          ray_err_t code = RAY_ERR_CODE(val);
          if (use_color) fprintf(fp, "\033[1;31m");
          fprintf(fp, "error: %s", ray_err_str(code));
          if (use_color) fprintf(fp, "\033[0m");
          fprintf(fp, "\n");
          return;
      }
      ray_fmt_print(fp, val, 1);
      fprintf(fp, "\n");
  }
  ```
- [ ] Remove from repl.c: `vec_is_null`, `fmt_vec_elem`, `print_vector`, `print_typed_vector`, `print_table`, `MAX_PRINT_ROWS`, `MAX_PRINT_ELEMS`, `MAX_COL_WIDTH`
- [ ] Build and test — all existing tests pass
- [ ] Smoke tests:
  ```bash
  echo '42' | ./build/rayforce           # → 42
  echo '3.14159' | ./build/rayforce      # → 3.14
  echo '(til 5)' | ./build/rayforce      # → [0 1 2 3 4]
  echo '(sum (til 100))' | ./build/rayforce  # → 4950
  ```
- [ ] Commit: `feat(format): wire into REPL, remove old formatting`

---

## Task 8: Format tests

**Files:** Create or modify test file

- [ ] Test atom formatting:
  - `ray_fmt(ray_i64(42), 1)` → `"42"`
  - `ray_fmt(ray_f64(3.14159), 1)` → `"3.14"`
  - `ray_fmt(ray_f64(1e7), 1)` → `"1.00e+07"`
  - `ray_fmt(ray_bool(true), 1)` → `"true"`
- [ ] Test null formatting:
  - Create null i64 atom → `"0Nl"`
  - Create null f64 atom → `"0Nf"`
- [ ] Test vector formatting:
  - I64 vector `[1,2,3]` → `"[1 2 3]"`
  - Empty vector → `"[]"`
- [ ] Test table formatting:
  - 2-column, 3-row table → verify box-drawing output contains `┌`, `│`, `└`, column names, type names, footer
- [ ] Test `ray_type_name`:
  - `ray_type_name(RAY_I64)` → `"i64"`
  - `ray_type_name(RAY_TABLE)` → `"table"`
- [ ] Build and test
- [ ] Commit: `test: format module tests`

---

## Dependency Graph

```
Task 1 (skeleton) → Task 2 (atoms) → Task 3 (date/time) → Task 4 (vector/list)
→ Task 5 (table) → Task 6 (error/func/lazy) → Task 7 (wire REPL) → Task 8 (tests)
```

All tasks are sequential.
