/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Query bridge: select, update, insert, upsert, join operations.
 *   Extracted from eval.c.
 */

#include "lang/eval_internal.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "ops/ops.h"
#include "table/sym.h"

#include <string.h>
#include <math.h>

/* ══════════════════════════════════════════
 * Select query — DAG bridge
 * ══════════════════════════════════════════ */

/* Helper: look up a key in a dict (RAY_LIST with ATTR_DICT).
 * Returns the value expression (unevaluated), or NULL if not found. */
static ray_t* dict_get(ray_t* dict, const char* key) {
    if (!dict || dict->type != RAY_LIST) return NULL;
    int64_t n = ray_len(dict);
    ray_t** elems = (ray_t**)ray_data(dict);
    int64_t key_id = ray_sym_intern(key, strlen(key));
    for (int64_t i = 0; i + 1 < n; i += 2) {
        if (elems[i]->type == -RAY_SYM && elems[i]->i64 == key_id)
            return elems[i + 1];
    }
    return NULL;
}

/* Map a Rayfall builtin name to a DAG binary op constructor */
typedef ray_op_t* (*dag_binary_ctor)(ray_graph_t*, ray_op_t*, ray_op_t*);
typedef ray_op_t* (*dag_unary_ctor)(ray_graph_t*, ray_op_t*);

static dag_binary_ctor resolve_binary_dag(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return NULL;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 1) {
        switch (name[0]) {
            case '+': return ray_add;
            case '-': return ray_sub;
            case '*': return ray_mul;
            case '/': return ray_div;
            case '%': return ray_mod;
            case '>': return ray_gt;
            case '<': return ray_lt;
        }
    } else if (len == 2) {
        if (name[0] == '>' && name[1] == '=') return ray_ge;
        if (name[0] == '<' && name[1] == '=') return ray_le;
        if (name[0] == '=' && name[1] == '=') return ray_eq;
        if (name[0] == '!' && name[1] == '=') return ray_ne;
        if (name[0] == 'o' && name[1] == 'r') return ray_or;
    } else if (len == 3 && name[0] == 'a' && name[1] == 'n' && name[2] == 'd') {
        return ray_and;
    }
    return NULL;
}

static dag_unary_ctor resolve_unary_dag(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return NULL;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 3) {
        if (memcmp(name, "neg", 3) == 0) return ray_neg;
        if (memcmp(name, "not", 3) == 0) return ray_not;
        if (memcmp(name, "abs", 3) == 0) return ray_abs;
        if (memcmp(name, "exp", 3) == 0) return ray_exp_op;
        if (memcmp(name, "log", 3) == 0) return ray_log_op;
    } else if (len == 4) {
        if (memcmp(name, "ceil", 4) == 0) return ray_ceil_op;
        if (memcmp(name, "sqrt", 4) == 0) return ray_sqrt_op;
    } else if (len == 5) {
        if (memcmp(name, "floor", 5) == 0) return ray_floor_op;
    }
    return NULL;
}

/* Map Rayfall aggregation name to DAG opcode */
static uint16_t resolve_agg_opcode(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return 0;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 3 && memcmp(name, "sum", 3) == 0) return OP_SUM;
    if (len == 3 && memcmp(name, "avg", 3) == 0) return OP_AVG;
    if (len == 3 && memcmp(name, "min", 3) == 0) return OP_MIN;
    if (len == 3 && memcmp(name, "max", 3) == 0) return OP_MAX;
    if (len == 5 && memcmp(name, "count", 5) == 0) return OP_COUNT;
    if (len == 5 && memcmp(name, "first", 5) == 0) return OP_FIRST;
    if (len == 4 && memcmp(name, "last", 4) == 0) return OP_LAST;
    return 0;
}

/* Apply sort (asc/desc) and take clauses to a materialized result table.
 * Used by eval-level paths that bypass the DAG (e.g., LIST/STR group keys).
 * Builds a temporary DAG for sorting (supports per-column direction flags)
 * and applies take via ray_head/ray_tail or ray_take_fn. */
static ray_t* apply_sort_take(ray_t* result, ray_t** dict_elems, int64_t dict_n,
                              int64_t asc_id, int64_t desc_id, int64_t take_id) {
    if (!result || RAY_IS_ERR(result)) return result;

    /* Check for sort/take clauses */
    bool has_sort = false;
    ray_t* take_val_expr = NULL;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid == asc_id || kid == desc_id) has_sort = true;
        if (kid == take_id) take_val_expr = dict_elems[i + 1];
    }
    if (!has_sort && !take_val_expr) return result;

    /* Build temporary DAG on the materialized result */
    ray_graph_t* g = ray_graph_new(result);
    if (!g) return result;
    ray_op_t* root = ray_const_table(g, result);

    /* Sort */
    if (has_sort) {
        ray_op_t* sort_keys[16];
        uint8_t   sort_descs[16];
        uint8_t   n_sort = 0;
        for (int64_t i = 0; i + 1 < dict_n && n_sort < 16; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            uint8_t is_desc = 0;
            if (kid == asc_id) is_desc = 0;
            else if (kid == desc_id) is_desc = 1;
            else continue;
            ray_t* val = dict_elems[i + 1];
            if (val->type == -RAY_SYM) {
                ray_t* s = ray_sym_str(val->i64);
                sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                sort_descs[n_sort] = is_desc;
                n_sort++;
            } else if (ray_is_vec(val) && val->type == RAY_SYM) {
                for (int64_t c = 0; c < val->len && n_sort < 16; c++) {
                    int64_t sid = ray_read_sym(ray_data(val), c, val->type, val->attrs);
                    ray_t* s = ray_sym_str(sid);
                    sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                    sort_descs[n_sort] = is_desc;
                    n_sort++;
                }
            }
        }
        if (n_sort > 0)
            root = ray_sort_op(g, root, sort_keys, sort_descs, NULL, n_sort);
    }

    /* Take (atom → DAG head/tail, vector → post-execute ray_take_fn) */
    ray_t* take_range = NULL;
    if (take_val_expr) {
        ray_t* tv = ray_eval(take_val_expr);
        if (!tv || RAY_IS_ERR(tv)) { ray_graph_free(g); ray_release(result); return tv ? tv : ray_error("domain", NULL); }
        if (ray_is_atom(tv) && (tv->type == -RAY_I64 || tv->type == -RAY_I32)) {
            int64_t n_take = (tv->type == -RAY_I64) ? tv->i64 : tv->i32;
            ray_release(tv);
            if (n_take >= 0) root = ray_head(g, root, n_take);
            else             root = ray_tail(g, root, -n_take);
        } else if (ray_is_vec(tv) && (tv->type == RAY_I64 || tv->type == RAY_I32) && tv->len == 2) {
            take_range = tv;
        } else {
            ray_release(tv); ray_graph_free(g); ray_release(result);
            return ray_error("domain", NULL);
        }
    }

    root = ray_optimize(g, root);
    ray_t* sorted = ray_execute(g, root);
    ray_graph_free(g);
    ray_release(result);

    if (take_range && sorted && !RAY_IS_ERR(sorted)) {
        ray_t* sliced = ray_take_fn(sorted, take_range);
        ray_release(sorted);
        ray_release(take_range);
        return sliced;
    }
    if (take_range) ray_release(take_range);
    return sorted;
}

/* Compile a Rayfall AST expression into a DAG node */
static ray_op_t* compile_expr_dag(ray_graph_t* g, ray_t* expr) {
    if (!expr) return NULL;

    /* Atom literal → const node */
    if (expr->type == -RAY_I64)
        return ray_const_i64(g, expr->i64);
    if (expr->type == -RAY_F64)
        return ray_const_f64(g, expr->f64);
    if (expr->type == -RAY_BOOL)
        return ray_const_bool(g, expr->b8);
    if (expr->type == -RAY_STR) {
        const char *ptr = ray_str_ptr(expr);
        size_t len = ray_str_len(expr);
        return ray_const_str(g, ptr, len);
    }

    /* Symbol literal → cannot compile to DAG (no const SYM node type).
     * Return NULL to trigger eval-level fallback. */
    if (expr->type == -RAY_SYM && !(expr->attrs & RAY_ATTR_NAME))
        return NULL;

    /* Name reference → column scan */
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        ray_t* s = ray_sym_str(expr->i64);
        if (!s) return NULL;
        return ray_scan(g, ray_str_ptr(s));
    }

    /* List → function call: (fn arg1 arg2 ...) */
    if (expr->type == RAY_LIST && !(expr->attrs & (RAY_ATTR_DICT))) {
        int64_t n = ray_len(expr);
        if (n == 0) return NULL;
        ray_t** elems = (ray_t**)ray_data(expr);
        ray_t* head = elems[0];

        /* Head must be a name referencing a builtin */
        if (head->type != -RAY_SYM) return NULL;
        int64_t fn_sym = head->i64;

        /* Check for xbar */
        ray_t* fn_name_str = ray_sym_str(fn_sym);
        if (fn_name_str && ray_str_len(fn_name_str) == 4
            && memcmp(ray_str_ptr(fn_name_str), "xbar", 4) == 0) {
            if (n != 3) return NULL;
            ray_op_t* col = compile_expr_dag(g, elems[1]);
            ray_op_t* bucket = compile_expr_dag(g, elems[2]);
            if (!col || !bucket) return NULL;
            /* xbar(x, b) = x - (x % b)  (stays in integer domain) */
            return ray_sub(g, col, ray_mod(g, col, bucket));
        }

        /* Binary op? */
        if (n == 3) {
            dag_binary_ctor ctor = resolve_binary_dag(fn_sym);
            if (ctor) {
                ray_op_t* left = compile_expr_dag(g, elems[1]);
                ray_op_t* right = compile_expr_dag(g, elems[2]);
                if (!left || !right) return NULL;
                return ctor(g, left, right);
            }
        }

        /* Unary op or aggregation? */
        if (n == 2) {
            /* Check for unary DAG ops */
            dag_unary_ctor uctor = resolve_unary_dag(fn_sym);
            if (uctor) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                return arg ? uctor(g, arg) : NULL;
            }
            /* Aggregation functions return DAG agg nodes */
            uint16_t agg_op = resolve_agg_opcode(fn_sym);
            if (agg_op) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                if (!arg) return NULL;
                switch (agg_op) {
                    case OP_SUM:   return ray_sum(g, arg);
                    case OP_AVG:   return ray_avg(g, arg);
                    case OP_MIN:   return ray_min_op(g, arg);
                    case OP_MAX:   return ray_max_op(g, arg);
                    case OP_COUNT: return ray_count(g, arg);
                    case OP_FIRST: return ray_first(g, arg);
                    case OP_LAST:  return ray_last(g, arg);
                    default: return NULL;
                }
            }
        }
    }

    return NULL;
}

/* Check if an expression is an aggregation call (head is an agg function) */
static int is_agg_expr(ray_t* expr) {
    if (!expr || expr->type != RAY_LIST) return 0;
    if (expr->attrs & (RAY_ATTR_DICT)) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (elems[0]->type != -RAY_SYM) return 0;
    return resolve_agg_opcode(elems[0]->i64) != 0;
}

/* Forward declarations for eval-level groupby fallback */

/* (select {from: t [where: pred] [by: key] [col: expr ...]})
 * Special form — receives unevaluated dict arg. */
