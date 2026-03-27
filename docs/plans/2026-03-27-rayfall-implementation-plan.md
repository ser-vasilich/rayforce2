# Rayfall on Teide — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a full Rayfall language frontend to Teide — lexer, parser, eval, bytecode VM, REPL — so that `td_eval("(+ 1 2)")` works end-to-end.

**Architecture:** Rayforce-style function objects (`TD_UNARY/TD_BINARY/TD_VARY`) registered in a global environment. Parser produces `td_t` objects directly (no separate AST). Tree-walking `eval()` dispatches by function type. Lambdas compile to bytecode and run in a stack-based computed-goto VM. `select`/`update` builtins bridge to Teide's DAG executor at runtime.

**Tech Stack:** C17, munit test framework, Teide allocator (`td_alloc`/`td_arena_alloc`), no external dependencies.

**Design doc:** `docs/plans/2026-03-27-rayfall-on-teide-design.md`

**Reference codebases:**
- Rayforce parser: `/home/hetoku/data/work/rayforce/core/parse.c`
- Rayforce env/registration: `/home/hetoku/data/work/rayforce/core/env.c`
- Rayforce VM: `/home/hetoku/data/work/rayforce/core/eval.c`
- Rayforce compiler: `/home/hetoku/data/work/kdb/src/lang/compile.c`
- Rayforce lambda: `/home/hetoku/data/work/rayforce/core/lambda.h`
- kdb ASCII dispatch: `/home/hetoku/data/work/kdb/src/lang/parse.c` (lines 25-66)

**Build & test commands:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
./build/test_teide --suite /lang       # run only lang tests
./build/test_teide --suite /lang/lex   # run only lexer tests
```

---

## Phase 1: Type System Foundation

### Task 1.1: Add function type tags to td.h

- [x] Add type tags, function attribute flags, and function typedefs to td.h
- [x] Verify build compiles
- [x] Run existing tests (no regressions)

**Files:**
- Modify: `include/teide/td.h`

**Step 1: Add type tag defines after the existing `TD_STR` block (around line 128)**

Add these after `#define TD_STR 21`:

```c
/* Function types (Rayforce-compatible) */
#define TD_LAMBDA    100   /* User-defined function (compiled body + env) */
#define TD_UNARY     101   /* Unary builtin: td_t* (*)(td_t*) */
#define TD_BINARY    102   /* Binary builtin: td_t* (*)(td_t*, td_t*) */
#define TD_VARY      103   /* Variadic builtin: td_t* (*)(td_t**, int64_t) */

/* Function atom types (negative = atom) */
#define TD_ATOM_LAMBDA    (-TD_LAMBDA)
#define TD_ATOM_UNARY     (-TD_UNARY)
#define TD_ATOM_BINARY    (-TD_BINARY)
#define TD_ATOM_VARY      (-TD_VARY)

/* Function attribute flags (stored in attrs byte) */
#define TD_FN_NONE          0x00
#define TD_FN_LEFT_ATOMIC   0x01  /* auto-map left arg over vectors */
#define TD_FN_RIGHT_ATOMIC  0x02  /* auto-map right arg over vectors */
#define TD_FN_ATOMIC        0x04  /* auto-map all args over vectors */
#define TD_FN_AGGR          0x08  /* aggregation function */
#define TD_FN_SPECIAL_FORM  0x10  /* receives unevaluated args */

/* AST name flag (distinguishes symbol literal from variable reference) */
#define TD_ATTR_NAME        0x20  /* td_t SYM atom with this flag = name reference */

/* Function type signatures */
typedef td_t* (*td_unary_fn)(td_t*);
typedef td_t* (*td_binary_fn)(td_t*, td_t*);
typedef td_t* (*td_vary_fn)(td_t**, int64_t);
```

**Step 2: Verify build compiles**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
Expected: Clean compile, no errors.

**Step 3: Run existing tests to verify no regressions**

Run: `cd build && ctest --output-on-failure`
Expected: All existing tests pass.

**Step 4: Commit**

```bash
git add include/teide/td.h
git commit -m "feat(lang): add TD_UNARY/TD_BINARY/TD_VARY/TD_LAMBDA type tags and FN_* flags"
```

---

### Task 1.2: Function object constructors

- [x] Implement function object constructors and environment
- [x] Write tests for env operations
- [x] Verify build and tests pass

**Files:**
- Create: `src/lang/env.h`
- Create: `src/lang/env.c`
- Create: `test/test_lang.c`
- Modify: `test/test_main.c`

**Step 1: Write test file `test/test_lang.c`**

