#include "lang/parse.h"
#include "lang/env.h"
#include <string.h>
#include <stdlib.h>

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

/* ── Number parsing ── */
static ray_t* parse_number(ray_parser_t *p) {
    const char *start = p->pos;
    if (*p->pos == '-') p->pos++;

    /* Scan digits */
    while (*p->pos >= '0' && *p->pos <= '9') p->pos++;

    /* Check for float */
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

    int64_t v = strtoll(start, NULL, 10);
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
            case 'n':  buf[out++] = '\n'; break;
            case 't':  buf[out++] = '\t'; break;
            case 'r':  buf[out++] = '\r'; break;
            case '\\': buf[out++] = '\\'; break;
            case '"':  buf[out++] = '"';  break;
            case '0':  buf[out++] = '\0'; break;
            default:   buf[out++] = '\\'; buf[out++] = *r; break;
            }
            r++;
        } else {
            buf[out++] = *r++;
        }
    }
    return ray_str(buf, out);
}

/* ── Symbol parsing: 'name ── */
static ray_t* parse_symbol(ray_parser_t *p) {
    p->pos++; /* skip ' */
    const char *start = p->pos;
    while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT || *p->pos == '_' || *p->pos == '.')
        p->pos++;
    size_t len = (size_t)(p->pos - start);
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

    /* Return as name symbol (with RAY_ATTR_NAME flag) */
    int64_t id = ray_sym_intern(start, len);
    ray_t* s = ray_sym(id);
    if (!RAY_IS_ERR(s)) s->attrs |= RAY_ATTR_NAME;
    return s;
}

/* ── Vector literal: [1 2 3] ── */
static ray_t* parse_vector(ray_parser_t *p) {
    p->pos++; /* skip [ */
    ray_t* list = ray_list_new(8);
    if (RAY_IS_ERR(list)) return list;
    list->attrs |= RAY_ATTR_VECTOR;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ']') {
        ray_t* elem = parse_expr(p);
        if (RAY_IS_ERR(elem)) { ray_release(list); return elem; }
        list = ray_list_append(list, elem);
        ray_release(elem);
        if (RAY_IS_ERR(list)) return list;
        skip_ws_and_comments(p);
    }
    if (*p->pos != ']') { ray_release(list); return RAY_ERR_PTR(RAY_ERR_PARSE); }
    p->pos++;
    return list;
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
