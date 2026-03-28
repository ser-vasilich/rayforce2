/*
 *   Copyright (c) 2024-2026 Anton Kundenko <singaraiona@gmail.com>
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
#include "ops/ops.h"
#include <string.h>
#include <math.h>

/* --------------------------------------------------------------------------
 * Helper: create a test table with columns id1(I64), v1(I64), v3(F64)
 * -------------------------------------------------------------------------- */

static ray_t* make_test_table(void) {
    (void)ray_sym_init();

    int64_t n = 10;
    int64_t id1_data[] = {1, 1, 2, 2, 3, 3, 1, 2, 3, 1};
    int64_t v1_data[]  = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    double  v3_data[]  = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5};

    ray_t* id1_vec = ray_vec_from_raw(RAY_I64, id1_data, n);
    ray_t* v1_vec  = ray_vec_from_raw(RAY_I64, v1_data, n);
    ray_t* v3_vec  = ray_vec_from_raw(RAY_F64, v3_data, n);

    int64_t name_id1 = ray_sym_intern("id1", 3);
    int64_t name_v1  = ray_sym_intern("v1", 2);
    int64_t name_v3  = ray_sym_intern("v3", 2);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, name_id1, id1_vec);
    tbl = ray_table_add_col(tbl, name_v1, v1_vec);
    tbl = ray_table_add_col(tbl, name_v3, v3_vec);

    ray_release(id1_vec);
    ray_release(v1_vec);
    ray_release(v3_vec);

    return tbl;
}

/* --------------------------------------------------------------------------
 * Test: scan + sum
 * -------------------------------------------------------------------------- */

static MunitResult test_scan_sum(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    munit_assert_ptr_not_null(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g);

    ray_op_t* v1 = ray_scan(g, "v1");
    munit_assert_ptr_not_null(v1);

    ray_op_t* result_op = ray_sum(g, v1);
    ray_t* result = ray_execute(g, result_op);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, -RAY_I64);
    munit_assert_int(result->i64, ==, 550);  /* 10+20+...+100 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: scan + filter + count
 * -------------------------------------------------------------------------- */

static MunitResult test_filter_count(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* threshold = ray_const_i64(g, 50);
    ray_op_t* pred = ray_ge(g, v1, threshold);
    ray_op_t* filtered = ray_filter(g, v1, pred);
    ray_op_t* cnt = ray_count(g, filtered);

    ray_t* result = ray_execute(g, cnt);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 6);  /* 50,60,70,80,90,100 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: arithmetic + reduction
 * -------------------------------------------------------------------------- */