```c
#include "munit.h"
#include <teide/td.h>
#include <string.h>

/* ---- Setup / Teardown ---- */

static void* lang_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    td_heap_init();
    (void)td_sym_init();
    return NULL;
}

static void lang_teardown(void* fixture) {
    (void)fixture;
    td_sym_destroy();
    td_heap_destroy();
}

/* ---- Dummy function for testing ---- */
static td_t* dummy_unary(td_t* x) { return td_retain(x); }
static td_t* dummy_binary(td_t* x, td_t* y) { (void)y; return td_retain(x); }
static td_t* dummy_vary(td_t** args, int64_t n) { (void)n; return td_retain(args[0]); }

/* ---- Test: create unary function object ---- */
static MunitResult test_fn_unary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* fn = td_fn_unary("neg", TD_FN_ATOMIC, dummy_unary);
    munit_assert_ptr_not_null(fn);
    munit_assert_false(TD_IS_ERR(fn));
    munit_assert_int(fn->type, ==, TD_ATOM_UNARY);
    munit_assert_uint8(fn->attrs & TD_FN_ATOMIC, !=, 0);
    td_release(fn);

    return MUNIT_OK;
}

/* ---- Test: create binary function object ---- */
static MunitResult test_fn_binary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* fn = td_fn_binary("+", TD_FN_ATOMIC, dummy_binary);
    munit_assert_ptr_not_null(fn);
    munit_assert_int(fn->type, ==, TD_ATOM_BINARY);
    td_release(fn);

    return MUNIT_OK;
}

/* ---- Test: create vary function object ---- */
static MunitResult test_fn_vary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* fn = td_fn_vary("list", TD_FN_NONE, dummy_vary);
    munit_assert_ptr_not_null(fn);
    munit_assert_int(fn->type, ==, TD_ATOM_VARY);
    td_release(fn);

    return MUNIT_OK;
}

/* ---- Suite definition ---- */
static MunitTest lang_tests[] = {
    { "/fn_unary",  test_fn_unary,  lang_setup, lang_teardown, 0, NULL },
    { "/fn_binary", test_fn_binary, lang_setup, lang_teardown, 0, NULL },
    { "/fn_vary",   test_fn_vary,   lang_setup, lang_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_lang_suite = { "/lang", lang_tests, NULL, 1, 0 };
```

**Step 2: Register suite in `test/test_main.c`**

Add `extern MunitSuite test_lang_suite;` to the extern declarations and add `test_lang_suite` to the `child_suites[]` array.

**Step 3: Run test to verify it fails**

Run: `cmake --build build && ./build/test_teide --suite /lang`
Expected: Linker error — `td_fn_unary`, `td_fn_binary`, `td_fn_vary` not found.

**Step 4: Create `src/lang/env.h`**

```c
#ifndef TD_ENV_H
#define TD_ENV_H

#include <teide/td.h>

/* Create function objects. Name is interned as a symbol.
 * The function pointer is stored in the i64 field of the atom. */
td_t* td_fn_unary(const char* name, uint8_t fn_attrs, td_unary_fn fn);
td_t* td_fn_binary(const char* name, uint8_t fn_attrs, td_binary_fn fn);
td_t* td_fn_vary(const char* name, uint8_t fn_attrs, td_vary_fn fn);

/* Global environment: symbol → function object dict */
td_err_t td_env_init(void);
void     td_env_destroy(void);
td_t*    td_env_get(int64_t sym_id);
void     td_env_set(int64_t sym_id, td_t* val);

#endif /* TD_ENV_H */
```

**Step 5: Create `src/lang/env.c`**

```c
#include "lang/env.h"
#include <string.h>

/* ---- Function constructors ---- */

td_t* td_fn_unary(const char* name, uint8_t fn_attrs, td_unary_fn fn) {
    td_t* obj = td_alloc(0);  /* atom, no data beyond header */
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_UNARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    (void)name;
    return obj;
}

td_t* td_fn_binary(const char* name, uint8_t fn_attrs, td_binary_fn fn) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_BINARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    (void)name;
    return obj;
}

td_t* td_fn_vary(const char* name, uint8_t fn_attrs, td_vary_fn fn) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_VARY;
    obj->attrs = fn_attrs;
    obj->i64 = (int64_t)(uintptr_t)fn;
    (void)name;
    return obj;
}

/* ---- Global environment ---- */

#define ENV_CAP 512

static struct {
    int64_t keys[ENV_CAP];
    td_t*   vals[ENV_CAP];
    int32_t count;
} g_env;

td_err_t td_env_init(void) {
    memset(&g_env, 0, sizeof(g_env));
    return TD_OK;
}

void td_env_destroy(void) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.vals[i]) td_release(g_env.vals[i]);
    }
    memset(&g_env, 0, sizeof(g_env));
}

td_t* td_env_get(int64_t sym_id) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) return g_env.vals[i];
    }
    return NULL;
}

void td_env_set(int64_t sym_id, td_t* val) {
    for (int32_t i = 0; i < g_env.count; i++) {
        if (g_env.keys[i] == sym_id) {
            if (g_env.vals[i]) td_release(g_env.vals[i]);
            g_env.vals[i] = td_retain(val);
            return;
        }
    }
    if (g_env.count < ENV_CAP) {
        g_env.keys[g_env.count] = sym_id;
        g_env.vals[g_env.count] = td_retain(val);
        g_env.count++;
    }
}
```

