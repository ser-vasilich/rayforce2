#include "app/format.h"
#include "mem/heap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

__attribute__((unused))
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

__attribute__((unused))
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

ray_t* ray_fmt(ray_t* obj, int mode) {
    (void)obj;
    (void)mode;
    (void)g_precision;
    (void)g_row_width;
    fmt_buf_t b;
    fmt_init(&b);
    fmt_puts(&b, "<todo>");
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