static MunitResult test_arithmetic(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v3 = ray_scan(g, "v3");
    ray_op_t* two = ray_const_f64(g, 2.0);
    ray_op_t* doubled = ray_mul(g, v3, two);
    ray_op_t* total = ray_sum(g, doubled);

    ray_t* result = ray_execute(g, total);
    munit_assert_false(RAY_IS_ERR(result));
    /* sum(v3) = 60.0, doubled = 120.0 */
    munit_assert_double_equal(result->f64, 120.0, 6);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: group by id1, sum(v1)
 * -------------------------------------------------------------------------- */

static MunitResult test_group_sum(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* key = ray_scan(g, "id1");
    ray_op_t* val = ray_scan(g, "v1");

    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->type, ==, RAY_TABLE);

    /* Should have 3 groups (id1=1,2,3) */
    munit_assert_int(ray_table_ncols(result), ==, 2);
    int64_t nrows = ray_table_nrows(result);
    munit_assert_int(nrows, ==, 3);

    /* Verify sums: id1=1: 10+20+70+100=200, id1=2: 30+40+80=150, id1=3: 50+60+90=200 */
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    munit_assert_ptr_not_null(sum_col);

    int64_t sum1 = 0, sum2 = 0, sum3 = 0;
    ray_t* id_col = ray_table_get_col_idx(result, 0);
    for (int64_t i = 0; i < nrows; i++) {
        int64_t id = ((int64_t*)ray_data(id_col))[i];
        int64_t s = ((int64_t*)ray_data(sum_col))[i];
        if (id == 1) sum1 = s;
        else if (id == 2) sum2 = s;
        else if (id == 3) sum3 = s;
    }
    munit_assert_int(sum1, ==, 200);
    munit_assert_int(sum2, ==, 150);
    munit_assert_int(sum3, ==, 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: graph new/free
 * -------------------------------------------------------------------------- */

static MunitResult test_graph_lifecycle(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g);
    munit_assert_uint(g->node_count, ==, 0);

    ray_op_t* v1 = ray_scan(g, "v1");
    munit_assert_ptr_not_null(v1);
    munit_assert_uint(v1->opcode, ==, OP_SCAN);
    munit_assert_uint(g->node_count, ==, 1);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: optimizer constant-folds scalar literal expressions
 * -------------------------------------------------------------------------- */

static MunitResult test_optimizer_constant_fold(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    ray_graph_t* g = ray_graph_new(NULL);
    munit_assert_ptr_not_null(g);

    ray_op_t* c1 = ray_const_i64(g, 2);
    ray_op_t* c2 = ray_const_i64(g, 3);
    ray_op_t* mul = ray_mul(g, c1, c2);       /* 6 */
    ray_op_t* c3 = ray_const_i64(g, 5);
    ray_op_t* add = ray_add(g, mul, c3);      /* 11 */
    munit_assert_ptr_not_null(add);
    munit_assert_int(add->opcode, ==, OP_ADD);

    ray_op_t* opt = ray_optimize(g, add);
    munit_assert_ptr_not_null(opt);
    munit_assert_int(opt->opcode, ==, OP_CONST);

    ray_t* out = ray_execute(g, opt);
    munit_assert_false(RAY_IS_ERR(out));
    munit_assert_int(out->type, ==, -RAY_I64);
    munit_assert_int(out->i64, ==, 11);

    ray_release(out);
    ray_graph_free(g);

    /* Also verify comparison folding to BOOL atom. */
    g = ray_graph_new(NULL);
    munit_assert_ptr_not_null(g);
    ray_op_t* a = ray_const_i64(g, 4);
    ray_op_t* b = ray_const_i64(g, 9);
    ray_op_t* lt = ray_lt(g, a, b);
    ray_op_t* opt_bool = ray_optimize(g, lt);
    munit_assert_ptr_not_null(opt_bool);
    munit_assert_int(opt_bool->opcode, ==, OP_CONST);

    ray_t* out_bool = ray_execute(g, opt_bool);
    munit_assert_false(RAY_IS_ERR(out_bool));
    munit_assert_int(out_bool->type, ==, -RAY_BOOL);
    munit_assert_int(out_bool->u8, ==, 1);
    ray_release(out_bool);
    ray_graph_free(g);

    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: optimizer simplifies FILTER with constant predicates
 * -------------------------------------------------------------------------- */

static MunitResult test_optimizer_filter_const_predicate(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_test_table();
    munit_assert_ptr_not_null(tbl);

    /* FILTER(..., true) -> pass-through */
    ray_graph_t* g_true = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_true);
    ray_op_t* v1_true = ray_scan(g_true, "v1");
    ray_op_t* pred_true = ray_const_bool(g_true, true);
    ray_op_t* filt_true = ray_filter(g_true, v1_true, pred_true);
    ray_op_t* opt_true = ray_optimize(g_true, filt_true);
    munit_assert_ptr_not_null(opt_true);
    munit_assert_int(opt_true->opcode, ==, OP_MATERIALIZE);
    ray_t* out_true = ray_execute(g_true, opt_true);
    munit_assert_false(RAY_IS_ERR(out_true));
    munit_assert_int(out_true->type, ==, RAY_I64);
    munit_assert_int(out_true->len, ==, 10);
    ray_release(out_true);
    ray_graph_free(g_true);

    /* FILTER(..., false) -> empty via HEAD 0 */
    ray_graph_t* g_false = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_false);
    ray_op_t* v1_false = ray_scan(g_false, "v1");
    ray_op_t* pred_false = ray_const_bool(g_false, false);
    ray_op_t* filt_false = ray_filter(g_false, v1_false, pred_false);
    ray_op_t* opt_false = ray_optimize(g_false, filt_false);
    munit_assert_ptr_not_null(opt_false);
    munit_assert_int(opt_false->opcode, ==, OP_HEAD);
    ray_t* out_false = ray_execute(g_false, opt_false);
    munit_assert_false(RAY_IS_ERR(out_false));
    munit_assert_int(out_false->type, ==, RAY_I64);
    munit_assert_int(out_false->len, ==, 0);
    ray_release(out_false);
    ray_graph_free(g_false);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Test: group aggregation over affine inputs (SUM/AVG(v1 + const))
 * -------------------------------------------------------------------------- */

static MunitResult test_group_affine_agg_input(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_test_table();
    munit_assert_ptr_not_null(tbl);

    /* Scalar aggregate: SUM(v1 + 1), AVG(v1 + 1) */
    ray_graph_t* g_scalar = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_scalar);
    ray_op_t* v1_scalar = ray_scan(g_scalar, "v1");
    ray_op_t* one_scalar = ray_const_i64(g_scalar, 1);
    ray_op_t* expr_scalar = ray_add(g_scalar, v1_scalar, one_scalar);
    uint16_t scalar_ops[] = { OP_SUM, OP_AVG };
    ray_op_t* scalar_ins[] = { expr_scalar, expr_scalar };
    ray_op_t* grp_scalar = ray_group(g_scalar, NULL, 0, scalar_ops, scalar_ins, 2);
    ray_t* out_scalar = ray_execute(g_scalar, grp_scalar);
    munit_assert_false(RAY_IS_ERR(out_scalar));
    munit_assert_int(out_scalar->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(out_scalar), ==, 1);
    munit_assert_int(ray_table_ncols(out_scalar), ==, 2);

    ray_t* sum_col = ray_table_get_col_idx(out_scalar, 0);
    ray_t* avg_col = ray_table_get_col_idx(out_scalar, 1);
    munit_assert_ptr_not_null(sum_col);
    munit_assert_ptr_not_null(avg_col);
    munit_assert_int(((int64_t*)ray_data(sum_col))[0], ==, 560);
    munit_assert_double_equal(((double*)ray_data(avg_col))[0], 56.0, 6);
    ray_release(out_scalar);
    ray_graph_free(g_scalar);

    /* Scalar variants: SUM(1 + v1), SUM(v1 - 1) */
    ray_graph_t* g_scalar2 = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_scalar2);
    ray_op_t* v1_s2 = ray_scan(g_scalar2, "v1");
    ray_op_t* one_s2a = ray_const_i64(g_scalar2, 1);
    ray_op_t* one_plus_v1 = ray_add(g_scalar2, one_s2a, v1_s2);
    ray_op_t* one_s2b = ray_const_i64(g_scalar2, 1);
    ray_op_t* v1_minus_one = ray_sub(g_scalar2, v1_s2, one_s2b);
    uint16_t scalar2_ops[] = { OP_SUM, OP_SUM };
    ray_op_t* scalar2_ins[] = { one_plus_v1, v1_minus_one };
    ray_op_t* grp_scalar2 = ray_group(g_scalar2, NULL, 0, scalar2_ops, scalar2_ins, 2);
    ray_t* out_scalar2 = ray_execute(g_scalar2, grp_scalar2);
    munit_assert_false(RAY_IS_ERR(out_scalar2));
    munit_assert_int(ray_table_nrows(out_scalar2), ==, 1);
    munit_assert_int(ray_table_ncols(out_scalar2), ==, 2);
    ray_t* sum_plus_col = ray_table_get_col_idx(out_scalar2, 0);
    ray_t* sum_minus_col = ray_table_get_col_idx(out_scalar2, 1);
    munit_assert_ptr_not_null(sum_plus_col);
    munit_assert_ptr_not_null(sum_minus_col);
    munit_assert_int(((int64_t*)ray_data(sum_plus_col))[0], ==, 560);
    munit_assert_int(((int64_t*)ray_data(sum_minus_col))[0], ==, 540);
    ray_release(out_scalar2);
    ray_graph_free(g_scalar2);

    /* Nested scalar expression in agg input: SUM(v1 + (2 * 1)) */
    ray_graph_t* g_nested = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_nested);
    ray_op_t* v1_nested = ray_scan(g_nested, "v1");
    ray_op_t* c2_nested = ray_const_i64(g_nested, 2);
    ray_op_t* c1_nested = ray_const_i64(g_nested, 1);
    ray_op_t* mul_nested = ray_mul(g_nested, c2_nested, c1_nested);
    ray_op_t* expr_nested = ray_add(g_nested, v1_nested, mul_nested);
    uint16_t nested_ops[] = { OP_SUM };
    ray_op_t* nested_ins[] = { expr_nested };
    ray_op_t* grp_nested = ray_group(g_nested, NULL, 0, nested_ops, nested_ins, 1);
    ray_t* out_nested = ray_execute(g_nested, grp_nested);
    munit_assert_false(RAY_IS_ERR(out_nested));
    munit_assert_int(ray_table_nrows(out_nested), ==, 1);
    ray_t* nested_sum_col = ray_table_get_col_idx(out_nested, 0);
    munit_assert_ptr_not_null(nested_sum_col);
    munit_assert_int(((int64_t*)ray_data(nested_sum_col))[0], ==, 570);
    ray_release(out_nested);
    ray_graph_free(g_nested);

    /* Nested constants on both sides: SUM((2*1) + v1), SUM(v1 - (2*1)) */
    ray_graph_t* g_nested2 = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_nested2);
    ray_op_t* v1_nested2 = ray_scan(g_nested2, "v1");
    ray_op_t* c2_nested2 = ray_const_i64(g_nested2, 2);
    ray_op_t* c1_nested2 = ray_const_i64(g_nested2, 1);
    ray_op_t* mul_nested2 = ray_mul(g_nested2, c2_nested2, c1_nested2);
    ray_op_t* expr_nested_add_lhs = ray_add(g_nested2, mul_nested2, v1_nested2);
    ray_op_t* expr_nested_sub_rhs = ray_sub(g_nested2, v1_nested2, mul_nested2);
    uint16_t nested2_ops[] = { OP_SUM, OP_SUM };
    ray_op_t* nested2_ins[] = { expr_nested_add_lhs, expr_nested_sub_rhs };
    ray_op_t* grp_nested2 = ray_group(g_nested2, NULL, 0, nested2_ops, nested2_ins, 2);
    ray_t* out_nested2 = ray_execute(g_nested2, grp_nested2);
    munit_assert_false(RAY_IS_ERR(out_nested2));
    munit_assert_int(ray_table_nrows(out_nested2), ==, 1);
    ray_t* nested2_sum_add = ray_table_get_col_idx(out_nested2, 0);
    ray_t* nested2_sum_sub = ray_table_get_col_idx(out_nested2, 1);
    munit_assert_ptr_not_null(nested2_sum_add);
    munit_assert_ptr_not_null(nested2_sum_sub);
    munit_assert_int(((int64_t*)ray_data(nested2_sum_add))[0], ==, 570);
    munit_assert_int(((int64_t*)ray_data(nested2_sum_sub))[0], ==, 530);
    ray_release(out_nested2);
    ray_graph_free(g_nested2);

    /* Generic linear forms: SUM((v1 + 1) * 2), AVG(v1 + id1 + 1) */
    ray_graph_t* g_linear = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_linear);
    ray_op_t* v1_linear = ray_scan(g_linear, "v1");
    ray_op_t* id1_linear = ray_scan(g_linear, "id1");
    ray_op_t* one_linear_a = ray_const_i64(g_linear, 1);
    ray_op_t* one_linear_b = ray_const_i64(g_linear, 1);
    ray_op_t* two_linear = ray_const_i64(g_linear, 2);
    ray_op_t* v1_plus_one = ray_add(g_linear, v1_linear, one_linear_a);
    ray_op_t* sum_expr = ray_mul(g_linear, v1_plus_one, two_linear);
    ray_op_t* avg_expr = ray_add(g_linear, ray_add(g_linear, v1_linear, id1_linear), one_linear_b);
    uint16_t linear_ops[] = { OP_SUM, OP_AVG };
    ray_op_t* linear_ins[] = { sum_expr, avg_expr };
    ray_op_t* grp_linear = ray_group(g_linear, NULL, 0, linear_ops, linear_ins, 2);
    ray_t* out_linear = ray_execute(g_linear, grp_linear);
    munit_assert_false(RAY_IS_ERR(out_linear));
    munit_assert_int(ray_table_nrows(out_linear), ==, 1);
    ray_t* linear_sum_col = ray_table_get_col_idx(out_linear, 0);
    ray_t* linear_avg_col = ray_table_get_col_idx(out_linear, 1);
    munit_assert_ptr_not_null(linear_sum_col);
    munit_assert_ptr_not_null(linear_avg_col);
    munit_assert_int(((int64_t*)ray_data(linear_sum_col))[0], ==, 1120);
    munit_assert_double_equal(((double*)ray_data(linear_avg_col))[0], 57.9, 6);
    ray_release(out_linear);
    ray_graph_free(g_linear);

    /* Pure constant agg input should broadcast per row: SUM(2 * 1) */
    ray_graph_t* g_const = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_const);
    ray_op_t* c2 = ray_const_i64(g_const, 2);
    ray_op_t* c1 = ray_const_i64(g_const, 1);
    ray_op_t* mul = ray_mul(g_const, c2, c1);
    uint16_t const_ops[] = { OP_SUM };
    ray_op_t* const_ins[] = { mul };
    ray_op_t* grp_const = ray_group(g_const, NULL, 0, const_ops, const_ins, 1);
    ray_t* out_const = ray_execute(g_const, grp_const);
    munit_assert_false(RAY_IS_ERR(out_const));
    munit_assert_int(ray_table_nrows(out_const), ==, 1);
    ray_t* const_sum_col = ray_table_get_col_idx(out_const, 0);
    munit_assert_ptr_not_null(const_sum_col);
    munit_assert_int(((int64_t*)ray_data(const_sum_col))[0], ==, 20);
    ray_release(out_const);
    ray_graph_free(g_const);

    /* Grouped aggregate: id1, SUM(v1 + 1) */
    ray_graph_t* g_group = ray_graph_new(tbl);
    munit_assert_ptr_not_null(g_group);
    ray_op_t* key = ray_scan(g_group, "id1");
    ray_op_t* v1_group = ray_scan(g_group, "v1");
    ray_op_t* one_group = ray_const_i64(g_group, 1);
    ray_op_t* expr_group = ray_add(g_group, v1_group, one_group);
    ray_op_t* keys[] = { key };
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { expr_group };
    ray_op_t* grp = ray_group(g_group, keys, 1, ops, ins, 1);
    ray_t* out_group = ray_execute(g_group, grp);
    munit_assert_false(RAY_IS_ERR(out_group));
    munit_assert_int(out_group->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(out_group), ==, 3);
    munit_assert_int(ray_table_ncols(out_group), ==, 2);

    ray_t* id_col = ray_table_get_col_idx(out_group, 0);
    ray_t* s_col = ray_table_get_col_idx(out_group, 1);
    munit_assert_ptr_not_null(id_col);
    munit_assert_ptr_not_null(s_col);
    int64_t sum1 = 0, sum2 = 0, sum3 = 0;
    for (int64_t i = 0; i < ray_table_nrows(out_group); i++) {
        int64_t id = ((int64_t*)ray_data(id_col))[i];
        int64_t s = ((int64_t*)ray_data(s_col))[i];
        if (id == 1) sum1 = s;
        else if (id == 2) sum2 = s;
        else if (id == 3) sum3 = s;
    }
    munit_assert_int(sum1, ==, 204);
    munit_assert_int(sum2, ==, 153);
    munit_assert_int(sum3, ==, 203);

    ray_release(out_group);
    ray_graph_free(g_group);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/* --------------------------------------------------------------------------
 * Suite
 * -------------------------------------------------------------------------- */

static MunitTest tests[] = {
    { "/lifecycle",    test_graph_lifecycle, NULL, NULL, 0, NULL },
    { "/scan_sum",     test_scan_sum,        NULL, NULL, 0, NULL },
    { "/filter_count", test_filter_count,    NULL, NULL, 0, NULL },
    { "/arithmetic",   test_arithmetic,      NULL, NULL, 0, NULL },
    { "/group_sum",    test_group_sum,       NULL, NULL, 0, NULL },
    { "/opt_fold",     test_optimizer_constant_fold, NULL, NULL, 0, NULL },
    { "/opt_filter_const", test_optimizer_filter_const_predicate, NULL, NULL, 0, NULL },
    { "/group_affine_agg", test_group_affine_agg_input, NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_graph_suite = {
    "/graph", tests, NULL, 1, 0
};
