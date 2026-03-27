#include "munit.h"
#include <teide/td.h>
#include <string.h>

/* Forward declarations for lang modules */
#include "lang/env.h"
#include "lang/parse.h"
#include "lang/eval.h"

/* ---- Setup / Teardown ---- */

static void* lang_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    td_heap_init();
    (void)td_sym_init();
    (void)td_lang_init();
    return NULL;
}

static void lang_teardown(void* fixture) {
    (void)fixture;
    td_lang_destroy();
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

/* ---- Test: eval literal passthrough ---- */
static MunitResult test_eval_literal(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("42");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 42);
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
    munit_assert_int(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval nested arithmetic ---- */
static MunitResult test_eval_nested_arith(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(+ (* 2 3) 4)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 10);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval subtraction ---- */
static MunitResult test_eval_sub(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(- 10 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->i64, ==, 7);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval division ---- */
static MunitResult test_eval_div(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(/ 10 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->i64, ==, 3);
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

/* ---- Test: eval set ---- */
static MunitResult test_eval_set(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set x 10) x)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 10);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval if true ---- */
static MunitResult test_eval_if_true(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(if true 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 1);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval if false ---- */
static MunitResult test_eval_if_false(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(if false 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 2);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval let ---- */
static MunitResult test_eval_let(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (let x 5) (+ x 3))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 8);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval lambda ---- */
static MunitResult test_eval_lambda(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set double (fn [x] (* x 2))) (double 5))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 10);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval lambda with multiple params ---- */
static MunitResult test_eval_lambda_multi(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set add3 (fn [a b c] (+ a (+ b c)))) (add3 1 2 3))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 6);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval lambda with let in body ---- */
static MunitResult test_eval_lambda_let(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set f (fn [a b] (let c (+ a b)) (+ c 1))) (f 3 4))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 8);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: compile basic lambda ---- */
static MunitResult test_compile_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(do (set f (fn [x] (+ x 1))) (f 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 11);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: compile closure ---- */
static MunitResult test_compile_closure(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Verify compiled lambda with multiple body exprs and let binding */
    td_t* result = td_eval_str("(do (set f (fn [a b] (let c (+ a b)) (* c 2))) (f 3 4))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 14);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: VM recursive fibonacci ---- */
static MunitResult test_vm_fib(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 55);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: VM tail-recursive loop ---- */
static MunitResult test_vm_loop(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set sum-to (fn [n acc] (if (== n 0) acc (sum-to (- n 1) (+ acc n))))) (sum-to 100 0))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 5050);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: try catches division by zero ---- */
static MunitResult test_eval_try(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(try (/ 10 0) (fn [e] 0))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 0);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: try catches explicit raise ---- */
static MunitResult test_eval_raise(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(try (raise \"boom\") (fn [e] 42))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 42);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: vector + scalar auto-mapping ---- */
static MunitResult test_eval_vector_add(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(+ [1 2 3] 10)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 11);
    munit_assert_int(elems[1]->i64, ==, 12);
    munit_assert_int(elems[2]->i64, ==, 13);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: vector + vector auto-mapping ---- */
static MunitResult test_eval_vector_add_vec(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(+ [1 2 3] [4 5 6])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 5);
    munit_assert_int(elems[1]->i64, ==, 7);
    munit_assert_int(elems[2]->i64, ==, 9);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: sum aggregation ---- */
static MunitResult test_eval_sum(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(sum [1 2 3 4 5])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 15);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: count ---- */
static MunitResult test_eval_count(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(count [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: avg ---- */
static MunitResult test_eval_avg(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(avg [2 4 6])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_F64);
    munit_assert_double(result->f64, ==, 4.0);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: min/max ---- */
static MunitResult test_eval_min_max(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* mn = td_eval_str("(min [5 2 8])");
    munit_assert_ptr_not_null(mn);
    munit_assert_false(TD_IS_ERR(mn));
    munit_assert_int(mn->type, ==, TD_ATOM_I64);
    munit_assert_int(mn->i64, ==, 2);
    td_release(mn);

    td_t* mx = td_eval_str("(max [5 2 8])");
    munit_assert_ptr_not_null(mx);
    munit_assert_false(TD_IS_ERR(mx));
    munit_assert_int(mx->type, ==, TD_ATOM_I64);
    munit_assert_int(mx->i64, ==, 8);
    td_release(mx);
    return MUNIT_OK;
}

/* ---- Test: first/last ---- */
static MunitResult test_eval_first_last(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* f = td_eval_str("(first [1 2 3])");
    munit_assert_ptr_not_null(f);
    munit_assert_false(TD_IS_ERR(f));
    munit_assert_int(f->type, ==, TD_ATOM_I64);
    munit_assert_int(f->i64, ==, 1);
    td_release(f);

    td_t* l = td_eval_str("(last [1 2 3])");
    munit_assert_ptr_not_null(l);
    munit_assert_false(TD_IS_ERR(l));
    munit_assert_int(l->type, ==, TD_ATOM_I64);
    munit_assert_int(l->i64, ==, 3);
    td_release(l);
    return MUNIT_OK;
}

/* ---- Test: map with binary fn and value ---- */
static MunitResult test_eval_map(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(map + 1 [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 2);
    munit_assert_int(elems[1]->i64, ==, 3);
    munit_assert_int(elems[2]->i64, ==, 4);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: pmap with binary fn and value ---- */
static MunitResult test_eval_pmap(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(pmap * 2 [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 2);
    munit_assert_int(elems[1]->i64, ==, 4);
    munit_assert_int(elems[2]->i64, ==, 6);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: fold (reduce) ---- */
static MunitResult test_eval_fold(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(fold + [1 2 3 4 5])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 15);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: scan (running fold) ---- */
static MunitResult test_eval_scan(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(scan + [1 2 3 4 5])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 5);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 3);
    munit_assert_int(elems[2]->i64, ==, 6);
    munit_assert_int(elems[3]->i64, ==, 10);
    munit_assert_int(elems[4]->i64, ==, 15);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: filter by boolean mask ---- */
static MunitResult test_eval_filter(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(filter [1 2 3 4 5] [true false true false true])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 3);
    munit_assert_int(elems[2]->i64, ==, 5);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: apply (zip-apply) ---- */
static MunitResult test_eval_apply(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(apply + [1 2] [3 4])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 2);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 4);
    munit_assert_int(elems[1]->i64, ==, 6);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: distinct ---- */
static MunitResult test_eval_distinct(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(distinct [1 1 2 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: in ---- */
static MunitResult test_eval_in(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(in 2 [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_BOOL);
    munit_assert_uint(result->b8, ==, 1);
    td_release(result);

    td_t* result2 = td_eval_str("(in 9 [1 2 3])");
    munit_assert_ptr_not_null(result2);
    munit_assert_false(TD_IS_ERR(result2));
    munit_assert_int(result2->type, ==, TD_ATOM_BOOL);
    munit_assert_uint(result2->b8, ==, 0);
    td_release(result2);
    return MUNIT_OK;
}

/* ---- Test: except ---- */
static MunitResult test_eval_except(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(except [1 2 3] [2])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 2);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: union ---- */
static MunitResult test_eval_union(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(union [1 2] [2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: sect (intersection) ---- */
static MunitResult test_eval_sect(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(sect [1 2 3] [2 3 4])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 2);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 2);
    munit_assert_int(elems[1]->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: take positive ---- */
static MunitResult test_eval_take(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(take [1 2 3 4 5] 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: take negative (from end) ---- */
static MunitResult test_eval_take_neg(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(take [1 2 3 4 5] -3)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 3);
    munit_assert_int(elems[1]->i64, ==, 4);
    munit_assert_int(elems[2]->i64, ==, 5);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: at (index into vector) ---- */
static MunitResult test_eval_at(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(at [10 20 30] 1)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 20);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: find ---- */
static MunitResult test_eval_find(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(find [1 2 3] 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 1);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: reverse ---- */
static MunitResult test_eval_reverse(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(reverse [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 3);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 1);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: table construction ---- */
static MunitResult test_eval_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(table ['a 'b] (list [1 2 3] [10 20 30]))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_ncols(result), ==, 2);
    munit_assert_int(td_table_nrows(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: at table (column access) ---- */
static MunitResult test_eval_at_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (at t 'a))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 3);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: key table (column names) ---- */
static MunitResult test_eval_key_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (key t))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_LIST);
    munit_assert_int(td_len(result), ==, 2);
    td_t** elems = (td_t**)td_data(result);
    munit_assert_int(elems[0]->type, ==, TD_ATOM_SYM);
    munit_assert_int(elems[1]->type, ==, TD_ATOM_SYM);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: count table (row count) ---- */
static MunitResult test_eval_count_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (count t))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: select all ---- */
static MunitResult test_eval_select_all(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(select {from: t}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);
    munit_assert_int(td_table_ncols(result), ==, 2);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: select where ---- */
static MunitResult test_eval_select_where(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(select {from: t where: (> salary 55000)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 2);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: select cols (projection) ---- */
static MunitResult test_eval_select_cols(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['name 'salary 'dept] "
        "(list [1 2 3] [50000 60000 70000] [10 20 10]))) "
        "(select {name: name salary: salary from: t}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_ncols(result), ==, 2);
    munit_assert_int(td_table_nrows(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: select groupby ---- */
static MunitResult test_eval_select_groupby(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['dept 'salary] "
        "(list [1 2 1 2] [50000 60000 70000 80000]))) "
        "(select {avg_sal: (avg salary) from: t by: dept}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 2);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: select xbar (time bucket) ---- */
static MunitResult test_eval_select_xbar(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['ts 'val] "
        "(list [100 250 300 450 500] [1 2 3 4 5]))) "
        "(select {total: (sum val) from: t by: (xbar ts 200)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    /* Buckets: 0(100), 200(250,300), 400(450,500) → 3 groups */
    munit_assert_int(td_table_nrows(result), ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: update ---- */
static MunitResult test_eval_update(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Update salary to (* salary 2) where name == 2 (second row) */
    td_t* result = td_eval_str(
        "(do (set t (table ['name 'dept 'salary] "
        "(list [1 2 3] [10 20 10] [50000 60000 70000]))) "
        "(update {salary: (* salary 2) from: t where: (== name 2)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);
    /* Check salary column: [50000, 120000, 70000] */
    int64_t sal_id = td_sym_intern("salary", 6);
    td_t* sal_col = td_table_get_col(result, sal_id);
    munit_assert_ptr_not_null(sal_col);
    int64_t* sal_data = (int64_t*)td_data(sal_col);
    munit_assert_int(sal_data[0], ==, 50000);
    munit_assert_int(sal_data[1], ==, 120000);
    munit_assert_int(sal_data[2], ==, 70000);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: insert ---- */
static MunitResult test_eval_insert(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(insert t (list 4 80000)))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 4);
    /* Verify last row */
    int64_t name_id = td_sym_intern("name", 4);
    td_t* name_col = td_table_get_col(result, name_id);
    munit_assert_int(((int64_t*)td_data(name_col))[3], ==, 4);
    int64_t sal_id = td_sym_intern("salary", 6);
    td_t* sal_col = td_table_get_col(result, sal_id);
    munit_assert_int(((int64_t*)td_data(sal_col))[3], ==, 80000);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: upsert (update existing row) ---- */
static MunitResult test_eval_upsert(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Upsert by 'name key — row with name=2 exists, update it */
    td_t* result = td_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(upsert t 'name (list 2 99000)))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);
    /* Verify row 2's salary was updated */
    int64_t sal_id = td_sym_intern("salary", 6);
    td_t* sal_col = td_table_get_col(result, sal_id);
    munit_assert_int(((int64_t*)td_data(sal_col))[1], ==, 99000);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: left join ---- */
static MunitResult test_eval_left_join(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t1 (table ['id 'name] (list [1 2 3] [10 20 30]))) "
        "(set t2 (table ['id 'val] (list [1 3 4] [100 300 400]))) "
        "(left-join t1 t2 ['id]))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    /* All 3 left rows kept */
    munit_assert_int(td_table_nrows(result), ==, 3);
    /* Should have columns: id, name, val */
    int64_t val_id = td_sym_intern("val", 3);
    td_t* val_col = td_table_get_col(result, val_id);
    munit_assert_ptr_not_null(val_col);
    int64_t* val_data = (int64_t*)td_data(val_col);
    munit_assert_int(val_data[0], ==, 100);  /* id=1 matched */
    munit_assert_int(val_data[2], ==, 300);  /* id=3 matched */
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: inner join ---- */
static MunitResult test_eval_inner_join(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str(
        "(do (set t1 (table ['id 'name] (list [1 2 3] [10 20 30]))) "
        "(set t2 (table ['id 'val] (list [1 3 4] [100 300 400]))) "
        "(inner-join t1 t2 ['id]))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    /* Only matching rows: id=1 and id=3 */
    munit_assert_int(td_table_nrows(result), ==, 2);
    int64_t val_id = td_sym_intern("val", 3);
    td_t* val_col = td_table_get_col(result, val_id);
    munit_assert_ptr_not_null(val_col);
    int64_t* val_data = (int64_t*)td_data(val_col);
    munit_assert_int(val_data[0], ==, 100);
    munit_assert_int(val_data[1], ==, 300);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: window join (ASOF) ---- */
static MunitResult test_eval_window_join(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* ASOF join: for each left row, find closest right row with ts <= left.ts
     * within same sym partition */
    td_t* result = td_eval_str(
        "(do (set trades (table ['sym 'ts 'price] "
        "(list [1 1] [100 200] [10 20]))) "
        "(set quotes (table ['sym 'ts 'bid] "
        "(list [1 1 1] [50 150 250] [5 15 25]))) "
        "(window-join trades quotes ['sym] 'ts))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    /* 2 left rows, each matched to closest quote */
    munit_assert_int(td_table_nrows(result), ==, 2);
    /* bid column from right: ts=100→bid=5 (closest ts=50), ts=200→bid=15 (closest ts=150) */
    int64_t bid_id = td_sym_intern("bid", 3);
    td_t* bid_col = td_table_get_col(result, bid_id);
    munit_assert_ptr_not_null(bid_col);
    int64_t* bid_data = (int64_t*)td_data(bid_col);
    munit_assert_int(bid_data[0], ==, 5);
    munit_assert_int(bid_data[1], ==, 15);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: println ---- */
static MunitResult test_eval_println(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(println \"hello\")");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    /* println returns null (i64 0) */
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 0);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: read/write CSV roundtrip ---- */
static MunitResult test_eval_read_write_csv(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Create a table, write it to CSV, read it back */
    td_t* result = td_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) "
        "(write-csv t \"/tmp/test_rayfall.csv\") "
        "(set t2 (read-csv \"/tmp/test_rayfall.csv\")) "
        "(count t2))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 3);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: as (type cast) ---- */
static MunitResult test_eval_as_cast(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* result = td_eval_str("(as 'I64 \"42\")");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, 42);
    td_release(result);
    return MUNIT_OK;
}

/* ---- Test: type introspection ---- */
static MunitResult test_eval_type(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* type of i64 literal */
    td_t* result = td_eval_str("(type 42)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_ATOM_I64);
    munit_assert_int(result->i64, ==, TD_ATOM_I64);
    td_release(result);
    /* type of f64 literal */
    result = td_eval_str("(type 3.14)");
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->i64, ==, TD_ATOM_F64);
    td_release(result);
    /* type of boolean */
    result = td_eval_str("(type true)");
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->i64, ==, TD_ATOM_BOOL);
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
    { "/eval/literal",      test_eval_literal,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/add",          test_eval_add,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/nested_arith", test_eval_nested_arith, lang_setup, lang_teardown, 0, NULL },
    { "/eval/sub",          test_eval_sub,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/div",          test_eval_div,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/cmp",          test_eval_cmp,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/set",          test_eval_set,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/if_true",      test_eval_if_true,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/if_false",     test_eval_if_false,     lang_setup, lang_teardown, 0, NULL },
    { "/eval/let",          test_eval_let,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/lambda",       test_eval_lambda,       lang_setup, lang_teardown, 0, NULL },
    { "/eval/lambda_multi", test_eval_lambda_multi, lang_setup, lang_teardown, 0, NULL },
    { "/eval/lambda_let",   test_eval_lambda_let,   lang_setup, lang_teardown, 0, NULL },
    { "/compile/basic",     test_compile_basic,     lang_setup, lang_teardown, 0, NULL },
    { "/compile/closure",   test_compile_closure,   lang_setup, lang_teardown, 0, NULL },
    { "/vm/fib",            test_vm_fib,            lang_setup, lang_teardown, 0, NULL },
    { "/vm/loop",           test_vm_loop,           lang_setup, lang_teardown, 0, NULL },
    { "/eval/try",          test_eval_try,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/raise",        test_eval_raise,        lang_setup, lang_teardown, 0, NULL },
    { "/eval/vector_add",      test_eval_vector_add,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/vector_add_vec",  test_eval_vector_add_vec,  lang_setup, lang_teardown, 0, NULL },
    { "/eval/sum",             test_eval_sum,             lang_setup, lang_teardown, 0, NULL },
    { "/eval/count",           test_eval_count,           lang_setup, lang_teardown, 0, NULL },
    { "/eval/avg",             test_eval_avg,             lang_setup, lang_teardown, 0, NULL },
    { "/eval/min_max",         test_eval_min_max,         lang_setup, lang_teardown, 0, NULL },
    { "/eval/first_last",      test_eval_first_last,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/map",             test_eval_map,             lang_setup, lang_teardown, 0, NULL },
    { "/eval/pmap",            test_eval_pmap,            lang_setup, lang_teardown, 0, NULL },
    { "/eval/fold",            test_eval_fold,            lang_setup, lang_teardown, 0, NULL },
    { "/eval/scan",            test_eval_scan,            lang_setup, lang_teardown, 0, NULL },
    { "/eval/filter",          test_eval_filter,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/apply",           test_eval_apply,           lang_setup, lang_teardown, 0, NULL },
    { "/eval/distinct",        test_eval_distinct,        lang_setup, lang_teardown, 0, NULL },
    { "/eval/in",              test_eval_in,              lang_setup, lang_teardown, 0, NULL },
    { "/eval/except",          test_eval_except,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/union",           test_eval_union,           lang_setup, lang_teardown, 0, NULL },
    { "/eval/sect",            test_eval_sect,            lang_setup, lang_teardown, 0, NULL },
    { "/eval/take",            test_eval_take,            lang_setup, lang_teardown, 0, NULL },
    { "/eval/take_neg",        test_eval_take_neg,        lang_setup, lang_teardown, 0, NULL },
    { "/eval/at",              test_eval_at,              lang_setup, lang_teardown, 0, NULL },
    { "/eval/find",            test_eval_find,            lang_setup, lang_teardown, 0, NULL },
    { "/eval/reverse",         test_eval_reverse,         lang_setup, lang_teardown, 0, NULL },
    { "/eval/table",           test_eval_table,           lang_setup, lang_teardown, 0, NULL },
    { "/eval/at_table",        test_eval_at_table,        lang_setup, lang_teardown, 0, NULL },
    { "/eval/key_table",       test_eval_key_table,       lang_setup, lang_teardown, 0, NULL },
    { "/eval/count_table",     test_eval_count_table,     lang_setup, lang_teardown, 0, NULL },
    { "/eval/select_all",      test_eval_select_all,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/select_where",    test_eval_select_where,    lang_setup, lang_teardown, 0, NULL },
    { "/eval/select_cols",     test_eval_select_cols,     lang_setup, lang_teardown, 0, NULL },
    { "/eval/select_groupby",  test_eval_select_groupby,  lang_setup, lang_teardown, 0, NULL },
    { "/eval/select_xbar",     test_eval_select_xbar,     lang_setup, lang_teardown, 0, NULL },
    { "/eval/update",          test_eval_update,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/insert",          test_eval_insert,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/upsert",          test_eval_upsert,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/left_join",       test_eval_left_join,       lang_setup, lang_teardown, 0, NULL },
    { "/eval/inner_join",      test_eval_inner_join,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/window_join",     test_eval_window_join,     lang_setup, lang_teardown, 0, NULL },
    { "/eval/println",         test_eval_println,         lang_setup, lang_teardown, 0, NULL },
    { "/eval/read_write_csv",  test_eval_read_write_csv,  lang_setup, lang_teardown, 0, NULL },
    { "/eval/as_cast",         test_eval_as_cast,         lang_setup, lang_teardown, 0, NULL },
    { "/eval/type",            test_eval_type,            lang_setup, lang_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_lang_suite = { "/lang", lang_tests, NULL, 1, 0 };