ray_t* ray_select_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return ray_error("type", NULL);

    /* Evaluate 'from:' to get the source table */
    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return ray_error("domain", NULL);
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", NULL); }

    ray_t* where_expr = dict_get(dict, "where");
    ray_t* by_expr = dict_get(dict, "by");
    ray_t* take_expr = dict_get(dict, "take");

    /* Collect output columns (keys that are not reserved) */
    int64_t dict_n = ray_len(dict);
    ray_t** dict_elems = (ray_t**)ray_data(dict);
    int64_t from_id  = ray_sym_intern("from",  4);
    int64_t where_id = ray_sym_intern("where", 5);
    int64_t by_id    = ray_sym_intern("by",    2);
    int64_t take_id  = ray_sym_intern("take",  4);
    int64_t asc_id   = ray_sym_intern("asc",   3);
    int64_t desc_id  = ray_sym_intern("desc",  4);

    /* Check for asc/desc presence */
    bool has_sort = false;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid == asc_id || kid == desc_id) { has_sort = true; break; }
    }

    /* Count output columns */
    int n_out = 0;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid != from_id && kid != where_id && kid != by_id &&
            kid != take_id && kid != asc_id && kid != desc_id)
            n_out++;
    }

    /* Simple case: no clauses at all → return table as-is */
    if (n_out == 0 && !where_expr && !by_expr && !take_expr && !has_sort)
        return tbl;

    /* Build DAG */
    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) { ray_release(tbl); return ray_error("oom", NULL); }

    ray_op_t* root = ray_const_table(g, tbl);

    /* Apply WHERE filter */
    if (where_expr) {
        ray_op_t* pred = compile_expr_dag(g, where_expr);
        if (!pred) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
        root = ray_filter(g, root, pred);
    }

    /* GROUP BY */
    if (by_expr) {
        /* Check if group key is a LIST column (e.g., string list) —
         * the DAG executor doesn't support LIST group keys, so fall back
         * to eval-level grouping. */
        int use_eval_group = 0;
        if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME)) {
            ray_t* key_col = ray_table_get_col(tbl, by_expr->i64);
            if (key_col && (key_col->type == RAY_LIST || key_col->type == RAY_STR || key_col->type == RAY_GUID))
                use_eval_group = 1;
        }
        if (use_eval_group) {
            /* Apply WHERE filter first (if any), then eval-level groupby */
            ray_t* eval_tbl = tbl;
            if (where_expr) {
                root = ray_optimize(g, root);
                ray_t* fres = ray_execute(g, root);
                ray_graph_free(g); g = NULL;
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                if (ray_is_lazy(fres)) fres = ray_lazy_materialize(fres);
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                eval_tbl = fres;
            } else {
                ray_graph_free(g); g = NULL;
            }
            ray_t* key_col = ray_table_get_col(eval_tbl, by_expr->i64);
            ray_t* groups = ray_group_fn(key_col);
            if (RAY_IS_ERR(groups)) { if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return groups; }

            /* groups is a dict: {key_val: [indices ...], ...} */
            int64_t gn = ray_len(groups);
            int64_t n_groups = gn / 2;

            /* Empty groups with no explicit aggs: return empty table with full schema */
            if (n_groups == 0 && n_out == 0) {
                ray_release(groups);
                int64_t nc0 = ray_table_ncols(eval_tbl);
                ray_t* empty = ray_table_new(nc0);
                if (!RAY_IS_ERR(empty)) {
                    /* Key column first */
                    { ray_t* sc = ray_table_get_col(eval_tbl, by_expr->i64);
                      if (sc) {
                        ray_t* ev = ray_vec_new(sc->type, 0);
                        if (ev && !RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, by_expr->i64, ev); ray_release(ev); }
                      }
                    }
                    for (int64_t c = 0; c < nc0; c++) {
                        int64_t cn = ray_table_col_name(eval_tbl, c);
                        if (cn == by_expr->i64) continue;
                        ray_t* sc = ray_table_get_col_idx(eval_tbl, c);
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) :
                                    (sc->type == RAY_LIST) ? ray_list_new(0) :
                                    ray_vec_new(sc->type, 0);
                        if (ev && !RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, cn, ev); ray_release(ev); }
                    }
                }
                if (eval_tbl != tbl) ray_release(eval_tbl);
                ray_release(tbl);
                return empty;
            }

            /* Collect aggregation results */
            int n_agg_out = 0;
            int64_t agg_names[16];
            ray_t* agg_results[16];
            for (int64_t i = 0; i + 1 < dict_n && n_agg_out < 16; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id || kid == take_id || kid == asc_id || kid == desc_id) continue;
                ray_t* val_expr_item = dict_elems[i + 1];
                if (!is_agg_expr(val_expr_item)) continue;

                ray_t** agg_elems = (ray_t**)ray_data(val_expr_item);
                ray_t* agg_fn_name = agg_elems[0];
                ray_t* agg_col_expr = agg_elems[1];

                /* Resolve source column from filtered table */
                ray_t* src_col_val = NULL;
                if (agg_col_expr->type == -RAY_SYM && (agg_col_expr->attrs & RAY_ATTR_NAME)) {
                    src_col_val = ray_table_get_col(eval_tbl, agg_col_expr->i64);
                    if (src_col_val) ray_retain(src_col_val);
                }
                if (!src_col_val) {
                    src_col_val = ray_eval(agg_col_expr);
                    if (RAY_IS_ERR(src_col_val)) { ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return src_col_val; }
                }

                /* For each group, compute aggregation */
                ray_t* agg_vec = NULL;
                ray_t** grp_items = (ray_t**)ray_data(groups);
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    ray_t* idx_list = grp_items[gi * 2 + 1];
                    ray_t* subset = ray_at_fn(src_col_val, idx_list);
                    if (RAY_IS_ERR(subset)) continue;
                    ray_t* agg_val = NULL;
                    ray_t* fn_obj = ray_env_get(agg_fn_name->i64);
                    if (fn_obj && fn_obj->type == RAY_UNARY) {
                        ray_unary_fn uf = (ray_unary_fn)(uintptr_t)fn_obj->i64;
                        agg_val = uf(subset);
                    }
                    ray_release(subset);
                    if (!agg_val || RAY_IS_ERR(agg_val)) continue;

                    if (!agg_vec) {
                        int8_t vt = -(agg_val->type);
                        agg_vec = ray_vec_new(vt, n_groups);
                        if (RAY_IS_ERR(agg_vec)) { ray_release(agg_val); break; }
                        agg_vec->len = n_groups;
                    }
                    store_typed_elem(agg_vec, gi, agg_val);
                    ray_release(agg_val);
                }
                ray_release(src_col_val);
                agg_names[n_agg_out] = kid;
                agg_results[n_agg_out] = agg_vec;
                n_agg_out++;
            }

            /* Build result table: key column + aggregation columns */
            ray_t* result = ray_table_new(1 + n_agg_out);
            if (RAY_IS_ERR(result)) { ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return result; }

            /* Key column: unique keys from groups */
            ray_t** grp_items = (ray_t**)ray_data(groups);
            ray_t* key_col_src = ray_table_get_col(eval_tbl, by_expr->i64);
            if (key_col_src && key_col_src->type == RAY_STR) {
                ray_t* key_vec = ray_vec_new(RAY_STR, n_groups);
                for (int64_t gi = 0; gi < n_groups && key_vec && !RAY_IS_ERR(key_vec); gi++) {
                    ray_t* k = grp_items[gi * 2];
                    const char* sp = ray_str_ptr(k);
                    size_t slen = ray_str_len(k);
                    key_vec = ray_str_vec_append(key_vec, sp ? sp : "", sp ? slen : 0);
                }
                if (!key_vec || RAY_IS_ERR(key_vec)) {
                    for (int i = 0; i < n_agg_out; i++) { if (agg_results[i]) ray_release(agg_results[i]); }
                    ray_release(result); ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                    return key_vec ? key_vec : ray_error("oom", NULL);
                }
                result = ray_table_add_col(result, by_expr->i64, key_vec);
                ray_release(key_vec);
            } else {
                ray_t* key_list = ray_alloc(n_groups * sizeof(ray_t*));
                if (!key_list) {
                    for (int i = 0; i < n_agg_out; i++) { if (agg_results[i]) ray_release(agg_results[i]); }
                    ray_release(result); ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return ray_error("oom", NULL);
                }
                key_list->type = RAY_LIST;
                key_list->len = n_groups;
                ray_t** key_out = (ray_t**)ray_data(key_list);
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    ray_retain(grp_items[gi * 2]);
                    key_out[gi] = grp_items[gi * 2];
                }
                result = ray_table_add_col(result, by_expr->i64, key_list);
                ray_release(key_list);
            }

            for (int i = 0; i < n_agg_out; i++) {
                if (agg_results[i])
                    result = ray_table_add_col(result, agg_names[i], agg_results[i]);
                if (agg_results[i]) ray_release(agg_results[i]);
            }

            /* No explicit aggs: gather first-of-group for all non-key columns */
            if (n_agg_out == 0 && n_groups > 0) {
                ray_t** gi_items = (ray_t**)ray_data(groups);
                /* Collect first index per group */
                int64_t fi_stack[256];
                ray_t* fi_hdr = NULL;
                int64_t* fi = (n_groups <= 256) ? fi_stack : NULL;
                if (!fi) {
                    fi_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                    if (!fi_hdr) { ray_release(result); ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                    fi = (int64_t*)ray_data(fi_hdr);
                }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    ray_t* il = gi_items[gi * 2 + 1];
                    int a = 0; ray_t* i0 = collection_elem(il, 0, &a);
                    fi[gi] = as_i64(i0);
                    if (a) ray_release(i0);
                }
                int64_t nc = ray_table_ncols(eval_tbl);
                for (int64_t c = 0; c < nc && !RAY_IS_ERR(result); c++) {
                    int64_t cn = ray_table_col_name(eval_tbl, c);
                    if (cn == by_expr->i64) continue;
                    ray_t* sc = ray_table_get_col_idx(eval_tbl, c);
                    ray_t* dst = NULL;
                    if (sc->type == RAY_STR) {
                        dst = ray_vec_new(RAY_STR, n_groups);
                        for (int64_t gi = 0; gi < n_groups && dst && !RAY_IS_ERR(dst); gi++) {
                            size_t slen = 0;
                            const char* sp = ray_str_vec_get(sc, fi[gi], &slen);
                            dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                        }
                    } else if (sc->type == RAY_LIST) {
                        dst = ray_alloc(n_groups * sizeof(ray_t*));
                        if (dst) {
                            dst->type = RAY_LIST; dst->len = n_groups;
                            ray_t** dout = (ray_t**)ray_data(dst);
                            ray_t** sitems = (ray_t**)ray_data(sc);
                            for (int64_t gi = 0; gi < n_groups; gi++) { dout[gi] = sitems[fi[gi]]; ray_retain(dout[gi]); }
                        }
                    } else {
                        dst = ray_vec_new(sc->type, n_groups);
                        if (dst && !RAY_IS_ERR(dst)) {
                            for (int64_t gi = 0; gi < n_groups; gi++) {
                                int a = 0; ray_t* v = collection_elem(sc, fi[gi], &a);
                                store_typed_elem(dst, gi, v);
                                if (a) ray_release(v);
                            }
                            dst->len = n_groups;
                        }
                    }
                    if (!dst || RAY_IS_ERR(dst)) {
                        if (dst) ray_release(dst);
                        ray_release(result);
                        result = ray_error("oom", NULL);
                        break;
                    }
                    result = ray_table_add_col(result, cn, dst);
                    ray_release(dst);
                }
                if (fi_hdr) ray_free(fi_hdr);
            }

            ray_release(groups);
            if (eval_tbl != tbl) ray_release(eval_tbl);
            ray_release(tbl);
            return apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);
        }

        /* Compile group key(s) */
        ray_op_t* key_ops[16];
        uint8_t n_keys = 0;

        if (by_expr->type == RAY_SYM) {
            /* Multiple keys as SYM vector: [col1 col2 ...] */
            int64_t nk = ray_len(by_expr);
            int64_t* sym_ids = (int64_t*)ray_data(by_expr);
            for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                ray_t* name_str = ray_sym_str(sym_ids[i]);
                if (!name_str) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                key_ops[n_keys] = ray_scan(g, ray_str_ptr(name_str));
                if (!key_ops[n_keys]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                n_keys++;
            }
        } else {
            /* Single key expression */
            key_ops[0] = compile_expr_dag(g, by_expr);
            if (!key_ops[0]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
            n_keys = 1;
        }

        /* Collect aggregation expressions from output columns */
        uint16_t agg_ops[16];
        ray_op_t* agg_ins[16];
        uint8_t n_aggs = 0;

        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id || kid == take_id || kid == asc_id || kid == desc_id) continue;

            ray_t* val_expr = dict_elems[i + 1];
            if (is_agg_expr(val_expr) && n_aggs < 16) {
                ray_t** agg_elems = (ray_t**)ray_data(val_expr);
                agg_ops[n_aggs] = resolve_agg_opcode(agg_elems[0]->i64);
                /* Compile the aggregation input (the column reference) */
                agg_ins[n_aggs] = compile_expr_dag(g, agg_elems[1]);
                if (!agg_ins[n_aggs]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                n_aggs++;
            }
        }

        if (n_aggs > 0) {
            root = ray_group(g, key_ops, n_keys, agg_ops, agg_ins, n_aggs);
        } else {
            /* No explicit aggregations — apply WHERE filter first (if any),
             * then use DAG GROUP+COUNT for fast hash-parallel group boundaries,
             * then gather first-of-group from the filtered table. */
            ray_t* filtered_tbl = tbl;
            if (where_expr) {
                root = ray_optimize(g, root);
                ray_t* fres = ray_execute(g, root);
                ray_graph_free(g); g = NULL;
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                if (ray_is_lazy(fres)) fres = ray_lazy_materialize(fres);
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                filtered_tbl = fres;
                /* Rebuild graph on filtered table for GROUP+COUNT */
                g = ray_graph_new(filtered_tbl);
                if (!g) { if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                n_keys = 0;
                if (by_expr->type == RAY_SYM) {
                    int64_t nk = ray_len(by_expr);
                    int64_t* sym_ids = (int64_t*)ray_data(by_expr);
                    for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                        ray_t* ns = ray_sym_str(sym_ids[i]);
                        if (ns) key_ops[n_keys++] = ray_scan(g, ray_str_ptr(ns));
                    }
                } else {
                    key_ops[0] = compile_expr_dag(g, by_expr);
                    if (key_ops[0]) n_keys = 1;
                }
            }

            uint16_t cnt_op = OP_COUNT;
            ray_op_t* cnt_in = key_ops[0];
            root = ray_group(g, key_ops, n_keys, &cnt_op, &cnt_in, 1);
            root = ray_optimize(g, root);
            ray_t* grouped = ray_execute(g, root);
            ray_graph_free(g); g = NULL;
            if (!grouped || RAY_IS_ERR(grouped)) { if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return grouped; }
            if (ray_is_lazy(grouped)) grouped = ray_lazy_materialize(grouped);

            int64_t n_groups = ray_table_nrows(grouped);

            /* Resolve key column sym early — needed for empty result schema */
            int64_t key_sym = -1;
            if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME))
                key_sym = by_expr->i64;
            else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1)
                key_sym = ((int64_t*)ray_data(by_expr))[0];

            if (n_groups == 0) {
                ray_release(grouped);
                int64_t nc0 = ray_table_ncols(filtered_tbl);
                ray_t* empty = ray_table_new(nc0);
                if (!RAY_IS_ERR(empty)) {
                    /* Key column first */
                    { ray_t* sc = ray_table_get_col(filtered_tbl, key_sym);
                      if (sc) {
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) : ray_vec_new(sc->type, 0);
                        if (!RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, key_sym, ev); ray_release(ev); }
                      }
                    }
                    for (int64_t c = 0; c < nc0; c++) {
                        int64_t cn = ray_table_col_name(filtered_tbl, c);
                        if (cn == key_sym) continue;
                        ray_t* sc = ray_table_get_col_idx(filtered_tbl, c);
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) :
                                    (sc->type == RAY_LIST) ? ray_list_new(0) :
                                    ray_vec_new(sc->type, 0);
                        if (!RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, cn, ev); ray_release(ev); }
                    }
                }
                if (filtered_tbl != tbl) ray_release(filtered_tbl);
                ray_release(tbl);
                return empty;
            }

            /* Build first_idx: scan filtered key column once, record first
             * occurrence of each group key value. */
            if (key_sym < 0) {
                /* Computed group key (e.g., xbar) — fall back to eval-level groupby */
                ray_release(grouped);
                int64_t tbl_ncols = ray_table_ncols(filtered_tbl);
                ray_env_push_scope();
                for (int64_t c = 0; c < tbl_ncols; c++) {
                    int64_t cn = ray_table_col_name(filtered_tbl, c);
                    ray_t* cv = ray_table_get_col_idx(filtered_tbl, c);
                    ray_env_set_local(cn, cv);
                }
                ray_t* computed_key = ray_eval(by_expr);
                ray_env_pop_scope();
                if (!computed_key || RAY_IS_ERR(computed_key)) {
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return computed_key ? computed_key : ray_error("domain", NULL);
                }
                ray_t* groups2 = ray_group_fn(computed_key);
                if (!groups2 || RAY_IS_ERR(groups2)) {
                    ray_release(computed_key);
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return groups2 ? groups2 : ray_error("domain", NULL);
                }
                int64_t ng2 = ray_len(groups2) / 2;
                if (ng2 == 0) { ray_release(groups2); ray_release(computed_key); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_table_new(0); }
                ray_t** gi2 = (ray_t**)ray_data(groups2);
                int64_t fi2[256];
                for (int64_t g2 = 0; g2 < ng2 && g2 < 256; g2++) {
                    int alloc2 = 0;
                    ray_t* i02 = collection_elem(gi2[g2 * 2 + 1], 0, &alloc2);
                    fi2[g2] = as_i64(i02);
                    if (alloc2) ray_release(i02);
                }
                int64_t ckey_name = ray_sym_intern("+", 1);
                if (by_expr->type == RAY_LIST && by_expr->len >= 2) {
                    ray_t** be = (ray_t**)ray_data(by_expr);
                    if (be[1]->type == -RAY_SYM && (be[1]->attrs & RAY_ATTR_NAME))
                        ckey_name = be[1]->i64;
                }
                ray_t* res2 = ray_table_new(tbl_ncols);
                /* Key column first */
                { ray_t* okc = ray_table_get_col(filtered_tbl, ckey_name);
                  if (okc) {
                    ray_t* kv = ray_vec_new(okc->type, ng2);
                    for (int64_t g2 = 0; g2 < ng2; g2++) { int a2 = 0; ray_t* v2 = collection_elem(okc, fi2[g2], &a2); store_typed_elem(kv, g2, v2); if (a2) ray_release(v2); }
                    kv->len = ng2;
                    res2 = ray_table_add_col(res2, ckey_name, kv); ray_release(kv);
                  }
                }
                for (int64_t c = 0; c < tbl_ncols; c++) {
                    int64_t cn = ray_table_col_name(filtered_tbl, c);
                    if (cn == ckey_name) continue;
                    ray_t* sc = ray_table_get_col_idx(filtered_tbl, c);
                    ray_t* dc = ray_vec_new(sc->type, ng2);
                    for (int64_t g2 = 0; g2 < ng2; g2++) { int a2 = 0; ray_t* v2 = collection_elem(sc, fi2[g2], &a2); store_typed_elem(dc, g2, v2); if (a2) ray_release(v2); }
                    dc->len = ng2;
                    res2 = ray_table_add_col(res2, cn, dc); ray_release(dc);
                }
                ray_release(groups2); ray_release(computed_key);
                if (filtered_tbl != tbl) ray_release(filtered_tbl);
                ray_release(tbl);
                return res2;
            }

            ray_t* orig_key_col = ray_table_get_col(filtered_tbl, key_sym);
            int64_t nrows_orig = orig_key_col ? orig_key_col->len : 0;

            /* Read group key values from grouped table BEFORE releasing it.
             * grp_key_col points into grouped — must not access after release. */
            ray_t* grp_key_col = ray_table_get_col(grouped, key_sym);
            int8_t kt = orig_key_col ? orig_key_col->type : 0;

            /* Heap-allocate gk_vals when n_groups > 256 */
            int64_t gk_stack[256];
            ray_t* gk_heap_hdr = NULL;
            int64_t* gk_vals = gk_stack;
            if (n_groups > 256) {
                gk_heap_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!gk_heap_hdr) { ray_release(grouped); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                gk_vals = (int64_t*)ray_data(gk_heap_hdr);
            }

            /* Copy group key values while grouped is still alive.
             * GUID: 16 bytes per key → separate buffer.
             * STR/LIST/GUID: handled by use_eval_group, never reach here. */
            /* GUID/STR/LIST keys are routed through use_eval_group above,
             * so only integer-like types reach here. */
            if (grp_key_col) {
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    if (kt == RAY_F64)
                        memcpy(&gk_vals[gi], &((double*)ray_data(grp_key_col))[gi], 8);
                    else
                        gk_vals[gi] = ray_read_sym(ray_data(grp_key_col), gi, kt, grp_key_col->attrs);
                }
            }
            ray_release(grouped); /* grp_key_col is now invalid */

            /* Allocate first_idx */
            int64_t first_idx_stack[256];
            ray_t* fi_heap_hdr = NULL;
            int64_t* first_idx = first_idx_stack;
            if (n_groups > 256) {
                fi_heap_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!fi_heap_hdr) { if (gk_heap_hdr) ray_free(gk_heap_hdr); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                first_idx = (int64_t*)ray_data(fi_heap_hdr);
            }

            /* Single scan: mark first occurrence of each group key */
            for (int64_t gi = 0; gi < n_groups; gi++) first_idx[gi] = -1;
            int64_t found = 0;
            for (int64_t r = 0; r < nrows_orig && found < n_groups; r++) {
                int64_t ov;
                if (kt == RAY_F64) memcpy(&ov, &((double*)ray_data(orig_key_col))[r], 8);
                else ov = ray_read_sym(ray_data(orig_key_col), r, kt, orig_key_col->attrs);
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    if (first_idx[gi] >= 0) continue;
                    if (ov == gk_vals[gi]) { first_idx[gi] = r; found++; break; }
                }
            }
            if (gk_heap_hdr) ray_free(gk_heap_hdr);

            /* Now build the result table using first_idx gathered above.
             * key_sym and n_groups are already set. */

            /* Build result table: key column first, then others */
            int64_t ncols = ray_table_ncols(filtered_tbl);
            ray_t* result = ray_table_new(ncols);
            if (RAY_IS_ERR(result)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); return result; }

            /* Add key column first */
            ray_t* key_vec_src = ray_table_get_col(filtered_tbl, key_sym);
            if (key_vec_src->type == RAY_STR) {
                ray_t* key_vec_dst = ray_vec_new(RAY_STR, n_groups);
                if (!key_vec_dst || RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst ? key_vec_dst : ray_error("oom", NULL); }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(key_vec_src, first_idx[gi], &slen);
                    key_vec_dst = ray_str_vec_append(key_vec_dst, sp ? sp : "", sp ? slen : 0);
                    if (RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst; }
                }
                result = ray_table_add_col(result, key_sym, key_vec_dst);
                ray_release(key_vec_dst);
            } else {
                ray_t* key_vec_dst = ray_vec_new(key_vec_src->type, n_groups);
                if (RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst; }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    int alloc = 0;
                    ray_t* val = collection_elem(key_vec_src, first_idx[gi], &alloc);
                    store_typed_elem(key_vec_dst, gi, val);
                    if (alloc) ray_release(val);
                }
                key_vec_dst->len = n_groups;
                result = ray_table_add_col(result, key_sym, key_vec_dst);
                ray_release(key_vec_dst);
            }

            /* Add non-key columns */
            for (int64_t c = 0; c < ncols; c++) {
                int64_t col_name = ray_table_col_name(filtered_tbl, c);
                if (col_name == key_sym) continue;
                ray_t* src_col = ray_table_get_col_idx(filtered_tbl, c);
                int8_t ct = src_col->type;

                if (ct == RAY_STR) {
                    /* String column: build STR vector */
                    ray_t* dst = ray_vec_new(RAY_STR, n_groups);
                    if (!dst || RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst ? dst : ray_error("oom", NULL); }
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        size_t slen = 0;
                        const char* sp = ray_str_vec_get(src_col, first_idx[gi], &slen);
                        dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                        if (RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst; }
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                } else if (ct == RAY_LIST) {
                    /* List column: pick items */
                    ray_t* dst = ray_alloc(n_groups * sizeof(ray_t*));
                    if (!dst) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return ray_error("oom", NULL); }
                    dst->type = RAY_LIST;
                    dst->len = n_groups;
                    ray_t** dout = (ray_t**)ray_data(dst);
                    ray_t** src_items = (ray_t**)ray_data(src_col);
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        dout[gi] = src_items[first_idx[gi]];
                        ray_retain(dout[gi]);
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                } else {
                    /* Typed vector: copy elements at first indices */
                    ray_t* dst = ray_vec_new(ct, n_groups);
                    if (RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst; }
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        int alloc = 0;
                        ray_t* val = collection_elem(src_col, first_idx[gi], &alloc);
                        store_typed_elem(dst, gi, val);
                        if (alloc) ray_release(val);
                    }
                    dst->len = n_groups;
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                }
                if (RAY_IS_ERR(result)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); return result; }
            }

            if (fi_heap_hdr) ray_free(fi_heap_hdr);
            if (filtered_tbl != tbl) ray_release(filtered_tbl);
            ray_release(tbl);
            return apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);
        }
    } else if (n_out > 0) {
        /* Projection only (no group by) — select specific columns */
        ray_op_t* col_ops[16];
        uint8_t nc = 0;
        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id || kid == take_id || kid == asc_id || kid == desc_id) continue;
            if (nc < 16) {
                col_ops[nc] = compile_expr_dag(g, dict_elems[i + 1]);
                if (!col_ops[nc]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                nc++;
            }
        }
        root = ray_select(g, root, col_ops, nc);
    }

    /* Sort: collect asc/desc columns in dict iteration order.
     * Only add to the DAG when there's no group-by — group-by changes the
     * output schema, so sort on output columns must happen post-execution.
     * Values are unevaluated — a SYM atom is a column name, a SYM vector
     * is multiple column names.  No ray_eval needed. */
    if (has_sort && !by_expr) {
        ray_op_t* sort_keys[16];
        uint8_t   sort_descs[16];
        uint8_t   n_sort = 0;
        for (int64_t i = 0; i + 1 < dict_n && n_sort < 16; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            uint8_t is_desc = 0;
            if (kid == asc_id) is_desc = 0;
            else if (kid == desc_id) is_desc = 1;
            else continue;
            ray_t* val = dict_elems[i + 1];
            if (val->type == -RAY_SYM) {
                /* Single column name */
                ray_t* s = ray_sym_str(val->i64);
                sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                sort_descs[n_sort] = is_desc;
                n_sort++;
            } else if (ray_is_vec(val) && val->type == RAY_SYM) {
                /* Multiple column names */
                for (int64_t c = 0; c < val->len && n_sort < 16; c++) {
                    int64_t sid = ray_read_sym(ray_data(val), c, val->type, val->attrs);
                    ray_t* s = ray_sym_str(sid);
                    sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                    sort_descs[n_sort] = is_desc;
                    n_sort++;
                }
            } else {
                ray_graph_free(g); ray_release(tbl);
                return ray_error("domain", NULL);
            }
        }
        if (n_sort > 0)
            root = ray_sort_op(g, root, sort_keys, sort_descs, NULL, n_sort);
    }

    /* Take: add to DAG only when no group-by (same reason as sort above).
     * Positive atom → head, negative → tail, [start count] → post-execution. */
    ray_t* take_range = NULL;
    if (take_expr && !by_expr) {
        ray_t* tv = ray_eval(take_expr);
        if (!tv || RAY_IS_ERR(tv)) { ray_graph_free(g); ray_release(tbl); return tv ? tv : ray_error("domain", NULL); }
        if (ray_is_atom(tv) && (tv->type == -RAY_I64 || tv->type == -RAY_I32)) {
            int64_t n_take = (tv->type == -RAY_I64) ? tv->i64 : tv->i32;
            ray_release(tv);
            if (n_take >= 0)
                root = ray_head(g, root, n_take);
            else
                root = ray_tail(g, root, -n_take);
        } else if (ray_is_vec(tv) && (tv->type == RAY_I64 || tv->type == RAY_I32) && tv->len == 2) {
            take_range = tv;  /* apply after DAG execution */
        } else {
            ray_release(tv);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("domain", NULL);
        }
    }

    /* Optimize and execute */
    root = ray_optimize(g, root);
    ray_t* result = ray_execute(g, root);

    ray_graph_free(g);

    /* Post-process: range take [start count] applied after execution */
    if (take_range && result && !RAY_IS_ERR(result)) {
        ray_t* sliced = ray_take_fn(result, take_range);
        ray_release(result);
        ray_release(take_range);
        result = sliced;
    } else if (take_range) {
        ray_release(take_range);
    }

    /* Post-process: reorder GROUP BY BOOL results to match first-occurrence
     * order in the original table (exec.c radix sort puts false before true) */
    if (by_expr && result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
        if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
            ray_t* key_col = ray_table_get_col_idx(result, 0);
            if (key_col && key_col->type == RAY_BOOL && key_col->len >= 2) {
                /* Find first-occurrence order of bool values in original table */
                int64_t by_sym = -1;
                if (by_expr->type == -RAY_SYM) by_sym = by_expr->i64;
                ray_t* orig_key = (by_sym >= 0) ? ray_table_get_col(tbl, by_sym) : NULL;
                if (orig_key && orig_key->type == RAY_BOOL && orig_key->len > 0) {
                    bool first_val = ((bool*)ray_data(orig_key))[0];
                    bool result_first = ((bool*)ray_data(key_col))[0];
                    if (first_val != result_first) {
                        /* Swap rows: reverse row order in all columns */
                        int64_t nrows_r = ray_table_nrows(result);
                        int64_t ncols_r = ray_table_ncols(result);
                        ray_t* reordered = ray_table_new((int32_t)ncols_r);
                        if (reordered && !RAY_IS_ERR(reordered)) {
                            int ok = 1;
                            for (int64_t c = 0; c < ncols_r && ok; c++) {
                                int64_t cn = ray_table_col_name(result, c);
                                ray_t* col = ray_table_get_col_idx(result, c);
                                int esz = ray_elem_size(col->type);
                                ray_t* new_col = ray_vec_new(col->type, nrows_r);
                                if (RAY_IS_ERR(new_col)) { ok = 0; break; }
                                new_col->len = nrows_r;
                                char* src = (char*)ray_data(col);
                                char* dst = (char*)ray_data(new_col);
                                for (int64_t r = 0; r < nrows_r; r++)
                                    memcpy(dst + r * esz, src + (nrows_r - 1 - r) * esz, esz);
                                reordered = ray_table_add_col(reordered, cn, new_col);
                                ray_release(new_col);
                                if (RAY_IS_ERR(reordered)) { ok = 0; break; }
                            }
                            if (ok) {
                                ray_release(result);
                                result = reordered;
                            } else if (reordered && !RAY_IS_ERR(reordered)) {
                                ray_release(reordered);
                            }
                        }
                    }
                }
            }
        }
    }

    ray_release(tbl);

    /* Rename output columns if user specified names */
    if (result && !RAY_IS_ERR(result) && n_out > 0) {
        /* Materialize lazy results if needed */
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
    }
    if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE && n_out > 0) {
        ray_t* schema = ray_table_schema(result);
        if (schema && !RAY_IS_ERR(schema) && schema->type > 0 && schema->type < RAY_TYPE_COUNT) {
            int64_t ncols = schema->len;
            /* Count key columns in by clause */
            int n_key_cols = 0;
            if (by_expr) {
                if (ray_is_vec(by_expr) && by_expr->type == RAY_SYM) n_key_cols = (int)ray_len(by_expr);
                else n_key_cols = 1;
            }
            /* User-defined output column names */
            int64_t user_names[16];
            int n_user = 0;
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid != from_id && kid != where_id && kid != by_id &&
                    kid != take_id && kid != asc_id && kid != desc_id && n_user < 16)
                    user_names[n_user++] = kid;
            }
            /* Rename agg columns (after key columns) using table API */
            for (int j = 0; j < n_user && n_key_cols + j < ncols; j++)
                ray_table_set_col_name(result, n_key_cols + j, user_names[j]);
        }
    }

    /* Post-process: apply sort/take for group-by queries (output schema
     * differs from input, so these can't be added to the original DAG) */
    if (by_expr && (has_sort || take_expr))
        result = apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);

    return result;
}

