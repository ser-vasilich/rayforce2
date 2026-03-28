#include "lang/parse.h"
#include "lang/env.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>

/* ══════════════════════════════════════════
 * ASCII dispatch table (128 bytes)
 * Single indexed read: PA(c) — zero branches.
 * ══════════════════════════════════════════ */

#define PA_ERR     0
#define PA_DIGIT   1
#define PA_ALPHA   2
#define PA_STRING  3
#define PA_QUOTE   4    /* ' symbol prefix */
#define PA_LPAREN  5
#define PA_RPAREN  6
#define PA_LBRACK  7
#define PA_RBRACK  8
#define PA_LBRACE  9
#define PA_RBRACE  10
#define PA_COLON   11
#define PA_WS      12
#define PA_END     13
#define PA_MINUS   14
#define PA_SEMI    15   /* ; comment */

static const char _PA[128] =
/*  NUL                              \t \n                         */
    "\x0d\x00\x00\x00\x00\x00\x00\x00\x00\x0c\x0c\x00\x00\x0c\x00\x00"
/*                                                                  */
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
/*  SP   !    "    #    $    %    &    '    (    )    *    +    ,    -    .    /  */
    "\x0c\x02\x03\x02\x02\x02\x02\x04\x05\x06\x02\x02\x02\x0e\x02\x02"
/*  0    1    2    3    4    5    6    7    8    9    :    ;    <    =    >    ?  */
    "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x0b\x0f\x02\x02\x02\x02"
/*  @    A    B    C    D    E    F    G    H    I    J    K    L    M    N    O  */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02"
/*  P    Q    R    S    T    U    V    W    X    Y    Z    [    \    ]    ^    _  */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x07\x00\x08\x02\x02"
/*  `    a    b    c    d    e    f    g    h    i    j    k    l    m    n    o  */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02"
/*  p    q    r    s    t    u    v    w    x    y    z    {    |    }    ~   DEL */
    "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x09\x02\x0a\x02\x00";

#define PA(c) ((unsigned char)(c) < 128 ? (int)(unsigned char)_PA[(unsigned char)(c)] : PA_ERR)

/* ══════════════════════════════════════════
 * Parser state
 * ══════════════════════════════════════════ */

typedef struct {
    const char *src;
    const char *pos;
} ray_parser_t;

static void skip_ws_and_comments(ray_parser_t *p) {
    for (;;) {
        while (*p->pos == ' ' || *p->pos == '\t' || *p->pos == '\n' || *p->pos == '\r')
            p->pos++;
        if (*p->pos == ';') {
            while (*p->pos && *p->pos != '\n') p->pos++;
            continue;
        }
        break;
    }
}

/* Forward declarations */
static ray_t* parse_expr(ray_parser_t *p);

/* ── Date/time/timestamp helpers for parser ── */

