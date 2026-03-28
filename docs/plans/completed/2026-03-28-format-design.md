# Rayforce-Compatible Format System — Design

**Goal:** Replace the ad-hoc REPL printer with a standalone format module (`src/app/format.h/c`) that matches Rayforce's output format exactly.

## Public API

```c
/* Format any ray_t value into a string for display.
 * mode: 0 = compact (one-line), 1 = full (REPL), 2 = show (no limits) */
ray_t* ray_fmt(ray_t* obj, int mode);

/* Format and write to FILE* */
void ray_fmt_print(FILE* fp, ray_t* obj, int mode);

/* Display settings */
void ray_fmt_set_precision(int digits);   /* f64 decimal places, default 2 */
void ray_fmt_set_width(int cols);         /* max row width, default 80 */
```

## Atom Formatting

| Type | Format | Null |
|------|--------|------|
| bool | `true` / `false` | — |
| u8 | `0x02` (hex) | — |
| char | `'c'`, `'\n'`, `'\t'` | `''` |
| i16 | `42` | `0Nh` |
| i32 | `42` | `0Ni` |
| i64 | `42` | `0Nl` |
| f64 | `3.14` / `1.23e+07` | `0Nf` |
| date | `2024.03.28` | `0Nd` |
| time | `12:30:45.000` | `0Nt` |
| timestamp | `2024.03.28D12:30:45.000000000` | `0Np` |
| guid | `abcd1234-...` | `0Ng` |
| symbol | `name` | `0Ns` |

### Float formatting
- Precision configurable, default 2 decimals
- `|value| >= 1e6 or |value| < 0.1` → scientific `%.*e`
- Otherwise → fixed `%.*f`
- NaN → `0Nf`, `-0.0` → `0.00`

### Null detection
- Vector elements: `ray_vec_is_null(vec, idx)` (bitmap)
- Atoms: sentinel check matching Rayforce (`INT64_MIN`, NaN, etc.)

## Vector Formatting

`[1 2 3 4 5]` — space-separated in `[]`, width-limited with `..` truncation.

Empty vector: `[]`

## List Formatting

`(a b c)` — space-separated in `()`.

- Max height: 50 items (mode 1), unlimited (mode 2)
- Truncated with `..`
- Empty list: `()`

## Dict Formatting

Key-value display in vertical layout.

## Table Formatting

```
┌──────┬───────┐
│  id  │  val  │
│  i64 │  i64  │
├──────┼───────┤
│ 1    │ 10    │
│ 2    │ 20    │
│ 3    │ 30    │
└──────┴───────┘
 3 rows (3 shown) 2 columns (2 shown)
```

### Layout rules
- Max 10 columns, max 20 rows (mode 1)
- Column names centered in header
- Type names centered below header
- `├─┼─┤` separator between header and data
- Column widths: max of name, type name, and element widths (sampled)
- Head+tail: first N/2 + last N/2 rows when truncated
- Footer: `N rows (M shown) C columns (K shown)`
- Hidden columns: `… │` indicator
- Compact mode (0): `(table [col1 col2]…)`

### Box-drawing glyphs
```
┌ ─ ┬ ┐    top border
│          vertical separator
├ ─ ┼ ┤    header/data separator
└ ─ ┴ ┘    bottom border
…          ellipsis (hidden content)
```

## Error Formatting

Red color: `\033[1;31m` + error message + `\033[0m`

## Function Formatting

- Builtins: type name (e.g. `builtin/1`, `builtin/2`, `builtin/n`)
- Lambdas: `lambda`

## Constants

```c
#define FMT_TABLE_MAX_WIDTH   10
#define FMT_TABLE_MAX_HEIGHT  20
#define FMT_LIST_MAX_HEIGHT   50
#define FMT_DEFAULT_ROW_WIDTH 80
#define FMT_DEFAULT_PRECISION  2
```

## Integration

`src/app/repl.c`'s `repl_print_result` becomes:
```c
ray_fmt_print(stdout, result, 1);
```

Remove all existing formatting code from repl.c (`print_table`, `print_vector`, `print_typed_vector`, `fmt_vec_elem`, `vec_is_null`).

## Type name strings

For table type row, need `type_name(type)` returning: `b8`, `u8`, `c8`, `i16`, `i32`, `i64`, `f64`, `f32`, `date`, `time`, `timestamp`, `guid`, `sym`, `str`.