/* (xbar col bucket) — time/value bucketing: floor(col/bucket)*bucket */
ray_t* ray_xbar_fn(ray_t* col, ray_t* bucket) {
    /* Recursive unwrap for nested collections (list of vectors) */
    if (is_collection(col) || is_collection(bucket))
        return atomic_map_binary(ray_xbar_fn, col, bucket);
    /* Both are integer types (i64, i32, i16) → integer xbar */
    if (is_numeric(col) && is_numeric(bucket) && !is_float_op(col, bucket)) {
        int64_t a = as_i64(col), b = as_i64(bucket);
        if (b == 0 || RAY_ATOM_IS_NULL(col) || RAY_ATOM_IS_NULL(bucket))
            return ray_error("domain", NULL);
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        /* Result type follows the wider of the two operands */
        if (col->type == -RAY_I32 && bucket->type == -RAY_I32) return make_i32((int32_t)result);
        if (col->type == -RAY_I16 && bucket->type == -RAY_I16) return make_i16((int16_t)result);
        return make_i64(result);
    }
    /* Float path: either operand is f64 */
    if (is_numeric(col) && is_numeric(bucket)) {
        double c = as_f64(col), b = as_f64(bucket);
        if (b == 0.0 || isnan(c) || isnan(b)) return ray_error("domain", NULL);
        double fq = floor(c / b);
        return make_f64(fq * b);
    }
    /* Temporal xbar: col is temporal, bucket is integer or temporal (not float) */
    if (is_temporal(col) && (is_temporal(bucket) ||
        (is_numeric(bucket) && bucket->type != -RAY_F64))) {
        int64_t a = col->i64, b;
        if (is_temporal(bucket)) {
            b = bucket->i64;
            /* Cross-temporal conversion: TIME(ms) bucket on TIMESTAMP(ns) col */
            if (col->type == -RAY_TIMESTAMP && bucket->type == -RAY_TIME)
                b *= 1000000LL;
        } else {
            b = as_i64(bucket);
        }
        if (b == 0 || RAY_ATOM_IS_NULL(bucket)) return ray_error("domain", NULL);
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        if (col->type == -RAY_TIME) return ray_time(result);
        if (col->type == -RAY_DATE) return ray_date(result);
        return ray_timestamp(result);
    }
    return ray_error("type", NULL);
}

