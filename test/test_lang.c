#include "munit.h"
#include <teide/td.h>
#include <string.h>

/* Forward declarations for lang modules */
#include "lang/env.h"
#include "lang/parse.h"

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
static td_t* dummy_unary(td_t* x) { return td_retain(x), x; }
static td_t* dummy_binary(td_t* x, td_t* y) { (void)y; return td_retain(x), x; }
static td_t* dummy_vary(td_t** args, int64_t n) { (void)n; return td_retain(args[0]), args[0]; }

/* ---- Test: create unary function object ---- */
static MunitResult test_fn_unary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* fn = td_fn_unary("neg", TD_FN_ATOMIC, dummy_unary);
    munit_assert_ptr_not_null(fn);
    munit_assert_false(TD_IS_ERR(fn));
    munit_assert_int(fn->type, ==, TD_ATOM_UNARY);
    munit_assert_uint(fn->attrs & TD_FN_ATOMIC, !=, 0);
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

/* ---- Test: lex integer ---- */
static MunitResult test_lex_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("42");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 42);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex negative integer ---- */
static MunitResult test_lex_neg_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("-7");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, -7);
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

/* ---- Test: parse s-expression ---- */
static MunitResult test_parse_sexpr(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("(+ 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    /* Should be a list of 3 elements: [name:"+", 1, 2] */
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse nested s-expressions ---- */
static MunitResult test_parse_nested(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("(+ (* 2 3) 4)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    /* Second element should be a list (the nested (* 2 3)) */
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[1]->type, ==, TD_LIST);
    munit_assert_int(td_len(elems[1]), ==, 3);
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
    munit_assert_int(td_len(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse empty list ---- */
static MunitResult test_parse_empty_list(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_parse("()");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 0);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Suite definition ---- */
static MunitTest lang_tests[] = {
    { "/fn_unary",   test_fn_unary,   lang_setup, lang_teardown, 0, NULL },
    { "/fn_binary",  test_fn_binary,  lang_setup, lang_teardown, 0, NULL },
    { "/fn_vary",    test_fn_vary,    lang_setup, lang_teardown, 0, NULL },
    { "/lex/i64",    test_lex_i64,    lang_setup, lang_teardown, 0, NULL },
    { "/lex/neg_i64",test_lex_neg_i64,lang_setup, lang_teardown, 0, NULL },
    { "/lex/f64",    test_lex_f64,    lang_setup, lang_teardown, 0, NULL },
    { "/lex/string", test_lex_string, lang_setup, lang_teardown, 0, NULL },
    { "/lex/symbol", test_lex_symbol, lang_setup, lang_teardown, 0, NULL },
    { "/lex/bool",   test_lex_bool,   lang_setup, lang_teardown, 0, NULL },
    { "/parse/sexpr",      test_parse_sexpr,      lang_setup, lang_teardown, 0, NULL },
    { "/parse/nested",     test_parse_nested,     lang_setup, lang_teardown, 0, NULL },
    { "/parse/vector",     test_parse_vector,     lang_setup, lang_teardown, 0, NULL },
    { "/parse/empty_list", test_parse_empty_list, lang_setup, lang_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_lang_suite = { "/lang", lang_tests, NULL, 1, 0 };
