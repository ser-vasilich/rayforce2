#include "munit.h"
#include <teide/td.h>
#include <string.h>

/* Forward declaration for env functions */
#include "lang/env.h"

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

/* ---- Suite definition ---- */
static MunitTest lang_tests[] = {
    { "/fn_unary",  test_fn_unary,  lang_setup, lang_teardown, 0, NULL },
    { "/fn_binary", test_fn_binary, lang_setup, lang_teardown, 0, NULL },
    { "/fn_vary",   test_fn_vary,   lang_setup, lang_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_lang_suite = { "/lang", lang_tests, NULL, 1, 0 };
