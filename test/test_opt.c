#include "munit.h"
#include <rayforce.h>
#include "ops/ops.h"
#include <string.h>

/* Helper: create a test table with columns id1(I64), v1(I64), v3(F64) */
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

/*
 * Test: filter with AND-combined predicates in "wrong" order.
 *
 * DAG: FILTER(AND(id1_eq, v3_gt), SCAN(v1))
 *   - id1_eq is cheap (I64 eq const) but listed first
 *   - v3_gt is expensive (F64 range cmp) but listed second
 *   - Optimizer should later split AND and reorder so cheap is innermost
 *
 * Baseline correctness: id1=1 AND v3>5.0 → count=2
 */
static MunitResult test_filter_reorder_by_type(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1    = ray_scan(g, "v1");
    ray_op_t* id1   = ray_scan(g, "id1");
    ray_op_t* v3    = ray_scan(g, "v3");
    ray_op_t* c1    = ray_const_i64(g, 1);
    ray_op_t* c5    = ray_const_f64(g, 5.0);

    ray_op_t* id1_eq = ray_eq(g, id1, c1);    /* cheap: const cmp + eq */
    ray_op_t* v3_gt  = ray_gt(g, v3, c5);     /* more expensive: range */

    /* AND with "wrong" order: cheap pred first, expensive second */
    ray_op_t* combined = ray_and(g, id1_eq, v3_gt);
    ray_op_t* filt = ray_filter(g, v1, combined);
    ray_op_t* cnt = ray_count(g, filt);

    /* Execute and verify correctness: id1=1 AND v3>5.0
     * Rows: id1={1,1,2,2,3,3,1,2,3,1}, v3={1.5,2.5,...,10.5}
     * id1=1 rows: indices 0,1,6,9 → v3={1.5,2.5,7.5,10.5}
     * v3>5.0 from those: indices 6,9 → count=2 */
    ray_t* result = ray_execute(g, cnt);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/*
 * Test: AND(pred_a, pred_b) in a single filter gets split into
 * two chained filters for independent reordering.
 *
 * DAG: FILTER(AND(v3 > 5.0, id1 = 1), SCAN(v1))
 * After: FILTER(v3 > 5.0, FILTER(id1 = 1, SCAN(v1)))
 * Verify via correctness — same result as test above.
 */
static MunitResult test_filter_and_split(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1    = ray_scan(g, "v1");
    ray_op_t* id1   = ray_scan(g, "id1");
    ray_op_t* v3    = ray_scan(g, "v3");
    ray_op_t* c1    = ray_const_i64(g, 1);
    ray_op_t* c5    = ray_const_f64(g, 5.0);

    ray_op_t* id1_eq = ray_eq(g, id1, c1);
    ray_op_t* v3_gt  = ray_gt(g, v3, c5);
    ray_op_t* combined = ray_and(g, v3_gt, id1_eq);

    ray_op_t* filt = ray_filter(g, v1, combined);
    ray_op_t* cnt = ray_count(g, filt);

    ray_t* result = ray_execute(g, cnt);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/*
 * Test: verify that after optimization, the inner filter (closest to scan)
 * has the cheaper predicate.
 *
 * Build: FILTER(eq_on_i64, FILTER(gt_on_f64, SCAN))
 * eq_on_i64 costs: const(+0) + i64_width(+3) + eq(+0) = 3
 * gt_on_f64 costs: const(+0) + f64_width(+3) + range(+2) = 5
 *
 * eq is cheaper, so after reorder the chain should be:
 *   FILTER(gt_on_f64, FILTER(eq_on_i64, SCAN))
 * i.e., outer predicate = gt_on_f64, inner predicate = eq_on_i64
 */
static MunitResult test_filter_reorder_dag(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1     = ray_scan(g, "v1");
    ray_op_t* id1    = ray_scan(g, "id1");
    ray_op_t* v3     = ray_scan(g, "v3");
    ray_op_t* c1     = ray_const_i64(g, 1);
    ray_op_t* c5     = ray_const_f64(g, 5.0);

    ray_op_t* eq_pred = ray_eq(g, id1, c1);     /* cost=3: const+i64+eq */
    ray_op_t* gt_pred = ray_gt(g, v3, c5);      /* cost=5: const+f64+gt */

    /* Build in WRONG order: cheap eq is outer, expensive gt is inner */
    ray_op_t* filt_inner = ray_filter(g, v1, gt_pred);
    ray_op_t* filt_outer = ray_filter(g, filt_inner, eq_pred);

    uint32_t eq_pred_id = eq_pred->id;
    uint32_t gt_pred_id = gt_pred->id;

    ray_op_t* opt = ray_optimize(g, filt_outer);
    munit_assert_ptr_not_null(opt);

    /* After reorder: outer should have gt (expensive), inner should have eq (cheap).
     * The pass swaps predicates, so:
     *   chain[0] (outer) gets the higher cost pred
     *   chain[1] (inner) gets the lower cost pred */
    munit_assert_int(opt->opcode, ==, OP_FILTER);
    ray_op_t* inner = opt->inputs[0];
    munit_assert_int(inner->opcode, ==, OP_FILTER);

    /* Inner pred should be eq (cheaper), outer pred should be gt (more expensive) */
    munit_assert_int(inner->inputs[1]->id, ==, eq_pred_id);
    munit_assert_int(opt->inputs[1]->id, ==, gt_pred_id);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/*
 * Test: predicate pushdown past projection.
 *
 * Build: FILTER(id1 = 1, PROJECT([id1, v1], SCAN))
 * After pushdown: PROJECT([id1, v1], FILTER(id1 = 1, SCAN))
 *
 * Verify both correctness and DAG structure.
 * id1=1 rows: indices 0,1,6,9 → count=4
 */
static MunitResult test_pushdown_past_select(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build: FILTER(pred, SELECT([id1, v1], SCAN)) */
    ray_op_t* v1   = ray_scan(g, "v1");
    ray_op_t* id1  = ray_scan(g, "id1");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, id1, c1);

    ray_op_t* sel_cols[] = { id1, v1 };
    ray_op_t* sel = ray_select(g, v1, sel_cols, 2);
    uint32_t sel_id = sel->id;
    ray_op_t* filt = ray_filter(g, sel, pred);

    /* Optimize and capture the new root (pushdown moves filter below select) */
    ray_op_t* opt_root = ray_optimize(g, filt);

    /* Verify DAG structure: filter should have been pushed below select */
    ray_op_t* sel_after = &g->nodes[sel_id];
    munit_assert_int(sel_after->opcode, ==, OP_SELECT);
    munit_assert_int(sel_after->inputs[0]->opcode, ==, OP_FILTER);

    /* Verify the optimized root is the select node (filter was pushed below) */
    munit_assert_uint(opt_root->id, ==, sel_id);

    /* Execute COUNT from the pushed-down filter to validate correctness.
     * The filter (now below select) should still produce the right row count. */
    ray_op_t* cnt = ray_count(g, sel_after->inputs[0]);
    ray_t* result = ray_execute(g, cnt);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/*
 * Test: FILTER above GROUP correctness (GROUP pushdown is disabled).
 *
 * Build: FILTER(id1 = 1, GROUP(id1, SUM(v1)))
 * After: unchanged (GROUP pushdown disabled — executor key/agg scans
 *        bypass filter, so pushdown would produce wrong results).
 *
 * Verify correctness of filter-above-group execution. Result: id1=1, sum=200
 */
static MunitResult test_pushdown_past_group(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* key = ray_scan(g, "id1");
    ray_op_t* val = ray_scan(g, "v1");
    ray_op_t* keys[] = { key };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { val };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    /* Filter on the group key column (id1 = 1).
     * GROUP pushdown is disabled (executor key/agg scans bypass filter),
     * so this tests FILTER-above-GROUP correctness only. */
    ray_op_t* id1_scan = ray_scan(g, "id1");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, id1_scan, c1);
    ray_op_t* filt = ray_filter(g, grp, pred);

    /* Verify correctness */
    ray_t* result = ray_execute(g, filt);
    munit_assert_false(RAY_IS_ERR(result));

    munit_assert_int(result->type, ==, RAY_TABLE);
    munit_assert_int(ray_table_nrows(result), ==, 1);
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    munit_assert_ptr_not_null(sum_col);
    munit_assert_int(((int64_t*)ray_data(sum_col))[0], ==, 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/*
 * Test: projection pushdown marks unreachable SCAN nodes dead.
 *
 * Build: SUM(SCAN("v1")) — only v1 is referenced.
 * After optimization, id1 and v3 scans are not in the DAG,
 * so they should not affect the result.
 * sum(v1) = 10+20+30+40+50+60+70+80+90+100 = 550
 */
static MunitResult test_projection_pushdown(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    ray_t *tbl = make_test_table();
    ray_graph_t *g = ray_graph_new(tbl);

    /* Only reference v1 — id1 and v3 should not affect result */
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *s = ray_sum(g, v1);
    ray_op_t *opt = ray_optimize(g, s);

    ray_t *result = ray_execute(g, opt);
    munit_assert_false(RAY_IS_ERR(result));
    munit_assert_int(result->i64, ==, 550);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

/*
 * Test: partition pruning smoke test.
 *
 * Build: SUM(FILTER(EQ(SCAN(id1), CONST(1)), SCAN(v1)))
 * Verify correctness: id1=1 rows: indices 0,1,6,9 → v1={10,20,70,100} → sum=200
 *
 * The partition pruning pass only activates for RAY_MAPCOMMON columns,
 * so with regular I64 columns this verifies the pass is a safe no-op.
 */
static MunitResult test_partition_pruning_smoke(const void* params, void* data) {
    (void)params; (void)data;
    ray_heap_init();
    ray_t *tbl = make_test_table();
    ray_graph_t *g = ray_graph_new(tbl);

    ray_op_t *id1 = ray_scan(g, "id1");
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *c1 = ray_const_i64(g, 1);
    ray_op_t *pred = ray_eq(g, id1, c1);
    ray_op_t *flt = ray_filter(g, v1, pred);
    ray_op_t *s = ray_sum(g, flt);

    ray_op_t *opt = ray_optimize(g, s);
    ray_t *result = ray_execute(g, opt);
    munit_assert_false(RAY_IS_ERR(result));

    /* id1=1 rows: v1={10,20,70,100} -> sum=200 */
    munit_assert_int(result->i64, ==, 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    return MUNIT_OK;
}

static MunitTest tests[] = {
    { "/filter_reorder_type", test_filter_reorder_by_type, NULL, NULL, 0, NULL },
    { "/filter_and_split",    test_filter_and_split,       NULL, NULL, 0, NULL },
    { "/filter_reorder_dag",  test_filter_reorder_dag,     NULL, NULL, 0, NULL },
    { "/pushdown_select",     test_pushdown_past_select,   NULL, NULL, 0, NULL },
    { "/pushdown_group",      test_pushdown_past_group,    NULL, NULL, 0, NULL },
    { "/projection_pushdown", test_projection_pushdown,   NULL, NULL, 0, NULL },
    { "/partition_pruning",   test_partition_pruning_smoke, NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_opt_suite = {
    "/opt", tests, NULL, 1, 0
};
