/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "app/format.h"
#include "table/sym.h"
#include "lang/eval.h"  /* RAY_ATTR_DICT */
#include "ops/ops.h"    /* RAY_LAZY, ray_lazy_materialize */
#include "mem/heap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>

/* ===== Internal growable buffer ===== */

typedef struct {
    char*   buf;
    int32_t len;
    int32_t cap;
    ray_t*  block;  /* ray_alloc'd backing block */
} fmt_buf_t;

static void fmt_init(fmt_buf_t* b) {
    b->block = ray_alloc(256);
    b->buf   = (char*)ray_data(b->block);
    b->len   = 0;
    b->cap   = 256;
}

static void fmt_destroy(fmt_buf_t* b) {
    if (b->block) {
        ray_free(b->block);
        b->block = NULL;
        b->buf   = NULL;
        b->len   = 0;
        b->cap   = 0;
    }
}

static void fmt_ensure(fmt_buf_t* b, int32_t extra) {
    if (b->len + extra <= b->cap) return;
    int32_t new_cap = b->cap;
    while (new_cap < b->len + extra)
        new_cap *= 2;
    ray_t* new_block = ray_alloc((size_t)new_cap);
    char*  new_buf   = (char*)ray_data(new_block);
    memcpy(new_buf, b->buf, (size_t)b->len);
    ray_free(b->block);
    b->block = new_block;
    b->buf   = new_buf;
    b->cap   = new_cap;
}

static void fmt_putc(fmt_buf_t* b, char c) {
    fmt_ensure(b, 1);
    b->buf[b->len++] = c;
}

static void fmt_puts(fmt_buf_t* b, const char* s) {
    int32_t slen = (int32_t)strlen(s);
    fmt_ensure(b, slen);
    memcpy(b->buf + b->len, s, (size_t)slen);
    b->len += slen;
}