/* ══════════════════════════════════════════
 * Update, Insert, Upsert
 * ══════════════════════════════════════════ */

/* Helper: convert a Rayfall list of atoms into a typed column vector by
 * appending to an existing column (for insert/upsert). */
static ray_t* append_atom_to_col(ray_t* col_vec, ray_t* atom) {
    if (RAY_ATOM_IS_NULL(atom)) {
        int64_t idx = col_vec->len;
        uint8_t zero[16] = {0};
        col_vec = ray_vec_append(col_vec, zero);
        if (!RAY_IS_ERR(col_vec))
            ray_vec_set_null(col_vec, idx, true);
        return col_vec;
    }
    int8_t ct = col_vec->type;
    if (ct == RAY_I64) {
        if (atom->type != -RAY_I64)
            return ray_error("type", NULL);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_SYM) {
        if (atom->type != -RAY_SYM)
            return ray_error("type", NULL);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_F64) {
        if (atom->type != -RAY_F64 && atom->type != -RAY_I64)
            return ray_error("type", NULL);
        double v = (atom->type == -RAY_F64) ? atom->f64 : (double)atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_BOOL) {
        if (atom->type != -RAY_BOOL)
            return ray_error("type", NULL);
        uint8_t v = atom->b8;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_STR && atom->type == -RAY_STR) {
        const char *sptr = ray_str_ptr(atom);
        size_t slen = ray_str_len(atom);
        return ray_str_vec_append(col_vec, sptr, slen);
    }
    return ray_error("type", NULL);
}

/* (update {col: expr ... from: t [where: pred]})
 * Special form — receives unevaluated dict arg.
 * For rows matching where (or all if no where), evaluate column expressions
 * and replace those column values. Returns a new table. */
/* Forward declarations */