static const uint32_t PARSE_MONTHDAYS[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

static int parse_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int32_t parse_years_by_days(int yy) {
    return (int32_t)((int64_t)yy * 365 + yy / 4 - yy / 100 + yy / 400);
}

#define PARSE_DATE_EPOCH 2000

static int32_t parse_ymd_to_days(int year, int month, int day) {
    int yy = (year > 0) ? year - 1 : 0;
    int32_t ydays = parse_years_by_days(yy);
    int leap = parse_leap_year(year);
    int mm = (month > 0) ? month - 1 : 0;
    int32_t mdays = (int32_t)PARSE_MONTHDAYS[leap][mm];
    return ydays - parse_years_by_days(PARSE_DATE_EPOCH - 1) + mdays + day - 1;
}

#define PARSE_NSECS_IN_DAY ((int64_t)24 * 60 * 60 * 1000000000LL)

/* Try to parse a time literal starting from 'start'.
 * Returns the char past the end on success, NULL on failure.
 * Writes the millisecond value into *ms_out, including sign. */
static const char* try_parse_time(const char* start, int32_t *ms_out) {
    const char* c = start;
    int sign = 1;
    if (*c == '-') { sign = -1; c++; }

    /* HH */
    if (!(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9')) return NULL;
    int hh = (c[0] - '0') * 10 + (c[1] - '0'); c += 2;
    if (*c != ':') return NULL; c++;

    /* MM */
    if (!(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9')) return NULL;
    int mm = (c[0] - '0') * 10 + (c[1] - '0'); c += 2;
    if (*c != ':') return NULL; c++;

    /* SS */
    if (!(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9')) return NULL;
    int ss = (c[0] - '0') * 10 + (c[1] - '0'); c += 2;

    /* .mmm (milliseconds) */
    int ms = 0;
    if (*c == '.') {
        c++;
        if (!(*c >= '0' && *c <= '9')) return NULL;
        ms = (*c - '0'); c++;
        if (*c >= '0' && *c <= '9') { ms = ms * 10 + (*c - '0'); c++; }
        if (*c >= '0' && *c <= '9') { ms = ms * 10 + (*c - '0'); c++; }
    }

    *ms_out = sign * (int32_t)((hh * 3600 + mm * 60 + ss) * 1000 + ms);
    return c;
}

/* ── Number parsing (with hex, nulls, typed suffixes, date/time/timestamp) ── */
static ray_t* parse_number(ray_parser_t *p) {
    const char *start = p->pos;
    int is_neg = 0;
    if (*p->pos == '-') { is_neg = 1; p->pos++; }

    /* Hex literal: 0x.. */
    if (p->pos[0] == '0' && p->pos[1] == 'x') {
        p->pos += 2;
        char *end;
        unsigned long v = strtoul(p->pos, &end, 16);
        if (end == p->pos) return RAY_ERR_PTR(RAY_ERR_PARSE);
        p->pos = end;
        return ray_u8((uint8_t)v);
    }

    /* Null literal: 0N{h,i,d,t,p,l,f,s} */
    if (!is_neg && p->pos[0] == '0' && p->pos[1] == 'N') {
        switch (p->pos[2]) {
        case 'h': p->pos += 3; return ray_i16(INT16_MIN);
        case 'i': p->pos += 3; return ray_i32(INT32_MIN);
        case 'd': p->pos += 3; return ray_date(INT32_MIN);
        case 't': p->pos += 3; return ray_time(INT32_MIN);
        case 'p': p->pos += 3; return ray_timestamp(INT64_MIN);
        case 'l': p->pos += 3; return ray_i64(INT64_MIN);
        case 'f': p->pos += 3; return ray_f64(__builtin_nan(""));
        case 's': p->pos += 3; { ray_t* s = ray_sym(INT64_MIN); return s; }
        }
    }

    /* Scan digits */
    const char *dstart = p->pos;
    while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    int ndigits = (int)(p->pos - dstart);

    /* Date/Timestamp: YYYY.MM.DD or YYYY.MM.DDDhh:mm:ss.nnnnnnnnn */
    if (ndigits == 4 && !is_neg && *p->pos == '.' &&
        p->pos[1] >= '0' && p->pos[1] <= '9' &&
        p->pos[2] >= '0' && p->pos[2] <= '9' &&
        p->pos[3] == '.') {
        int year = (int)strtol(dstart, NULL, 10);
        p->pos++; /* skip first '.' */
        int month = (p->pos[0] - '0') * 10 + (p->pos[1] - '0');
        p->pos += 2;
        if (*p->pos != '.') { p->pos = start; goto plain_number; }
        p->pos++; /* skip second '.' */
        if (!(p->pos[0] >= '0' && p->pos[0] <= '9' &&
              p->pos[1] >= '0' && p->pos[1] <= '9')) {
            p->pos = start; goto plain_number;
        }
        int day = (p->pos[0] - '0') * 10 + (p->pos[1] - '0');
        p->pos += 2;

        int32_t days = parse_ymd_to_days(year, month, day);

        /* Check for timestamp separator 'D' */
        if (*p->pos == 'D') {
            p->pos++; /* skip D */
            /* Parse HH:MM:SS.nnnnnnnnn */
            if (!(p->pos[0] >= '0' && p->pos[0] <= '9' &&
                  p->pos[1] >= '0' && p->pos[1] <= '9'))
                return RAY_ERR_PTR(RAY_ERR_PARSE);
            int hh = (p->pos[0] - '0') * 10 + (p->pos[1] - '0'); p->pos += 2;
            if (*p->pos != ':') return RAY_ERR_PTR(RAY_ERR_PARSE);
            p->pos++;
            int mi = (p->pos[0] - '0') * 10 + (p->pos[1] - '0'); p->pos += 2;
            if (*p->pos != ':') return RAY_ERR_PTR(RAY_ERR_PARSE);
            p->pos++;
            int ss = (p->pos[0] - '0') * 10 + (p->pos[1] - '0'); p->pos += 2;
            if (*p->pos != '.') return RAY_ERR_PTR(RAY_ERR_PARSE);
            p->pos++;
            /* Parse fractional seconds (up to 9 digits for nanoseconds) */
            const char* fstart = p->pos;
            while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
            int flen = (int)(p->pos - fstart);
            uint64_t nanos = 0;
            for (int i = 0; i < flen && i < 9; i++)
                nanos = nanos * 10 + (uint64_t)(fstart[i] - '0');
            /* Pad to 9 digits */
            for (int i = flen; i < 9; i++) nanos *= 10;

            int64_t day_ns = (int64_t)days * PARSE_NSECS_IN_DAY;
            int64_t time_ns = ((int64_t)hh * 3600 + mi * 60 + ss) * 1000000000LL + (int64_t)nanos;
            return ray_timestamp(day_ns + time_ns);
        }

        return ray_date(days);
    }

    /* Time literal: HH:MM:SS.mmm (detected by colon after 2 digits from digit-start) */
    if (ndigits == 2 && *p->pos == ':') {
        p->pos = start; /* reset — let try_parse_time handle sign */
        int32_t ms;
        const char* end = try_parse_time(start, &ms);
        if (end) { p->pos = end; return ray_time(ms); }
        /* Not a valid time — fall through to regular number parsing */
        p->pos = start;
        if (is_neg) p->pos++;
        while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    }

plain_number:;
    /* At this point p->pos is past the digits. Check for float */
    int is_float = 0;
    if (*p->pos == '.' && p->pos[1] >= '0' && p->pos[1] <= '9') {
        is_float = 1;
        p->pos++;
        while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    }
    if (*p->pos == 'e' || *p->pos == 'E') {
        is_float = 1;
        p->pos++;
        if (*p->pos == '+' || *p->pos == '-') p->pos++;
        while (*p->pos >= '0' && *p->pos <= '9') p->pos++;
    }

    if (is_float) {
        double v = strtod(start, NULL);
        return ray_f64(v);
    }

    /* Check for integer overflow → promote to f64 */
    errno = 0;
    char* endp;
    int64_t v = strtoll(start, &endp, 10);
    if (errno == ERANGE) {
        double fv = strtod(start, NULL);
        return ray_f64(fv);
    }

    /* Type suffix: h (i16), i (i32) */
    if (*p->pos == 'h') {
        p->pos++;
        if (v < -32767 || v > 32767) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        return ray_i16((int16_t)v);
    }
    if (*p->pos == 'i') {
        p->pos++;
        if (v < -2147483647LL || v > 2147483647LL) return RAY_ERR_PTR(RAY_ERR_DOMAIN);
        return ray_i32((int32_t)v);
    }

    return ray_i64(v);
}

/* ── String parsing with escape sequence decoding ── */
static ray_t* parse_string(ray_parser_t *p) {
    p->pos++; /* skip opening " */
    const char *start = p->pos;

    /* First pass: scan for closing " and check for escapes */
    bool has_escape = false;
    const char *scan = p->pos;
    while (*scan && *scan != '"') {
        if (*scan == '\\' && scan[1]) { has_escape = true; scan++; }
        scan++;
    }
    size_t raw_len = (size_t)(scan - start);
    if (*scan != '"') return RAY_ERR_PTR(RAY_ERR_PARSE); /* unterminated string */
    scan++;
    p->pos = scan;

    if (!has_escape) return ray_str(start, raw_len);

    /* Decode escape sequences into a temporary buffer */
    char buf[4096];
    size_t out = 0;
    const char *r = start;
    const char *end = start + raw_len;
    while (r < end) {
        if (out >= sizeof(buf) - 2)
            return RAY_ERR_PTR(RAY_ERR_DOMAIN);  /* string too long for escape buffer */
        if (*r == '\\' && r + 1 < end) {
            r++;
            switch (*r) {
            case 'n':  buf[out++] = '\n'; r++; break;
            case 't':  buf[out++] = '\t'; r++; break;
            case 'r':  buf[out++] = '\r'; r++; break;
            case '\\': buf[out++] = '\\'; r++; break;
            case '"':  buf[out++] = '"';  r++; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                /* Octal escape: \OOO (1-3 digits) */
                char ch = (char)(*r - '0'); r++;
                if (r < end && *r >= '0' && *r <= '7') {
                    ch = (char)((ch << 3) | (*r - '0')); r++;
                    if (r < end && *r >= '0' && *r <= '7') {
                        ch = (char)((ch << 3) | (*r - '0')); r++;
                    }
                }
                buf[out++] = ch;
                break;
            }
            default:   buf[out++] = '\\'; buf[out++] = *r; r++; break;
            }
        } else {
            buf[out++] = *r++;
        }
    }
    return ray_str(buf, out);
}

/* ── Symbol/char parsing: 'name or 'a' ── */
static ray_t* parse_symbol(ray_parser_t *p) {
    p->pos++; /* skip ' */
    const char *start = p->pos;

    /* Empty symbol (bare tick at end or before terminator) */
    if (*p->pos == 0 || *p->pos == ' ' || *p->pos == '\t' || *p->pos == '\n' ||
        *p->pos == ')' || *p->pos == ']' || *p->pos == '}') {
        /* Null symbol 0Ns */
        return ray_sym(INT64_MIN);
    }

    /* Char literal: 'X' or '\n' etc. */
    if (*p->pos == '\\') {
        /* Escape sequence char literal */
        const char *esc = p->pos + 1;
        char ch;
        int esc_len = 1;
        switch (*esc) {
        case 'n':  ch = '\n'; break;
        case 'r':  ch = '\r'; break;
        case 't':  ch = '\t'; break;
        case '\\': ch = '\\'; break;
        case '\'': ch = '\''; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* Octal escape: \OOO */
            ch = (char)(*esc - '0');
            if (esc[1] >= '0' && esc[1] <= '7') {
                ch = (char)((ch << 3) | (esc[1] - '0'));
                if (esc[2] >= '0' && esc[2] <= '7') {
                    ch = (char)((ch << 3) | (esc[2] - '0'));
                    esc_len = 3;
                } else {
                    esc_len = 2;
                }
            }
            break;
        }
        default: ch = *esc; break;
        }
        if (esc[esc_len] == '\'') {
            /* Closing quote found — it's a char literal */
            p->pos = esc + esc_len + 1;
            return ray_char(ch);
        }
        /* Not a char literal — fall through to symbol parsing */
    } else if (start[1] == '\'') {
        /* Simple char literal like 'a' */
        char ch = *start;
        p->pos = start + 2; /* skip char + closing quote */
        return ray_char(ch);
    }

    /* Regular symbol */
    while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT || *p->pos == '_' || *p->pos == '.')
        p->pos++;
    size_t len = (size_t)(p->pos - start);
    if (len == 0) return ray_sym(INT64_MIN); /* empty symbol */
    int64_t id = ray_sym_intern(start, len);
    return ray_sym(id);
}

/* ── Name parsing ── */
static ray_t* parse_name(ray_parser_t *p) {
    const char *start = p->pos;
    /* Name chars: alpha, digit, _, ., -, !, ?, +, *, /, %, <, >, =, & */
    while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT
           || *p->pos == '_' || *p->pos == '.' || *p->pos == '-'
           || *p->pos == '!' || *p->pos == '?' || *p->pos == '+'
           || *p->pos == '*' || *p->pos == '/' || *p->pos == '%'
           || *p->pos == '<' || *p->pos == '>' || *p->pos == '='
           || *p->pos == '&' || *p->pos == '|')
        p->pos++;
    size_t len = (size_t)(p->pos - start);
    if (len == 0) return RAY_ERR_PTR(RAY_ERR_PARSE);

    /* Check for true/false */
    if (len == 4 && memcmp(start, "true", 4) == 0)  return ray_bool(true);
    if (len == 5 && memcmp(start, "false", 5) == 0) return ray_bool(false);
    /* null is handled as a name that resolves to NULL at eval time */

    /* Return as name symbol (with RAY_ATTR_NAME flag) */
    int64_t id = ray_sym_intern(start, len);
    ray_t* s = ray_sym(id);
    if (!RAY_IS_ERR(s)) s->attrs |= RAY_ATTR_NAME;
    return s;
}

/* ── Vector literal: [1 2 3] ── */
static ray_t* parse_vector(ray_parser_t *p) {
    p->pos++; /* skip [ */

    /* Collect parsed elements into a temporary array */
    ray_t* elems[4096];
    int32_t count = 0;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ']') {
        if (count >= 4096) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return RAY_ERR_PTR(RAY_ERR_LIMIT);
        }
        ray_t* elem = parse_expr(p);
        if (RAY_IS_ERR(elem)) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return elem;
        }
        elems[count++] = elem;
        skip_ws_and_comments(p);
    }
    if (*p->pos != ']') {
        for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
        return RAY_ERR_PTR(RAY_ERR_PARSE);
    }
    p->pos++;

    if (count == 0) {
        /* Empty vector -> empty i64 vector */
        return ray_vec_new(RAY_I64, 0);
    }

    /* Determine element types.
     * Name references (RAY_ATTR_NAME) must stay as boxed atoms because
     * the evaluator, compiler, and fn-builder dereference them as ray_t*. */
    int8_t first_type = elems[0]->type;
    bool homogeneous = true;
    bool has_float = (first_type == -RAY_F64);
    bool has_int   = (first_type == -RAY_I64);
    bool all_numeric = (first_type == -RAY_I64 || first_type == -RAY_F64);

    for (int32_t i = 0; i < count; i++) {
        /* Inside [...], names are symbol literals, not variable references */
        if (elems[i]->attrs & RAY_ATTR_NAME) {
            elems[i]->attrs &= ~RAY_ATTR_NAME;
            /* type is already -RAY_SYM from parse_expr */
        }
        if (i == 0) continue;
        int8_t t = elems[i]->type;
        if (t != first_type) homogeneous = false;
        if (t == -RAY_F64)      has_float = true;
        else if (t == -RAY_I64) has_int = true;
        if (t != -RAY_I64 && t != -RAY_F64) all_numeric = false;
    }

    /* All same atom type -> typed vector */
    if (homogeneous && first_type < 0) {
        int8_t vec_type = -first_type;
        ray_t* vec = ray_vec_new(vec_type, count);
        if (RAY_IS_ERR(vec)) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return vec;
        }
        switch (vec_type) {
            case RAY_I64: case RAY_TIMESTAMP: {
                int64_t* d = (int64_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i64;
                break;
            }
            case RAY_F64: {
                double* d = (double*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->f64;
                break;
            }
            case RAY_I32: case RAY_DATE: case RAY_TIME: {
                int32_t* d = (int32_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i32;
                break;
            }
            case RAY_I16: {
                int16_t* d = (int16_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i16;
                break;
            }
            case RAY_BOOL: {
                bool* d = (bool*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->b8;
                break;
            }
            case RAY_SYM: {
                int64_t* d = (int64_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->i64;
                break;
            }
            case RAY_U8: {
                uint8_t* d = (uint8_t*)ray_data(vec);
                for (int32_t i = 0; i < count; i++) d[i] = elems[i]->u8;
                break;
            }
            case RAY_STR: {
                /* String vectors use ray_str_vec_append */
                ray_t* svec = ray_vec_new(RAY_STR, count);
                if (RAY_IS_ERR(svec)) {
                    ray_free(vec);
                    for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
                    return svec;
                }
                for (int32_t i = 0; i < count; i++) {
                    const char* s = ray_str_ptr(elems[i]);
                    size_t slen = ray_str_len(elems[i]);
                    svec = ray_str_vec_append(svec, s, slen);
                    if (RAY_IS_ERR(svec)) {
                        for (int32_t j = i; j < count; j++) ray_release(elems[j]);
                        return svec;
                    }
                }
                ray_free(vec);
                for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
                return svec;
            }
            default: goto boxed_list;
        }
        vec->len = count;
        for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
        return vec;
    }

    /* Mixed int/float -> promote to f64 */
    if (has_float && has_int && all_numeric) {
        ray_t* vec = ray_vec_new(RAY_F64, count);
        if (RAY_IS_ERR(vec)) {
            for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
            return vec;
        }
        double* d = (double*)ray_data(vec);
        for (int32_t i = 0; i < count; i++) {
            d[i] = (elems[i]->type == -RAY_F64) ? elems[i]->f64
                                                 : (double)elems[i]->i64;
        }
        vec->len = count;
        for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
        return vec;
    }

boxed_list:
    /* Mixed types in vector literal — domain error */
    for (int32_t i = 0; i < count; i++) ray_release(elems[i]);
    return RAY_ERR_PTR(RAY_ERR_DOMAIN);
}

/* ── Dict literal: {key: val key: val ...} ── */
static ray_t* parse_dict(ray_parser_t *p) {
    p->pos++; /* skip { */
    ray_t* list = ray_list_new(8);
    if (RAY_IS_ERR(list)) return list;
    list->attrs |= RAY_ATTR_DICT;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != '}') {
        /* Parse key: must be a name (alpha start) */
        const char *kstart = p->pos;
        while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT
               || *p->pos == '_' || *p->pos == '-')
            p->pos++;
        size_t klen = (size_t)(p->pos - kstart);
        if (klen == 0) { ray_release(list); return RAY_ERR_PTR(RAY_ERR_PARSE); }

        int64_t kid = ray_sym_intern(kstart, klen);
        ray_t* key = ray_sym(kid);
        if (RAY_IS_ERR(key)) { ray_release(list); return key; }

        /* Expect colon */
        skip_ws_and_comments(p);
        if (*p->pos != ':') { ray_release(key); ray_release(list); return RAY_ERR_PTR(RAY_ERR_PARSE); }
        p->pos++; /* skip : */
        skip_ws_and_comments(p);

        /* Parse value expression */
        ray_t* val = parse_expr(p);
        if (RAY_IS_ERR(val)) { ray_release(key); ray_release(list); return val; }

        /* Append key then value */
        list = ray_list_append(list, key);
        ray_release(key);
        if (RAY_IS_ERR(list)) { ray_release(val); return list; }
        list = ray_list_append(list, val);
        ray_release(val);
        if (RAY_IS_ERR(list)) return list;

        skip_ws_and_comments(p);
    }
    if (*p->pos != '}') { ray_release(list); return RAY_ERR_PTR(RAY_ERR_PARSE); }
    p->pos++;
    return list;
}

/* ── List (s-expression): (fn arg1 arg2 ...) ── */
static ray_t* parse_list(ray_parser_t *p) {
    p->pos++; /* skip ( */
    ray_t* list = ray_list_new(4);
    if (RAY_IS_ERR(list)) return list;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ')') {
        ray_t* elem = parse_expr(p);
        if (RAY_IS_ERR(elem)) { ray_release(list); return elem; }
        list = ray_list_append(list, elem);
        ray_release(elem);
        if (RAY_IS_ERR(list)) return list;
        skip_ws_and_comments(p);
    }
    if (*p->pos != ')') { ray_release(list); return RAY_ERR_PTR(RAY_ERR_PARSE); }
    p->pos++;
    return list;
}

/* ── Main expression dispatch ── */
static ray_t* parse_expr(ray_parser_t *p) {
    skip_ws_and_comments(p);

    switch (PA(*p->pos)) {
        case PA_END:    return RAY_ERR_PTR(RAY_ERR_PARSE);
        case PA_DIGIT:  return parse_number(p);
        case PA_MINUS:
            if (p->pos[1] >= '0' && p->pos[1] <= '9')
                return parse_number(p);
            return parse_name(p);  /* standalone '-' or '-name' */
        case PA_ALPHA:  return parse_name(p);
        case PA_STRING: return parse_string(p);
        case PA_QUOTE:  return parse_symbol(p);
        case PA_LPAREN: return parse_list(p);
        case PA_LBRACK: return parse_vector(p);
        case PA_LBRACE: return parse_dict(p);
        case PA_RPAREN: return RAY_ERR_PTR(RAY_ERR_PARSE);
        case PA_RBRACK: return RAY_ERR_PTR(RAY_ERR_PARSE);
        case PA_RBRACE: return RAY_ERR_PTR(RAY_ERR_PARSE);
        default:        return parse_name(p);  /* operators like +, *, etc. */
    }
}

/* ── Public API ── */
ray_t* ray_parse(const char* source) {
    if (!source) return RAY_ERR_PTR(RAY_ERR_PARSE);
    ray_parser_t p = { .src = source, .pos = source };
    ray_t* first = parse_expr(&p);
    if (RAY_IS_ERR(first)) return first;

    /* Check if there are more expressions after the first */
    skip_ws_and_comments(&p);
    if (*p.pos == '\0') return first;  /* single expression */

    /* Multiple expressions: collect into (do expr1 expr2 ...) */
    ray_t* exprs[256];
    int32_t count = 0;
    exprs[count++] = first;

    while (*p.pos) {
        if (count >= 256) {
            for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
            return RAY_ERR_PTR(RAY_ERR_DOMAIN);  /* too many top-level expressions */
        }
        ray_t* expr = parse_expr(&p);
        if (RAY_IS_ERR(expr)) {
            for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
            return expr;
        }
        exprs[count++] = expr;
        skip_ws_and_comments(&p);
    }

    /* Build (do expr1 expr2 ...) list */
    ray_t* do_list = ray_alloc((count + 1) * sizeof(ray_t*));
    if (!do_list) {
        for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
        return RAY_ERR_PTR(RAY_ERR_OOM);
    }
    do_list->type = RAY_LIST;
    do_list->len = 0;
    ray_t** elems = (ray_t**)ray_data(do_list);
    /* Build a name-reference atom for "do" so parsing is independent of runtime */
    ray_t* do_sym = ray_alloc(0);
    if (!do_sym) {
        ray_release(do_list);
        for (int32_t i = 0; i < count; i++) ray_release(exprs[i]);
        return RAY_ERR_PTR(RAY_ERR_OOM);
    }
    do_sym->type = -RAY_SYM;
    do_sym->attrs = RAY_ATTR_NAME;
    do_sym->i64 = ray_sym_intern("do", 2);
    elems[0] = do_sym;
    for (int32_t i = 0; i < count; i++)
        elems[i + 1] = exprs[i];
    do_list->len = count + 1;
    return do_list;
}
