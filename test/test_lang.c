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

#include "munit.h"
#include <rayforce.h>
#include "mem/heap.h"
#include <string.h>

/* Forward declarations for lang modules */
#include "lang/env.h"
#include "lang/parse.h"
#include "lang/eval.h"
#include "lang/format.h"

/* Forward-declare runtime API to avoid ray_vm_t redefinition from runtime.h */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t *__RUNTIME;

/* ═══════════════════════════════════════════════════════════════
 * String-roundtrip assertion macros (mirrors rayforce test style)
 * ═══════════════════════════════════════════════════════════════ */

/* ASSERT_EQ: evaluate both sides, format, compare strings.
 * This mirrors the rayforce TEST_ASSERT_EQ semantics exactly:
 * both LHS and RHS are evaluated as expressions, formatted, then compared. */
#define ASSERT_EQ(expr, expected) do { \
    ray_t* _le = ray_eval_str(expr); \
    if (_le && RAY_IS_ERR(_le)) { \
        ray_t* _es = ray_fmt(_le, 0); \
        const char* _ep = _es ? ray_str_ptr(_es) : "?"; \
        int _en = _es ? (int)ray_str_len(_es) : 1; \
        fprintf(stderr, "  %s:%d: eval error: %.*s\n -- expr: %s\n", \
                __FILE__, __LINE__, _en, _ep, expr); \
        if (_es) ray_release(_es); \
        return MUNIT_FAIL; \
    } \
    ray_t* _re = ray_eval_str(expected); \
    if (_re && RAY_IS_ERR(_re)) { \
        ray_t* _es = ray_fmt(_re, 0); \
        const char* _ep = _es ? ray_str_ptr(_es) : "?"; \
        int _en = _es ? (int)ray_str_len(_es) : 1; \
        fprintf(stderr, "  %s:%d: RHS eval error: %.*s\n -- expected: %s\n", \
                __FILE__, __LINE__, _en, _ep, expected); \
        if (_es) ray_release(_es); \
        if (_le && !RAY_IS_ERR(_le)) ray_release(_le); \
        return MUNIT_FAIL; \
    } \
    ray_t* _ls = _le ? ray_fmt(_le, 0) : NULL; \
    ray_t* _rs = _re ? ray_fmt(_re, 0) : NULL; \
    const char* _lp = _ls ? ray_str_ptr(_ls) : "null"; \
    const char* _rp = _rs ? ray_str_ptr(_rs) : "null"; \
    int _ll = _ls ? (int)ray_str_len(_ls) : 4; \
    int _rl = _rs ? (int)ray_str_len(_rs) : 4; \
    if (_ll != _rl || memcmp(_lp, _rp, (size_t)_rl) != 0) { \
        fprintf(stderr, "  %s:%d: expected \"%.*s\", got \"%.*s\"\n -- expr: %s\n", \
                __FILE__, __LINE__, _rl, _rp, _ll, _lp, expr); \
        if (_le && !RAY_IS_ERR(_le)) ray_release(_le); \
        if (_re && !RAY_IS_ERR(_re)) ray_release(_re); \
        if (_ls) ray_release(_ls); \
        if (_rs) ray_release(_rs); \
        return MUNIT_FAIL; \
    } \
    if (_le && !RAY_IS_ERR(_le)) ray_release(_le); \
    if (_re && !RAY_IS_ERR(_re)) ray_release(_re); \
    if (_ls) ray_release(_ls); \
    if (_rs) ray_release(_rs); \
} while(0)

/* ASSERT_ER: evaluate expr, assert it produces an error */
#define ASSERT_ER(expr, err_substr) do { \
    ray_t* _le = ray_eval_str(expr); \
    if (!RAY_IS_ERR(_le)) { \
        ray_t* _s = ray_fmt(_le, 0); \
        fprintf(stderr, "  %s:%d: expected error, got: %.*s\n -- expr: %s\n", \
                __FILE__, __LINE__, \
                (int)(_s ? ray_str_len(_s) : 0), \
                _s ? ray_str_ptr(_s) : "", expr); \
        if (_s) ray_release(_s); \
        ray_release(_le); \
        return MUNIT_FAIL; \
    } \
} while(0)

/* ---- Setup / Teardown ---- */

static void* lang_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    ray_runtime_create(0, NULL);
    return NULL;
}

static void lang_teardown(void* fixture) {
    (void)fixture;
    ray_runtime_destroy(__RUNTIME);
}

/* ---- Dummy function for testing ---- */
static ray_t* dummy_unary(ray_t* x) { return ray_retain(x), x; }
static ray_t* dummy_binary(ray_t* x, ray_t* y) { (void)y; return ray_retain(x), x; }
static ray_t* dummy_vary(ray_t** args, int64_t n) { (void)n; return ray_retain(args[0]), args[0]; }

/* ---- Test: create unary function object ---- */
static MunitResult test_fn_unary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* fn = ray_fn_unary("neg", RAY_FN_ATOMIC, dummy_unary);
    munit_assert_ptr_not_null(fn);
    munit_assert_false(RAY_IS_ERR(fn));
    munit_assert_int(fn->type, ==, RAY_UNARY);
    munit_assert_uint(fn->attrs & RAY_FN_ATOMIC, !=, 0);
    ray_release(fn);

    return MUNIT_OK;
}

/* ---- Test: create binary function object ---- */
static MunitResult test_fn_binary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* fn = ray_fn_binary("+", RAY_FN_ATOMIC, dummy_binary);
    munit_assert_ptr_not_null(fn);
    munit_assert_int(fn->type, ==, RAY_BINARY);
    ray_release(fn);

    return MUNIT_OK;
}