ray_t* ray_update_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_LIST || !(dict->attrs & RAY_ATTR_DICT))
        return ray_error("type", NULL);

    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return ray_error("domain", NULL);
    /* Detect in-place update: from: 't means quoted symbol */
    int64_t inplace_sym = -1;
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type == -RAY_SYM) {
        /* from: 't — resolve symbol to table variable */
        inplace_sym = tbl->i64;
        ray_release(tbl);
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    }
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", NULL); }

    ray_t* where_expr = dict_get(dict, "where");
    ray_t* by_expr = dict_get(dict, "by");

    /* UPDATE WITH BY: group, compute aggregate, broadcast back */
    if (by_expr && !where_expr) {
        int64_t dict_n = ray_len(dict);
        ray_t** dict_elems = (ray_t**)ray_data(dict);
        int64_t from_id  = ray_sym_intern("from",  4);
        int64_t where_id = ray_sym_intern("where", 5);
        int64_t by_id    = ray_sym_intern("by",    2);

        /* Resolve group key column name.
         * by_expr is a name reference (not evaluated) — extract sym_id directly */
        int64_t by_col_name = -1;
        if (by_expr->type == -RAY_SYM) {
            by_col_name = by_expr->i64;
        }
        if (by_col_name < 0) { ray_release(tbl); return ray_error("type", NULL); }

        /* Find group column in table */
        ray_t* grp_col = ray_table_get_col(tbl, by_col_name);
        if (!grp_col) { ray_release(tbl); return ray_error("domain", NULL); }
        int64_t nrows2 = ray_table_nrows(tbl);

        /* Use ray_group_fn to get group indices: {key: [indices]} */
        ray_t* groups = NULL;
        {
            groups = ray_group_fn(grp_col);
            if (!groups || RAY_IS_ERR(groups)) { ray_release(tbl); return groups ? groups : ray_error("oom", NULL); }
        }

        /* Start with a copy of the original table */
        int64_t ncols = ray_table_ncols(tbl);
        ray_t* result = ray_table_new((int32_t)ncols);
        if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            ray_t* col = ray_table_get_col_idx(tbl, c);
            ray_retain(col);
            result = ray_table_add_col(result, cn, col);
            ray_release(col);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        }

        /* For each aggregate expression, compute per group and broadcast */
        for (int64_t d = 0; d + 1 < dict_n; d += 2) {
            int64_t kid = dict_elems[d]->i64;
            if (kid == from_id || kid == where_id || kid == by_id) continue;
            ray_t* agg_expr = dict_elems[d + 1];

            /* Evaluate the aggregate for each group and broadcast */
            ray_t* grp_items = (ray_t**)ray_data(groups) ? groups : NULL;
            if (!grp_items) { ray_release(result); ray_release(groups); ray_release(tbl); return ray_error("oom", NULL); }
            int64_t ngroups = groups->len / 2;
            ray_t** gdata = (ray_t**)ray_data(groups);

            /* We need to evaluate the aggregate per group.
             * Build the result column by evaluating the expression on each group's subset. */
            ray_t* out_col = ray_vec_new(RAY_I64, nrows2); /* will be resized to correct type */
            if (RAY_IS_ERR(out_col)) { ray_release(result); ray_release(groups); ray_release(tbl); return out_col; }

            int8_t out_type = RAY_I64;
            int first_group = 1;

            for (int64_t gi = 0; gi < ngroups; gi++) {
                ray_t* idx_vec = gdata[gi * 2 + 1]; /* index vector for this group */
                int64_t gsize = ray_len(idx_vec);

                /* Build a sub-table for this group */
                ray_t* sub_tbl = ray_table_new((int32_t)ncols);
                if (RAY_IS_ERR(sub_tbl)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_tbl; }
                for (int64_t c = 0; c < ncols; c++) {
                    int64_t cn = ray_table_col_name(tbl, c);
                    ray_t* full_col = ray_table_get_col_idx(tbl, c);
                    int8_t ct = full_col->type;
                    ray_t* sub_col = ray_vec_new(ct, gsize);
                    if (RAY_IS_ERR(sub_col)) { ray_release(sub_tbl); ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_col; }
                    sub_col->len = gsize;
                    int esz = ray_elem_size(ct);
                    char* src = (char*)ray_data(full_col);
                    char* dst = (char*)ray_data(sub_col);
                    int64_t* idxs = (int64_t*)ray_data(idx_vec);
                    for (int64_t r = 0; r < gsize; r++)
                        memcpy(dst + r * esz, src + idxs[r] * esz, esz);
                    sub_tbl = ray_table_add_col(sub_tbl, cn, sub_col);
                    ray_release(sub_col);
                    if (RAY_IS_ERR(sub_tbl)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_tbl; }
                }

                /* Evaluate expression on sub-table via DAG */
                ray_graph_t* ug = ray_graph_new(sub_tbl);
                ray_op_t* expr_op = compile_expr_dag(ug, agg_expr);
                if (!expr_op) { ray_graph_free(ug); ray_release(sub_tbl); ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return ray_error("domain", NULL); }
                expr_op = ray_optimize(ug, expr_op);
                ray_t* agg_result = ray_execute(ug, expr_op);
                ray_graph_free(ug);
                ray_release(sub_tbl);

                if (RAY_IS_ERR(agg_result)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return agg_result; }

                /* Determine output type from first group */
                if (first_group) {
                    if (ray_is_atom(agg_result)) out_type = -agg_result->type;
                    else if (ray_is_vec(agg_result)) out_type = agg_result->type;
                    ray_release(out_col);
                    out_col = ray_vec_new(out_type, nrows2);
                    if (RAY_IS_ERR(out_col)) { ray_release(agg_result); ray_release(result); ray_release(groups); ray_release(tbl); return out_col; }
                    out_col->len = nrows2;
                    first_group = 0;
                }

                /* Broadcast aggregate value to all rows in this group */
                int64_t* idxs = (int64_t*)ray_data(idx_vec);
                if (ray_is_atom(agg_result)) {
                    for (int64_t r = 0; r < gsize; r++)
                        store_typed_elem(out_col, idxs[r], agg_result);
                }
                ray_release(agg_result);
            }

            /* Add the new column to the result table */
            result = ray_table_add_col(result, kid, out_col);
            ray_release(out_col);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        }

        ray_release(groups);
        /* Store in-place if needed */
        if (inplace_sym >= 0) {
            ray_env_set(inplace_sym, result);
        }
        ray_release(tbl);
        return result;
    }

    /* Evaluate WHERE using the DAG to get a boolean mask */
    int64_t nrows = ray_table_nrows(tbl);
    uint8_t* mask = NULL;

    if (where_expr) {
        /* Try DAG compilation first, fall back to eval-level */
        ray_t* mask_vec = NULL;
        ray_graph_t* g = ray_graph_new(tbl);
        if (g) {
            ray_op_t* pred = compile_expr_dag(g, where_expr);
            if (pred) {
                pred = ray_optimize(g, pred);
                mask_vec = ray_execute(g, pred);
            }
            ray_graph_free(g);
        }
        /* Fallback: eval-level predicate evaluation */
        if (!mask_vec || RAY_IS_ERR(mask_vec)) {
            /* Bind column names to column vectors in env, then eval */
            int64_t ncols2 = ray_table_ncols(tbl);
            ray_env_push_scope();
            for (int64_t c = 0; c < ncols2; c++) {
                int64_t cn = ray_table_col_name(tbl, c);
                ray_t* col = ray_table_get_col_idx(tbl, c);
                ray_env_set(cn, col);
            }
            mask_vec = ray_eval(where_expr);
            ray_env_pop_scope();
        }
        if (!mask_vec || RAY_IS_ERR(mask_vec)) { ray_release(tbl); return mask_vec ? mask_vec : ray_error("type", NULL); }
        if (mask_vec->type != RAY_BOOL || mask_vec->len != nrows) {
            ray_release(mask_vec);
            ray_release(tbl);
            return ray_error("type", NULL);
        }
        mask = (uint8_t*)ray_data(mask_vec);
        /* Keep mask_vec alive until we're done */

        /* Build a new table with updated columns */
        int64_t ncols = ray_table_ncols(tbl);
        int64_t dict_n = ray_len(dict);
        ray_t** dict_elems = (ray_t**)ray_data(dict);
        int64_t from_id = ray_sym_intern("from", 4);
        int64_t where_id = ray_sym_intern("where", 5);

        ray_t* result = ray_table_new(ncols);
        if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }

        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            ray_t* orig_col = ray_table_get_col_idx(tbl, c);

            /* Check if this column has an update expression */
            ray_t* update_expr = NULL;
            for (int64_t d = 0; d + 1 < dict_n; d += 2) {
                int64_t kid = dict_elems[d]->i64;
                if (kid == from_id || kid == where_id) continue;
                if (kid == col_name) { update_expr = dict_elems[d + 1]; break; }
            }

            if (!update_expr) {
                /* No update for this column — copy as-is */
                ray_retain(orig_col);
                result = ray_table_add_col(result, col_name, orig_col);
                ray_release(orig_col);
            } else {
                /* Evaluate the expression for each row and apply to matching rows */
                int8_t ct = orig_col->type;
                ray_t* new_col = ray_vec_new(ct, nrows);
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }

                /* Evaluate expression via DAG, fallback to eval-level */
                ray_t* expr_vec = NULL;
                {
                    ray_graph_t* ug = ray_graph_new(tbl);
                    if (ug) {
                        ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                        if (expr_op) {
                            expr_op = ray_optimize(ug, expr_op);
                            expr_vec = ray_execute(ug, expr_op);
                        }
                        ray_graph_free(ug);
                    }
                }
                if (!expr_vec || RAY_IS_ERR(expr_vec)) {
                    /* Fallback: eval with column bindings */
                    int64_t ncols_e = ray_table_ncols(tbl);
                    ray_env_push_scope();
                    for (int64_t c2 = 0; c2 < ncols_e; c2++) {
                        int64_t cn = ray_table_col_name(tbl, c2);
                        ray_t* col2 = ray_table_get_col_idx(tbl, c2);
                        ray_env_set(cn, col2);
                    }
                    expr_vec = ray_eval(update_expr);
                    ray_env_pop_scope();
                }
                if (!expr_vec || RAY_IS_ERR(expr_vec)) { ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return expr_vec ? expr_vec : ray_error("type", NULL); }

                /* WHERE update: expression result replaces ONLY masked rows.
                 * When type differs (e.g., I64 col, F64 expr from (* col 1.1)),
                 * keep original column type and cast expr results.
                 * Only numeric promotions are allowed — STR↔numeric is a type error. */
                int8_t expr_type = (expr_vec->type < 0) ? -expr_vec->type : expr_vec->type;
                if (expr_type != ct && expr_type > 0 && ray_is_vec(expr_vec)) {
                    /* Only allow numeric promotions (I64↔F64, I32↔F64) */
                    int is_numeric_promo = (ct == RAY_I64 || ct == RAY_I32 || ct == RAY_F64) &&
                                           (expr_type == RAY_I64 || expr_type == RAY_I32 || expr_type == RAY_F64);
                    if (!is_numeric_promo) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return ray_error("type", NULL);
                    }
                    /* Copy original column values first */
                    int esz = ray_elem_size(ct);
                    memcpy(ray_data(new_col), ray_data(orig_col), (size_t)(nrows * esz));
                    new_col->len = nrows;
                    /* Overlay masked rows with type conversion */
                    for (int64_t r = 0; r < nrows; r++) {
                        if (!mask[r]) continue;
                        if (ct == RAY_I64 && expr_type == RAY_F64)
                            ((int64_t*)ray_data(new_col))[r] = (int64_t)((double*)ray_data(expr_vec))[r];
                        else if (ct == RAY_I32 && expr_type == RAY_F64)
                            ((int32_t*)ray_data(new_col))[r] = (int32_t)((double*)ray_data(expr_vec))[r];
                        else if (ct == RAY_F64 && expr_type == RAY_I64)
                            ((double*)ray_data(new_col))[r] = (double)((int64_t*)ray_data(expr_vec))[r];
                    }
                    ray_release(expr_vec);
                    result = ray_table_add_col(result, col_name, new_col);
                    ray_release(new_col);
                    if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
                    continue;
                }

                /* Broadcast scalar atom to full column vector if needed */
                if (expr_vec->type < 0) {
                    /* Type check atom against column type BEFORE broadcast */
                    int ok = (expr_vec->type == -ct);
                    if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                    if (!ok && ct == RAY_LIST && expr_vec->type == -RAY_SYM) ok = 1;
                    if (!ok && ct == RAY_SYM && expr_vec->type == -RAY_SYM) ok = 1;
                    if (!ok) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return ray_error("type", NULL);
                    }
                    /* SYM atom to LIST column: build boxed list, merge with mask */
                    if (ct == RAY_LIST && expr_vec->type == -RAY_SYM) {
                        ray_free(new_col);
                        ray_t* new_list = ray_list_new((int32_t)nrows);
                        if (RAY_IS_ERR(new_list)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_list; }
                        ray_t** orig_elems = (ray_t**)ray_data(orig_col);
                        for (int64_t r = 0; r < nrows; r++) {
                            ray_t* elem = mask[r] ? expr_vec : orig_elems[r];
                            ray_retain(elem);
                            new_list = ray_list_append(new_list, elem);
                            ray_release(elem);
                            if (RAY_IS_ERR(new_list)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_list; }
                        }
                        ray_release(expr_vec);
                        result = ray_table_add_col(result, col_name, new_list);
                        ray_release(new_list);
                        if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
                        continue;
                    }
                    ray_t* bcast = ray_vec_new(ct, nrows);
                    if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                    if (ct == RAY_STR && expr_vec->type == -RAY_STR) {
                        const char* sp = ray_str_ptr(expr_vec);
                        size_t sl = ray_str_len(expr_vec);
                        for (int64_t r = 0; r < nrows; r++) {
                            bcast = ray_str_vec_append(bcast, sp, sl);
                            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                        }
                    } else {
                        size_t esz = (ct == RAY_BOOL) ? 1 : 8;
                        uint8_t elem[8] = {0};
                        if (ct == RAY_F64 && expr_vec->type == -RAY_I64) {
                            double promoted = (double)expr_vec->i64;
                            memcpy(elem, &promoted, 8);
                        } else {
                            memcpy(elem, &expr_vec->i64, esz);
                        }
                        for (int64_t r = 0; r < nrows; r++) {
                            bcast = ray_vec_append(bcast, elem);
                            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                        }
                    }
                    ray_release(expr_vec);
                    expr_vec = bcast;
                }

                /* Promote I64 vector to F64 if column is F64 */
                if (expr_vec->type == RAY_I64 && ct == RAY_F64) {
                    int64_t nr = ray_len(expr_vec);
                    ray_t* promoted = ray_vec_new(RAY_F64, nr);
                    if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return promoted; }
                    int64_t* src_data = (int64_t*)ray_data(expr_vec);
                    for (int64_t r = 0; r < nr; r++) {
                        double v = (double)src_data[r];
                        promoted = ray_vec_append(promoted, &v);
                        if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return promoted; }
                    }
                    ray_release(expr_vec);
                    expr_vec = promoted;
                }

                /* Type check: expr_vec must match original column type */
                if (expr_vec->type != ct) {
                    ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                    return ray_error("type", NULL);
                }

                /* Merge: use expr_vec for matching rows, orig_col for non-matching */
                if (ct == RAY_STR) {
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        size_t slen = 0;
                        const char* sp = ray_str_vec_get(src_vec, r, &slen);
                        new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                    }
                } else if (ct == RAY_SYM) {
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        int64_t sym_val = ray_read_sym(ray_data(src_vec), r, src_vec->type, src_vec->attrs);
                        new_col = ray_vec_append(new_col, &sym_val);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                    }
                } else {
                    size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
                    uint8_t* orig_data = (uint8_t*)ray_data(orig_col);
                    uint8_t* expr_data = (uint8_t*)ray_data(expr_vec);
                    for (int64_t r = 0; r < nrows; r++) {
                        void* src = mask[r] ? (expr_data + r * elem_sz) : (orig_data + r * elem_sz);
                        new_col = ray_vec_append(new_col, src);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                    }
                }
                result = ray_table_add_col(result, col_name, new_col);
                ray_release(new_col);
                ray_release(expr_vec);
            }
            if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
        }

        ray_release(mask_vec);
        if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
        }
        ray_release(tbl);
        return result;
    }

    /* No WHERE — update all rows */
    int64_t ncols = ray_table_ncols(tbl);
    int64_t dict_n = ray_len(dict);
    ray_t** dict_elems = (ray_t**)ray_data(dict);
    int64_t from_id = ray_sym_intern("from", 4);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);

        ray_t* update_expr = NULL;
        for (int64_t d = 0; d + 1 < dict_n; d += 2) {
            int64_t kid = dict_elems[d]->i64;
            if (kid == from_id) continue;
            if (kid == col_name) { update_expr = dict_elems[d + 1]; break; }
        }

        if (!update_expr) {
            ray_retain(orig_col);
            result = ray_table_add_col(result, col_name, orig_col);
            ray_release(orig_col);
        } else {
            ray_t* expr_vec = NULL;
            {
                ray_graph_t* ug = ray_graph_new(tbl);
                if (ug) {
                    ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                    if (expr_op) {
                        expr_op = ray_optimize(ug, expr_op);
                        expr_vec = ray_execute(ug, expr_op);
                    }
                    ray_graph_free(ug);
                }
            }
            if (!expr_vec || RAY_IS_ERR(expr_vec)) {
                /* Fallback: eval with column bindings */
                int64_t ncols_f = ray_table_ncols(tbl);
                ray_env_push_scope();
                for (int64_t cf = 0; cf < ncols_f; cf++) {
                    int64_t cn = ray_table_col_name(tbl, cf);
                    ray_t* colf = ray_table_get_col_idx(tbl, cf);
                    ray_env_set(cn, colf);
                }
                expr_vec = ray_eval(update_expr);
                ray_env_pop_scope();
            }
            if (!expr_vec || RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec ? expr_vec : ray_error("type", NULL); }

            /* Broadcast scalar atom to full column vector if needed */
            if (expr_vec->type < 0) {
                int64_t nrows = ray_table_nrows(tbl);
                int8_t ct = orig_col->type;
                /* Type check atom against column type BEFORE broadcast */
                int ok = (expr_vec->type == -ct);
                if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                /* SYM atom → LIST column (LIST of SYM atoms) */
                if (!ok && ct == RAY_LIST && expr_vec->type == -RAY_SYM) ok = 1;
                if (!ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return ray_error("type", NULL);
                }
                /* SYM atom to LIST column: broadcast as boxed list */
                if (ct == RAY_LIST && expr_vec->type == -RAY_SYM) {
                    ray_t* bcast = ray_list_new((int32_t)nrows);
                    if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_retain(expr_vec);
                        bcast = ray_list_append(bcast, expr_vec);
                        ray_release(expr_vec);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                    ray_release(expr_vec);
                    expr_vec = bcast;
                    goto no_where_add_col;
                }
                ray_t* bcast = ray_vec_new(ct, nrows);
                if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                if (ct == RAY_STR && expr_vec->type == -RAY_STR) {
                    const char* sp = ray_str_ptr(expr_vec);
                    size_t sl = ray_str_len(expr_vec);
                    for (int64_t r = 0; r < nrows; r++) {
                        bcast = ray_str_vec_append(bcast, sp, sl);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                } else {
                    size_t esz = (ct == RAY_BOOL) ? 1 : 8;
                    uint8_t elem[8] = {0};
                    if (ct == RAY_F64 && expr_vec->type == -RAY_I64) {
                        double promoted = (double)expr_vec->i64;
                        memcpy(elem, &promoted, 8);
                    } else {
                        memcpy(elem, &expr_vec->i64, esz);
                    }
                    for (int64_t r = 0; r < nrows; r++) {
                        bcast = ray_vec_append(bcast, elem);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                }
                ray_release(expr_vec);
                expr_vec = bcast;
            }

            /* Promote I64 vector to F64 if column is F64 */
            if (expr_vec->type == RAY_I64 && orig_col->type == RAY_F64) {
                int64_t nr = ray_len(expr_vec);
                ray_t* promoted = ray_vec_new(RAY_F64, nr);
                if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return promoted; }
                int64_t* src_data = (int64_t*)ray_data(expr_vec);
                for (int64_t r = 0; r < nr; r++) {
                    double v = (double)src_data[r];
                    promoted = ray_vec_append(promoted, &v);
                    if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return promoted; }
                }
                ray_release(expr_vec);
                expr_vec = promoted;
            }

            /* No-WHERE update: allow type change for same-category types.
             * Atoms (type<0) will be broadcast later, check after broadcast.
             * For vectors, check now: only numeric promotions or same type.
             * Also allow SYM/LIST interop (columns may be stored as LIST). */
            if (expr_vec->type > 0 && expr_vec->type != orig_col->type) {
                int is_ok = 0;
                /* Numeric promotions */
                if ((orig_col->type == RAY_I64 || orig_col->type == RAY_I32 || orig_col->type == RAY_F64) &&
                    (expr_vec->type == RAY_I64 || expr_vec->type == RAY_I32 || expr_vec->type == RAY_F64))
                    is_ok = 1;
                /* SYM/LIST interop */
                if ((orig_col->type == RAY_SYM || orig_col->type == RAY_LIST) &&
                    (expr_vec->type == RAY_SYM || expr_vec->type == RAY_LIST))
                    is_ok = 1;
                if (!is_ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return ray_error("type", NULL);
                }
            }

no_where_add_col:
            result = ray_table_add_col(result, col_name, expr_vec);
            ray_release(expr_vec);
        }
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    /* Add NEW columns from dict (columns not already in the table) */
    for (int64_t d = 0; d + 1 < dict_n; d += 2) {
        int64_t kid = dict_elems[d]->i64;
        if (kid == from_id) continue;
        /* Check if this column already exists */
        int exists = 0;
        for (int64_t c = 0; c < ncols; c++) {
            if (ray_table_col_name(tbl, c) == kid) { exists = 1; break; }
        }
        if (exists) continue;

        /* New column: evaluate expression and add */
        ray_t* update_expr = dict_elems[d + 1];
        ray_graph_t* ug = ray_graph_new(tbl);
        ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
        if (!expr_op) { ray_release(result); ray_release(tbl); ray_graph_free(ug); return ray_error("domain", NULL); }
        expr_op = ray_optimize(ug, expr_op);
        ray_t* expr_vec = ray_execute(ug, expr_op);
        ray_graph_free(ug);
        if (RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec; }

        /* Broadcast scalar to column */
        if (expr_vec->type < 0) {
            int64_t nrows = ray_table_nrows(tbl);
            int8_t ct = -expr_vec->type;
            ray_t* bcast = ray_vec_new(ct, nrows);
            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
            size_t esz = ray_elem_size(ct);
            uint8_t elem[8] = {0};
            memcpy(elem, &expr_vec->i64, esz > 8 ? 8 : esz);
            for (int64_t r = 0; r < nrows; r++) {
                bcast = ray_vec_append(bcast, elem);
                if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
            }
            ray_release(expr_vec);
            expr_vec = bcast;
        }

        result = ray_table_add_col(result, kid, expr_vec);
        ray_release(expr_vec);
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    /* Store in-place if from: 't */
    if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
    }
    ray_release(tbl);
    return result;
}