**Step 6: Run tests**

Run: `cmake --build build && ./build/test_teide --suite /lang`
Expected: 3/3 PASS.

**Step 7: Commit**

```bash
git add src/lang/env.h src/lang/env.c test/test_lang.c test/test_main.c
git commit -m "feat(lang): function object constructors and global environment"
```

---

## Phase 2: Lexer

### Task 2.1: ASCII dispatch table and basic tokenization

- [x] Implement ASCII dispatch table and basic tokenization

**Files:**
- Create: `src/lang/parse.h`
- Create: `src/lang/parse.c`
- Modify: `test/test_lang.c`

**Step 1: Add lexer tests to `test/test_lang.c`**

Append these tests after the existing fn tests:

```c
/* ---- Test: lex integer ---- */
static MunitResult test_lex_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("42");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int64(result->i64, ==, 42);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex negative integer ---- */
static MunitResult test_lex_neg_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("-7");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int64(result->i64, ==, -7);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex float ---- */
static MunitResult test_lex_f64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("3.14");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_F64);
    munit_assert_double(result->f64, ==, 3.14);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex string ---- */
static MunitResult test_lex_string(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("\"hello\"");
    munit_assert_ptr_not_null(result);
    /* short string = SSO atom */
    munit_assert_int(result->type, ==, TD_ATOM_STR);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex symbol ---- */
static MunitResult test_lex_symbol(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("'AAPL");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_SYM);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex true/false ---- */
static MunitResult test_lex_bool(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* t = td_parse("true");
    munit_assert_ptr_not_null(t);
    munit_assert_int(t->type, ==, TD_ATOM_BOOL);
    munit_assert_uint(t->b8, ==, 1);
    td_release(t);

    td_t* f = td_parse("false");
    munit_assert_int(f->type, ==, TD_ATOM_BOOL);
    munit_assert_uint(f->b8, ==, 0);
    td_release(f);
    return MUNIT_OK;
}
```

Add these to the `lang_tests[]` array:

```c
{ "/lex/i64",    test_lex_i64,    lang_setup, lang_teardown, 0, NULL },
{ "/lex/neg_i64",test_lex_neg_i64,lang_setup, lang_teardown, 0, NULL },
{ "/lex/f64",    test_lex_f64,    lang_setup, lang_teardown, 0, NULL },
{ "/lex/string", test_lex_string, lang_setup, lang_teardown, 0, NULL },
{ "/lex/symbol", test_lex_symbol, lang_setup, lang_teardown, 0, NULL },
{ "/lex/bool",   test_lex_bool,   lang_setup, lang_teardown, 0, NULL },
```

**Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/test_teide --suite /lang/lex`
Expected: Linker error — `td_parse` not found.

**Step 3: Create `src/lang/parse.h`**

```c
#ifndef TD_PARSE_H
#define TD_PARSE_H

#include <teide/td.h>

/* Parse a Rayfall source string into a td_t object tree.
 * Returns a single expression, or a list of expressions if the
 * source contains multiple top-level forms. */
td_t* td_parse(const char* source);

#endif /* TD_PARSE_H */
```

**Step 4: Create `src/lang/parse.c` with ASCII dispatch table and parser**

Reference: `/home/hetoku/data/work/kdb/src/lang/parse.c` lines 25-66 for the `_PA[]` table pattern.

```c
#include "lang/parse.h"
#include "lang/env.h"
#include <string.h>
#include <stdlib.h>