/* ---- Test: create vary function object ---- */
static MunitResult test_fn_vary(const void* params, void* fixture) {
    (void)params; (void)fixture;

    ray_t* fn = ray_fn_vary("list", RAY_FN_NONE, dummy_vary);
    munit_assert_ptr_not_null(fn);
    munit_assert_int(fn->type, ==, RAY_VARY);
    ray_release(fn);

    return MUNIT_OK;
}

/* ---- Test: lex integer ---- */
static MunitResult test_lex_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("42");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 42);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex negative integer ---- */
static MunitResult test_lex_neg_i64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("-7");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, -7);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex float ---- */
static MunitResult test_lex_f64(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("3.14");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_F64);
    munit_assert_double(result->f64, ==, 3.14);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex string ---- */
static MunitResult test_lex_string(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("\"hello\"");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_STR);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex symbol ---- */
static MunitResult test_lex_symbol(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("'AAPL");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_SYM);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: lex true/false ---- */
static MunitResult test_lex_bool(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* t = ray_parse("true");
    munit_assert_ptr_not_null(t);
    munit_assert_int(t->type, ==, -RAY_BOOL);
    munit_assert_uint(t->b8, ==, 1);
    ray_release(t);

    ray_t* f = ray_parse("false");
    munit_assert_int(f->type, ==, -RAY_BOOL);
    munit_assert_uint(f->b8, ==, 0);
    ray_release(f);
    return MUNIT_OK;
}

