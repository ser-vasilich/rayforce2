#include "app/format.h"
#include "table/sym.h"
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
    if (!full) {
        if (val) fmt_putc(b, val);
        return;
    }
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
    if (val == -0.0 && signbit(val)) {
        fmt_printf(b, "%.*f", g_precision, 0.0);
        return;
    }
    double absval = val < 0 ? -val : val;
    double order = log10(absval);
    if (val != 0.0 && (order > 6 || order < -1))
        fmt_printf(b, "%.*e", g_precision, val);
    else
        fmt_printf(b, "%.*f", g_precision, val);
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

static void fmt_str_atom(fmt_buf_t* b, ray_t* obj, int full) {
    const char* p = ray_str_ptr(obj);
    size_t      n = ray_str_len(obj);
    if (full) {
        fmt_putc(b, '"');
        fmt_putn(b, p, (int32_t)n);
        fmt_putc(b, '"');
    } else {
        fmt_putn(b, p, (int32_t)n);
    }
}

ray_t* ray_fmt(ray_t* obj, int mode) {
    if (!obj) return ray_str("null", 4);
    if (RAY_IS_ERR(obj)) {
        fmt_buf_t b;
        fmt_init(&b);
        ray_err_t code = RAY_ERR_CODE(obj);
        fmt_puts(&b, "error: ");
        fmt_puts(&b, ray_err_str(code));
        return fmt_to_str(&b);
    }

    (void)g_row_width;

    fmt_buf_t b;
    fmt_init(&b);

    int8_t type = obj->type;
    if (type < 0) {
        /* Atom: type is negated */
        switch (-type) {
        case RAY_BOOL: fmt_bool(&b, obj->b8); break;
        case RAY_U8:   fmt_u8(&b, obj->u8); break;
        case RAY_CHAR: fmt_char(&b, obj->c8, mode > 0); break;
        case RAY_I16:  fmt_i16(&b, obj->i16); break;
        case RAY_I32:  fmt_i32(&b, obj->i32); break;
        case RAY_I64:  fmt_i64(&b, obj->i64); break;
        case RAY_F64:  fmt_f64(&b, obj->f64); break;
        case RAY_SYM:  fmt_sym(&b, obj->i64); break;
        case RAY_STR:  fmt_str_atom(&b, obj, mode > 0); break;
        default:       fmt_puts(&b, "?"); break;
        }
    } else {
        fmt_puts(&b, "<todo>"); /* vectors/tables later */
    }

    return fmt_to_str(&b);
}

void ray_fmt_print(FILE* fp, ray_t* obj, int mode) {
    ray_t* s = ray_fmt(obj, mode);
    if (s) {
        const char* p = (const char*)ray_data(s);
        int64_t     n = s->len;
        fwrite(p, 1, (size_t)n, fp);
        ray_release(s);
    }
}