static void fmt_printf(fmt_buf_t* b, const char* fmt, ...) {
    va_list ap;

    /* Try to fit in remaining space first */
    va_start(ap, fmt);
    int32_t avail = b->cap - b->len;
    int n = vsnprintf(b->buf + b->len, (size_t)avail, fmt, ap);
    va_end(ap);

    if (n < 0) return; /* encoding error */

    if (n < avail) {
        b->len += n;
        return;
    }

    /* Need more space — grow and retry */
    fmt_ensure(b, n + 1);
    va_start(ap, fmt);
    vsnprintf(b->buf + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    b->len += n;
}

static void fmt_putn(fmt_buf_t* b, const char* s, int32_t n) {
    fmt_ensure(b, n);
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += n;
}

static ray_t* fmt_to_str(fmt_buf_t* b) {
    ray_t* result = ray_str(b->buf, (size_t)b->len);
    fmt_destroy(b);
    return result;
}

/* ===== Static globals ===== */

static int g_precision = FMT_DEFAULT_PRECISION;
static int g_row_width = FMT_DEFAULT_ROW_WIDTH;

/* ===== Public API ===== */

void ray_fmt_set_precision(int digits) {
    if (digits >= 0 && digits <= 20)
        g_precision = digits;
}

void ray_fmt_set_width(int cols) {
    if (cols > 0)
        g_row_width = cols;
}

const char* ray_type_name(int8_t type) {
    switch (type) {
    case RAY_LIST:      return "list";
    case RAY_BOOL:      return "b8";
    case RAY_U8:        return "u8";
    case RAY_CHAR:      return "c8";
    case RAY_I16:       return "i16";
    case RAY_I32:       return "i32";
    case RAY_I64:       return "i64";
    case RAY_F64:       return "f64";
    case RAY_F32:       return "f32";
    case RAY_DATE:      return "date";
    case RAY_TIME:      return "time";
    case RAY_TIMESTAMP: return "timestamp";
    case RAY_GUID:      return "guid";
    case RAY_SYM:       return "sym";
    case RAY_STR:       return "str";
    case RAY_TABLE:     return "table";
    case RAY_DICT:      return "dict";
    default:            return "?";
    }
}

/* ===== Atom formatters ===== */

static void fmt_bool(fmt_buf_t* b, uint8_t val) {
    fmt_puts(b, val ? "true" : "false");
}

static void fmt_u8(fmt_buf_t* b, uint8_t val) {
    fmt_printf(b, "0x%02x", val);
}

static void fmt_char(fmt_buf_t* b, char val, int full) {
    (void)full;
    fmt_putc(b, '\'');
    switch (val) {
    case '\0': /* empty char literal */ break;
    case '\n': fmt_puts(b, "\\n"); break;
    case '\t': fmt_puts(b, "\\t"); break;
    case '\r': fmt_puts(b, "\\r"); break;
    case '"':  fmt_puts(b, "\\\""); break;
    default:   fmt_putc(b, val); break;
    }
    fmt_putc(b, '\'');
}

static void fmt_i16(fmt_buf_t* b, int16_t val) {
    if (val == INT16_MIN) { fmt_puts(b, "0Nh"); return; }
    fmt_printf(b, "%d", (int)val);
}

static void fmt_i32(fmt_buf_t* b, int32_t val) {
    if (val == INT32_MIN) { fmt_puts(b, "0Ni"); return; }
    fmt_printf(b, "%d", (int)val);
}

static void fmt_i64(fmt_buf_t* b, int64_t val) {
    if (val == INT64_MIN) { fmt_puts(b, "0Nl"); return; }
    fmt_printf(b, "%" PRId64, val);
}

static void fmt_f64(fmt_buf_t* b, double val) {
    if (isnan(val)) { fmt_puts(b, "0Nf"); return; }
    if (val == -0.0 && signbit(val)) val = 0.0; /* normalize -0.0 */
    if (val == 0.0) {
        /* Zero: format as "0.0" (after trailing-zero strip) */
        char tmp[16];
        int n = snprintf(tmp, sizeof(tmp), "%.*f", g_precision, 0.0);
        char* dot = strchr(tmp, '.');
        if (dot) { char* end = tmp + n - 1; while (end > dot + 1 && *end == '0') end--; n = (int)(end - tmp + 1); }
        fmt_putn(b, tmp, (int32_t)n);
        return;
    }
    double absval = val < 0 ? -val : val;
    double order = log10(absval);

    /* Format with requested precision */
    char tmp[64];
    int n;
    if (val != 0.0 && (order > 6 || order < -1))
        n = snprintf(tmp, sizeof(tmp), "%.*e", g_precision, val);
    else
        n = snprintf(tmp, sizeof(tmp), "%.*f", g_precision, val);

    if (n <= 0 || n >= (int)sizeof(tmp)) {
        fmt_puts(b, "?");
        return;
    }

    /* Strip trailing zeros after decimal point, keeping at least one
     * digit after '.'.  Do NOT touch exponential notation. */
    char* dot = strchr(tmp, '.');
    char* e   = strchr(tmp, 'e');
    if (dot && !e) {
        char* end = tmp + n - 1;
        while (end > dot + 1 && *end == '0')
            end--;
        n = (int)(end - tmp + 1);
    }

    fmt_putn(b, tmp, (int32_t)n);
}

static void fmt_guid(fmt_buf_t* b, const uint8_t* bytes) {
    static const char hex[] = "0123456789abcdef";
    /* Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    static const int groups[] = {4, 2, 2, 2, 6};
    int pos = 0;
    for (int g = 0; g < 5; g++) {
        if (g > 0) fmt_putc(b, '-');
        for (int j = 0; j < groups[g]; j++) {
            fmt_putc(b, hex[bytes[pos] >> 4]);
            fmt_putc(b, hex[bytes[pos] & 0x0F]);
            pos++;
        }
    }
}

static void fmt_sym(fmt_buf_t* b, int64_t sym_id) {
    if (sym_id == INT64_MIN) { fmt_puts(b, "0Ns"); return; }
    ray_t* s = ray_sym_str(sym_id);
    if (s && !RAY_IS_ERR(s)) {
        const char* p = ray_str_ptr(s);
        size_t      n = ray_str_len(s);
        fmt_putn(b, p, (int32_t)n);
        ray_release(s);
    } else {
        fmt_puts(b, "0Ns");
    }
}

/* ===== Date/time/timestamp helpers (ported from Rayforce) ===== */

/* Cumulative days-in-month lookup: [leap][month].
 * Index 0 = Jan start (0 days), index 12 = Dec end (365 or 366). */
static const uint32_t MONTHDAYS_FWD[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

#define DATE_EPOCH 2000

static int date_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int32_t date_years_by_days(int yy) {
    return (int32_t)((int64_t)yy * 365 + yy / 4 - yy / 100 + yy / 400);
}

static void date_to_ymd(int32_t days, int* y, int* m, int* d) {
    int32_t offset = days + date_years_by_days(DATE_EPOCH - 1);
    double approx = (double)offset / 365.2425;
    int32_t years = (int32_t)(approx >= 0.0 ? approx + 0.5 : approx - 0.5);

    if (date_years_by_days(years) > offset)
        years -= 1;

    int32_t rem = offset - date_years_by_days(years);
    int yy = years + 1;
    int leap = date_leap_year(yy);
    int mid = 0;

    for (mid = 12; mid > 0; mid--)
        if (MONTHDAYS_FWD[leap][mid] != 0 && rem / (int32_t)MONTHDAYS_FWD[leap][mid] != 0)
            break;

    if (mid == 12 || mid < 0)
        mid = 0;

    *y = yy;
    *m = 1 + mid % 12;
    *d = 1 + rem - (int32_t)MONTHDAYS_FWD[leap][mid];
}

static void time_to_hms(int32_t ms, int* h, int* min, int* s, int* ms_out) {
    int32_t mask = ms >> 31;
    int32_t val  = (mask ^ ms) - mask;  /* absolute value */

    int32_t secs = val / 1000;
    *ms_out = (int)(val % 1000);
    *h      = (int)(secs / 3600);
    int32_t rem = secs % 3600;
    *min    = (int)(rem / 60);
    *s      = (int)(rem % 60);
}

#define NSECS_IN_DAY ((int64_t)24 * 60 * 60 * 1000000000LL)

static void ts_to_parts(int64_t ns, int* y, int* mo, int* d,
                         int* h, int* mi, int* s, int* nanos) {
    int64_t days = ns / NSECS_IN_DAY;
    int64_t span = ns % NSECS_IN_DAY;

    if (span < 0) {
        days -= 1;
        span += NSECS_IN_DAY;
    }

    date_to_ymd((int32_t)days, y, mo, d);

    /* timespan_from_nanos */
    int64_t secs = span / 1000000000LL;
    *nanos = (int)(span % 1000000000LL);
    *h  = (int)(secs / 3600);
    int64_t rem = secs % 3600;
    *mi = (int)(rem / 60);
    *s  = (int)(rem % 60);
}

static void fmt_date(fmt_buf_t* b, int32_t val) {
    if (val == INT32_MIN) { fmt_puts(b, "0Nd"); return; }
    int y, m, d;
    date_to_ymd(val, &y, &m, &d);
    fmt_printf(b, "%04d.%02d.%02d", y, m, d);
}

static void fmt_time(fmt_buf_t* b, int32_t val) {
    if (val == INT32_MIN) { fmt_puts(b, "0Nt"); return; }
    int h, m, s, ms;
    time_to_hms(val, &h, &m, &s, &ms);
    if (val < 0) fmt_putc(b, '-');
    fmt_printf(b, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

static void fmt_timestamp(fmt_buf_t* b, int64_t val) {
    if (val == INT64_MIN) { fmt_puts(b, "0Np"); return; }
    int y, mo, d, h, mi, s, ns;
    ts_to_parts(val, &y, &mo, &d, &h, &mi, &s, &ns);
    fmt_printf(b, "%04d.%02d.%02dD%02d:%02d:%02d.%09d", y, mo, d, h, mi, s, ns);
}

static void fmt_str_atom(fmt_buf_t* b, ray_t* obj, int full) {
    (void)full;
    const char* p = ray_str_ptr(obj);
    size_t      n = ray_str_len(obj);
    fmt_putc(b, '"');
    fmt_putn(b, p, (int32_t)n);
    fmt_putc(b, '"');
}

/* ===== Forward declarations ===== */

static void fmt_obj(fmt_buf_t* b, ray_t* obj, int mode);

/* ===== Vector element formatter ===== */

static void fmt_raw_elem(fmt_buf_t* b, ray_t* vec, int64_t idx) {
    /* Check for null */
    if (ray_vec_is_null(vec, idx)) {
        switch (vec->type) {
        case RAY_I16:       fmt_puts(b, "0Nh"); return;
        case RAY_I32:       fmt_puts(b, "0Ni"); return;
        case RAY_DATE:      fmt_puts(b, "0Nd"); return;
        case RAY_TIME:      fmt_puts(b, "0Nt"); return;
        case RAY_I64:       fmt_puts(b, "0Nl"); return;
        case RAY_TIMESTAMP: fmt_puts(b, "0Np"); return;
        case RAY_F64:       fmt_puts(b, "0Nf"); return;
        case RAY_SYM:       fmt_puts(b, "0Ns"); return;
        default:            fmt_puts(b, "null"); return;
        }
    }

    switch (vec->type) {
    case RAY_BOOL:      fmt_bool(b, ((bool*)ray_data(vec))[idx]); break;
    case RAY_U8:        fmt_u8(b, ((uint8_t*)ray_data(vec))[idx]); break;
    case RAY_CHAR:      fmt_char(b, ((char*)ray_data(vec))[idx], 0); break;
    case RAY_I16:       fmt_i16(b, ((int16_t*)ray_data(vec))[idx]); break;
    case RAY_I32:       fmt_i32(b, ((int32_t*)ray_data(vec))[idx]); break;
    case RAY_I64:       fmt_i64(b, ((int64_t*)ray_data(vec))[idx]); break;
    case RAY_F64:       fmt_f64(b, ((double*)ray_data(vec))[idx]); break;
    case RAY_DATE:      fmt_date(b, ((int32_t*)ray_data(vec))[idx]); break;
    case RAY_TIME:      fmt_time(b, ((int32_t*)ray_data(vec))[idx]); break;
    case RAY_TIMESTAMP: fmt_timestamp(b, ((int64_t*)ray_data(vec))[idx]); break;
    case RAY_SYM: {
        int64_t sym_id = ray_read_sym(ray_data(vec), idx, vec->type, vec->attrs);
        fmt_sym(b, sym_id);
        break;
    }
    case RAY_STR: {
        size_t slen = 0;
        const char* p = ray_str_vec_get(vec, idx, &slen);
        if (p) {
            fmt_putc(b, '"');
            fmt_putn(b, p, (int32_t)slen);
            fmt_putc(b, '"');
        }
        break;
    }
    case RAY_GUID:
        fmt_guid(b, ((uint8_t*)ray_data(vec)) + idx * 16);
        break;
    default:
        fmt_puts(b, "?");
        break;
    }
}

/* ===== Vector formatter ===== */

static void fmt_vector(fmt_buf_t* b, ray_t* vec, int limit) {
    int64_t len = ray_len(vec);
    if (len == 0) { fmt_puts(b, "[]"); return; }

    fmt_puts(b, "[");
    int32_t start_len = b->len;

    for (int64_t i = 0; i < len; i++) {
        if (i > 0) fmt_putc(b, ' ');

        int32_t before = b->len;
        fmt_raw_elem(b, vec, i);

        /* Width limiting: check if we exceeded the limit */
        if (limit > 0 && (b->len - start_len) > limit) {
            /* Rewind to before this element and truncate */
            b->len = before;
            fmt_puts(b, "..]");
            return;
        }
    }

    fmt_puts(b, "]");
}

/* ===== List formatter ===== */

static void fmt_dict(fmt_buf_t* b, ray_t* dict, int mode);

static void fmt_list(fmt_buf_t* b, ray_t* list, int mode) {
    int64_t len = ray_len(list);
    if (len == 0) { fmt_puts(b, "()"); return; }

    /* Dict check */
    if (list->attrs & RAY_ATTR_DICT) {
        fmt_dict(b, list, mode);
        return;
    }

    /* Homogeneous atom list → format as vector [...] */
    ray_t** items = (ray_t**)ray_data(list);
    if (items && len > 0 && items[0] && !RAY_IS_ERR(items[0]) && ray_is_atom(items[0])) {
        int8_t first_type = items[0]->type;
        int homogeneous = 1;
        for (int64_t i = 1; i < len; i++) {
            if (!items[i] || RAY_IS_ERR(items[i]) || items[i]->type != first_type) {
                homogeneous = 0; break;
            }
        }
        if (homogeneous) {
            fmt_puts(b, "[");
            for (int64_t i = 0; i < len; i++) {
                if (i > 0) fmt_putc(b, ' ');
                fmt_obj(b, items[i], mode);
            }
            fmt_puts(b, "]");
            return;
        }
    }

    /* mode 0 = compact/round-trippable: "(list ...)" prefix required
     * mode 1 = REPL display: "(...)" matching rayforce 1 output */
    if (mode == 0)
        fmt_puts(b, "(list ");
    else
        fmt_puts(b, "(");

    int64_t max_elems = (mode == 1) ? FMT_LIST_MAX_HEIGHT : len;
    int64_t show = len < max_elems ? len : max_elems;

    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fmt_putc(b, ' ');
        ray_t* elem = ray_list_get(list, i);
        fmt_obj(b, elem, mode);
    }

    if (len > show) fmt_puts(b, " ..");
    fmt_puts(b, ")");
}

/* ===== Dict formatter ===== */

static void fmt_dict(fmt_buf_t* b, ray_t* dict, int mode) {
    int64_t len = ray_len(dict);
    int64_t npairs = len / 2;
    if (npairs == 0) { fmt_puts(b, "{}"); return; }

    int64_t max_pairs = (mode == 1) ? FMT_LIST_MAX_HEIGHT : npairs;
    int64_t show = npairs < max_pairs ? npairs : max_pairs;

    fmt_puts(b, "{");
    for (int64_t i = 0; i < show; i++) {
        if (i > 0) fmt_putc(b, ' ');
        ray_t* key = ray_list_get(dict, i * 2);
        ray_t* val = ray_list_get(dict, i * 2 + 1);
        fmt_obj(b, key, mode);
        fmt_putc(b, ':');
        fmt_obj(b, val, mode);
    }
    if (npairs > show) fmt_puts(b, " ..");
    fmt_puts(b, "}");
}

/* ===== Box-drawing glyphs (UTF-8) ===== */

#define G_TL "\xe2\x94\x8c"    /* ┌ */
#define G_TR "\xe2\x94\x90"    /* ┐ */
#define G_BL "\xe2\x94\x94"    /* └ */
#define G_BR "\xe2\x94\x98"    /* ┘ */
#define G_H  "\xe2\x94\x80"    /* ─ */
#define G_V  "\xe2\x94\x82"    /* │ */
#define G_TT "\xe2\x94\xac"    /* ┬ */
#define G_BT "\xe2\x94\xb4"    /* ┴ */
#define G_LT "\xe2\x94\x9c"    /* ├ */
#define G_RT "\xe2\x94\xa4"    /* ┤ */
#define G_X  "\xe2\x94\xbc"    /* ┼ */
#define G_HDOTS "\xe2\x80\xa6" /* … */
#define G_VDOTS "\xe2\x94\x86" /* ┆ */

/* ===== Table formatter helpers ===== */

static void fmt_centered(fmt_buf_t* b, const char* s, int32_t slen, int32_t width) {
    int32_t left  = (width - slen) / 2;
    int32_t right = width - slen - left;
    for (int32_t i = 0; i < left; i++)  fmt_putc(b, ' ');
    fmt_putn(b, s, slen);
    for (int32_t i = 0; i < right; i++) fmt_putc(b, ' ');
}

/* Maximum pre-formatted cells: FMT_TABLE_MAX_WIDTH * FMT_TABLE_MAX_HEIGHT = 200 */
#define FMT_CELL_BUF_SIZE 64

typedef struct {
    char    str[FMT_CELL_BUF_SIZE];
    int32_t len;
} fmt_cell_t;

static void fmt_table(fmt_buf_t* b, ray_t* tbl, int mode) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    /* Compact mode: round-trippable (table [names] (list col1 col2 ...)) */
    if (mode == 0) {
        fmt_puts(b, "(table [");
        for (int64_t i = 0; i < ncols; i++) {
            if (i > 0) fmt_putc(b, ' ');
            int64_t name_id = ray_table_col_name(tbl, i);
            ray_t* name_str = ray_sym_str(name_id);
            if (name_str && !RAY_IS_ERR(name_str)) {
                fmt_putn(b, ray_str_ptr(name_str), (int32_t)ray_str_len(name_str));
                ray_release(name_str);
            }
        }
        fmt_puts(b, "] (list ");
        for (int64_t i = 0; i < ncols; i++) {
            if (i > 0) fmt_putc(b, ' ');
            ray_t* col = ray_table_get_col_idx(tbl, i);
            if (col) {
                fmt_obj(b, col, mode);
            }
        }
        fmt_puts(b, "))");
        return;
    }

    /* Full mode (1) and show mode (2) */
    int64_t table_width  = ncols;
    int64_t table_height = nrows;

    if (mode == 1) {
        if (table_width > FMT_TABLE_MAX_WIDTH)
            table_width = FMT_TABLE_MAX_WIDTH;
        if (table_height > FMT_TABLE_MAX_HEIGHT)
            table_height = FMT_TABLE_MAX_HEIGHT;
    }

    if (table_width == 0) {
        fmt_puts(b, "<table>");
        return;
    }

    bool has_hidden_cols = (table_width < ncols);
    bool has_hidden_rows = (table_height < nrows);

    /* Allocate metadata arrays.  For mode 1 they fit on the stack
     * (max 10 cols x 20 rows).  For mode 2 we heap-allocate. */
    bool heap_alloc = (table_width > FMT_TABLE_MAX_WIDTH ||
                       table_height > FMT_TABLE_MAX_HEIGHT);

    int32_t     col_widths_stack[FMT_TABLE_MAX_WIDTH];
    const char* col_names_stack[FMT_TABLE_MAX_WIDTH];
    int32_t     col_name_lens_stack[FMT_TABLE_MAX_WIDTH];
    const char* col_types_stack[FMT_TABLE_MAX_WIDTH];
    int32_t     col_type_lens_stack[FMT_TABLE_MAX_WIDTH];
    ray_t*      name_refs_stack[FMT_TABLE_MAX_WIDTH];
    fmt_cell_t  cells_stack[FMT_TABLE_MAX_WIDTH * FMT_TABLE_MAX_HEIGHT];

    /* Heap-backed pointers (NULL when using stack) */
    ray_t* heap_widths_blk = NULL;
    ray_t* heap_names_blk  = NULL;
    ray_t* heap_nlen_blk   = NULL;
    ray_t* heap_types_blk  = NULL;
    ray_t* heap_tlen_blk   = NULL;
    ray_t* heap_refs_blk   = NULL;
    ray_t* heap_cells_blk  = NULL;

    int32_t*     col_widths;
    const char** col_names;
    int32_t*     col_name_lens;
    const char** col_types;
    int32_t*     col_type_lens;
    ray_t**      name_refs;
    fmt_cell_t*  cells;

    if (!heap_alloc) {
        col_widths    = col_widths_stack;
        col_names     = col_names_stack;
        col_name_lens = col_name_lens_stack;
        col_types     = col_types_stack;
        col_type_lens = col_type_lens_stack;
        name_refs     = name_refs_stack;
        cells         = cells_stack;
    } else {
        heap_widths_blk = ray_alloc((size_t)(table_width * (int64_t)sizeof(int32_t)));
        heap_names_blk  = ray_alloc((size_t)(table_width * (int64_t)sizeof(const char*)));
        heap_nlen_blk   = ray_alloc((size_t)(table_width * (int64_t)sizeof(int32_t)));
        heap_types_blk  = ray_alloc((size_t)(table_width * (int64_t)sizeof(const char*)));
        heap_tlen_blk   = ray_alloc((size_t)(table_width * (int64_t)sizeof(int32_t)));
        heap_refs_blk   = ray_alloc((size_t)(table_width * (int64_t)sizeof(ray_t*)));
        heap_cells_blk  = ray_alloc((size_t)(table_width * table_height * (int64_t)sizeof(fmt_cell_t)));

        col_widths    = (int32_t*)ray_data(heap_widths_blk);
        col_names     = (const char**)ray_data(heap_names_blk);
        col_name_lens = (int32_t*)ray_data(heap_nlen_blk);
        col_types     = (const char**)ray_data(heap_types_blk);
        col_type_lens = (int32_t*)ray_data(heap_tlen_blk);
        name_refs     = (ray_t**)ray_data(heap_refs_blk);
        cells         = (fmt_cell_t*)ray_data(heap_cells_blk);
    }

    /* Pre-format cells and calculate column widths */
    for (int64_t ci = 0; ci < table_width; ci++) {
        /* Column name */
        int64_t name_id = ray_table_col_name(tbl, ci);
        ray_t* name_str = ray_sym_str(name_id);
        name_refs[ci] = name_str;
        if (name_str && !RAY_IS_ERR(name_str)) {
            col_names[ci]     = ray_str_ptr(name_str);
            col_name_lens[ci] = (int32_t)ray_str_len(name_str);
        } else {
            col_names[ci]     = "?";
            col_name_lens[ci] = 1;
            name_refs[ci]     = NULL;
        }

        /* Column type */
        ray_t* col_vec = ray_table_get_col_idx(tbl, ci);
        const char* tname = ray_type_name(col_vec ? col_vec->type : 0);
        col_types[ci]     = tname;
        col_type_lens[ci] = (int32_t)strlen(tname);

        /* Start with max of name and type lengths */
        int32_t max_w = col_name_lens[ci];
        if (col_type_lens[ci] > max_w) max_w = col_type_lens[ci];

        int64_t col_len = col_vec ? ray_len(col_vec) : 0;

        /* Format first half (head rows) */
        int64_t half = table_height / 2;
        for (int64_t ri = 0; ri < half; ri++) {
            fmt_cell_t* cell = &cells[ci * table_height + ri];
            if (ri < col_len) {
                fmt_buf_t tmp;
                fmt_init(&tmp);
                fmt_raw_elem(&tmp, col_vec, ri);
                int32_t clen = tmp.len < FMT_CELL_BUF_SIZE - 1 ? tmp.len : FMT_CELL_BUF_SIZE - 1;
                memcpy(cell->str, tmp.buf, (size_t)clen);
                cell->str[clen] = '\0';
                cell->len = clen;
                fmt_destroy(&tmp);
            } else {
                memcpy(cell->str, "NA", 3);
                cell->len = 2;
            }
            if (cell->len > max_w) max_w = cell->len;
        }

        /* Format second half (tail rows) */
        for (int64_t ri = half; ri < table_height; ri++) {
            fmt_cell_t* cell = &cells[ci * table_height + ri];
            int64_t src_idx;
            if (table_height == col_len || !has_hidden_rows) {
                src_idx = ri;
            } else {
                src_idx = col_len - table_height + ri;
            }
            if (src_idx >= 0 && src_idx < col_len) {
                fmt_buf_t tmp;
                fmt_init(&tmp);
                fmt_raw_elem(&tmp, col_vec, src_idx);
                int32_t clen = tmp.len < FMT_CELL_BUF_SIZE - 1 ? tmp.len : FMT_CELL_BUF_SIZE - 1;
                memcpy(cell->str, tmp.buf, (size_t)clen);
                cell->str[clen] = '\0';
                cell->len = clen;
                fmt_destroy(&tmp);
            } else {
                memcpy(cell->str, "NA", 3);
                cell->len = 2;
            }
            if (cell->len > max_w) max_w = cell->len;
        }

        col_widths[ci] = max_w + 2; /* +2 for padding (1 space each side) */
    }

    /* Calculate total width (sum of col widths + separators between columns) */
    int32_t total_width = 0;
    for (int64_t ci = 0; ci < table_width; ci++)
        total_width += col_widths[ci];
    total_width += (int32_t)(table_width - 1); /* separators between columns */

    /* Format footer to check if we need to widen the last column */
    char footer[128];
    int footer_len = snprintf(footer, sizeof(footer),
        " %" PRId64 " rows (%" PRId64 " shown) %" PRId64 " columns (%" PRId64 " shown)",
        nrows, table_height, ncols, table_width);

    if (total_width < footer_len) {
        col_widths[table_width - 1] += footer_len - total_width;
        total_width = footer_len;
    }

    /* Extra width for hidden columns indicator */
    if (has_hidden_cols)
        total_width += 4; /* "───┐" or " … │" */

    /* === Render === */

    /* 1. Top border: ┌──┬──┐ */
    fmt_puts(b, G_TL);
    for (int64_t ci = 0; ci < table_width; ci++) {
        for (int32_t j = 0; j < col_widths[ci]; j++)
            fmt_puts(b, G_H);
        if (ci < table_width - 1)
            fmt_puts(b, G_TT);
        else if (has_hidden_cols)
            fmt_puts(b, G_TT);
        else
            fmt_puts(b, G_TR);
    }
    if (has_hidden_cols) {
        fmt_puts(b, G_H G_H G_H G_TR);
    }

    /* 2. Header row: │ name │ (centered) */
    fmt_putc(b, '\n');
    fmt_puts(b, G_V);
    for (int64_t ci = 0; ci < table_width; ci++) {
        fmt_centered(b, col_names[ci], col_name_lens[ci], col_widths[ci]);
        fmt_puts(b, G_V);
    }
    if (has_hidden_cols) {
        fmt_puts(b, " " G_HDOTS " " G_V);
    }

    /* 3. Type row: │ type │ (centered) */
    fmt_putc(b, '\n');
    fmt_puts(b, G_V);
    for (int64_t ci = 0; ci < table_width; ci++) {
        fmt_centered(b, col_types[ci], col_type_lens[ci], col_widths[ci]);
        fmt_puts(b, G_V);
    }
    if (has_hidden_cols) {
        fmt_puts(b, " " G_HDOTS " " G_V);
    }

    /* 4. Separator: ├──┼──┤ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_LT);
    for (int64_t ci = 0; ci < table_width; ci++) {
        for (int32_t j = 0; j < col_widths[ci]; j++)
            fmt_puts(b, G_H);
        if (ci < table_width - 1)
            fmt_puts(b, G_X);
        else if (has_hidden_cols)
            fmt_puts(b, G_X);
        else
            fmt_puts(b, G_RT);
    }
    if (has_hidden_cols) {
        fmt_puts(b, G_H G_H G_H G_RT);
    }

    /* 5. Data rows */
    int64_t half = table_height / 2;
    for (int64_t ri = 0; ri < table_height; ri++) {
        fmt_putc(b, '\n');

        /* 6. Truncation indicator row between head and tail */
        if (has_hidden_rows && ri == half) {
            fmt_puts(b, G_VDOTS);
            for (int64_t ci = 0; ci < table_width; ci++) {
                /* Center the ellipsis (3 bytes, 1 display char) */
                int32_t left  = (col_widths[ci] - 1) / 2;
                int32_t right = col_widths[ci] - 1 - left;
                for (int32_t p = 0; p < left; p++)  fmt_putc(b, ' ');
                fmt_puts(b, G_HDOTS);
                for (int32_t p = 0; p < right; p++) fmt_putc(b, ' ');
                fmt_puts(b, G_VDOTS);
            }
            if (has_hidden_cols) {
                fmt_puts(b, " " G_HDOTS " " G_VDOTS);
            }
            fmt_putc(b, '\n');
        }

        /* Data row: │ val │ (left-aligned with 1-space padding) */
        fmt_puts(b, G_V);
        for (int64_t ci = 0; ci < table_width; ci++) {
            fmt_cell_t* cell = &cells[ci * table_height + ri];
            fmt_putc(b, ' ');
            fmt_putn(b, cell->str, cell->len);
            int32_t pad = col_widths[ci] - cell->len - 1;
            for (int32_t p = 0; p < pad; p++)
                fmt_putc(b, ' ');
            fmt_puts(b, G_V);
        }
        if (has_hidden_cols) {
            fmt_puts(b, " " G_HDOTS " " G_V);
        }
    }

    /* 7. Bottom border (separator before footer): ├──┴──┤ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_LT);
    for (int64_t ci = 0; ci < table_width; ci++) {
        for (int32_t j = 0; j < col_widths[ci]; j++)
            fmt_puts(b, G_H);
        if (ci < table_width - 1)
            fmt_puts(b, G_BT);
        else if (has_hidden_cols)
            fmt_puts(b, G_BT);
        else
            fmt_puts(b, G_RT);
    }
    if (has_hidden_cols) {
        fmt_puts(b, G_H G_H G_H G_RT);
    }

    /* 8. Footer row: │ N rows (M shown) C columns (K shown) │ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_V);
    fmt_putn(b, footer, footer_len);
    for (int32_t i = footer_len; i < total_width; i++)
        fmt_putc(b, ' ');
    fmt_puts(b, G_V);

    /* Final bottom border: └───┘ */
    fmt_putc(b, '\n');
    fmt_puts(b, G_BL);
    for (int32_t i = 0; i < total_width; i++)
        fmt_puts(b, G_H);
    fmt_puts(b, G_BR);

    /* Release name string refs */
    for (int64_t ci = 0; ci < table_width; ci++) {
        if (name_refs[ci]) ray_release(name_refs[ci]);
    }

    /* Free heap allocations if used */
    if (heap_alloc) {
        ray_free(heap_widths_blk);
        ray_free(heap_names_blk);
        ray_free(heap_nlen_blk);
        ray_free(heap_types_blk);
        ray_free(heap_tlen_blk);
        ray_free(heap_refs_blk);
        ray_free(heap_cells_blk);
    }
}

/* ===== Core dispatch ===== */

static void fmt_obj(fmt_buf_t* b, ray_t* obj, int mode) {
    if (!obj) { fmt_puts(b, "null"); return; }
    if (RAY_IS_ERR(obj)) {
        ray_err_t code = RAY_ERR_CODE(obj);
        fmt_puts(b, "error: ");
        fmt_puts(b, ray_err_str(code));
        return;
    }

    int8_t type = obj->type;
    if (type < 0) {
        /* Atom: type is negated */
        switch (-type) {
        case RAY_BOOL: fmt_bool(b, obj->b8); break;
        case RAY_U8:   fmt_u8(b, obj->u8); break;
        case RAY_CHAR: fmt_char(b, obj->c8, mode > 0); break;
        case RAY_I16:  fmt_i16(b, obj->i16); break;
        case RAY_I32:  fmt_i32(b, obj->i32); break;
        case RAY_I64:  fmt_i64(b, obj->i64); break;
        case RAY_F64:       fmt_f64(b, obj->f64); break;
        case RAY_DATE:      fmt_date(b, obj->i32); break;
        case RAY_TIME:      fmt_time(b, obj->i32); break;
        case RAY_TIMESTAMP: fmt_timestamp(b, obj->i64); break;
        case RAY_SYM:  fmt_sym(b, obj->i64); break;
        case RAY_STR:  fmt_str_atom(b, obj, mode > 0); break;
        case RAY_GUID: fmt_guid(b, obj->obj ? (const uint8_t*)ray_data(obj->obj) : (const uint8_t*)ray_data(obj)); break;
        default:       fmt_puts(b, "?"); break;
        }
    } else if (ray_is_vec(obj)) {
        int limit = (mode == 1) ? g_row_width : -1;
        fmt_vector(b, obj, limit);
    } else if (type == RAY_LIST) {
        fmt_list(b, obj, mode);
    } else if (type == RAY_TABLE) {
        fmt_table(b, obj, mode);
    } else if (type == RAY_DICT) {
        fmt_dict(b, obj, mode);
    } else if (type == RAY_LAMBDA) {
        fmt_puts(b, "lambda");
    } else if (type == RAY_UNARY) {
        fmt_puts(b, "builtin/1");
    } else if (type == RAY_BINARY) {
        fmt_puts(b, "builtin/2");
    } else if (type == RAY_VARY) {
        fmt_puts(b, "builtin/n");
    } else if (type == RAY_LAZY) {
        ray_t* concrete = ray_lazy_materialize(obj);
        fmt_obj(b, concrete, mode);
        return;
    } else {
        fmt_printf(b, "<%s>", ray_type_name(type));
    }
}

ray_t* ray_fmt(ray_t* obj, int mode) {
    fmt_buf_t b;
    fmt_init(&b);
    fmt_obj(&b, obj, mode);
    return fmt_to_str(&b);
}

void ray_fmt_print(FILE* fp, ray_t* obj, int mode) {
    ray_t* s = ray_fmt(obj, mode);
    if (s) {
        fwrite(ray_str_ptr(s), 1, ray_str_len(s), fp);
        ray_release(s);
    }
}