/* ---- Test: parse s-expression ---- */
static MunitResult test_parse_sexpr(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("(+ 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    /* Should be a list of 3 elements: [name:"+", 1, 2] */
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse nested s-expressions ---- */
static MunitResult test_parse_nested(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("(+ (* 2 3) 4)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 3);
    /* Second element should be a list (the nested (* 2 3)) */
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[1]->type, ==, RAY_LIST);
    munit_assert_int(ray_len(elems[1]), ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse vector literal ---- */
static MunitResult test_parse_vector(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("[1 2 3]");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    /* Should be a list of 3 i64 elements */
    munit_assert_int(ray_len(result), ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: parse empty list ---- */
static MunitResult test_parse_empty_list(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_parse("()");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 0);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval literal passthrough ---- */
static MunitResult test_eval_literal(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("42");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 42);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval addition ---- */
static MunitResult test_eval_add(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(+ 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval nested arithmetic ---- */
static MunitResult test_eval_nested_arith(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(+ (* 2 3) 4)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 10);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval subtraction ---- */
static MunitResult test_eval_sub(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(- 10 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->i64, ==, 7);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval division ---- */
static MunitResult test_eval_div(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(/ 10 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->i64, ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval comparison ---- */
static MunitResult test_eval_cmp(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(> 5 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_int(result->type, ==, -RAY_BOOL);
    munit_assert_uint(result->b8, ==, 1);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval set ---- */
static MunitResult test_eval_set(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(do (set x 10) x)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 10);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval if true ---- */
static MunitResult test_eval_if_true(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(if true 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 1);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval if false ---- */
static MunitResult test_eval_if_false(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(if false 1 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 2);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval let ---- */
static MunitResult test_eval_let(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(do (let x 5) (+ x 3))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 8);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval lambda ---- */
static MunitResult test_eval_lambda(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(do (set double (fn [x] (* x 2))) (double 5))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 10);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval lambda with multiple params ---- */
static MunitResult test_eval_lambda_multi(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(do (set add3 (fn [a b c] (+ a (+ b c)))) (add3 1 2 3))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 6);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: eval lambda with let in body ---- */
static MunitResult test_eval_lambda_let(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(do (set f (fn [a b] (let c (+ a b)) (+ c 1))) (f 3 4))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 8);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: compile basic lambda ---- */
static MunitResult test_compile_basic(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(do (set f (fn [x] (+ x 1))) (f 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 11);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: compile closure ---- */
static MunitResult test_compile_closure(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Verify compiled lambda with multiple body exprs and let binding */
    ray_t* result = ray_eval_str("(do (set f (fn [a b] (let c (+ a b)) (* c 2))) (f 3 4))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 14);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: VM recursive fibonacci ---- */
static MunitResult test_vm_fib(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set fib (fn [n] (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 55);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: VM tail-recursive loop ---- */
static MunitResult test_vm_loop(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set sum-to (fn [n acc] (if (== n 0) acc (sum-to (- n 1) (+ acc n))))) (sum-to 100 0))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 5050);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: try catches division by zero ---- */
static MunitResult test_eval_try(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(try (/ 10 0) (fn [e] 0))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_true(result->i64 == 0 || result->i64 == INT64_MIN);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: try catches explicit raise ---- */
static MunitResult test_eval_raise(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(try (raise \"boom\") (fn [e] 42))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 42);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: vector + scalar auto-mapping ---- */
static MunitResult test_eval_vector_add(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(+ [1 2 3] 10)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_I64);
    munit_assert_int(ray_len(result), ==, 3);
    int64_t* elems = (int64_t*)ray_data(result);
    munit_assert_int(elems[0], ==, 11);
    munit_assert_int(elems[1], ==, 12);
    munit_assert_int(elems[2], ==, 13);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: vector + vector auto-mapping ---- */
static MunitResult test_eval_vector_add_vec(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(+ [1 2 3] [4 5 6])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_I64);
    munit_assert_int(ray_len(result), ==, 3);
    int64_t* elems = (int64_t*)ray_data(result);
    munit_assert_int(elems[0], ==, 5);
    munit_assert_int(elems[1], ==, 7);
    munit_assert_int(elems[2], ==, 9);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: sum aggregation ---- */
static MunitResult test_eval_sum(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(sum [1 2 3 4 5])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 15);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: count ---- */
static MunitResult test_eval_count(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(count [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: avg ---- */
static MunitResult test_eval_avg(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(avg [2 4 6])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_F64);
    munit_assert_double(result->f64, ==, 4.0);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: min/max ---- */
static MunitResult test_eval_min_max(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* mn = ray_eval_str("(min [5 2 8])");
    munit_assert_ptr_not_null(mn);
    munit_assert_false(RAY_IS_ERR(mn));
    munit_assert_int(mn->type, ==, -RAY_I64);
    munit_assert_int(mn->i64, ==, 2);
    ray_release(mn);

    ray_t* mx = ray_eval_str("(max [5 2 8])");
    munit_assert_ptr_not_null(mx);
    munit_assert_false(RAY_IS_ERR(mx));
    munit_assert_int(mx->type, ==, -RAY_I64);
    munit_assert_int(mx->i64, ==, 8);
    ray_release(mx);
    return MUNIT_OK;
}

/* ---- Test: first/last ---- */
static MunitResult test_eval_first_last(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* f = ray_eval_str("(first [1 2 3])");
    munit_assert_ptr_not_null(f);
    munit_assert_false(RAY_IS_ERR(f));
    munit_assert_int(f->type, ==, -RAY_I64);
    munit_assert_int(f->i64, ==, 1);
    ray_release(f);

    ray_t* l = ray_eval_str("(last [1 2 3])");
    munit_assert_ptr_not_null(l);
    munit_assert_false(RAY_IS_ERR(l));
    munit_assert_int(l->type, ==, -RAY_I64);
    munit_assert_int(l->i64, ==, 3);
    ray_release(l);
    return MUNIT_OK;
}

/* ---- Test: map with binary fn and value ---- */
static MunitResult test_eval_map(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(map + 1 [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 3);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->i64, ==, 2);
    munit_assert_int(elems[1]->i64, ==, 3);
    munit_assert_int(elems[2]->i64, ==, 4);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: pmap with binary fn and value ---- */
static MunitResult test_eval_pmap(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(pmap * 2 [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 3);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->i64, ==, 2);
    munit_assert_int(elems[1]->i64, ==, 4);
    munit_assert_int(elems[2]->i64, ==, 6);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: fold (reduce) ---- */
static MunitResult test_eval_fold(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(fold + [1 2 3 4 5])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 15);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: scan (running fold) ---- */
static MunitResult test_eval_scan(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(scan + [1 2 3 4 5])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 5);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 3);
    munit_assert_int(elems[2]->i64, ==, 6);
    munit_assert_int(elems[3]->i64, ==, 10);
    munit_assert_int(elems[4]->i64, ==, 15);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: filter by boolean mask ---- */
static MunitResult test_eval_filter(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(filter [1 2 3 4 5] [true false true false true])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(ray_len(result), ==, 3);
    if (result->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(result);
        munit_assert_int(d[0], ==, 1);
        munit_assert_int(d[1], ==, 3);
        munit_assert_int(d[2], ==, 5);
    } else {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 1);
        munit_assert_int(elems[1]->i64, ==, 3);
        munit_assert_int(elems[2]->i64, ==, 5);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: apply (zip-apply) ---- */
static MunitResult test_eval_apply(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(apply + [1 2] [3 4])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 2);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->i64, ==, 4);
    munit_assert_int(elems[1]->i64, ==, 6);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: distinct ---- */
static MunitResult test_eval_distinct(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(distinct [1 1 2 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_true(result->type == RAY_LIST || ray_is_vec(result));
    munit_assert_int(ray_len(result), ==, 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 1);
        munit_assert_int(elems[1]->i64, ==, 2);
        munit_assert_int(elems[2]->i64, ==, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        munit_assert_int(vals[0], ==, 1);
        munit_assert_int(vals[1], ==, 2);
        munit_assert_int(vals[2], ==, 3);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: in ---- */
static MunitResult test_eval_in(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(in 2 [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_BOOL);
    munit_assert_uint(result->b8, ==, 1);
    ray_release(result);

    ray_t* result2 = ray_eval_str("(in 9 [1 2 3])");
    munit_assert_ptr_not_null(result2);
    munit_assert_false(RAY_IS_ERR(result2));
    munit_assert_int(result2->type, ==, -RAY_BOOL);
    munit_assert_uint(result2->b8, ==, 0);
    ray_release(result2);
    return MUNIT_OK;
}

/* ---- Test: except ---- */
static MunitResult test_eval_except(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(except [1 2 3] [2])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_true(result->type == RAY_LIST || ray_is_vec(result));
    munit_assert_int(ray_len(result), ==, 2);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 1);
        munit_assert_int(elems[1]->i64, ==, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        munit_assert_int(vals[0], ==, 1);
        munit_assert_int(vals[1], ==, 3);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: union ---- */
static MunitResult test_eval_union(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(union [1 2] [2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_true(result->type == RAY_LIST || ray_is_vec(result));
    munit_assert_int(ray_len(result), ==, 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 1);
        munit_assert_int(elems[1]->i64, ==, 2);
        munit_assert_int(elems[2]->i64, ==, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        munit_assert_int(vals[0], ==, 1);
        munit_assert_int(vals[1], ==, 2);
        munit_assert_int(vals[2], ==, 3);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: sect (intersection) ---- */
static MunitResult test_eval_sect(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(sect [1 2 3] [2 3 4])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_true(result->type == RAY_LIST || ray_is_vec(result));
    munit_assert_int(ray_len(result), ==, 2);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 2);
        munit_assert_int(elems[1]->i64, ==, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        munit_assert_int(vals[0], ==, 2);
        munit_assert_int(vals[1], ==, 3);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: take positive ---- */
static MunitResult test_eval_take(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(take [1 2 3 4 5] 3)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_true(result->type == RAY_LIST || ray_is_vec(result));
    munit_assert_int(ray_len(result), ==, 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 1);
        munit_assert_int(elems[1]->i64, ==, 2);
        munit_assert_int(elems[2]->i64, ==, 3);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        munit_assert_int(vals[0], ==, 1);
        munit_assert_int(vals[1], ==, 2);
        munit_assert_int(vals[2], ==, 3);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: take negative (from end) ---- */
static MunitResult test_eval_take_neg(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(take [1 2 3 4 5] -3)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_true(result->type == RAY_LIST || ray_is_vec(result));
    munit_assert_int(ray_len(result), ==, 3);
    if (result->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(result);
        munit_assert_int(elems[0]->i64, ==, 3);
        munit_assert_int(elems[1]->i64, ==, 4);
        munit_assert_int(elems[2]->i64, ==, 5);
    } else {
        int64_t* vals = (int64_t*)ray_data(result);
        munit_assert_int(vals[0], ==, 3);
        munit_assert_int(vals[1], ==, 4);
        munit_assert_int(vals[2], ==, 5);
    }
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: at (index into vector) ---- */
static MunitResult test_eval_at(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(at [10 20 30] 1)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 20);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: find ---- */
static MunitResult test_eval_find(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(find [1 2 3] 2)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 1);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: reverse ---- */
static MunitResult test_eval_reverse(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(reverse [1 2 3])");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 3);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->i64, ==, 3);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 1);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: table construction ---- */
static MunitResult test_eval_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(table ['a 'b] (list [1 2 3] [10 20 30]))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_ncols(result), ==, 2);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: at table (column access) ---- */
static MunitResult test_eval_at_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (at t 'a))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 3);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->i64, ==, 1);
    munit_assert_int(elems[1]->i64, ==, 2);
    munit_assert_int(elems[2]->i64, ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: key table (column names) ---- */
static MunitResult test_eval_key_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (key t))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_LIST);
    munit_assert_int(ray_len(result), ==, 2);
    ray_t** elems = (ray_t**)ray_data(result);
    munit_assert_int(elems[0]->type, ==, -RAY_SYM);
    munit_assert_int(elems[1]->type, ==, -RAY_SYM);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: count table (row count) ---- */
static MunitResult test_eval_count_table(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) (count t))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: select all ---- */
static MunitResult test_eval_select_all(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(select {from: t}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    munit_assert_int(ray_table_ncols(result), ==, 2);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: select where ---- */
static MunitResult test_eval_select_where(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(select {from: t where: (> salary 55000)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 2);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: select cols (projection) ---- */
static MunitResult test_eval_select_cols(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary 'dept] "
        "(list [1 2 3] [50000 60000 70000] [10 20 10]))) "
        "(select {name: name salary: salary from: t}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_ncols(result), ==, 2);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: select groupby ---- */
static MunitResult test_eval_select_groupby(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['dept 'salary] "
        "(list [1 2 1 2] [50000 60000 70000 80000]))) "
        "(select {avg_sal: (avg salary) from: t by: dept}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 2);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: select xbar (time bucket) ---- */
static MunitResult test_eval_select_xbar(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['ts 'val] "
        "(list [100 250 300 450 500] [1 2 3 4 5]))) "
        "(select {total: (sum val) from: t by: (xbar ts 200)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    /* Buckets: 0(100), 200(250,300), 400(450,500) → 3 groups */
    munit_assert_int(ray_table_nrows(result), ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: update ---- */
static MunitResult test_eval_update(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Update salary to (* salary 2) where name == 2 (second row) */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'dept 'salary] "
        "(list [1 2 3] [10 20 10] [50000 60000 70000]))) "
        "(update {salary: (* salary 2) from: t where: (== name 2)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    /* Check salary column: [50000, 120000, 70000] */
    int64_t sal_id = ray_sym_intern("salary", 6);
    ray_t* sal_col = ray_table_get_col(result, sal_id);
    munit_assert_ptr_not_null(sal_col);
    int64_t* sal_data = (int64_t*)ray_data(sal_col);
    munit_assert_int(sal_data[0], ==, 50000);
    munit_assert_int(sal_data[1], ==, 120000);
    munit_assert_int(sal_data[2], ==, 70000);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: update without where broadcasts scalar ---- */
static MunitResult test_eval_update_no_where(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['x 'y] (list [1 2 3] [10 20 30]))) "
        "(update {x: 99 from: t}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    int64_t x_id = ray_sym_intern("x", 1);
    ray_t* x_col = ray_table_get_col(result, x_id);
    munit_assert_ptr_not_null(x_col);
    munit_assert_int(x_col->type, ==, RAY_I64);
    munit_assert_int(x_col->len, ==, 3);
    int64_t* xd = (int64_t*)ray_data(x_col);
    munit_assert_int(xd[0], ==, 99);
    munit_assert_int(xd[1], ==, 99);
    munit_assert_int(xd[2], ==, 99);
    /* y column should be unchanged */
    int64_t y_id = ray_sym_intern("y", 1);
    ray_t* y_col = ray_table_get_col(result, y_id);
    int64_t* yd = (int64_t*)ray_data(y_col);
    munit_assert_int(yd[0], ==, 10);
    munit_assert_int(yd[1], ==, 20);
    munit_assert_int(yd[2], ==, 30);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: masked update on string column ---- */
static MunitResult test_eval_update_str_masked(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['id 'name] (list [1 2 3] [\"alice\" \"bob\" \"carol\"]))) "
        "(update {name: \"REPLACED\" from: t where: (== id 2)}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* name_col = ray_table_get_col(result, name_id);
    munit_assert_ptr_not_null(name_col);
    munit_assert_int(name_col->type, ==, RAY_STR);
    size_t slen;
    const char* s0 = ray_str_vec_get(name_col, 0, &slen);
    munit_assert_int(slen, ==, 5);
    munit_assert_memory_equal(5, s0, "alice");
    const char* s1 = ray_str_vec_get(name_col, 1, &slen);
    munit_assert_int(slen, ==, 8);
    munit_assert_memory_equal(8, s1, "REPLACED");
    const char* s2 = ray_str_vec_get(name_col, 2, &slen);
    munit_assert_int(slen, ==, 5);
    munit_assert_memory_equal(5, s2, "carol");
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: table with mixed types rejects non-string in string column ---- */
static MunitResult test_eval_table_mixed_type_error(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(table ['s] (list [\"ok\" 42]))");
    munit_assert_true(RAY_IS_ERR(result));
    return MUNIT_OK;
}

/* ---- Test: update string column with non-string expr returns error ---- */
static MunitResult test_eval_update_str_type_mismatch(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['id 'name] (list [1 2 3] [\"alice\" \"bob\" \"carol\"]))) "
        "(update {name: id from: t where: (== id 2)}))");
    munit_assert_true(RAY_IS_ERR(result));
    return MUNIT_OK;
}

/* ---- Test: select constant over empty table ---- */
static MunitResult test_eval_select_empty_const(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Create table then filter all rows to get empty table */
    ray_t* result = ray_eval_str(
        "(do (set t (select {x: x from: (table ['x] (list [1])) where: (== x 0)})) "
        "(select {y: 1 from: t}))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 0);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: insert ---- */
static MunitResult test_eval_insert(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(insert t (list 4 80000)))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 4);
    /* Verify last row */
    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* name_col = ray_table_get_col(result, name_id);
    munit_assert_int(((int64_t*)ray_data(name_col))[3], ==, 4);
    int64_t sal_id = ray_sym_intern("salary", 6);
    ray_t* sal_col = ray_table_get_col(result, sal_id);
    munit_assert_int(((int64_t*)ray_data(sal_col))[3], ==, 80000);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: upsert (update existing row) ---- */
static MunitResult test_eval_upsert(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Upsert by 'name key — row with name=2 exists, update it */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['name 'salary] (list [1 2 3] [50000 60000 70000]))) "
        "(upsert t 'name (list 2 99000)))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 3);
    /* Verify row 2's salary was updated */
    int64_t sal_id = ray_sym_intern("salary", 6);
    ray_t* sal_col = ray_table_get_col(result, sal_id);
    munit_assert_int(((int64_t*)ray_data(sal_col))[1], ==, 99000);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: upsert with F64 key and I64 promotion ---- */
static MunitResult test_eval_upsert_f64_key(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Table has F64 key column; upsert with integer literal should promote */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'v] (list [1.0 2.0] [10 20]))) "
        "(upsert t 'k (list 2 99)))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    /* Should update, not insert — still 2 rows */
    munit_assert_int(ray_table_nrows(result), ==, 2);
    int64_t v_id = ray_sym_intern("v", 1);
    ray_t* v_col = ray_table_get_col(result, v_id);
    munit_assert_int(((int64_t*)ray_data(v_col))[1], ==, 99);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: upsert with string key ---- */
static MunitResult test_eval_upsert_str_key(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'v] (list [\"a\" \"b\"] [1 2]))) "
        "(upsert t 'k (list \"b\" 99)))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    /* Should update row with key "b", not insert */
    munit_assert_int(ray_table_nrows(result), ==, 2);
    int64_t v_id = ray_sym_intern("v", 1);
    ray_t* v_col = ray_table_get_col(result, v_id);
    munit_assert_int(((int64_t*)ray_data(v_col))[1], ==, 99);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: upsert type mismatch returns error ---- */
static MunitResult test_eval_upsert_type_mismatch(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Passing integer key to string key column should return error, not crash */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['k 'v] (list [\"a\" \"b\"] [1 2]))) "
        "(upsert t 'k (list 42 99)))");
    munit_assert_ptr_not_null(result);
    munit_assert_true(RAY_IS_ERR(result));
    return MUNIT_OK;
}

/* ---- Test: left join ---- */
static MunitResult test_eval_left_join(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t1 (table ['id 'name] (list [1 2 3] [10 20 30]))) "
        "(set t2 (table ['id 'val] (list [1 3 4] [100 300 400]))) "
        "(left-join t1 t2 ['id]))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    /* All 3 left rows kept */
    munit_assert_int(ray_table_nrows(result), ==, 3);
    /* Should have columns: id, name, val */
    int64_t val_id = ray_sym_intern("val", 3);
    ray_t* val_col = ray_table_get_col(result, val_id);
    munit_assert_ptr_not_null(val_col);
    int64_t* val_data = (int64_t*)ray_data(val_col);
    munit_assert_int(val_data[0], ==, 100);  /* id=1 matched */
    munit_assert_int(val_data[2], ==, 300);  /* id=3 matched */
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: inner join ---- */
static MunitResult test_eval_inner_join(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str(
        "(do (set t1 (table ['id 'name] (list [1 2 3] [10 20 30]))) "
        "(set t2 (table ['id 'val] (list [1 3 4] [100 300 400]))) "
        "(inner-join t1 t2 ['id]))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    /* Only matching rows: id=1 and id=3 */
    munit_assert_int(ray_table_nrows(result), ==, 2);
    int64_t val_id = ray_sym_intern("val", 3);
    ray_t* val_col = ray_table_get_col(result, val_id);
    munit_assert_ptr_not_null(val_col);
    int64_t* val_data = (int64_t*)ray_data(val_col);
    munit_assert_int(val_data[0], ==, 100);
    munit_assert_int(val_data[1], ==, 300);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: window join (ASOF) ---- */
static MunitResult test_eval_window_join(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* ASOF join: for each left row, find closest right row with ts <= left.ts
     * within same sym partition */
    ray_t* result = ray_eval_str(
        "(do (set trades (table ['sym 'ts 'price] "
        "(list [1 1] [100 200] [10 20]))) "
        "(set quotes (table ['sym 'ts 'bid] "
        "(list [1 1 1] [50 150 250] [5 15 25]))) "
        "(window-join trades quotes ['sym] 'ts))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);
    /* 2 left rows, each matched to closest quote */
    munit_assert_int(ray_table_nrows(result), ==, 2);
    /* bid column from right: ts=100→bid=5 (closest ts=50), ts=200→bid=15 (closest ts=150) */
    int64_t bid_id = ray_sym_intern("bid", 3);
    ray_t* bid_col = ray_table_get_col(result, bid_id);
    munit_assert_ptr_not_null(bid_col);
    int64_t* bid_data = (int64_t*)ray_data(bid_col);
    munit_assert_int(bid_data[0], ==, 5);
    munit_assert_int(bid_data[1], ==, 15);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: println ---- */
static MunitResult test_eval_println(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(println \"hello\")");
    /* println returns RAY_NULL_OBJ (no value — side-effect only) */
    munit_assert_true(RAY_IS_NULL(result));
    return MUNIT_OK;
}

/* ---- Test: read/write CSV roundtrip ---- */
static MunitResult test_eval_read_write_csv(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Create a table, write it to CSV, read it back */
    ray_t* result = ray_eval_str(
        "(do (set t (table ['a 'b] (list [1 2 3] [10 20 30]))) "
        "(write-csv t \"/tmp/test_rayfall.csv\") "
        "(set t2 (read-csv \"/tmp/test_rayfall.csv\")) "
        "(count t2))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 3);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: as (type cast) ---- */
static MunitResult test_eval_as_cast(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(as 'I64 \"42\")");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 42);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Test: type introspection ---- */
static MunitResult test_eval_type(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* type returns a symbol name like 'i64, 'f64, 'b8 */
    ray_t* result = ray_eval_str("(type 42)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_SYM);
    ray_release(result);
    result = ray_eval_str("(type 3.14)");
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_SYM);
    ray_release(result);
    result = ray_eval_str("(type true)");
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_SYM);
    ray_release(result);
    return MUNIT_OK;
}

/* ---- Suite definition ---- */
/* ---- Test: env prefix lookup ---- */
static MunitResult test_env_lookup_prefix(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* After ray_lang_init(), global env has builtins like "select", "sum", etc. */
    const char* results[64];

    /* Exact prefix match for "sum" — should find it */
    int64_t n = ray_env_lookup_prefix("sum", 3, results, 64);
    munit_assert_int((int)n, >=, 1);
    int found_sum = 0;
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(results[i], "sum") == 0) { found_sum = 1; break; }
    }
    munit_assert_true(found_sum);

    /* Prefix "sel" should match "select" */
    n = ray_env_lookup_prefix("sel", 3, results, 64);
    munit_assert_int((int)n, >=, 1);
    int found_select = 0;
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(results[i], "select") == 0) { found_select = 1; break; }
    }
    munit_assert_true(found_select);

    /* Keywords: prefix "fn" should match keyword "fn" */
    n = ray_env_lookup_prefix("fn", 2, results, 64);
    munit_assert_int((int)n, >=, 1);
    int found_fn = 0;
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(results[i], "fn") == 0) { found_fn = 1; break; }
    }
    munit_assert_true(found_fn);

    /* Nonsense prefix should return 0 */
    n = ray_env_lookup_prefix("zzzzz", 5, results, 64);
    munit_assert_int((int)n, ==, 0);

    /* Results should be sorted */
    n = ray_env_lookup_prefix("a", 1, results, 64);
    for (int64_t i = 1; i < n; i++) {
        munit_assert_int(strcmp(results[i - 1], results[i]), <=, 0);
    }

    return MUNIT_OK;
}

/* ---- Verb engine integration tests ---- */

static MunitResult test_verb_sum_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(sum (til 100))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 4950);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_avg_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(avg (til 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_F64);
    munit_assert_double_equal(result->f64, 4.5, 4);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_min_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(min (til 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 0);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_max_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(max (til 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 9);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_count_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(count (til 100))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 100);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_first_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(first (til 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 0);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_last_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(last (til 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 9);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_dev_til(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(dev (til 10))");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_F64);
    munit_assert_double_equal(result->f64, 2.8722813232690143, 4);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_if_sum(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(if (> (sum (til 10)) 0) 1 0)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 1);
    ray_release(result);
    return MUNIT_OK;
}

static MunitResult test_verb_sum_var(const void* params, void* fixture) {
    (void)params; (void)fixture;
    ray_t* result = ray_eval_str("(set x (til 10)) (sum x)");
    munit_assert_ptr_not_null(result);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 45);
    ray_release(result);
    return MUNIT_OK;
}

/* Regression: binary op on boxed list with nested vector used to segfault
 * in release mode because the raw atom fn received a vector argument. */
static MunitResult test_atomic_map_nested_vec(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* scalar + list containing a vector → recursive auto-map */
    ASSERT_EQ("(+ 1 (list 1 2 (til 5)))", "(list 2 3 [1 2 3 4 5])");
    /* scalar + list containing only atoms (homogeneous) */
    ASSERT_EQ("(+ 1 (list 1 2 3))", "[2 3 4]");
    /* scalar + list with nested list */
    ASSERT_EQ("(+ 10 (list 1 (list 2 3)))", "(list 11 (list 12 13))");
    /* type error still propagated for incompatible element */
    ASSERT_ER("(+ 1 (list 1 2 \"s\"))", "type");
    return MUNIT_OK;
}

/* Verify that errors in compiled lambdas produce a trace with source info */
static MunitResult test_error_trace_exists(const void* params, void* fixture) {
    (void)params; (void)fixture;
    /* Eval a lambda that will error — the trace should be built */
    ray_clear_error_trace();
    ray_t* r = ray_eval_str("((fn [x] (+ x \"s\")) 1)");
    munit_assert(RAY_IS_ERR(r));
    ray_t* trace = ray_get_error_trace();
    munit_assert_ptr_not_null(trace);
    munit_assert(ray_len(trace) > 0);
    /* First frame should have a span with non-zero id */
    ray_t* frame = ((ray_t**)ray_data(trace))[0];
    munit_assert_ptr_not_null(frame);
    munit_assert_int(ray_len(frame), ==, 4);
    ray_t** fe = (ray_t**)ray_data(frame);
    munit_assert_ptr_not_null(fe[0]); /* span atom */
    munit_assert(fe[0]->i64 != 0); /* non-zero span */
    ray_clear_error_trace();
    return MUNIT_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * Datalog: recursive rule (semi-naive fixpoint)
 * ═══════════════════════════════════════════════════════════════ */

static MunitResult test_datalog_fixpoint(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* Build an EAV database: 1->2, 2->3, 3->4 */
    ray_t* db = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (set db (assert-fact db 2 'edge 3))"
        "  (set db (assert-fact db 3 'edge 4))"
        "  db)"
    );
    munit_assert(db != NULL);
    munit_assert(!RAY_IS_ERR(db));

    /* Define base rule: (rule (path ?x ?y) (?x :edge ?y)) */
    ray_t* r1 = ray_eval_str("(rule (path ?x ?y) (?x :edge ?y))");
    munit_assert(r1 != NULL);
    munit_assert(!RAY_IS_ERR(r1));
    ray_release(r1);

    /* Define recursive rule: (rule (path ?x ?z) (?x :edge ?y) (path ?y ?z)) */
    ray_t* r2 = ray_eval_str("(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))");
    munit_assert(r2 != NULL);
    munit_assert(!RAY_IS_ERR(r2));
    ray_release(r2);

    /* Query: find all reachable pairs */
    ray_t* result = ray_eval_str(
        "(do"
        "  (set db (datoms))"
        "  (set db (assert-fact db 1 'edge 2))"
        "  (set db (assert-fact db 2 'edge 3))"
        "  (set db (assert-fact db 3 'edge 4))"
        "  (query db (find ?x ?y) (where (path ?x ?y))))"
    );
    munit_assert(result != NULL);
    if (RAY_IS_ERR(result)) {
        ray_t* es = ray_fmt(result, 0);
        fprintf(stderr, "  query error: %.*s\n",
                (int)(es ? ray_str_len(es) : 0), es ? ray_str_ptr(es) : "?");
        if (es) ray_release(es);
        return MUNIT_FAIL;
    }
    munit_assert_int(result->type, ==, RAY_TABLE);

    /* Expect 6 rows: 1->2, 2->3, 3->4, 1->3, 2->4, 1->4 */
    int64_t nrows = ray_table_nrows(result);
    munit_assert_int((int)nrows, ==, 6);

    ray_release(result);
    ray_release(db);
    return MUNIT_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * Ported rayforce lang tests (41 functions, ~3800 assertions)
 * ═══════════════════════════════════════════════════════════════ */
#include "test_lang_rf.inc"

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
    { "/eval/update_no_where", test_eval_update_no_where, lang_setup, lang_teardown, 0, NULL },
    { "/eval/update_str_masked", test_eval_update_str_masked, lang_setup, lang_teardown, 0, NULL },
    { "/eval/table_mixed_type_error", test_eval_table_mixed_type_error, lang_setup, lang_teardown, 0, NULL },
    { "/eval/update_str_type_mismatch", test_eval_update_str_type_mismatch, lang_setup, lang_teardown, 0, NULL },
    { "/eval/select_empty_const", test_eval_select_empty_const, lang_setup, lang_teardown, 0, NULL },
    { "/eval/insert",          test_eval_insert,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/upsert",          test_eval_upsert,          lang_setup, lang_teardown, 0, NULL },
    { "/eval/upsert_f64_key",  test_eval_upsert_f64_key,  lang_setup, lang_teardown, 0, NULL },
    { "/eval/upsert_str_key",  test_eval_upsert_str_key,  lang_setup, lang_teardown, 0, NULL },
    { "/eval/upsert_type_mismatch", test_eval_upsert_type_mismatch, lang_setup, lang_teardown, 0, NULL },
    { "/eval/left_join",       test_eval_left_join,       lang_setup, lang_teardown, 0, NULL },
    { "/eval/inner_join",      test_eval_inner_join,      lang_setup, lang_teardown, 0, NULL },
    { "/eval/window_join",     test_eval_window_join,     lang_setup, lang_teardown, 0, NULL },
    { "/eval/println",         test_eval_println,         lang_setup, lang_teardown, 0, NULL },
    { "/eval/read_write_csv",  test_eval_read_write_csv,  lang_setup, lang_teardown, 0, NULL },
    { "/eval/as_cast",         test_eval_as_cast,         lang_setup, lang_teardown, 0, NULL },
    { "/eval/type",            test_eval_type,            lang_setup, lang_teardown, 0, NULL },
    { "/env/lookup_prefix",    test_env_lookup_prefix,    lang_setup, lang_teardown, 0, NULL },
    { "/verb/sum_til",         test_verb_sum_til,         lang_setup, lang_teardown, 0, NULL },
    { "/verb/avg_til",         test_verb_avg_til,         lang_setup, lang_teardown, 0, NULL },
    { "/verb/min_til",         test_verb_min_til,         lang_setup, lang_teardown, 0, NULL },
    { "/verb/max_til",         test_verb_max_til,         lang_setup, lang_teardown, 0, NULL },
    { "/verb/count_til",       test_verb_count_til,       lang_setup, lang_teardown, 0, NULL },
    { "/verb/first_til",       test_verb_first_til,       lang_setup, lang_teardown, 0, NULL },
    { "/verb/last_til",        test_verb_last_til,        lang_setup, lang_teardown, 0, NULL },
    { "/verb/dev_til",         test_verb_dev_til,         lang_setup, lang_teardown, 0, NULL },
    { "/verb/if_sum",          test_verb_if_sum,          lang_setup, lang_teardown, 0, NULL },
    { "/verb/sum_var",         test_verb_sum_var,         lang_setup, lang_teardown, 0, NULL },
    { "/atomic_map_nested_vec", test_atomic_map_nested_vec, lang_setup, lang_teardown, 0, NULL },
    { "/error_trace_exists",    test_error_trace_exists,    lang_setup, lang_teardown, 0, NULL },
    /* Ported rayforce lang tests */
    { "/rf/map",                   test_rf_map,           lang_setup, lang_teardown, 0, NULL },
    { "/rf/basic",                 test_rf_basic,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/math",                  test_rf_math,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/take",                  test_rf_take,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/split",                 test_rf_split,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/query",                 test_rf_query,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/update",                test_rf_update,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/serde",                 test_rf_serde,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/literals",              test_rf_literals,      lang_setup, lang_teardown, 0, NULL },
    { "/rf/cmp",                   test_rf_cmp,           lang_setup, lang_teardown, 0, NULL },
    { "/rf/distinct",              test_rf_distinct,      lang_setup, lang_teardown, 0, NULL },
    { "/rf/concat",                test_rf_concat,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/raze",                  test_rf_raze,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/filter",                test_rf_filter,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/in",                    test_rf_in,            lang_setup, lang_teardown, 0, NULL },
    { "/rf/except",                test_rf_except,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/or",                    test_rf_or,            lang_setup, lang_teardown, 0, NULL },
    { "/rf/and",                   test_rf_and,           lang_setup, lang_teardown, 0, NULL },
    { "/rf/bin",                   test_rf_bin,           lang_setup, lang_teardown, 0, NULL },
    { "/rf/timestamp",             test_rf_timestamp,     lang_setup, lang_teardown, 0, NULL },
    { "/rf/aggregations",          test_rf_aggregations,  lang_setup, lang_teardown, 0, NULL },
    { "/rf/joins",                 test_rf_joins,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/temporal",              test_rf_temporal,       lang_setup, lang_teardown, 0, NULL },
    { "/rf/iteration",             test_rf_iteration,     lang_setup, lang_teardown, 0, NULL },
    { "/rf/conditionals",          test_rf_conditionals,  lang_setup, lang_teardown, 0, NULL },
    { "/rf/dict",                  test_rf_dict,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/list",                  test_rf_list,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/alter",                 test_rf_alter,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/null",                  test_rf_null,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/set_ops",               test_rf_set_ops,       lang_setup, lang_teardown, 0, NULL },
    { "/rf/cast",                  test_rf_cast,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/lambda",                test_rf_lambda,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/group",                 test_rf_group,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/find",                  test_rf_find,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/rand",                  test_rf_rand,          lang_setup, lang_teardown, 0, NULL },
    { "/rf/unary_ops",             test_rf_unary_ops,     lang_setup, lang_teardown, 0, NULL },
    { "/rf/string_ops",            test_rf_string_ops,    lang_setup, lang_teardown, 0, NULL },
    { "/rf/do_let",                test_rf_do_let,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/error",                 test_rf_error,         lang_setup, lang_teardown, 0, NULL },
    { "/rf/safety",                test_rf_safety,        lang_setup, lang_teardown, 0, NULL },
    { "/rf/read_csv",              test_rf_read_csv,      lang_setup, lang_teardown, 0, NULL },
    { "/datalog/fixpoint",          test_datalog_fixpoint, lang_setup, lang_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_lang_suite = { "/lang", lang_tests, NULL, 1, 0 };