/* ══════════════════════════════════════════
 * ASCII dispatch table (128 bytes)
 * Single indexed read: PA(c) — zero branches.
 * Reference: kdb/src/lang/parse.c
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
    int neg = 0;
    if (*p->pos == '-') { neg = 1; p->pos++; }

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

    /* Check for date: YYYY.MM.DD */
    int ndig = (int)(p->pos - start) - neg;
    if (!is_float && ndig == 4 && *p->pos == '.'
        && p->pos[1] >= '0' && p->pos[2] >= '0'
        && p->pos[3] == '.'
        && p->pos[4] >= '0' && p->pos[5] >= '0') {
        /* Date parsing — extend later */
        /* For now fall through to int/float */
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
    /* Collect elements into a list, then try to unify */
    td_t* list = td_list_new(8);
    if (TD_IS_ERR(list)) return list;

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
    p->pos++; /* skip { */
    td_t* keys = td_list_new(8);
    td_t* vals = td_list_new(8);
    if (TD_IS_ERR(keys) || TD_IS_ERR(vals)) {
        if (!TD_IS_ERR(keys)) td_release(keys);
        if (!TD_IS_ERR(vals)) td_release(vals);
        return TD_ERR_PTR(TD_ERR_OOM);
    }

    skip_ws_and_comments(p);
    while (*p->pos && *p->pos != '}') {
        /* Parse key (name without TD_ATTR_NAME — it's a symbol literal key) */
        const char *kstart = p->pos;
        while (PA(*p->pos) == PA_ALPHA || PA(*p->pos) == PA_DIGIT || *p->pos == '_' || *p->pos == '-')
            p->pos++;
        size_t klen = (size_t)(p->pos - kstart);
        int64_t kid = td_sym_intern(kstart, klen);
        td_t* key = td_sym(kid);

        skip_ws_and_comments(p);
        if (*p->pos == ':') p->pos++; /* skip : */
        skip_ws_and_comments(p);

        /* Parse value */
        td_t* val = parse_expr(p);
        if (TD_IS_ERR(val)) { td_release(keys); td_release(vals); td_release(key); return val; }

        keys = td_list_append(keys, key);
        vals = td_list_append(vals, val);
        td_release(key);
        td_release(val);
        skip_ws_and_comments(p);
    }
    if (*p->pos == '}') p->pos++;

    td_t* d = td_dict(keys, vals);
    td_release(keys);
    td_release(vals);
    return d;
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
```

**Note:** This implementation may need `TD_ERR_PARSE` added to td.h error codes, and uses `td_dict`, `td_list_new`, `td_list_append`. Check that these exist in td.h. If `td_dict` or `td_list_append` don't exist yet, implement stubs or use the existing list API.

**Step 5: Run tests**

Run: `cmake --build build && ./build/test_teide --suite /lang`
Expected: All lexer tests pass. Fix any missing API functions.

**Step 6: Commit**

```bash
git add src/lang/parse.h src/lang/parse.c test/test_lang.c
git commit -m "feat(lang): lexer with ASCII dispatch table and recursive descent parser"
```

---

### Task 2.2: Parse s-expressions and vector literals

- [x] Implement s-expression and vector literal parsing

**Files:**
- Modify: `test/test_lang.c`

**Step 1: Add parse tests**

```c
/* ---- Test: parse s-expression ---- */
static MunitResult test_parse_sexpr(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("(+ 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    /* Should be a list of 3 elements: [name:"+", 1, 2] */
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int64(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse nested s-expressions ---- */
static MunitResult test_parse_nested(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("(+ (* 2 3) 4)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int64(td_len(result), ==, 3);
    /* Second element should be a list (the nested (* 2 3)) */
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[1]->type, ==, TD_LIST);
    munit_assert_int64(td_len(elems[1]), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse vector literal ---- */
static MunitResult test_parse_vector(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("[1 2 3]");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    /* Should be a list of 3 i64 elements */
    munit_assert_int64(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse empty list ---- */
static MunitResult test_parse_empty_list(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("()");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int64(td_len(result), ==, 0);
    td_release(result);
    return MUNIT_OK;
}
```

Add to `lang_tests[]`:
```c
{ "/parse/sexpr",      test_parse_sexpr,      lang_setup, lang_teardown, 0, NULL },
{ "/parse/nested",     test_parse_nested,     lang_setup, lang_teardown, 0, NULL },
{ "/parse/vector",     test_parse_vector,     lang_setup, lang_teardown, 0, NULL },
{ "/parse/empty_list", test_parse_empty_list, lang_setup, lang_teardown, 0, NULL },
```

**Step 2: Run tests**

Run: `cmake --build build && ./build/test_teide --suite /lang/parse`
Expected: All pass (parser already handles these in Task 2.1).

**Step 3: Commit**

```bash
git add test/test_lang.c
git commit -m "test(lang): s-expression and vector parsing tests"
```

---

## Phase 3: Tree-Walking Eval

### Task 3.1: Eval with builtin dispatch

- [ ] Implement eval with builtin dispatch

**Files:**
- Create: `src/lang/eval.h`
- Create: `src/lang/eval.c`
- Modify: `test/test_lang.c`
- Modify: `include/teide/td.h` (add `td_eval` declaration)

**Step 1: Add eval tests to `test/test_lang.c`**

```c
/* ---- Test: eval literal passthrough ---- */
static MunitResult test_eval_literal(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("42");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int64(result->i64, ==, 42);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval addition ---- */
static MunitResult test_eval_add(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(+ 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int64(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval nested arithmetic ---- */
static MunitResult test_eval_nested_arith(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(+ (* 2 3) 4)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int64(result->i64, ==, 10);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval subtraction ---- */
static MunitResult test_eval_sub(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(- 10 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int64(result->i64, ==, 7);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval division ---- */
static MunitResult test_eval_div(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(/ 10 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int64(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval comparison ---- */
static MunitResult test_eval_cmp(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(> 5 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_BOOL);
    munit_assert_uint(result->b8, ==, 1);
    td_release(result);
    return MUNIT_OK;
}
```

Add to `lang_tests[]`:
```c
{ "/eval/literal",      test_eval_literal,      lang_setup, lang_teardown, 0, NULL },
{ "/eval/add",          test_eval_add,          lang_setup, lang_teardown, 0, NULL },
{ "/eval/nested_arith", test_eval_nested_arith, lang_setup, lang_teardown, 0, NULL },
{ "/eval/sub",          test_eval_sub,          lang_setup, lang_teardown, 0, NULL },
{ "/eval/div",          test_eval_div,          lang_setup, lang_teardown, 0, NULL },
{ "/eval/cmp",          test_eval_cmp,          lang_setup, lang_teardown, 0, NULL },
```

**Step 2: Update `lang_setup` to initialize builtins**

The setup function must call `td_env_init()` and register the arithmetic builtins. Add a `td_lang_init()` function that does both `td_env_init()` + `td_register_builtins()`.

**Step 3: Create `src/lang/eval.h`**

```c
#ifndef TD_EVAL_H
#define TD_EVAL_H

#include <teide/td.h>

/* Initialize the Rayfall runtime: symbols, environment, builtins. */
td_err_t td_lang_init(void);
void     td_lang_destroy(void);

/* Evaluate a parsed td_t object tree. */
td_t* td_eval(td_t* obj);

/* Parse + eval convenience. */
td_t* td_eval_str(const char* source);

#endif /* TD_EVAL_H */
```

**Step 4: Create `src/lang/eval.c`**

Implement tree-walking eval with dispatch by function type. Register basic arithmetic builtins (`+`, `-`, `*`, `/`, `%`, `>`, `<`, `>=`, `<=`, `==`, `!=`, `not`, `neg`, `and`, `or`, `if`, `count`, `sum`, `avg`, `min`, `max`, `first`, `last`).

Key structure:
```c
td_t* td_eval(td_t* obj) {
    if (!obj || TD_IS_ERR(obj)) return obj;

    /* Atoms: return themselves (retain) */
    if (td_is_atom(obj)) {
        /* Name reference: resolve from env */
        if (obj->type == TD_ATOM_SYM && (obj->attrs & TD_ATTR_NAME)) {
            td_t* val = td_env_get(obj->i64);
            if (!val) return TD_ERR_PTR(TD_ERR_VALUE);
            return td_retain(val);
        }
        return td_retain(obj);
    }

    /* Non-list vectors: return themselves */
    if (obj->type != TD_LIST) return td_retain(obj);

    /* Empty list */
    if (td_len(obj) == 0) return td_retain(obj);

    /* List: evaluate first element, dispatch by type */
    td_t** elems = (td_t**)td_data(obj);
    td_t* head = td_eval(elems[0]);
    if (TD_IS_ERR(head)) return head;

    int64_t n = td_len(obj);

    switch (head->type) {
        case TD_ATOM_UNARY: {
            td_unary_fn fn = (td_unary_fn)(uintptr_t)head->i64;
            td_t* arg = td_eval(elems[1]);
            td_release(head);
            if (TD_IS_ERR(arg)) return arg;
            td_t* result = fn(arg);
            td_release(arg);
            return result;
        }
        case TD_ATOM_BINARY: {
            td_binary_fn fn = (td_binary_fn)(uintptr_t)head->i64;
            if (head->attrs & TD_FN_SPECIAL_FORM) {
                td_release(head);
                return fn(elems[1], elems[2]);  /* unevaluated args */
            }
            td_t* left = td_eval(elems[1]);
            if (TD_IS_ERR(left)) { td_release(head); return left; }
            td_t* right = td_eval(elems[2]);
            if (TD_IS_ERR(right)) { td_release(head); td_release(left); return right; }
            td_release(head);
            td_t* result = fn(left, right);
            td_release(left);
            td_release(right);
            return result;
        }
        case TD_ATOM_VARY: {
            td_vary_fn fn = (td_vary_fn)(uintptr_t)head->i64;
            if (head->attrs & TD_FN_SPECIAL_FORM) {
                td_release(head);
                return fn(elems + 1, n - 1);  /* unevaluated args */
            }
            /* Eval all args */
            int64_t argc = n - 1;
            td_t* args[64];
            for (int64_t i = 0; i < argc && i < 64; i++) {
                args[i] = td_eval(elems[i + 1]);
                if (TD_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) td_release(args[j]);
                    td_release(head);
                    return args[i];
                }
            }
            td_release(head);
            td_t* result = fn(args, argc);
            for (int64_t i = 0; i < argc; i++) td_release(args[i]);
            return result;
        }
        default:
            td_release(head);
            return TD_ERR_PTR(TD_ERR_TYPE);
    }
}

td_t* td_eval_str(const char* source) {
    td_t* parsed = td_parse(source);
    if (TD_IS_ERR(parsed)) return parsed;
    td_t* result = td_eval(parsed);
    td_release(parsed);
    return result;
}
```

Implement builtin arithmetic functions (`ray_add`, `ray_sub`, etc.) that handle `TD_ATOM_I64` and `TD_ATOM_F64` scalar cases. Register them in `td_lang_init()`.

**Step 5: Run tests**

Run: `cmake --build build && ./build/test_teide --suite /lang/eval`
Expected: All eval tests pass.

**Step 6: Commit**

```bash
git add src/lang/eval.h src/lang/eval.c test/test_lang.c include/teide/td.h
git commit -m "feat(lang): tree-walking eval with arithmetic builtins"
```

---

### Task 3.2: Variable binding (set, let) and conditionals (if)

- [ ] Implement variable binding and conditionals

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
/* ---- Test: set and resolve ---- */
static MunitResult test_eval_set(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* (do (set x 10) x) */
    td_t* result = td_eval_str("(do (set x 10) x)");
    munit_assert_ptr_not_null(result);
    munit_assert_int64(result->i64, ==, 10);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: if conditional ---- */
static MunitResult test_eval_if(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* t = td_eval_str("(if true 1 2)");
    munit_assert_int64(t->i64, ==, 1);
    td_release(t);

    td_t* f = td_eval_str("(if false 1 2)");
    munit_assert_int64(f->i64, ==, 2);
    td_release(f);
    return MUNIT_OK;
}

/* ---- Test: let binding ---- */
static MunitResult test_eval_let(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (let x 5) (+ x 3))");
    munit_assert_int64(result->i64, ==, 8);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement `ray_set`, `ray_let`, `ray_cond` (if), `ray_do` builtins**

These are `FN_SPECIAL_FORM` — they receive unevaluated args and call `td_eval` themselves.

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): set/let variable binding and if conditional"
```

---

### Task 3.3: Lambda functions

- [ ] Implement lambda functions

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
/* ---- Test: lambda definition and call ---- */
static MunitResult test_eval_lambda(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set double (fn [x] (* x 2))) (double 5))");
    munit_assert_int64(result->i64, ==, 10);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lambda with multiple args ---- */
static MunitResult test_eval_lambda_multi(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set add3 (fn [a b c] (+ a (+ b c)))) (add3 1 2 3))");
    munit_assert_int64(result->i64, ==, 6);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lambda with let ---- */
static MunitResult test_eval_lambda_let(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set f (fn [a b] (let c (+ a b)) (+ c 1))) (f 3 4))");
    munit_assert_int64(result->i64, ==, 8);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement `ray_fn` (FN_SPECIAL_FORM)** — creates a `TD_LAMBDA` object from args vector + body. Implement lambda call in eval dispatch (when head is `TD_ATOM_LAMBDA`: bind args into a local env frame, eval body).

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): lambda functions with lexical binding"
```

---

## Phase 4: Bytecode Compiler & VM

### Task 4.1: Bytecode compiler (AST → bytecode)

- [ ] Implement bytecode compiler

**Files:**
- Create: `src/lang/compile.c`
- Modify: `src/lang/eval.h` (add compile API)
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
/* ---- Test: compiled lambda matches interpreted ---- */
static MunitResult test_compile_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Compiled lambda should produce same result as interpreted */
    td_t* result = td_eval_str("(do (set f (fn [x] (+ x 1))) (f 10))");
    munit_assert_int64(result->i64, ==, 11);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement compiler**

Reference: `/home/hetoku/data/work/kdb/src/lang/compile.c`

The compiler walks the parsed `td_t` list/atom tree and emits bytecode:
- `OP_LOADCONST` for literals
- `OP_LOADENV` / `OP_STOREENV` for local variables
- `OP_RESOLVE` for global lookups
- `OP_CALL1` / `OP_CALL2` / `OP_CALLN` for function application
- `OP_JMP` / `OP_JMPF` for control flow
- `OP_RET` for return

Lambda compilation is triggered on first call (lazy, same as Rayforce).

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): bytecode compiler for lambda functions"
```

---

### Task 4.2: Stack-based VM (computed goto)

- [ ] Implement stack-based VM

**Files:**
- Modify: `src/lang/eval.c` (add `td_vm_eval`)
- Modify: `src/lang/eval.h` (add VM struct)
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
/* ---- Test: VM handles recursive fibonacci ---- */
static MunitResult test_vm_fib(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 10))");
    munit_assert_int64(result->i64, ==, 55);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: VM handles tail-recursive loop ---- */
static MunitResult test_vm_loop(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set sum-to (fn [n acc] (if (== n 0) acc (sum-to (- n 1) (+ acc n))))) (sum-to 100 0))");
    munit_assert_int64(result->i64, ==, 5050);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement VM**

Reference: `/home/hetoku/data/work/kdb/src/lang/vm.c` and `/home/hetoku/data/work/rayforce/core/eval.c`

Key elements:
- `td_vm_t` struct with `ps[]` (program stack), `rs[]` (return stack), `sp`, `fp`, `rp`
- Computed goto dispatch table with `&&op_loadconst`, `&&op_call1`, etc.
- `OP_CALL1` pops fn + arg from stack, dispatches by fn type
- `OP_CALLF` pushes return frame, switches to callee bytecode
- `OP_RET` pops frame, pushes result
- Thread-local `__VM` pointer

Lambda `td_eval` path: first call → `td_compile(lambda)` → all subsequent calls → `td_vm_eval(lambda)`

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): stack-based VM with computed goto dispatch"
```

---

### Task 4.3: Error handling (try/raise)

- [ ] Implement error handling

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
static MunitResult test_eval_try(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(try (/ 10 0) (fn [e] 0))");
    munit_assert_int64(result->i64, ==, 0);
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_raise(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(try (raise \"boom\") (fn [e] 42))");
    munit_assert_int64(result->i64, ==, 42);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement trap frames in VM** (same pattern as Rayforce `OP_TRAP` / `OP_TRAP_END`).

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): try/raise error handling with trap frames"
```

---

## Phase 5: Collection & Higher-Order Builtins

### Task 5.1: Vector operations

- [ ] Implement vector operations

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
static MunitResult test_eval_vector_add(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(+ [1 2 3] 10)");
    munit_assert_ptr_not_null(result);
    /* Should produce [11 12 13] */
    munit_assert_int64(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_sum(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(sum [1 2 3 4 5])");
    munit_assert_int64(result->i64, ==, 15);
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_count(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(count [1 2 3])");
    munit_assert_int64(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement `FN_ATOMIC` auto-mapping** — when a `FN_ATOMIC` function receives a vector argument, auto-map element-wise (same as Rayforce's atomic dispatch). Implement `ray_sum`, `ray_count`, `ray_first`, `ray_last`, `ray_min`, `ray_max`, `ray_avg`.

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): FN_ATOMIC auto-mapping and aggregation builtins"
```

---

### Task 5.2: Higher-order functions (map, fold, scan, filter)

- [ ] Implement higher-order functions

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
static MunitResult test_eval_map(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(map + 1 [1 2 3])");
    /* Should produce [2 3 4] */
    munit_assert_int64(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_fold(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(fold + [1 2 3 4 5])");
    munit_assert_int64(result->i64, ==, 15);
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_scan(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(scan + [1 2 3 4 5])");
    /* Should produce [1 3 6 10 15] */
    munit_assert_int64(td_len(result), ==, 5);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement `ray_map`, `ray_fold`, `ray_scan`, `ray_filter`, `ray_apply`**

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): higher-order functions (map, fold, scan, filter)"
```

---

## Phase 6: Table Queries (DAG Bridge)

### Task 6.1: Table construction and column access

- [ ] Implement table construction and column access

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
static MunitResult test_eval_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(table [a b] (list [1 2 3] [10 20 30]))");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_TABLE);
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_at_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table [a b] (list [1 2 3] [10 20 30]))) (at t 'a))");
    munit_assert_ptr_not_null(result);
    munit_assert_int64(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement `ray_table`, `ray_at`**

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): table construction and column access"
```

---

### Task 6.2: Select queries (bridge to Teide DAG)

- [ ] Implement select queries

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
static MunitResult test_eval_select_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table [name salary] (list ['Alice 'Bob 'Charlie] [50000 70000 60000])))"
        "    (select {from: t where: (> salary 55000)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_TABLE);
    /* Should have 2 rows (Bob=70000, Charlie=60000) */
    td_release(result);
    return MUNIT_OK;
}

static MunitResult test_eval_select_groupby(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table [dept salary] (list ['IT 'HR 'IT 'HR] [80000 60000 90000 55000])))"
        "    (select {avg_sal: (avg salary) from: t by: dept}))");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_TABLE);
    td_release(result);
    return MUNIT_OK;
}
```

**Step 2: Implement `ray_select`**

This is the DAG bridge. `ray_select` receives an evaluated dict, extracts `from:`, `where:`, `by:`, and output column expressions. When `from:` is a table:
1. Call `td_graph_new(table)`
2. Walk expression trees, emit DAG nodes via `td_scan`, `td_add`, `td_filter`, `td_group`, etc.
3. Call `td_optimize(g, root)` then `td_execute(g, root)`

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): select queries bridging to Teide DAG executor"
```

---

## Phase 7: I/O & REPL

### Task 7.1: I/O builtins

- [ ] Implement I/O builtins

**Files:**
- Modify: `src/lang/eval.c`
- Modify: `test/test_lang.c`

**Step 1: Add tests**

```c
static MunitResult test_eval_read_csv(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Write a temp CSV, read it back */
    /* Test that read-csv produces a table */
    return MUNIT_OK; /* placeholder */
}
```

**Step 2: Implement `ray_read_csv`, `ray_write_csv`, `ray_println`, `ray_read`, `ray_write`**

Wire these to Teide's existing `td_csv_load` and file I/O.

**Step 3: Run tests, commit**

```bash
git commit -m "feat(lang): I/O builtins (read-csv, write-csv, println)"
```

---

### Task 7.2: REPL binary

- [ ] Implement REPL binary

**Files:**
- Create: `src/lang/repl.c`
- Modify: `CMakeLists.txt`

**Step 1: Create `src/lang/repl.c`**

A simple read-eval-print loop:
```c
int main(int argc, char* argv[]) {
    td_heap_init();
    td_sym_init();
    td_lang_init();

    if (argc > 1) {
        /* File mode: read and eval file */
    } else {
        /* REPL mode */
        char line[4096];
        printf("teide> ");
        while (fgets(line, sizeof(line), stdin)) {
            td_t* result = td_eval_str(line);
            if (!TD_IS_ERR(result)) {
                td_print(result);
                printf("\n");
                td_release(result);
            } else {
                printf("error: %s\n", td_err_str(TD_ERR_CODE(result)));
            }
            printf("teide> ");
        }
    }

    td_lang_destroy();
    td_sym_destroy();
    td_heap_destroy();
    return 0;
}
```

**Step 2: Add to `CMakeLists.txt`**

```cmake
# Rayfall REPL executable
add_executable(teide_repl src/lang/repl.c)
target_link_libraries(teide_repl PRIVATE teide_static)
target_include_directories(teide_repl PRIVATE include src)
```

**Step 3: Build and smoke test**

Run: `cmake --build build && echo '(+ 1 2)' | ./build/teide_repl`
Expected: Output `3`.

**Step 4: Commit**

```bash
git add src/lang/repl.c CMakeLists.txt
git commit -m "feat(lang): Rayfall REPL binary"
```

---

## Phase 8: Heap Threading (Per-VM Heaps)

### Task 8.1: Add heap_id to td_t and per-VM heap allocation

- [ ] Implement heap_id and per-VM heap allocation

**Files:**
- Modify: `include/teide/td.h`
- Modify: `src/mem/buddy.c` (or equivalent allocator file)
- Modify: `test/test_lang.c`

**Step 1: Add `heap_id` field**

Steal 2 bytes from td_t header for `heap_id` (u16). Add foreign block queue and O(1) ownership check on free.

Reference: `/home/hetoku/data/work/rayforce/core/heap.h` and `/home/hetoku/data/work/rayforce/core/heap.c`

**Step 2: Test cross-thread allocation/free**

**Step 3: Commit**

```bash
git commit -m "feat(mem): per-VM heap with heap_id and foreign block queue"
```

---

## Summary: Task Dependency Graph

```
Phase 1: Type Foundation
  1.1 Type tags in td.h
  1.2 Function constructors + env  ← depends on 1.1

Phase 2: Lexer
  2.1 ASCII dispatch + parser      ← depends on 1.2
  2.2 Parse tests                  ← depends on 2.1

Phase 3: Eval
  3.1 Tree-walking eval + arith    ← depends on 2.1
  3.2 set/let/if                   ← depends on 3.1
  3.3 Lambda functions             ← depends on 3.2

Phase 4: VM
  4.1 Bytecode compiler            ← depends on 3.3
  4.2 Stack VM (computed goto)     ← depends on 4.1
  4.3 Error handling (try/raise)   ← depends on 4.2

Phase 5: Collections
  5.1 Vector ops + FN_ATOMIC       ← depends on 3.1
  5.2 map/fold/scan/filter         ← depends on 5.1, 3.3

Phase 6: Table Queries
  6.1 Table construction           ← depends on 3.1
  6.2 select (DAG bridge)          ← depends on 6.1, 5.1

Phase 7: I/O & REPL
  7.1 I/O builtins                 ← depends on 6.1
  7.2 REPL binary                  ← depends on 3.1

Phase 8: Heap Threading
  8.1 Per-VM heaps                 ← independent (can be done in parallel)
```

Phases 5-8 can be worked on in parallel once Phase 3 is done. The critical path is: **1.1 → 1.2 → 2.1 → 3.1 → 3.2 → 3.3 → 4.1 → 4.2**.