/* (insert table (list val1 val2 ...)) — append a row to a table */
ray_t* ray_insert_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);

    /* Special form: detect 'sym (quoted symbol for in-place insert) */
    int64_t inplace_sym = -1;
    ray_t* tbl_raw = args[0];
    ray_t* tbl;

    /* Detect calling convention: already-evaluated args (from upsert) vs raw parse tree */
    int already_eval = (tbl_raw && tbl_raw->type == RAY_TABLE);

    if (!already_eval && tbl_raw && tbl_raw->type == -RAY_SYM && !(tbl_raw->attrs & RAY_ATTR_NAME)) {
        /* Quoted symbol 'sym (no ATTR_NAME) — in-place insert */
        inplace_sym = tbl_raw->i64;
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    } else if (already_eval) {
        tbl = tbl_raw;
        ray_retain(tbl);
    } else {
        tbl = ray_eval(tbl_raw);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", NULL);
    }

    /* Evaluate the row argument (skip if already evaluated) */
    ray_t* row = already_eval ? (ray_retain(args[1]), args[1]) : ray_eval(args[1]);
    if (!row || RAY_IS_ERR(row)) { ray_release(tbl); return row ? row : ray_error("type", NULL); }
    if (tbl->type != RAY_TABLE) { ray_release(tbl); ray_release(row); return ray_error("type", NULL); }

    int64_t ncols = ray_table_ncols(tbl);
    ray_t* row_orig = row; /* keep original eval result for cleanup */

    if (!is_list(row) && row->type != RAY_TABLE) { ray_release(tbl); ray_release(row); return ray_error("type", NULL); }

    /* Table row: convert to list of column vectors */
    ray_t* tbl_row_list = NULL;
    if (row->type == RAY_TABLE) {
        int64_t src_ncols = ray_table_ncols(row);
        if (src_ncols != ncols) { ray_release(tbl); ray_release(row); return ray_error("domain", NULL); }
        tbl_row_list = ray_alloc(ncols * sizeof(ray_t*));
        if (!tbl_row_list) { ray_release(tbl); ray_release(row_orig); return ray_error("oom", NULL); }
        tbl_row_list->type = RAY_LIST;
        tbl_row_list->len = ncols;
        ray_t** trl = (ray_t**)ray_data(tbl_row_list);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            ray_t* src_col = ray_table_get_col(row, col_name);
            if (!src_col) src_col = ray_table_get_col_idx(row, c);
            if (!src_col) {
                tbl_row_list->len = 0;
                ray_free(tbl_row_list);
                ray_release(tbl); ray_release(row_orig);
                return ray_error("domain", NULL);
            }
            trl[c] = src_col;
            ray_retain(src_col);
        }
        row = tbl_row_list;
    }

    /* Dict row: extract values in table column order */
    ray_t* dict_vals = NULL;
    if (row->attrs & RAY_ATTR_DICT) {
        dict_vals = ray_alloc(ncols * sizeof(ray_t*));
        if (!dict_vals) { ray_release(tbl); ray_release(row_orig); return ray_error("oom", NULL); }
        dict_vals->type = RAY_LIST;
        dict_vals->len = ncols;
        ray_t** dv = (ray_t**)ray_data(dict_vals);
        ray_t** dict_items = (ray_t**)ray_data(row);
        int64_t dict_len = ray_len(row);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            dv[c] = NULL;
            for (int64_t d = 0; d + 1 < dict_len; d += 2) {
                if (dict_items[d]->type == -RAY_SYM && dict_items[d]->i64 == col_name) {
                    dv[c] = dict_items[d + 1];
                    ray_retain(dv[c]);
                    break;
                }
            }
            /* dv[c] may be NULL for missing keys — will insert null
             * (but only if ALL dict keys exist as table columns) */
        }
        /* Verify all dict keys exist as table columns */
        for (int64_t d = 0; d + 1 < dict_len; d += 2) {
            if (dict_items[d]->type != -RAY_SYM) continue;
            int64_t dk = dict_items[d]->i64;
            int found_in_tbl = 0;
            for (int64_t c = 0; c < ncols; c++) {
                if (ray_table_col_name(tbl, c) == dk) { found_in_tbl = 1; break; }
            }
            if (!found_in_tbl) {
                for (int64_t c = 0; c < ncols; c++) if (dv[c]) ray_release(dv[c]);
                dict_vals->len = 0;
                ray_free(dict_vals);
                ray_release(tbl); ray_release(row_orig);
                return ray_error("domain", NULL);
            }
        }
        row = dict_vals;
    }

    if (ray_len(row) != ncols) {
        if (dict_vals) {
            for (int64_t c = 0; c < ncols; c++) ray_release(((ray_t**)ray_data(dict_vals))[c]);
            dict_vals->len = 0;
            ray_free(dict_vals);
        }
        ray_release(tbl); ray_release(row_orig);
        return ray_error("domain", NULL);
    }

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows + 1);
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        /* Copy existing data */
        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                size_t slen = 0;
                const char* sp = ray_str_vec_get(orig_col, r, &slen);
                new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                new_col = ray_vec_append(new_col, &sym_val);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                new_col = ray_vec_append(new_col, src + r * elem_sz);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        }

        /* Append new row value(s) — atom for single row, vector for multi-row */
        if (!row_elems[c]) {
            /* NULL = null value for this column type */
            ray_t* null_atom = ray_typed_null(-ct);
            new_col = append_atom_to_col(new_col, null_atom);
            ray_release(null_atom);
        } else if (ray_is_atom(row_elems[c])) {
            new_col = append_atom_to_col(new_col, row_elems[c]);
        } else if (ray_is_vec(row_elems[c]) || row_elems[c]->type == RAY_LIST) {
            ray_t* merged = ray_concat_fn(new_col, row_elems[c]);
            ray_release(new_col);
            new_col = merged;
        } else {
            new_col = append_atom_to_col(new_col, row_elems[c]);
        }
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    /* Cleanup dict_vals, tbl_row_list, and original row */
    if (dict_vals) {
        ray_t** dv = (ray_t**)ray_data(dict_vals);
        for (int64_t c = 0; c < ncols; c++) if (dv[c]) ray_release(dv[c]);
        dict_vals->len = 0; /* prevent ray_free from double-releasing children */
        ray_free(dict_vals);
    }
    if (tbl_row_list) {
        ray_t** trl = (ray_t**)ray_data(tbl_row_list);
        for (int64_t c = 0; c < ncols; c++) if (trl[c]) ray_release(trl[c]);
        tbl_row_list->len = 0;
        ray_free(tbl_row_list);
    }
    ray_release(tbl);
    ray_release(row_orig);

    /* In-place: update the variable in the env */
    if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
        ray_retain(result);
        return result;
    }
    return result;
}

