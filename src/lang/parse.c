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

#define PA(c) ((int)(unsigned char)_PA[(unsigned char)(c)])

/* ══════════════════════════════════════════
 * Parser state
 * ══════════════════════════════════════════ */

typedef struct {
    const char *src;
    const char *pos;
} td_parser_t;

static void skip_ws_and_comments(td_parser_t *p) {
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
static td_t* parse_expr(td_parser_t *p);

/* ── Number parsing ── */
static td_t* parse_number(td_parser_t *p) {
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
        return td_f64(v);
    }

    int64_t v = strtoll(start, NULL, 10);
    return td_i64(v);
}

/* ── String parsing ── */
static td_t* parse_string(td_parser_t *p) {
    p->pos++; /* skip opening " */
    const char *start = p->pos;

    /* Scan for closing " (handle escapes later) */
    while (*p->pos && *p->pos != '"') {
        if (*p->pos == '\\' && p->pos[1]) p->pos++;
        p->pos++;
    }
    size_t len = (size_t)(p->pos - start);
    if (*p->pos == '"') p->pos++;

    return td_str(start, len);
}

/* ── Symbol parsing: 'name ── */
static td_t* parse_symbol(td_parser_t *p) {
    p->pos++; /* skip ' */
    const char *start = p->pos;
    while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT || *p->pos == '_' || *p->pos == '.')
        p->pos++;
    size_t len = (size_t)(p->pos - start);
    int64_t id = td_sym_intern(start, len);
    return td_sym(id);
}

/* ── Name parsing ── */
static td_t* parse_name(td_parser_t *p) {
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

    /* Check for true/false */
    if (len == 4 && memcmp(start, "true", 4) == 0)  return td_bool(true);
    if (len == 5 && memcmp(start, "false", 5) == 0) return td_bool(false);

    /* Return as name symbol (with TD_ATTR_NAME flag) */
    int64_t id = td_sym_intern(start, len);
    td_t* s = td_sym(id);
    if (!TD_IS_ERR(s)) s->attrs |= TD_ATTR_NAME;
    return s;
}

/* ── Vector literal: [1 2 3] ── */
static td_t* parse_vector(td_parser_t *p) {
    p->pos++; /* skip [ */
    td_t* list = td_list_new(8);
    if (TD_IS_ERR(list)) return list;
    list->attrs |= TD_ATTR_VECTOR;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ']') {
        td_t* elem = parse_expr(p);
        if (TD_IS_ERR(elem)) { td_release(list); return elem; }
        list = td_list_append(list, elem);
        td_release(elem);
        if (TD_IS_ERR(list)) return list;
        skip_ws_and_comments(p);
    }
    if (*p->pos == ']') p->pos++;
    return list;
}

/* ── Dict literal: {key: val key: val} ── */
static td_t* parse_dict(td_parser_t *p) {
    /* Skip over dict content for now — td_dict not yet implemented */
    p->pos++; /* skip { */
    int depth = 1;
    while (*p->pos && depth > 0) {
        if (*p->pos == '{') depth++;
        else if (*p->pos == '}') depth--;
        p->pos++;
    }
    return TD_ERR_PTR(TD_ERR_NYI);
}

/* ── List (s-expression): (fn arg1 arg2 ...) ── */
static td_t* parse_list(td_parser_t *p) {
    p->pos++; /* skip ( */
    td_t* list = td_list_new(4);
    if (TD_IS_ERR(list)) return list;

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != ')') {
        td_t* elem = parse_expr(p);
        if (TD_IS_ERR(elem)) { td_release(list); return elem; }
        list = td_list_append(list, elem);
        td_release(elem);
        if (TD_IS_ERR(list)) return list;
        skip_ws_and_comments(p);
    }
    if (*p->pos == ')') p->pos++;
    return list;
}

/* ── Main expression dispatch ── */
static td_t* parse_expr(td_parser_t *p) {
    skip_ws_and_comments(p);

    switch (PA(*p->pos)) {
        case PA_END:    return TD_ERR_PTR(TD_ERR_PARSE);
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
        default:        return parse_name(p);  /* operators like +, *, etc. */
    }
}

/* ── Public API ── */
td_t* td_parse(const char* source) {
    if (!source) return TD_ERR_PTR(TD_ERR_PARSE);
    td_parser_t p = { .src = source, .pos = source };
    return parse_expr(&p);
}