/* (upsert table key_col (list val1 val2 ...)) — update row if key matches, else insert.
 * Special form: first arg may be 'sym for in-place, other args are evaluated. */
ray_t* ray_upsert_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);

    /* Detect calling convention: already-evaluated args (from recursive call) vs raw parse tree */
    int64_t inplace_sym = -1;
    ray_t* tbl_raw = args[0];
    int already_eval = (tbl_raw && tbl_raw->type == RAY_TABLE);
    ray_t* tbl;

    if (!already_eval && tbl_raw && tbl_raw->type == -RAY_SYM && !(tbl_raw->attrs & RAY_ATTR_NAME)) {
        inplace_sym = tbl_raw->i64;
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    } else if (already_eval) {
        tbl = tbl_raw;
        ray_retain(tbl);
    } else {
        tbl = ray_eval(tbl_raw);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", NULL);
    }

    ray_t* key_sym = already_eval ? (ray_retain(args[1]), args[1]) : ray_eval(args[1]);
    if (!key_sym || RAY_IS_ERR(key_sym)) { ray_release(tbl); return key_sym ? key_sym : ray_error("type", NULL); }

    ray_t* row = already_eval ? (ray_retain(args[2]), args[2]) : ray_eval(args[2]);
    if (!row || RAY_IS_ERR(row)) { ray_release(tbl); ray_release(key_sym); return row ? row : ray_error("type", NULL); }

    if (tbl->type != RAY_TABLE) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("type", NULL); }
    if (!is_list(row) && row->type != RAY_TABLE) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("type", NULL); }

    int64_t ncols = ray_table_ncols(tbl);

    /* Table row: iterate row-by-row for proper upsert semantics */
    if (row->type == RAY_TABLE) {
        int64_t src_nrows = ray_table_nrows(row);
        /* Get source columns by matching target column names; missing cols → NULL */
        ray_t* src_cols[64];
        for (int64_t c = 0; c < ncols && c < 64; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            src_cols[c] = ray_table_get_col(row, cn);
        }
        ray_t* cur_tbl = tbl;
        ray_retain(cur_tbl);
        for (int64_t r = 0; r < src_nrows; r++) {
            ray_t* single = ray_alloc(ncols * sizeof(ray_t*));
            if (!single) { ray_release(cur_tbl); ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
            single->type = RAY_LIST;
            single->len = ncols;
            ray_t** sr = (ray_t**)ray_data(single);
            for (int64_t c = 0; c < ncols; c++) {
                int alloc = 0;
                sr[c] = src_cols[c] ? collection_elem(src_cols[c], r, &alloc) : NULL;
                if (!alloc && sr[c]) ray_retain(sr[c]);
            }
            ray_t* upsert_args[3] = { cur_tbl, key_sym, single };
            ray_t* new_tbl = ray_upsert_fn(upsert_args, 3);
            for (int64_t c = 0; c < ncols; c++) if (sr[c]) ray_release(sr[c]);
            single->len = 0;
            ray_free(single);
            ray_release(cur_tbl);
            if (RAY_IS_ERR(new_tbl)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return new_tbl; }
            cur_tbl = new_tbl;
        }
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(cur_tbl)) {
            ray_env_set(inplace_sym, cur_tbl);
            ray_retain(cur_tbl);
        }
        return cur_tbl;
    }

    /* Dict row: extract values in column order to create a plain list */
    ray_t* dict_row_list = NULL;
    if (row->attrs & RAY_ATTR_DICT) {
        dict_row_list = ray_alloc(ncols * sizeof(ray_t*));
        if (!dict_row_list) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
        dict_row_list->type = RAY_LIST;
        dict_row_list->len = ncols;
        ray_t** drl = (ray_t**)ray_data(dict_row_list);
        ray_t** dict_items = (ray_t**)ray_data(row);
        int64_t dict_len = ray_len(row);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            drl[c] = NULL;
            for (int64_t d = 0; d + 1 < dict_len; d += 2) {
                if (dict_items[d]->type == -RAY_SYM && dict_items[d]->i64 == col_name) {
                    drl[c] = dict_items[d + 1];
                    ray_retain(drl[c]);
                    break;
                }
            }
            if (!drl[c]) {
                for (int64_t j = 0; j < c; j++) if (drl[j]) ray_release(drl[j]);
                dict_row_list->len = 0;
                ray_free(dict_row_list);
                ray_release(tbl); ray_release(key_sym); ray_release(row);
                return ray_error("domain", NULL);
            }
        }
        ray_release(row);
        row = dict_row_list;
    }

    if (ray_len(row) != ncols) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    /* Determine key columns — integer N means "first N columns are keys" */
    int64_t n_key_cols = 1;
    int64_t key_col_indices[16];
    if (key_sym->type == -RAY_SYM) {
        key_col_indices[0] = -1;
        for (int64_t c = 0; c < ncols; c++) {
            if (ray_table_col_name(tbl, c) == key_sym->i64) {
                key_col_indices[0] = c;
                break;
            }
        }
        if (key_col_indices[0] < 0) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }
    } else if (key_sym->type == -RAY_I64) {
        n_key_cols = key_sym->i64;
        if (n_key_cols <= 0 || n_key_cols > ncols || n_key_cols > 16) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }
        for (int64_t k = 0; k < n_key_cols; k++) key_col_indices[k] = k;
    } else {
        ray_release(tbl); ray_release(key_sym); ray_release(row);
        return ray_error("type", NULL);
    }

    /* Multi-row upsert: if row values are vectors, iterate row-by-row */
    ray_t* key_elem = row_elems[key_col_indices[0]];
    if (ray_is_vec(key_elem) || key_elem->type == RAY_LIST) {
        int64_t new_nrows = ray_len(key_elem);
        ray_t* cur_tbl = tbl;
        ray_retain(cur_tbl);
        for (int64_t r = 0; r < new_nrows; r++) {
            /* Build single-row list from multi-row columns */
            ray_t* single_row = ray_alloc(ncols * sizeof(ray_t*));
            if (!single_row) { ray_release(cur_tbl); ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
            single_row->type = RAY_LIST;
            single_row->len = ncols;
            ray_t** sr = (ray_t**)ray_data(single_row);
            for (int64_t c = 0; c < ncols; c++) {
                int alloc = 0;
                sr[c] = collection_elem(row_elems[c], r, &alloc);
                if (!alloc && sr[c]) ray_retain(sr[c]);
            }
            /* Upsert single row into current table */
            ray_t* upsert_args[3] = { cur_tbl, key_sym, single_row };
            ray_t* new_tbl = ray_upsert_fn(upsert_args, 3);
            /* Clean up single_row */
            for (int64_t c = 0; c < ncols; c++) if (sr[c]) ray_release(sr[c]);
            single_row->len = 0;
            ray_free(single_row);
            ray_release(cur_tbl);
            if (RAY_IS_ERR(new_tbl)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return new_tbl; }
            cur_tbl = new_tbl;
        }
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(cur_tbl)) {
            ray_env_set(inplace_sym, cur_tbl);
            ray_retain(cur_tbl);
        }
        return cur_tbl;
    }

    /* Type-check key columns before searching */
    for (int64_t k = 0; k < n_key_cols; k++) {
        int64_t kci = key_col_indices[k];
        ray_t* key_col = ray_table_get_col_idx(tbl, kci);
        ray_t* key_atom = row_elems[kci];
        int8_t kt = key_col->type;
        if (kt == RAY_STR && key_atom->type != -RAY_STR) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
        if (kt == RAY_SYM && key_atom->type != -RAY_SYM) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
    }

    /* Find the row to update by composite key match */
    int64_t match_row = -1;
    for (int64_t r = 0; r < nrows; r++) {
        int match = 1;
        for (int64_t k = 0; k < n_key_cols && match; k++) {
            int64_t kci = key_col_indices[k];
            ray_t* key_col = ray_table_get_col_idx(tbl, kci);
            ray_t* key_atom = row_elems[kci];
            int8_t kt = key_col->type;
            if (kt == RAY_F64) {
                double needle = (key_atom->type == -RAY_F64) ? key_atom->f64 : (double)key_atom->i64;
                if (((double*)ray_data(key_col))[r] != needle) match = 0;
            } else if (kt == RAY_SYM) {
                if (ray_read_sym(ray_data(key_col), r, key_col->type, key_col->attrs) != key_atom->i64) match = 0;
            } else if (kt == RAY_STR) {
                const char* ns = ray_str_ptr(key_atom);
                size_t nl = ray_str_len(key_atom);
                size_t rl = 0;
                const char* rs = ray_str_vec_get(key_col, r, &rl);
                if (rl != nl || (nl > 0 && (!rs || !ns || memcmp(rs, ns, nl) != 0))) match = 0;
            } else {
                int64_t needle = elem_as_i64(key_atom);
                int64_t existing = (kt == RAY_I64 || kt == RAY_TIMESTAMP) ?
                    ((int64_t*)ray_data(key_col))[r] :
                    (kt == RAY_I32 || kt == RAY_DATE || kt == RAY_TIME) ?
                    (int64_t)((int32_t*)ray_data(key_col))[r] :
                    (kt == RAY_BOOL) ? (int64_t)((uint8_t*)ray_data(key_col))[r] :
                    ((int64_t*)ray_data(key_col))[r];
                if (existing != needle) match = 0;
            }
        }
        if (match) { match_row = r; break; }
    }

    if (match_row < 0) {
        /* Key not found — insert: pass pre-evaluated args */
        ray_t* insert_args[2] = { tbl, row };
        ray_t* result = ray_insert_fn(insert_args, 2);
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
            ray_retain(result);
        }
        return result;
    }

    /* Key found — update that row */
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows);
        if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }

        /* If row_elems[c] is NULL (missing column), keep original values */
        int has_new_val = (row_elems[c] != NULL);

        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(orig_col, r, &slen);
                    new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                    new_col = ray_vec_append(new_col, &sym_val);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    new_col = ray_vec_append(new_col, src + r * elem_sz);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return result; }
    }

    ray_release(tbl);
    ray_release(key_sym);
    ray_release(row);

    if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
        ray_retain(result);
    }
    return result;
}

/* ══════════════════════════════════════════
 * Join operations
 * ══════════════════════════════════════════ */

/* Shared implementation for left-join (join_type=1) and inner-join (join_type=0).
 * (left-join t1 t2 [key ...]) / (inner-join t1 t2 [key ...]) */
static ray_t* join_impl(ray_t** args, int64_t n, uint8_t join_type) {
    if (n < 3) return ray_error("domain", NULL);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* keys      = args[2];

    /* Detect alternative calling convention: (join [keys] t1 t2) */
    if (left_tbl->type != RAY_TABLE && args[1]->type == RAY_TABLE && args[2]->type == RAY_TABLE) {
        keys      = args[0];
        left_tbl  = args[1];
        right_tbl = args[2];
    }

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    ray_t* _bxk = NULL;
    keys = unbox_vec_arg(keys, &_bxk);
    if (RAY_IS_ERR(keys)) return keys;
    if (!is_list(keys))
        { if (_bxk) ray_release(_bxk); return ray_error("type", NULL); }

    int64_t nk = ray_len(keys);
    if (nk == 0 || nk > 16) { if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_t** key_elems = (ray_t**)ray_data(keys);

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_op_t* lk[16], *rk[16];
    for (int64_t i = 0; i < nk; i++) {
        if (key_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* name_str = ray_sym_str(key_elems[i]->i64);
        if (!name_str) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        lk[i] = ray_scan(g, ray_str_ptr(name_str));
        rk[i] = ray_scan(g, ray_str_ptr(name_str));
        if (!lk[i] || !rk[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_join(g, left_node, lk, right_node, rk,
                           (uint8_t)nk, join_type);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

ray_t* ray_left_join_fn(ray_t** args, int64_t n)  { return join_impl(args, n, 1); }
ray_t* ray_inner_join_fn(ray_t** args, int64_t n) { return join_impl(args, n, 0); }

/* (antijoin left right [keys])
 * Anti-semi-join: keep rows from left that have NO match in right on keys. */
static ray_t* antijoin_impl(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* keys      = args[2];

    /* Detect alternative calling convention: (antijoin [keys] t1 t2) */
    if (left_tbl->type != RAY_TABLE && args[1]->type == RAY_TABLE && args[2]->type == RAY_TABLE) {
        keys      = args[0];
        left_tbl  = args[1];
        right_tbl = args[2];
    }

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    ray_t* _bxk = NULL;
    keys = unbox_vec_arg(keys, &_bxk);
    if (RAY_IS_ERR(keys)) return keys;
    if (!is_list(keys))
        { if (_bxk) ray_release(_bxk); return ray_error("type", NULL); }

    int64_t nk = ray_len(keys);
    if (nk == 0 || nk > 16) { if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_t** key_elems = (ray_t**)ray_data(keys);

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_op_t* lk[16], *rk[16];
    for (int64_t i = 0; i < nk; i++) {
        if (key_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* name_str = ray_sym_str(key_elems[i]->i64);
        if (!name_str) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        lk[i] = ray_scan(g, ray_str_ptr(name_str));
        rk[i] = ray_scan(g, ray_str_ptr(name_str));
        if (!lk[i] || !rk[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_antijoin(g, left_node, lk, right_node, rk, (uint8_t)nk);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

ray_t* ray_anti_join_fn(ray_t** args, int64_t n) { return antijoin_impl(args, n); }

/* (window-join t1 t2 [eq-keys] time-col)
 * ASOF join: for each left row, find closest right row with time <= left.time
 * within the same equality partition. */
ray_t* ray_window_join_fn(ray_t** args, int64_t n) {
    if (n < 4) return ray_error("domain", NULL);

    /* Special form: evaluate first 4 args, keep agg dict (args[4]) unevaluated */
    ray_t* eargs[5];
    for (int i = 0; i < 4 && i < (int)n; i++) {
        eargs[i] = ray_eval(args[i]);
        if (!eargs[i] || RAY_IS_ERR(eargs[i])) {
            for (int j = 0; j < i; j++) ray_release(eargs[j]);
            return eargs[i] ? eargs[i] : ray_error("type", NULL);
        }
    }
    eargs[4] = (n >= 5) ? args[4] : NULL; /* agg dict stays unevaluated */

    /* Detect rayforce calling convention:
     * (window-join [eq+time keys] intervals left right {agg})
     * vs teide convention:
     * (window-join left right [eq-keys] time-sym) */
    if (n >= 5 && ray_is_vec(eargs[0]) && eargs[0]->type == RAY_SYM &&
        eargs[2]->type == RAY_TABLE && eargs[3]->type == RAY_TABLE) {
        /* Rayforce convention: implement at eval level */
        ray_t* keys_vec = eargs[0];      /* [Sym Time] — equality + time keys */
        ray_t* intervals = eargs[1];     /* list of [lo hi] time windows */
        ray_t* left_tbl = eargs[2];      /* trades */
        ray_t* right_tbl = eargs[3];     /* quotes */
        ray_t* agg_dict = eargs[4];      /* unevaluated dict */

        int64_t nkeys = ray_len(keys_vec);
        if (nkeys < 2) return ray_error("domain", NULL);
        int64_t* key_ids = (int64_t*)ray_data(keys_vec);

        /* Last key is the time key, rest are equality keys */
        int64_t time_key = key_ids[nkeys - 1];
        int64_t n_eq = nkeys - 1;

        int64_t left_nrows = ray_table_nrows(left_tbl);
        int64_t right_nrows = ray_table_nrows(right_tbl);

        /* Get left time column */
        ray_t* left_time = ray_table_get_col(left_tbl, time_key);
        ray_t* right_time = ray_table_get_col(right_tbl, time_key);
        if (!left_time || !right_time) return ray_error("domain", NULL);

        /* Get equality columns */
        ray_t* left_eq[16], *right_eq[16];
        for (int64_t e = 0; e < n_eq && e < 16; e++) {
            left_eq[e] = ray_table_get_col(left_tbl, key_ids[e]);
            right_eq[e] = ray_table_get_col(right_tbl, key_ids[e]);
            if (!left_eq[e] || !right_eq[e]) return ray_error("domain", NULL);
        }

        /* Get aggregation info from dict */
        int64_t agg_result_name = -1;
        uint16_t agg_op = OP_MIN;
        int64_t agg_src_col = -1;
        if (agg_dict && agg_dict->type == RAY_LIST && (agg_dict->attrs & RAY_ATTR_DICT)) {
            ray_t** ad = (ray_t**)ray_data(agg_dict);
            int64_t adn = ray_len(agg_dict);
            if (adn >= 2) {
                agg_result_name = ad[0]->i64; /* minBid */
                ray_t* agg_expr = ad[1]; /* (min Bid) */
                if (agg_expr->type == RAY_LIST && agg_expr->len >= 2) {
                    ray_t** ae = (ray_t**)ray_data(agg_expr);
                    if (ae[0]->type == -RAY_SYM && (ae[0]->attrs & RAY_ATTR_NAME))
                        agg_op = resolve_agg_opcode(ae[0]->i64);
                    if (ae[1]->type == -RAY_SYM && (ae[1]->attrs & RAY_ATTR_NAME))
                        agg_src_col = ae[1]->i64;
                }
            }
        }

        ray_t* right_agg_col = (agg_src_col >= 0) ? ray_table_get_col(right_tbl, agg_src_col) : NULL;
        if (agg_src_col >= 0 && !right_agg_col) return ray_error("domain", NULL);

        /* For each left row, find matching right rows within the time window */
        /* intervals is a list of [lo, hi] pairs, one per left row */
        ray_t* result_agg = ray_vec_new(RAY_I64, left_nrows);
        if (RAY_IS_ERR(result_agg)) return result_agg;

        for (int64_t lr = 0; lr < left_nrows; lr++) {
            /* Get interval for this left row */
            int alloc_iv = 0;
            ray_t* iv = collection_elem(intervals, lr, &alloc_iv);
            if (!iv || RAY_IS_ERR(iv) || ray_len(iv) < 2) {
                if (alloc_iv && iv) ray_release(iv);
                ray_release(result_agg);
                return ray_error("domain", NULL);
            }
            int alloc_lo = 0, alloc_hi = 0;
            ray_t* lo_atom = collection_elem(iv, 0, &alloc_lo);
            ray_t* hi_atom = collection_elem(iv, 1, &alloc_hi);
            int64_t lo = as_i64(lo_atom);
            int64_t hi = as_i64(hi_atom);
            if (alloc_lo) ray_release(lo_atom);
            if (alloc_hi) ray_release(hi_atom);
            if (alloc_iv) ray_release(iv);

            /* Find right rows matching equality keys AND time in [lo, hi] */
            int64_t best_val_i = INT64_MAX;
            double best_val_f = 1e300;
            int found = 0;
            int is_f64 = (right_agg_col && right_agg_col->type == RAY_F64);

            for (int64_t rr = 0; rr < right_nrows; rr++) {
                /* Check equality keys */
                int eq_match = 1;
                for (int64_t e = 0; e < n_eq && eq_match; e++) {
                    int64_t lv = (left_eq[e]->type == RAY_SYM) ?
                        ((int64_t*)ray_data(left_eq[e]))[lr] :
                        ((int64_t*)ray_data(left_eq[e]))[lr];
                    int64_t rv = (right_eq[e]->type == RAY_SYM) ?
                        ((int64_t*)ray_data(right_eq[e]))[rr] :
                        ((int64_t*)ray_data(right_eq[e]))[rr];
                    if (lv != rv) eq_match = 0;
                }
                if (!eq_match) continue;

                /* Check time window — TIME is i32, TIMESTAMP is i64 */
                int64_t rt;
                if (right_time->type == RAY_TIME || right_time->type == RAY_I32 || right_time->type == RAY_DATE)
                    rt = (int64_t)((int32_t*)ray_data(right_time))[rr];
                else
                    rt = ((int64_t*)ray_data(right_time))[rr];
                if (rt < lo || rt > hi) continue;

                /* Apply aggregation */
                if (right_agg_col) {
                    if (is_f64) {
                        double v = ((double*)ray_data(right_agg_col))[rr];
                        if (!found || (agg_op == OP_MIN && v < best_val_f) ||
                            (agg_op == OP_MAX && v > best_val_f))
                            best_val_f = v;
                    } else {
                        int64_t v = ((int64_t*)ray_data(right_agg_col))[rr];
                        if (!found || (agg_op == OP_MIN && v < best_val_i) ||
                            (agg_op == OP_MAX && v > best_val_i))
                            best_val_i = v;
                    }
                    found = 1;
                }
            }

            /* Store result */
            if (found) {
                if (is_f64) {
                    double v = best_val_f;
                    result_agg = ray_vec_append(result_agg, &v);
                } else {
                    int64_t v = best_val_i;
                    result_agg = ray_vec_append(result_agg, &v);
                }
            } else {
                int64_t idx = result_agg->len;
                uint8_t zero[8] = {0};
                result_agg = ray_vec_append(result_agg, zero);
                if (!RAY_IS_ERR(result_agg))
                    ray_vec_set_null(result_agg, idx, true);
            }
            if (RAY_IS_ERR(result_agg)) return result_agg;
        }

        /* Build result table: left table + aggregation column */
        int64_t ncols = ray_table_ncols(left_tbl);
        ray_t* result = ray_table_new(ncols + 1);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(left_tbl, c);
            ray_t* cv = ray_table_get_col_idx(left_tbl, c);
            ray_retain(cv);
            result = ray_table_add_col(result, cn, cv);
            ray_release(cv);
        }
        if (agg_result_name >= 0) {
            result = ray_table_add_col(result, agg_result_name, result_agg);
        }
        ray_release(result_agg);
        for (int i = 0; i < 4; i++) ray_release(eargs[i]);
        return result;
    }

    ray_t* left_tbl  = eargs[0];
    ray_t* right_tbl = eargs[1];
    ray_t* eq_keys   = eargs[2];
    ray_t* time_sym  = eargs[3];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    if (time_sym->type != -RAY_SYM)
        return ray_error("type", NULL);

    uint8_t n_eq = 0;
    ray_t** eq_elems = NULL;
    ray_t* _bxeq = NULL;
    eq_keys = unbox_vec_arg(eq_keys, &_bxeq);
    if (is_list(eq_keys)) {
        n_eq = (uint8_t)ray_len(eq_keys);
        eq_elems = (ray_t**)ray_data(eq_keys);
    }

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) return ray_error("oom", NULL);

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); return ray_error("domain", NULL); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); return ray_error("domain", NULL); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g);
            return ray_error("type", NULL);
        }
        ray_t* nm = ray_sym_str(eq_elems[i]->i64);
        if (!nm) { ray_graph_free(g); return ray_error("domain", NULL); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); return ray_error("domain", NULL); }
    }

    if (_bxeq) ray_release(_bxeq);

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

/* (asof-join [key1 key2 ... timeKey] leftTable rightTable)
 * Last key is the time/asof column, rest are equality keys. */
ray_t* ray_asof_join_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);
    ray_t* keys_vec   = args[0];
    ray_t* left_tbl   = args[1];
    ray_t* right_tbl  = args[2];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);

    /* Keys vector must be a SYM vector with at least 2 elements (eq + time) */
    ray_t* _bxk = NULL;
    keys_vec = unbox_vec_arg(keys_vec, &_bxk);
    if (!is_list(keys_vec) || ray_len(keys_vec) < 2) {
        if (_bxk) ray_release(_bxk);
        return ray_error("domain", NULL);
    }
    ray_t** kelems = (ray_t**)ray_data(keys_vec);
    int64_t nkeys = ray_len(keys_vec);

    /* Last key is the time column */
    ray_t* time_sym = kelems[nkeys - 1];
    if (time_sym->type != -RAY_SYM) {
        if (_bxk) ray_release(_bxk);
        return ray_error("type", NULL);
    }

    /* Remaining keys are equality keys */
    uint8_t n_eq = (uint8_t)(nkeys - 1);
    ray_t** eq_syms = kelems; /* first n_eq elements */

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_syms[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* nm = ray_sym_str(eq_syms[i]->i64);
        if (!nm) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}
