/*
 * datalog.c — Datalog evaluation engine for Rayforce
 *
 * Compiles Datalog rules into ray_graph_t operation DAGs and evaluates
 * them to fixpoint using semi-naive evaluation with stratified negation.
 */
#include "ops/datalog.h"
#include "lang/eval_internal.h"
#include "lang/env.h"
#include "table/sym.h"
#include "ops/ops.h"
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Program lifecycle
 * ======================================================================== */

dl_program_t* dl_program_new(void) {
    /* Allocate via ray_alloc and use the data region for the program struct.
     * This avoids alignment issues since ray_alloc returns a ray_t* header. */
    ray_t* block = ray_alloc(sizeof(dl_program_t));
    if (!block) return NULL;
    dl_program_t* prog = (dl_program_t*)ray_data(block);
    memset(prog, 0, sizeof(dl_program_t));
    return prog;
}

/* Recover the ray_t header from a dl_program_t pointer for ray_free. */
static inline ray_t* dl_prog_block(dl_program_t* prog) {
    return (ray_t*)((char*)prog - 32);  /* ray_data is at offset 32 */
}

void dl_program_free(dl_program_t* prog) {
    if (!prog) return;
    for (int i = 0; i < prog->n_rels; i++) {
        if (prog->rels[i].table && !RAY_IS_ERR(prog->rels[i].table))
            ray_release(prog->rels[i].table);
        if (prog->rels[i].prov_col && !RAY_IS_ERR(prog->rels[i].prov_col))
            ray_release(prog->rels[i].prov_col);
    }
    ray_free(dl_prog_block(prog));
}

/* ========================================================================
 * Relation management
 * ======================================================================== */

int dl_find_rel(dl_program_t* prog, const char* name) {
    for (int i = 0; i < prog->n_rels; i++) {
        if (strcmp(prog->rels[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Generate a unique column name for a relation: "{relname}__c{idx}" */
static int64_t dl_col_sym(const char* rel_name, int col_idx) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%s__c%d", rel_name, col_idx);
    return ray_sym_intern(buf, strlen(buf));
}

int dl_add_edb(dl_program_t* prog, const char* name, ray_t* table, int arity) {
    if (!prog || !name || !table || prog->n_rels >= DL_MAX_RELS)
        return -1;

    int idx = prog->n_rels++;
    dl_rel_t* rel = &prog->rels[idx];
    memset(rel, 0, sizeof(dl_rel_t));

    size_t name_len = strlen(name);
    if (name_len >= sizeof(rel->name)) name_len = sizeof(rel->name) - 1;
    memcpy(rel->name, name, name_len);
    rel->name[name_len] = '\0';

    rel->arity = arity;
    rel->is_idb = false;

    /* Build a new table with relation-prefixed column names to avoid
     * collisions when multiple tables participate in a join. */
    for (int c = 0; c < arity && c < DL_MAX_ARITY; c++)
        rel->col_names[c] = dl_col_sym(name, c);

    ray_t* new_tbl = ray_table_new(arity);
    for (int c = 0; c < arity; c++) {
        ray_t* col = ray_table_get_col_idx(table, c);
        if (col)
            new_tbl = ray_table_add_col(new_tbl, rel->col_names[c], col);
    }
    rel->table = new_tbl;

    return idx;
}

int dl_ensure_idb(dl_program_t* prog, const char* name, int arity) {
    int idx = dl_find_rel(prog, name);
    if (idx >= 0) return idx;

    if (prog->n_rels >= DL_MAX_RELS) return -1;
    idx = prog->n_rels++;
    dl_rel_t* rel = &prog->rels[idx];
    memset(rel, 0, sizeof(dl_rel_t));

    size_t name_len = strlen(name);
    if (name_len >= sizeof(rel->name)) name_len = sizeof(rel->name) - 1;
    memcpy(rel->name, name, name_len);
    rel->name[name_len] = '\0';

    /* Create empty table with arity columns */
    rel->table = ray_table_new(arity);
    if (!rel->table || RAY_IS_ERR(rel->table)) return -1;

    rel->arity = arity;
    rel->is_idb = true;

    for (int c = 0; c < arity && c < DL_MAX_ARITY; c++) {
        rel->col_names[c] = dl_col_sym(name, c);
        ray_t* empty_col = ray_vec_new(RAY_I64, 0);
        if (empty_col && !RAY_IS_ERR(empty_col)) {
            rel->table = ray_table_add_col(rel->table, rel->col_names[c], empty_col);
            ray_release(empty_col);
        }
    }

    return idx;
}

/* ========================================================================
 * Rule management
 * ======================================================================== */

int dl_add_rule(dl_program_t* prog, const dl_rule_t* rule) {
    if (!prog || !rule || prog->n_rules >= DL_MAX_RULES)
        return -1;
    int idx = prog->n_rules++;
    memcpy(&prog->rules[idx], rule, sizeof(dl_rule_t));
    prog->rules[idx].stratum = -1;

    /* Ensure IDB relation exists for the head predicate */
    dl_ensure_idb(prog, rule->head_pred, rule->head_arity);

    return idx;
}

/* ========================================================================
 * Rule builder helpers
 * ======================================================================== */

void dl_rule_init(dl_rule_t* rule, const char* head_pred, int head_arity) {
    memset(rule, 0, sizeof(dl_rule_t));
    size_t len = strlen(head_pred);
    if (len >= sizeof(rule->head_pred)) len = sizeof(rule->head_pred) - 1;
    memcpy(rule->head_pred, head_pred, len);
    rule->head_pred[len] = '\0';
    rule->head_arity = head_arity;
    rule->n_body = 0;
    rule->n_vars = 0;
    rule->stratum = -1;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        rule->head_vars[i] = DL_CONST;
}

void dl_rule_head_var(dl_rule_t* rule, int pos, int var_idx) {
    if (pos < 0 || pos >= rule->head_arity) return;
    rule->head_vars[pos] = var_idx;
    if (var_idx + 1 > rule->n_vars) rule->n_vars = var_idx + 1;
}

void dl_rule_head_const(dl_rule_t* rule, int pos, int64_t val) {
    if (pos < 0 || pos >= rule->head_arity) return;
    rule->head_vars[pos] = DL_CONST;
    rule->head_consts[pos] = val;
}

int dl_rule_add_atom(dl_rule_t* rule, const char* pred, int arity) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_POS;
    size_t len = strlen(pred);
    if (len >= sizeof(b->pred)) len = sizeof(b->pred) - 1;
    memcpy(b->pred, pred, len);
    b->pred[len] = '\0';
    b->arity = arity;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        b->vars[i] = DL_CONST;
    return idx;
}

void dl_body_set_var(dl_rule_t* rule, int body_idx, int pos, int var_idx) {
    if (body_idx < 0 || body_idx >= rule->n_body) return;
    if (pos < 0 || pos >= rule->body[body_idx].arity) return;
    rule->body[body_idx].vars[pos] = var_idx;
    if (var_idx + 1 > rule->n_vars) rule->n_vars = var_idx + 1;
}

void dl_body_set_const(dl_rule_t* rule, int body_idx, int pos, int64_t val) {
    if (body_idx < 0 || body_idx >= rule->n_body) return;
    if (pos < 0 || pos >= rule->body[body_idx].arity) return;
    rule->body[body_idx].vars[pos] = DL_CONST;
    rule->body[body_idx].const_vals[pos] = val;
}

int dl_rule_add_neg(dl_rule_t* rule, const char* pred, int arity) {
    int idx = dl_rule_add_atom(rule, pred, arity);
    if (idx >= 0) rule->body[idx].type = DL_NEG;
    return idx;
}

int dl_rule_add_cmp(dl_rule_t* rule, int cmp_op, int lhs_var, int rhs_var) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs = lhs_var;
    b->cmp_rhs = rhs_var;
    if (lhs_var + 1 > rule->n_vars) rule->n_vars = lhs_var + 1;
    if (rhs_var + 1 > rule->n_vars) rule->n_vars = rhs_var + 1;
    return idx;
}

int dl_rule_add_cmp_const(dl_rule_t* rule, int cmp_op, int lhs_var, int64_t rhs_val) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs = lhs_var;
    b->cmp_rhs = DL_CONST;
    b->cmp_const = rhs_val;
    if (lhs_var + 1 > rule->n_vars) rule->n_vars = lhs_var + 1;
    return idx;
}

/* ========================================================================
 * Expression tree builders
 * ======================================================================== */

static dl_expr_t* dl_expr_alloc(void) {
    ray_t* block = ray_alloc(sizeof(dl_expr_t));
    if (!block) return NULL;
    dl_expr_t* e = (dl_expr_t*)ray_data(block);
    memset(e, 0, sizeof(dl_expr_t));
    return e;
}

dl_expr_t* dl_expr_const(int64_t val) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_CONST;
    e->const_val = val;
    return e;
}

dl_expr_t* dl_expr_var(int var_idx) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_VAR;
    e->var_idx = var_idx;
    return e;
}

dl_expr_t* dl_expr_binop(int op, dl_expr_t* left, dl_expr_t* right) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_BINOP;
    e->binop = op;
    e->left = left;
    e->right = right;
    return e;
}

/* ========================================================================
 * Assignment and builtin rule builders
 * ======================================================================== */

int dl_rule_add_assign(dl_rule_t* rule, int target_var, int op, dl_expr_t* expr) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_ASSIGN;
    b->assign_var = target_var;
    b->assign_expr = expr;
    if (target_var + 1 > rule->n_vars) rule->n_vars = target_var + 1;
    (void)op;  /* reserved for future assignment operators */
    return idx;
}

int dl_rule_add_builtin(dl_rule_t* rule, int builtin_id, int arity) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_BUILTIN;
    b->builtin_id = builtin_id;
    b->arity = arity;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        b->vars[i] = DL_CONST;
    return idx;
}

int dl_rule_add_cmp_expr(dl_rule_t* rule, int cmp_op, dl_expr_t* lhs, dl_expr_t* rhs) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs_expr = lhs;
    b->cmp_rhs_expr = rhs;
    /* n_vars may need updating from the expression trees.
     * Walk the trees to find max var_idx. */
    return idx;
}

int dl_rule_add_interval(dl_rule_t* rule, int fact_var, int start_var, int end_var) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_INTERVAL;
    b->interval_fact_var = fact_var;
    b->interval_start_var = start_var;
    b->interval_end_var = end_var;
    if (fact_var + 1 > rule->n_vars) rule->n_vars = fact_var + 1;
    if (start_var + 1 > rule->n_vars) rule->n_vars = start_var + 1;
    if (end_var + 1 > rule->n_vars) rule->n_vars = end_var + 1;
    return idx;
}

/* ========================================================================
 * Stratification — topological sort on negation dependency graph
 * ======================================================================== */

int dl_stratify(dl_program_t* prog) {
    if (!prog) return -1;

    /* Build dependency graph: for each IDB predicate, which other IDB
     * predicates does it depend on positively or negatively? */
    int n = prog->n_rels;
    /* dep[i][j]: 0 = no dep, 1 = positive dep, 2 = negative dep */
    int dep[DL_MAX_RELS][DL_MAX_RELS];
    memset(dep, 0, sizeof(dep));

    for (int r = 0; r < prog->n_rules; r++) {
        dl_rule_t* rule = &prog->rules[r];
        int head_idx = dl_find_rel(prog, rule->head_pred);
        if (head_idx < 0) continue;

        for (int b = 0; b < rule->n_body; b++) {
            dl_body_t* body = &rule->body[b];
            if (body->type != DL_POS && body->type != DL_NEG) continue;
            int body_idx = dl_find_rel(prog, body->pred);
            if (body_idx < 0) continue;
            if (body->type == DL_NEG)
                dep[head_idx][body_idx] = 2;  /* negative dep */
            else if (dep[head_idx][body_idx] == 0)
                dep[head_idx][body_idx] = 1;  /* positive dep (don't override neg) */
        }
    }

    /* Assign strata: predicates with no negative dependencies go to stratum 0.
     * A predicate with a negative dep on stratum S goes to stratum S+1.
     * Repeat until stable. If unstable after n iterations, there's a cycle. */
    int stratum[DL_MAX_RELS];
    memset(stratum, 0, sizeof(stratum));

    for (int iter = 0; iter < n + 1; iter++) {
        bool changed = false;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if (dep[i][j] == 2) {
                    /* Negative dependency: head must be in higher stratum */
                    if (stratum[i] <= stratum[j]) {
                        stratum[i] = stratum[j] + 1;
                        changed = true;
                    }
                } else if (dep[i][j] == 1) {
                    /* Positive dependency: head must be >= stratum */
                    if (stratum[i] < stratum[j]) {
                        stratum[i] = stratum[j];
                        changed = true;
                    }
                }
            }
        }
        if (!changed) break;
        if (iter == n) return -1;  /* unstratifiable negation cycle */
    }

    /* Build strata arrays */
    int max_stratum = 0;
    for (int i = 0; i < n; i++) {
        if (stratum[i] > max_stratum) max_stratum = stratum[i];
    }
    prog->n_strata = max_stratum + 1;
    memset(prog->strata_sizes, 0, sizeof(prog->strata_sizes));

    for (int i = 0; i < n; i++) {
        int s = stratum[i];
        if (s < DL_MAX_STRATA && prog->strata_sizes[s] < DL_MAX_RELS) {
            prog->strata[s][prog->strata_sizes[s]++] = i;
        }
    }

    /* Assign stratum to each rule */
    for (int r = 0; r < prog->n_rules; r++) {
        int head_idx = dl_find_rel(prog, prog->rules[r].head_pred);
        if (head_idx >= 0)
            prog->rules[r].stratum = stratum[head_idx];
    }

    return 0;
}

/* ========================================================================
 * Rule compiler — materializing approach
 *
 * Instead of building a single graph with joins, we execute each body
 * atom separately, producing intermediate tables, and join them C-level.
 * This avoids column-name-collision issues in the graph-level join.
 * ======================================================================== */

/* ========================================================================
 * Expression evaluation — compute column from expression tree
 * ======================================================================== */

/* Evaluate an expression tree against the accumulator table.
 * Returns a new owned I64 vector of length nrows. */
static ray_t* dl_eval_expr(dl_expr_t* expr, ray_t* accum,
                             int* var_col, int64_t nrows) {
    if (!expr) return NULL;

    switch (expr->kind) {
    case DL_EXPR_CONST: {
        ray_t* col = ray_vec_new(RAY_I64, nrows);
        if (!col || RAY_IS_ERR(col)) return NULL;
        col->len = nrows;
        int64_t* d = (int64_t*)ray_data(col);
        for (int64_t r = 0; r < nrows; r++)
            d[r] = expr->const_val;
        return col;
    }
    case DL_EXPR_VAR: {
        int ci = var_col[expr->var_idx];
        ray_t* src = ray_table_get_col_idx(accum, ci);
        if (!src) return NULL;
        ray_t* dst = ray_vec_new(RAY_I64, nrows);
        if (!dst || RAY_IS_ERR(dst)) return NULL;
        dst->len = nrows;
        memcpy(ray_data(dst), ray_data(src), (size_t)nrows * sizeof(int64_t));
        return dst;
    }
    case DL_EXPR_BINOP: {
        ray_t* lv = dl_eval_expr(expr->left, accum, var_col, nrows);
        ray_t* rv = dl_eval_expr(expr->right, accum, var_col, nrows);
        if (!lv || !rv) {
            if (lv) ray_release(lv);
            if (rv) ray_release(rv);
            return NULL;
        }
        ray_t* out = ray_vec_new(RAY_I64, nrows);
        if (!out || RAY_IS_ERR(out)) {
            ray_release(lv); ray_release(rv); return NULL;
        }
        out->len = nrows;
        int64_t* ld = (int64_t*)ray_data(lv);
        int64_t* rd = (int64_t*)ray_data(rv);
        int64_t* od = (int64_t*)ray_data(out);
        for (int64_t r = 0; r < nrows; r++) {
            switch (expr->binop) {
            case OP_ADD: od[r] = ld[r] + rd[r]; break;
            case OP_SUB: od[r] = ld[r] - rd[r]; break;
            case OP_MUL: od[r] = ld[r] * rd[r]; break;
            case OP_DIV: od[r] = rd[r] != 0 ? ld[r] / rd[r] : 0; break;
            default:     od[r] = 0; break;
            }
        }
        ray_release(lv);
        ray_release(rv);
        return out;
    }
    }
    return NULL;
}

/* Helper: append a new column to a table. Returns new owned table. */
static ray_t* dl_table_add_computed_col(ray_t* tbl, ray_t* new_col, const char* name) {
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* out = ray_table_new((int)(ncols + 1));
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, ray_table_col_name(tbl, c), col);
    }
    int64_t sym = ray_sym_intern(name, strlen(name));
    out = ray_table_add_col(out, sym, new_col);
    return out;
}

/* ========================================================================
 * Builtin predicate evaluation helpers
 * ======================================================================== */

/* before(S, E, T): keep rows where T < S */
static ray_t* dl_builtin_before(ray_t* tbl, int s_col, int t_col) {
    if (!tbl || RAY_IS_ERR(tbl) || ray_table_nrows(tbl) == 0) return tbl;

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);
    int64_t* sd = (int64_t*)ray_data(ray_table_get_col_idx(tbl, s_col));
    int64_t* t_data = (int64_t*)ray_data(ray_table_get_col_idx(tbl, t_col));

    int64_t count = 0;
    for (int64_t r = 0; r < nrows; r++)
        if (t_data[r] < sd[r]) count++;

    if (count == nrows) { ray_retain(tbl); return tbl; }

    ray_t* out = ray_table_new((int)ncols);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        if (!src) continue;
        ray_t* dst = ray_vec_new(src->type, count);
        if (!dst || RAY_IS_ERR(dst)) continue;
        dst->len = count;
        int64_t* src_d = (int64_t*)ray_data(src);
        int64_t* dst_d = (int64_t*)ray_data(dst);
        int64_t j = 0;
        for (int64_t r = 0; r < nrows; r++)
            if (t_data[r] < sd[r])
                dst_d[j++] = src_d[r];
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), dst);
        ray_release(dst);
    }
    return out;
}

/* duration_since(T1, T2, D): compute D = T2 - T1, append as new column */
static ray_t* dl_builtin_duration_since(ray_t* tbl, int t1_col, int t2_col,
                                          const char* out_name) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    int64_t* t1 = (int64_t*)ray_data(ray_table_get_col_idx(tbl, t1_col));
    int64_t* t2 = (int64_t*)ray_data(ray_table_get_col_idx(tbl, t2_col));

    ray_t* col = ray_vec_new(RAY_I64, nrows);
    if (!col || RAY_IS_ERR(col)) { ray_retain(tbl); return tbl; }
    col->len = nrows;
    int64_t* d = (int64_t*)ray_data(col);
    for (int64_t r = 0; r < nrows; r++)
        d[r] = t2[r] - t1[r];

    ray_t* out = dl_table_add_computed_col(tbl, col, out_name);
    ray_release(col);
    return out;
}

/* abs(X, Y): compute Y = |X|, append as new column */
static ray_t* dl_builtin_abs(ray_t* tbl, int x_col, const char* out_name) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    int64_t* xd = (int64_t*)ray_data(ray_table_get_col_idx(tbl, x_col));

    ray_t* col = ray_vec_new(RAY_I64, nrows);
    if (!col || RAY_IS_ERR(col)) { ray_retain(tbl); return tbl; }
    col->len = nrows;
    int64_t* d = (int64_t*)ray_data(col);
    for (int64_t r = 0; r < nrows; r++)
        d[r] = xd[r] < 0 ? -xd[r] : xd[r];

    ray_t* out = dl_table_add_computed_col(tbl, col, out_name);
    ray_release(col);
    return out;
}

/* Helper: join two tables on specified column pairs. Returns new owned table.
 * left_cols[k] and right_cols[k] are column indices in left/right tables. */
static ray_t* dl_join_tables(ray_t* left, ray_t* right,
                              const int* left_cols, const int* right_cols, int n_keys) {
    if (!left || RAY_IS_ERR(left) || !right || RAY_IS_ERR(right)) return NULL;
    if (ray_table_nrows(left) == 0 || ray_table_nrows(right) == 0) {
        /* Return empty table with left+right non-key columns */
        int64_t lnc = ray_table_ncols(left);
        int64_t rnc = ray_table_ncols(right);
        ray_t* empty = ray_table_new((int)(lnc + rnc));
        for (int64_t c = 0; c < lnc; c++) {
            ray_t* col = ray_table_get_col_idx(left, c);
            if (!col) continue;
            ray_t* ec = ray_vec_new(col->type, 0);
            if (ec && !RAY_IS_ERR(ec)) {
                empty = ray_table_add_col(empty, ray_table_col_name(left, c), ec);
                ray_release(ec);
            }
        }
        return empty;
    }

    /* Build unique column names for the join using a single graph */
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return NULL;

    /* Create copies with unique names */
    int64_t lnc = ray_table_ncols(left);
    int64_t rnc = ray_table_ncols(right);
    ray_t* ltbl = ray_table_new((int)lnc);
    for (int64_t c = 0; c < lnc; c++) {
        ray_t* col = ray_table_get_col_idx(left, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "L%d", (int)c);
        int64_t sym = ray_sym_intern(name, strlen(name));
        ltbl = ray_table_add_col(ltbl, sym, col);
    }
    ray_t* rtbl = ray_table_new((int)rnc);
    for (int64_t c = 0; c < rnc; c++) {
        ray_t* col = ray_table_get_col_idx(right, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "R%d", (int)c);
        int64_t sym = ray_sym_intern(name, strlen(name));
        rtbl = ray_table_add_col(rtbl, sym, col);
    }

    uint16_t l_tid = ray_graph_add_table(g, ltbl);
    uint16_t r_tid = ray_graph_add_table(g, rtbl);
    ray_op_t* l_op = ray_const_table(g, ltbl);
    ray_op_t* r_op = ray_const_table(g, rtbl);

    ray_op_t* lkeys[DL_MAX_ARITY];
    ray_op_t* rkeys[DL_MAX_ARITY];
    for (int k = 0; k < n_keys; k++) {
        char lname[32]; snprintf(lname, sizeof(lname), "L%d", left_cols[k]);
        char rname[32]; snprintf(rname, sizeof(rname), "R%d", right_cols[k]);
        lkeys[k] = ray_scan_table(g, l_tid, lname);
        rkeys[k] = ray_scan_table(g, r_tid, rname);
    }

    ray_op_t* join = ray_join(g, l_op, lkeys, r_op, rkeys, (uint8_t)n_keys, 0);
    ray_t* result = ray_execute(g, join);
    ray_graph_free(g);
    ray_release(ltbl);
    ray_release(rtbl);
    return result;
}

/* Helper: antijoin two tables on specified column pairs. Returns new owned table. */
static ray_t* dl_antijoin_tables(ray_t* left, ray_t* right,
                                  const int* left_cols, const int* right_cols, int n_keys) {
    if (!left || RAY_IS_ERR(left)) return left;
    if (!right || RAY_IS_ERR(right) || ray_table_nrows(right) == 0) {
        ray_retain(left); return left;
    }
    if (ray_table_nrows(left) == 0) { ray_retain(left); return left; }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) { ray_retain(left); return left; }

    int64_t lnc = ray_table_ncols(left);
    int64_t rnc = ray_table_ncols(right);
    ray_t* ltbl = ray_table_new((int)lnc);
    for (int64_t c = 0; c < lnc; c++) {
        ray_t* col = ray_table_get_col_idx(left, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "L%d", (int)c);
        ltbl = ray_table_add_col(ltbl, ray_sym_intern(name, strlen(name)), col);
    }
    ray_t* rtbl = ray_table_new((int)rnc);
    for (int64_t c = 0; c < rnc; c++) {
        ray_t* col = ray_table_get_col_idx(right, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "R%d", (int)c);
        rtbl = ray_table_add_col(rtbl, ray_sym_intern(name, strlen(name)), col);
    }

    uint16_t l_tid = ray_graph_add_table(g, ltbl);
    uint16_t r_tid = ray_graph_add_table(g, rtbl);
    ray_op_t* l_op = ray_const_table(g, ltbl);
    ray_op_t* r_op = ray_const_table(g, rtbl);

    ray_op_t* lkeys[DL_MAX_ARITY];
    ray_op_t* rkeys[DL_MAX_ARITY];
    for (int k = 0; k < n_keys; k++) {
        char lname[32]; snprintf(lname, sizeof(lname), "L%d", left_cols[k]);
        char rname[32]; snprintf(rname, sizeof(rname), "R%d", right_cols[k]);
        lkeys[k] = ray_scan_table(g, l_tid, lname);
        rkeys[k] = ray_scan_table(g, r_tid, rname);
    }

    ray_op_t* aj = ray_antijoin(g, l_op, lkeys, r_op, rkeys, (uint8_t)n_keys);
    ray_t* result = ray_execute(g, aj);
    ray_graph_free(g);
    ray_release(ltbl);
    ray_release(rtbl);
    return result;
}

/* Helper: filter a table to rows where column col_idx == value */
static ray_t* dl_filter_eq(ray_t* tbl, int col_idx, int64_t value) {
    if (!tbl || RAY_IS_ERR(tbl) || ray_table_nrows(tbl) == 0) return tbl;

    ray_t* col = ray_table_get_col_idx(tbl, col_idx);
    if (!col) return tbl;

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);
    int64_t* data = (int64_t*)ray_data(col);

    /* Count matching rows */
    int64_t count = 0;
    for (int64_t r = 0; r < nrows; r++)
        if (data[r] == value) count++;

    if (count == nrows) { ray_retain(tbl); return tbl; }

    /* Build filtered table */
    ray_t* out = ray_table_new((int)ncols);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        if (!src) continue;
        ray_t* dst = ray_vec_new(src->type, count);
        if (!dst || RAY_IS_ERR(dst)) continue;
        dst->len = count;
        int64_t* src_d = (int64_t*)ray_data(src);
        int64_t* dst_d = (int64_t*)ray_data(dst);
        int64_t j = 0;
        for (int64_t r = 0; r < nrows; r++) {
            if (data[r] == value)
                dst_d[j++] = src_d[r];
        }
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), dst);
        ray_release(dst);
    }
    return out;
}

/* Helper: project table to selected columns, producing output with head relation naming */
static ray_t* dl_project(ray_t* tbl, const int* col_indices, int n_out,
                          dl_rel_t* head_rel) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    ray_t* out = ray_table_new(n_out);
    for (int c = 0; c < n_out; c++) {
        int src_idx = col_indices[c];
        if (src_idx < 0) continue;  /* constant — handled separately */
        ray_t* src = ray_table_get_col_idx(tbl, src_idx);
        if (!src) continue;
        ray_t* dst = ray_vec_new(src->type, nrows);
        if (!dst || RAY_IS_ERR(dst)) continue;
        dst->len = nrows;
        memcpy(ray_data(dst), ray_data(src), (size_t)nrows * sizeof(int64_t));
        out = ray_table_add_col(out, head_rel->col_names[c], dst);
        ray_release(dst);
    }
    return out;
}

ray_op_t* dl_compile_rule(dl_program_t* prog, dl_rule_t* rule,
                          int delta_pos, int rule_idx, ray_graph_t* g) {
    /* Materializing approach: execute body atoms one at a time.
     *
     * For each positive body atom, we get the relation table and apply
     * constant filters. Then join with the accumulated result.
     * Variable bindings track which column in the accumulated table
     * holds each variable's value.
     *
     * var_col[v] = column index in `accum` table for variable v.
     */
    int var_col[DL_MAX_ARITY * DL_MAX_BODY];  /* column index in accum per variable */
    bool var_bound[DL_MAX_ARITY * DL_MAX_BODY];
    memset(var_bound, 0, sizeof(var_bound));
    memset(var_col, -1, sizeof(var_col));

    ray_t* accum = NULL;  /* accumulated result table */

    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type != DL_POS) continue;

        int rel_idx = dl_find_rel(prog, body->pred);
        if (rel_idx < 0) { if (accum) ray_release(accum); return NULL; }
        dl_rel_t* rel = &prog->rels[rel_idx];
        ray_t* body_tbl = rel->table;
        ray_retain(body_tbl);

        /* Apply constant filters */
        for (int c = 0; c < body->arity; c++) {
            if (body->vars[c] == DL_CONST) {
                ray_t* filtered = dl_filter_eq(body_tbl, c, body->const_vals[c]);
                ray_release(body_tbl);
                body_tbl = filtered;
            }
        }

        if (accum == NULL) {
            /* First body atom: accum = body_tbl */
            accum = body_tbl;
            /* Bind variables to column indices */
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (!var_bound[v]) {
                    var_bound[v] = true;
                    var_col[v] = c;
                }
            }
        } else {
            /* Join accum with body_tbl on shared variables */
            int lkeys[DL_MAX_ARITY], rkeys[DL_MAX_ARITY];
            int n_jk = 0;
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (var_bound[v]) {
                    lkeys[n_jk] = var_col[v];
                    rkeys[n_jk] = c;
                    n_jk++;
                }
            }

            ray_t* joined;
            if (n_jk > 0) {
                joined = dl_join_tables(accum, body_tbl, lkeys, rkeys, n_jk);
            } else {
                /* Cross product: use dummy key */
                int lk0 = 0, rk0 = 0;
                joined = dl_join_tables(accum, body_tbl, &lk0, &rk0, 0);
            }

            int64_t accum_ncols = ray_table_ncols(accum);
            ray_release(accum);
            ray_release(body_tbl);
            accum = joined;

            /* Bind new variables: their columns come after left columns in join output.
             * Join output = [all left cols] + [non-key right cols].
             * We need to track which right columns appear in output. */
            int right_col_map[DL_MAX_ARITY]; /* right col c -> output col idx */
            int out_idx = (int)accum_ncols;
            for (int c = 0; c < body->arity; c++) {
                bool is_key = false;
                for (int k = 0; k < n_jk; k++) {
                    if (rkeys[k] == c) { is_key = true; break; }
                }
                if (is_key) {
                    right_col_map[c] = -1;  /* key col not in output */
                } else {
                    right_col_map[c] = out_idx++;
                }
            }

            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (!var_bound[v]) {
                    var_bound[v] = true;
                    var_col[v] = right_col_map[c];
                }
            }
        }
    }

    if (!accum) return NULL;

    /* Process non-join body literals in declared order.
     * This ensures dependencies between literals (e.g., interval bind before
     * assignment, assignment before comparison) are respected. */
    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type == DL_POS) continue;  /* already processed above */
        if (!accum || RAY_IS_ERR(accum)) break;

        switch (body->type) {
        case DL_NEG: {
            int rel_idx = dl_find_rel(prog, body->pred);
            if (rel_idx < 0) { ray_release(accum); return NULL; }
            dl_rel_t* rel = &prog->rels[rel_idx];

            /* Apply constant filters to the negated relation first */
            ray_t* neg_tbl = rel->table;
            ray_retain(neg_tbl);
            for (int c = 0; c < body->arity; c++) {
                if (body->vars[c] == DL_CONST) {
                    ray_t* filtered = dl_filter_eq(neg_tbl, c, body->const_vals[c]);
                    ray_release(neg_tbl);
                    neg_tbl = filtered;
                }
            }

            int lkeys[DL_MAX_ARITY], rkeys[DL_MAX_ARITY];
            int n_keys = 0;
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (var_bound[v]) {
                    lkeys[n_keys] = var_col[v];
                    rkeys[n_keys] = c;
                    n_keys++;
                }
            }

            if (n_keys > 0) {
                ray_t* result = dl_antijoin_tables(accum, neg_tbl, lkeys, rkeys, n_keys);
                ray_release(accum);
                accum = result;
            }
            ray_release(neg_tbl);
            break;
        }

        case DL_ASSIGN: {
            int64_t nrows = ray_table_nrows(accum);
            ray_t* new_col = dl_eval_expr(body->assign_expr, accum, var_col, nrows);
            if (!new_col || RAY_IS_ERR(new_col)) break;

            int new_col_idx = (int)ray_table_ncols(accum);
            char colname[32];
            snprintf(colname, sizeof(colname), "_a%d", body->assign_var);
            ray_t* new_accum = dl_table_add_computed_col(accum, new_col, colname);
            ray_release(new_col);
            ray_release(accum);
            accum = new_accum;

            var_bound[body->assign_var] = true;
            var_col[body->assign_var] = new_col_idx;
            break;
        }

        case DL_BUILTIN: {
            switch (body->builtin_id) {
            case DL_BUILTIN_BEFORE: {
                int s_col = var_col[body->vars[0]];
                int t_col = var_col[body->vars[2]];
                ray_t* filtered = dl_builtin_before(accum, s_col, t_col);
                ray_release(accum);
                accum = filtered;
                break;
            }
            case DL_BUILTIN_DURATION_SINCE: {
                int t1_col = var_col[body->vars[0]];
                int t2_col = var_col[body->vars[1]];
                int d_var = body->vars[2];
                int new_idx = (int)ray_table_ncols(accum);
                char colname[32];
                snprintf(colname, sizeof(colname), "_d%d", d_var);
                ray_t* result = dl_builtin_duration_since(accum, t1_col, t2_col, colname);
                ray_release(accum);
                accum = result;
                var_bound[d_var] = true;
                var_col[d_var] = new_idx;
                break;
            }
            case DL_BUILTIN_ABS: {
                int x_col = var_col[body->vars[0]];
                int y_var = body->vars[1];
                int new_idx = (int)ray_table_ncols(accum);
                char colname[32];
                snprintf(colname, sizeof(colname), "_abs%d", y_var);
                ray_t* result = dl_builtin_abs(accum, x_col, colname);
                ray_release(accum);
                accum = result;
                var_bound[y_var] = true;
                var_col[y_var] = new_idx;
                break;
            }
            }
            break;
        }

        case DL_CMP: {
            int64_t nrows = ray_table_nrows(accum);
            if (nrows == 0) break;

            ray_t* lhs_evaled = NULL;
            ray_t* rhs_evaled = NULL;
            int64_t* lhs_data;
            int64_t* rhs_data;

            if (body->cmp_lhs_expr) {
                /* Expression-based LHS */
                lhs_evaled = dl_eval_expr(body->cmp_lhs_expr, accum, var_col, nrows);
                if (!lhs_evaled || RAY_IS_ERR(lhs_evaled)) break;
                lhs_data = (int64_t*)ray_data(lhs_evaled);
            } else {
                /* Simple variable LHS */
                int lhs_col = var_col[body->cmp_lhs];
                ray_t* lhs_vec = ray_table_get_col_idx(accum, lhs_col);
                if (!lhs_vec) break;
                lhs_data = (int64_t*)ray_data(lhs_vec);
            }

            if (body->cmp_rhs_expr) {
                /* Expression-based RHS */
                rhs_evaled = dl_eval_expr(body->cmp_rhs_expr, accum, var_col, nrows);
                if (!rhs_evaled || RAY_IS_ERR(rhs_evaled)) {
                    if (lhs_evaled) ray_release(lhs_evaled);
                    break;
                }
                rhs_data = (int64_t*)ray_data(rhs_evaled);
            } else if (body->cmp_rhs != DL_CONST) {
                /* Simple variable RHS */
                int rhs_col = var_col[body->cmp_rhs];
                ray_t* rhs_vec = ray_table_get_col_idx(accum, rhs_col);
                if (!rhs_vec) {
                    if (lhs_evaled) ray_release(lhs_evaled);
                    break;
                }
                rhs_data = (int64_t*)ray_data(rhs_vec);
            } else {
                rhs_data = NULL;  /* constant RHS */
            }

            /* Build boolean mask */
            ray_t* mask_block = ray_alloc((size_t)nrows * sizeof(bool));
            if (!mask_block) {
                if (lhs_evaled) ray_release(lhs_evaled);
                if (rhs_evaled) ray_release(rhs_evaled);
                break;
            }
            bool* mask = (bool*)ray_data(mask_block);
            int64_t count = 0;
            for (int64_t r = 0; r < nrows; r++) {
                int64_t rv = rhs_data ? rhs_data[r] : body->cmp_const;
                bool pass = false;
                switch (body->cmp_op) {
                case DL_CMP_EQ: pass = (lhs_data[r] == rv); break;
                case DL_CMP_NE: pass = (lhs_data[r] != rv); break;
                case DL_CMP_LT: pass = (lhs_data[r] <  rv); break;
                case DL_CMP_LE: pass = (lhs_data[r] <= rv); break;
                case DL_CMP_GT: pass = (lhs_data[r] >  rv); break;
                case DL_CMP_GE: pass = (lhs_data[r] >= rv); break;
                }
                mask[r] = pass;
                if (pass) count++;
            }

            if (lhs_evaled) ray_release(lhs_evaled);
            if (rhs_evaled) ray_release(rhs_evaled);

            if (count == nrows) {
                ray_free(mask_block);
                break;  /* all rows pass */
            }

            /* Build filtered table */
            int64_t ncols = ray_table_ncols(accum);
            ray_t* out = ray_table_new((int)ncols);
            for (int64_t c = 0; c < ncols; c++) {
                ray_t* src = ray_table_get_col_idx(accum, c);
                if (!src) continue;
                ray_t* dst = ray_vec_new(src->type, count);
                if (!dst || RAY_IS_ERR(dst)) continue;
                dst->len = count;
                int64_t* src_d = (int64_t*)ray_data(src);
                int64_t* dst_d = (int64_t*)ray_data(dst);
                int64_t j = 0;
                for (int64_t r = 0; r < nrows; r++)
                    if (mask[r]) dst_d[j++] = src_d[r];
                out = ray_table_add_col(out, ray_table_col_name(accum, c), dst);
                ray_release(dst);
            }
            ray_free(mask_block);
            ray_release(accum);
            accum = out;
            break;
        }

        case DL_INTERVAL: {
            int fact_col = var_col[body->interval_fact_var];
            int start_col = fact_col;
            int end_col = fact_col + 1;

            var_bound[body->interval_start_var] = true;
            var_col[body->interval_start_var] = start_col;

            var_bound[body->interval_end_var] = true;
            var_col[body->interval_end_var] = end_col;
            break;
        }
        } /* switch */
    }

    /* Project to head variables */
    int head_idx = dl_find_rel(prog, rule->head_pred);
    if (head_idx < 0) { ray_release(accum); return NULL; }
    dl_rel_t* head_rel = &prog->rels[head_idx];

    int proj_cols[DL_MAX_ARITY];
    for (int c = 0; c < rule->head_arity; c++) {
        int v = rule->head_vars[c];
        if (v == DL_CONST) {
            proj_cols[c] = -1;
        } else {
            proj_cols[c] = var_col[v];
        }
    }

    ray_t* projected = dl_project(accum, proj_cols, rule->head_arity, head_rel);
    ray_release(accum);

    /* Store result in the graph as a const_table so the caller can execute */
    ray_op_t* result_node = ray_const_table(g, projected);
    ray_release(projected);
    return result_node;
}

/* ========================================================================
 * Table utilities for fixpoint evaluation
 * ======================================================================== */

/* Rename table columns to match the head relation's expected names.
 * This is needed because ray_select output column names come from the scan
 * nodes (e.g., "edge__c0"), but we need them to match the head relation
 * (e.g., "path__c0"). Returns a new owned table. */
static ray_t* table_rename_cols(ray_t* tbl, dl_rel_t* target_rel) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols <= 0) { ray_retain(tbl); return tbl; }

    int arity = target_rel->arity;
    if (ncols != arity) { ray_retain(tbl); return tbl; }

    /* Check if renaming is needed */
    bool needs_rename = false;
    for (int c = 0; c < arity; c++) {
        if (ray_table_col_name(tbl, c) != target_rel->col_names[c]) {
            needs_rename = true;
            break;
        }
    }
    if (!needs_rename) { ray_retain(tbl); return tbl; }

    /* Build new table with correct column names sharing the same column data */
    ray_t* out = ray_table_new(arity);
    for (int c = 0; c < arity; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, target_rel->col_names[c], col);
    }
    return out;
}

/* Canonicalize column names to "c0","c1",... Returns new owned table. */
static ray_t* canonicalize(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        int64_t sym = ray_sym_intern(buf, strlen(buf));
        out = ray_table_add_col(out, sym, col);
    }
    return out;
}

/* Restore original column names from `src` onto `tbl`. */
static ray_t* restore_names(ray_t* tbl, ray_t* src) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, ray_table_col_name(src, c), col);
    }
    ray_release(tbl);
    return out;
}

/* Create a table by concatenating all rows from tables a and b (same schema).
 * Uses column-wise ray_vec_concat. Returns new owned table with a's names. */
static ray_t* table_union(ray_t* a, ray_t* b) {
    if (!a || RAY_IS_ERR(a)) return b;
    if (!b || RAY_IS_ERR(b)) return a;
    if (ray_table_nrows(a) == 0) { ray_retain(b); return b; }
    if (ray_table_nrows(b) == 0) { ray_retain(a); return a; }

    int64_t ncols_a = ray_table_ncols(a);
    int64_t ncols_b = ray_table_ncols(b);
    int64_t ncols = ncols_a < ncols_b ? ncols_a : ncols_b;

    ray_t* out = ray_table_new((int)ncols);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col_a = ray_table_get_col_idx(a, c);
        ray_t* col_b = ray_table_get_col_idx(b, c);
        if (!col_a || !col_b) continue;
        ray_t* merged = ray_vec_concat(col_a, col_b);
        if (merged && !RAY_IS_ERR(merged)) {
            out = ray_table_add_col(out, ray_table_col_name(a, c), merged);
            ray_release(merged);
        }
    }
    return out;
}

/* Deduplicate table rows on all columns. Returns new owned table. */
static ray_t* table_distinct(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    if (nrows <= 1) { ray_retain(tbl); return tbl; }

    int64_t ncols = ray_table_ncols(tbl);
    if (ncols <= 0) { ray_retain(tbl); return tbl; }

    ray_t* canonical = canonicalize(tbl);

    ray_graph_t* g = ray_graph_new(canonical);
    if (!g) { ray_release(canonical); ray_retain(tbl); return tbl; }

    ray_op_t* keys[DL_MAX_ARITY];
    for (int64_t c = 0; c < ncols && c < DL_MAX_ARITY; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        keys[c] = ray_scan(g, buf);
    }

    ray_op_t* dist = ray_distinct(g, keys, (uint8_t)ncols);
    ray_optimize(g, dist);
    ray_t* deduped = ray_execute(g, dist);
    ray_graph_free(g);
    ray_release(canonical);

    return restore_names(deduped, tbl);
}

/* Anti-join: rows in `left` that don't appear in `right` (same schema).
 * Returns new owned table with left's original column names. */
static ray_t* table_antijoin(ray_t* left, ray_t* right) {
    if (!left || RAY_IS_ERR(left)) return left;
    if (!right || RAY_IS_ERR(right) || ray_table_nrows(right) == 0) {
        ray_retain(left);
        return left;
    }
    if (ray_table_nrows(left) == 0) {
        ray_retain(left);
        return left;
    }

    int64_t ncols = ray_table_ncols(left);
    if (ncols <= 0) { ray_retain(left); return left; }

    ray_t* cl = canonicalize(left);
    ray_t* cr = canonicalize(right);

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) { ray_release(cl); ray_release(cr); ray_retain(left); return left; }

    ray_op_t* l = ray_const_table(g, cl);
    ray_op_t* r = ray_const_table(g, cr);

    uint16_t l_tid = ray_graph_add_table(g, cl);
    uint16_t r_tid = ray_graph_add_table(g, cr);

    ray_op_t* lkeys[DL_MAX_ARITY];
    ray_op_t* rkeys[DL_MAX_ARITY];
    for (int64_t c = 0; c < ncols && c < DL_MAX_ARITY; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        lkeys[c] = ray_scan_table(g, l_tid, buf);
        rkeys[c] = ray_scan_table(g, r_tid, buf);
    }

    ray_op_t* aj = ray_antijoin(g, l, lkeys, r, rkeys, (uint8_t)ncols);
    ray_t* raw = ray_execute(g, aj);
    ray_graph_free(g);
    ray_release(cl);
    ray_release(cr);

    return restore_names(raw, left);
}

/* Normalize column names of a table to match the target relation's naming scheme.
 * Returns a new owned table with correct names (shares column data).
 * Currently unused but retained for future use by external callers. */
static ray_t* normalize_columns(ray_t* tbl, dl_rel_t* rel)
    __attribute__((unused));
static ray_t* normalize_columns(ray_t* tbl, dl_rel_t* rel) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols != rel->arity) {
        /* Arity mismatch — can't normalize */
        ray_retain(tbl);
        return tbl;
    }
    /* Check if already correct */
    bool ok = true;
    for (int c = 0; c < rel->arity; c++) {
        if (ray_table_col_name(tbl, c) != rel->col_names[c]) { ok = false; break; }
    }
    if (ok) { ray_retain(tbl); return tbl; }

    /* Rebuild with correct names, sharing column data */
    ray_t* out = ray_table_new(rel->arity);
    for (int c = 0; c < rel->arity; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, rel->col_names[c], col);
    }
    return out;
}

/* ========================================================================
 * Provenance helpers
 * ======================================================================== */

/* Check if a row in `tbl` at position `row` matches any row in `ref`.
 * Both tables must have the same arity. Returns true if found. */
static bool dl_row_in_table(ray_t* tbl, int64_t row, ray_t* ref) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t ref_rows = ray_table_nrows(ref);
    for (int64_t r = 0; r < ref_rows; r++) {
        bool match = true;
        for (int64_t c = 0; c < ncols; c++) {
            int64_t* td = (int64_t*)ray_data(ray_table_get_col_idx(tbl, c));
            int64_t* rd = (int64_t*)ray_data(ray_table_get_col_idx(ref, c));
            if (td[row] != rd[r]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/* Build provenance for all IDB relations.
 * For each rule, compile with final tables and mark matching tuples. */
static void dl_build_provenance(dl_program_t* prog) {
    for (int ri = 0; ri < prog->n_rels; ri++) {
        dl_rel_t* rel = &prog->rels[ri];
        if (!rel->is_idb) continue;

        int64_t nrows = ray_table_nrows(rel->table);
        if (nrows == 0) continue;

        /* Allocate provenance column initialized to -1 (unknown) */
        ray_t* prov = ray_vec_new(RAY_I64, nrows);
        if (!prov || RAY_IS_ERR(prov)) continue;
        prov->len = nrows;
        int64_t* pd = (int64_t*)ray_data(prov);
        for (int64_t r = 0; r < nrows; r++)
            pd[r] = -1;

        /* For each rule with this head predicate */
        for (int r = 0; r < prog->n_rules; r++) {
            dl_rule_t* rule = &prog->rules[r];
            if (strcmp(rule->head_pred, rel->name) != 0) continue;

            /* Compile and execute the rule to get its derivable tuples */
            ray_graph_t* g = ray_graph_new(NULL);
            if (!g) continue;

            ray_op_t* output = dl_compile_rule(prog, rule, -1, r, g);
            if (!output) { ray_graph_free(g); continue; }

            ray_t* raw = ray_execute(g, output);
            ray_graph_free(g);
            if (!raw || RAY_IS_ERR(raw)) continue;

            ray_t* derived = table_rename_cols(raw, rel);
            ray_release(raw);
            if (!derived || RAY_IS_ERR(derived)) continue;

            /* Mark rows in rel->table that appear in derived */
            for (int64_t row = 0; row < nrows; row++) {
                if (pd[row] >= 0) continue;  /* already attributed */
                if (dl_row_in_table(rel->table, row, derived))
                    pd[row] = r;
            }
            ray_release(derived);
        }

        if (rel->prov_col) ray_release(rel->prov_col);
        rel->prov_col = prov;
    }
}

/* ========================================================================
 * Semi-naive fixpoint evaluation
 * ======================================================================== */

int dl_eval(dl_program_t* prog) {
    if (!prog) return -1;

    /* Stratify if not already done */
    if (prog->n_strata == 0) {
        if (dl_stratify(prog) != 0) return -1;
    }

    /* Process each stratum */
    for (int s = 0; s < prog->n_strata; s++) {
        /* Collect rules in this stratum */
        dl_rule_t* stratum_rules[DL_MAX_RULES];
        int stratum_rule_idx[DL_MAX_RULES];  /* original index in prog->rules */
        int n_stratum_rules = 0;

        for (int r = 0; r < prog->n_rules; r++) {
            if (prog->rules[r].stratum == s) {
                stratum_rule_idx[n_stratum_rules] = r;
                stratum_rules[n_stratum_rules++] = &prog->rules[r];
            }
        }
        if (n_stratum_rules == 0) continue;

        /* Phase A: Initial evaluation — evaluate each rule with full relations */
        /* Group rules by head predicate */
        for (int ri = 0; ri < n_stratum_rules; ri++) {
            dl_rule_t* rule = stratum_rules[ri];
            int head_idx = dl_find_rel(prog, rule->head_pred);
            if (head_idx < 0) continue;
            dl_rel_t* head_rel = &prog->rels[head_idx];

            ray_graph_t* g = ray_graph_new(NULL);
            if (!g) continue;

            ray_op_t* output = dl_compile_rule(prog, rule, -1, stratum_rule_idx[ri], g);
            if (!output) { ray_graph_free(g); continue; }

            ray_t* raw_tuples = ray_execute(g, output);
            ray_graph_free(g);

            if (!raw_tuples || RAY_IS_ERR(raw_tuples)) continue;

            /* Rename columns to match head relation's expected names */
            ray_t* new_tuples = table_rename_cols(raw_tuples, head_rel);
            ray_release(raw_tuples);
            if (!new_tuples || RAY_IS_ERR(new_tuples)) continue;

            /* Merge into the head relation's table */
            ray_t* merged = table_union(head_rel->table, new_tuples);
            ray_release(new_tuples);
            if (merged && !RAY_IS_ERR(merged)) {
                ray_t* deduped = table_distinct(merged);
                ray_release(merged);
                if (deduped && !RAY_IS_ERR(deduped)) {
                    ray_release(head_rel->table);
                    head_rel->table = deduped;
                }
            }
        }

        /* Phase B: Semi-naive loop — iterate with delta relations */
        /* For each IDB predicate in this stratum, compute delta as the
         * difference between current and previous table states. */
        ray_t* prev_tables[DL_MAX_RELS];
        ray_t* delta_tables[DL_MAX_RELS];
        memset(prev_tables, 0, sizeof(prev_tables));
        memset(delta_tables, 0, sizeof(delta_tables));

        /* Initially, delta = full table (all tuples are new) */
        for (int p = 0; p < prog->strata_sizes[s]; p++) {
            int rel_idx = prog->strata[s][p];
            dl_rel_t* rel = &prog->rels[rel_idx];
            if (rel->is_idb) {
                ray_retain(rel->table);
                delta_tables[rel_idx] = rel->table;
                /* prev = empty table with same schema as the relation */
                prev_tables[rel_idx] = ray_table_new(rel->arity);
                for (int c = 0; c < rel->arity && c < DL_MAX_ARITY; c++) {
                    ray_t* empty_col = ray_vec_new(RAY_I64, 0);
                    if (empty_col && !RAY_IS_ERR(empty_col)) {
                        prev_tables[rel_idx] = ray_table_add_col(
                            prev_tables[rel_idx], rel->col_names[c], empty_col);
                        ray_release(empty_col);
                    }
                }
            }
        }

        /* Semi-naive iteration */
        int max_iter = 1000;
        for (int iter = 0; iter < max_iter; iter++) {
            /* Check convergence: all deltas empty */
            bool any_new = false;
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                if (delta_tables[rel_idx] &&
                    !RAY_IS_ERR(delta_tables[rel_idx]) &&
                    ray_table_nrows(delta_tables[rel_idx]) > 0) {
                    any_new = true;
                    break;
                }
            }
            if (!any_new) break;

            /* For each rule, for each positive body position that uses a
             * delta relation, compile and execute */
            ray_t* new_tuples_per_rel[DL_MAX_RELS];
            memset(new_tuples_per_rel, 0, sizeof(new_tuples_per_rel));

            for (int ri = 0; ri < n_stratum_rules; ri++) {
                dl_rule_t* rule = stratum_rules[ri];
                int head_idx = dl_find_rel(prog, rule->head_pred);
                if (head_idx < 0) continue;

                for (int b = 0; b < rule->n_body; b++) {
                    dl_body_t* body = &rule->body[b];
                    if (body->type != DL_POS) continue;

                    int body_rel = dl_find_rel(prog, body->pred);
                    if (body_rel < 0) continue;
                    if (!prog->rels[body_rel].is_idb) continue;
                    if (!delta_tables[body_rel] ||
                        ray_table_nrows(delta_tables[body_rel]) == 0) continue;

                    /* Swap in delta relation for this body position */
                    ray_t* saved = prog->rels[body_rel].table;
                    prog->rels[body_rel].table = delta_tables[body_rel];

                    ray_graph_t* g = ray_graph_new(NULL);
                    if (!g) { prog->rels[body_rel].table = saved; continue; }

                    ray_op_t* output = dl_compile_rule(prog, rule, b, stratum_rule_idx[ri], g);
                    if (!output) {
                        ray_graph_free(g);
                        prog->rels[body_rel].table = saved;
                        continue;
                    }

                    ray_t* raw_result = ray_execute(g, output);
                    ray_graph_free(g);
                    prog->rels[body_rel].table = saved;

                    if (!raw_result || RAY_IS_ERR(raw_result)) continue;

                    /* Rename columns to match head relation */
                    dl_rel_t* head_rel2 = &prog->rels[head_idx];
                    ray_t* result = table_rename_cols(raw_result, head_rel2);
                    ray_release(raw_result);
                    if (!result || RAY_IS_ERR(result)) continue;

                    /* Accumulate new tuples for this head */
                    if (new_tuples_per_rel[head_idx]) {
                        ray_t* u = table_union(new_tuples_per_rel[head_idx], result);
                        ray_release(new_tuples_per_rel[head_idx]);
                        ray_release(result);
                        new_tuples_per_rel[head_idx] = u;
                    } else {
                        new_tuples_per_rel[head_idx] = result;
                    }
                }
            }

            /* For each IDB: dedup new tuples, subtract existing, merge */
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                dl_rel_t* rel = &prog->rels[rel_idx];
                if (!rel->is_idb) continue;

                /* Free old delta */
                if (delta_tables[rel_idx] && !RAY_IS_ERR(delta_tables[rel_idx]))
                    ray_release(delta_tables[rel_idx]);
                delta_tables[rel_idx] = NULL;

                ray_t* new_tuples = new_tuples_per_rel[rel_idx];
                if (!new_tuples || RAY_IS_ERR(new_tuples)) {
                    delta_tables[rel_idx] = NULL;
                    continue;
                }

                /* Deduplicate */
                ray_t* deduped = table_distinct(new_tuples);
                ray_release(new_tuples);
                if (!deduped || RAY_IS_ERR(deduped)) continue;

                /* Subtract existing relation to get true delta */
                ray_t* delta = table_antijoin(deduped, rel->table);
                ray_release(deduped);
                if (!delta || RAY_IS_ERR(delta)) continue;

                delta_tables[rel_idx] = delta;

                /* Merge delta into full relation */
                if (ray_table_nrows(delta) > 0) {
                    ray_t* merged = table_union(rel->table, delta);
                    if (merged && !RAY_IS_ERR(merged)) {
                        ray_release(rel->table);
                        rel->table = merged;
                    }
                }
            }

            /* Update prev tables */
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                if (prev_tables[rel_idx] && !RAY_IS_ERR(prev_tables[rel_idx]))
                    ray_release(prev_tables[rel_idx]);
                ray_retain(prog->rels[rel_idx].table);
                prev_tables[rel_idx] = prog->rels[rel_idx].table;
            }
        }

        /* Cleanup stratum temporaries */
        for (int p = 0; p < prog->strata_sizes[s]; p++) {
            int rel_idx = prog->strata[s][p];
            if (prev_tables[rel_idx] && !RAY_IS_ERR(prev_tables[rel_idx]))
                ray_release(prev_tables[rel_idx]);
            if (delta_tables[rel_idx] && !RAY_IS_ERR(delta_tables[rel_idx]))
                ray_release(delta_tables[rel_idx]);
        }
    }

    /* Build provenance if requested */
    if (prog->flags & DL_FLAG_PROVENANCE)
        dl_build_provenance(prog);

    return 0;
}

/* ========================================================================
 * Query — retrieve result after evaluation
 * ======================================================================== */

ray_t* dl_query(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].table;
}

ray_t* dl_get_provenance(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    if (!(prog->flags & DL_FLAG_PROVENANCE)) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].prov_col;
}

/* Stub: deep provenance source offsets (not yet implemented in C engine).
 * Returns NULL; Rust side treats this as "no deep provenance available". */
ray_t* dl_get_provenance_src_offsets(dl_program_t* prog, const char* pred_name) {
    (void)prog; (void)pred_name;
    return NULL;
}

/* Stub: deep provenance source data (not yet implemented in C engine).
 * Returns NULL; Rust side treats this as "no deep provenance available". */
ray_t* dl_get_provenance_src_data(dl_program_t* prog, const char* pred_name) {
    (void)prog; (void)pred_name;
    return NULL;
}

/* ── Builtins ── */

/* ══════════════════════════════════════════
 * EAV triple storage — datoms, assert-fact, scan-eav
 * ══════════════════════════════════════════ */

/* (datoms) — create empty EAV table with schema [e a v] */
ray_t* ray_datoms_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", "datoms takes no arguments");

    int64_t e_id = ray_sym_intern("e", 1);
    int64_t a_id = ray_sym_intern("a", 1);
    int64_t v_id = ray_sym_intern("v", 1);

    ray_t* tbl = ray_table_new(3);
    if (RAY_IS_ERR(tbl)) return tbl;

    /* e column: RAY_I64 */
    ray_t* e_col = ray_vec_new(RAY_I64, 0);
    if (RAY_IS_ERR(e_col)) { ray_release(tbl); return e_col; }
    tbl = ray_table_add_col(tbl, e_id, e_col);
    ray_release(e_col);
    if (RAY_IS_ERR(tbl)) return tbl;

    /* a column: RAY_SYM */
    ray_t* a_col = ray_vec_new(RAY_SYM, 0);
    if (RAY_IS_ERR(a_col)) { ray_release(tbl); return a_col; }
    tbl = ray_table_add_col(tbl, a_id, a_col);
    ray_release(a_col);
    if (RAY_IS_ERR(tbl)) return tbl;

    /* v column: RAY_I64 (symbols stored as intern ID, integers as-is) */
    ray_t* v_col = ray_vec_new(RAY_I64, 0);
    if (RAY_IS_ERR(v_col)) { ray_release(tbl); return v_col; }
    tbl = ray_table_add_col(tbl, v_id, v_col);
    ray_release(v_col);

    return tbl;
}

/* (assert-fact db entity attr value) — append a triple to the datoms table */
ray_t* ray_assert_fact_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("arity", "assert-fact expects 4 arguments: db entity attr value");

    ray_t* db     = args[0];
    ray_t* entity = args[1];
    ray_t* attr   = args[2];
    ray_t* value  = args[3];

    /* Validate db is a table with 3 columns */
    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "assert-fact: first arg must be a datoms table");

    /* Validate entity is i64 */
    if (entity->type != -RAY_I64)
        return ray_error("type", "assert-fact: entity must be an integer");

    /* Validate attr is a symbol */
    if (attr->type != -RAY_SYM)
        return ray_error("type", "assert-fact: attr must be a symbol");

    /* Value: accept i64 or sym. Store as i64 (sym -> intern ID). */
    int64_t v_val;
    if (value->type == -RAY_I64) {
        v_val = value->i64;
    } else if (value->type == -RAY_SYM) {
        v_val = value->i64;  /* sym intern ID is already i64 */
    } else {
        return ray_error("type", "assert-fact: value must be an integer or symbol");
    }

    /* Build new table with appended row */
    int64_t ncols = 3;
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* old_col = ray_table_get_col_idx(db, c);
        int64_t col_name = ray_table_col_name(db, c);

        /* Clone the column via retain + COW on append */
        ray_retain(old_col);
        ray_t* new_col = old_col;

        if (c == 0) {
            /* e column: append entity i64 */
            int64_t e_val = entity->i64;
            new_col = ray_vec_append(new_col, &e_val);
        } else if (c == 1) {
            /* a column: append attr sym ID */
            int64_t a_val = attr->i64;
            new_col = ray_vec_append(new_col, &a_val);
        } else {
            /* v column: append value as i64 */
            new_col = ray_vec_append(new_col, &v_val);
        }

        if (RAY_IS_ERR(new_col)) {
            /* ray_cow inside ray_vec_append already released old_col ref on error/copy */
            ray_release(result);
            return new_col;
        }
        /* ray_cow consumed our retain when it copied; don't double-release old_col */

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    return result;
}

/* (retract-fact db entity attr value) — remove a triple from the datoms table */
ray_t* ray_retract_fact_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("arity", "retract-fact expects 4 arguments: db entity attr value");

    ray_t* db     = args[0];
    ray_t* entity = args[1];
    ray_t* attr   = args[2];
    ray_t* value  = args[3];

    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "retract-fact: first arg must be a datoms table");
    if (entity->type != -RAY_I64)
        return ray_error("type", "retract-fact: entity must be an integer");
    if (attr->type != -RAY_SYM)
        return ray_error("type", "retract-fact: attr must be a symbol");

    int64_t match_e = entity->i64;
    int64_t match_a = attr->i64;
    int64_t match_v;
    if (value->type == -RAY_I64)
        match_v = value->i64;
    else if (value->type == -RAY_SYM)
        match_v = value->i64;
    else
        return ray_error("type", "retract-fact: value must be an integer or symbol");

    /* Get existing columns */
    ray_t* e_col = ray_table_get_col_idx(db, 0);
    ray_t* a_col = ray_table_get_col_idx(db, 1);
    ray_t* v_col = ray_table_get_col_idx(db, 2);
    int64_t nrows = ray_len(e_col);

    int64_t* e_data = (int64_t*)ray_data(e_col);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    int64_t* v_data = (int64_t*)ray_data(v_col);

    /* Build new columns, skipping matching rows */
    ray_t* new_e = ray_vec_new(RAY_I64, nrows);
    if (RAY_IS_ERR(new_e)) return new_e;
    ray_t* new_a = ray_vec_new(RAY_SYM, nrows);
    if (RAY_IS_ERR(new_a)) { ray_release(new_e); return new_a; }
    ray_t* new_v = ray_vec_new(RAY_I64, nrows);
    if (RAY_IS_ERR(new_v)) { ray_release(new_e); ray_release(new_a); return new_v; }

    for (int64_t r = 0; r < nrows; r++) {
        if (e_data[r] == match_e && a_data[r] == match_a && v_data[r] == match_v)
            continue; /* skip this row */
        new_e = ray_vec_append(new_e, &e_data[r]);
        if (RAY_IS_ERR(new_e)) { ray_release(new_a); ray_release(new_v); return new_e; }
        new_a = ray_vec_append(new_a, &a_data[r]);
        if (RAY_IS_ERR(new_a)) { ray_release(new_e); ray_release(new_v); return new_a; }
        new_v = ray_vec_append(new_v, &v_data[r]);
        if (RAY_IS_ERR(new_v)) { ray_release(new_e); ray_release(new_a); return new_v; }
    }

    /* Build result table */
    ray_t* result = ray_table_new(3);
    if (RAY_IS_ERR(result)) { ray_release(new_e); ray_release(new_a); ray_release(new_v); return result; }
    result = ray_table_add_col(result, ray_table_col_name(db, 0), new_e);
    ray_release(new_e);
    if (RAY_IS_ERR(result)) { ray_release(new_a); ray_release(new_v); return result; }
    result = ray_table_add_col(result, ray_table_col_name(db, 1), new_a);
    ray_release(new_a);
    if (RAY_IS_ERR(result)) { ray_release(new_v); return result; }
    result = ray_table_add_col(result, ray_table_col_name(db, 2), new_v);
    ray_release(new_v);
    return result;
}

/* (scan-eav db attr) — filter by attribute, return [e v] table
   (scan-eav db entity attr) — filter by entity+attr, return single value */
ray_t* ray_scan_eav_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3)
        return ray_error("arity", "scan-eav expects 2 or 3 arguments");

    ray_t* db = args[0];
    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "scan-eav: first arg must be a datoms table");

    ray_t* e_col = ray_table_get_col_idx(db, 0);
    ray_t* a_col = ray_table_get_col_idx(db, 1);
    ray_t* v_col = ray_table_get_col_idx(db, 2);
    int64_t nrows = ray_table_nrows(db);

    if (n == 2) {
        /* (scan-eav db attr) — filter by attribute, return [e v] table */
        ray_t* attr_arg = args[1];
        if (attr_arg->type != -RAY_SYM)
            return ray_error("type", "scan-eav: attr must be a symbol");
        int64_t attr_id = attr_arg->i64;

        int64_t e_name = ray_sym_intern("e", 1);
        int64_t v_name = ray_sym_intern("v", 1);

        ray_t* re = ray_vec_new(RAY_I64, nrows);
        if (RAY_IS_ERR(re)) return re;
        ray_t* rv = ray_vec_new(RAY_I64, nrows);
        if (RAY_IS_ERR(rv)) { ray_release(re); return rv; }

        const int64_t* e_data = (const int64_t*)ray_data(e_col);
        const int64_t* v_data = (const int64_t*)ray_data(v_col);

        for (int64_t r = 0; r < nrows; r++) {
            int64_t a_val = ray_read_sym(ray_data(a_col), r, a_col->type, a_col->attrs);
            if (a_val == attr_id) {
                re = ray_vec_append(re, &e_data[r]);
                if (RAY_IS_ERR(re)) { ray_release(rv); return re; }
                rv = ray_vec_append(rv, &v_data[r]);
                if (RAY_IS_ERR(rv)) { ray_release(re); return rv; }
            }
        }

        ray_t* result = ray_table_new(2);
        if (RAY_IS_ERR(result)) { ray_release(re); ray_release(rv); return result; }
        result = ray_table_add_col(result, e_name, re);
        ray_release(re);
        if (RAY_IS_ERR(result)) { ray_release(rv); return result; }
        result = ray_table_add_col(result, v_name, rv);
        ray_release(rv);
        return result;

    } else {
        /* (scan-eav db entity attr) — filter by entity+attr, return single value */
        ray_t* entity_arg = args[1];
        ray_t* attr_arg   = args[2];

        if (entity_arg->type != -RAY_I64)
            return ray_error("type", "scan-eav: entity must be an integer");
        if (attr_arg->type != -RAY_SYM)
            return ray_error("type", "scan-eav: attr must be a symbol");

        int64_t entity_id = entity_arg->i64;
        int64_t attr_id   = attr_arg->i64;

        const int64_t* e_data = (const int64_t*)ray_data(e_col);

        const int64_t* v_data = (const int64_t*)ray_data(v_col);

        for (int64_t r = 0; r < nrows; r++) {
            if (e_data[r] != entity_id) continue;
            int64_t a_val = ray_read_sym(ray_data(a_col), r, a_col->type, a_col->attrs);
            if (a_val == attr_id) {
                return ray_i64(v_data[r]);
            }
        }

        return ray_error("value", "scan-eav: no matching triple found");
    }
}

/* (pull db entity) — all attributes of entity as dict
   (pull db entity [attrs]) — only specified attributes as dict */
ray_t* ray_pull_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3)
        return ray_error("arity", "pull expects 2 or 3 arguments: db entity [attrs]");

    ray_t* db     = args[0];
    ray_t* entity = args[1];

    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "pull: first arg must be a datoms table");
    if (entity->type != -RAY_I64)
        return ray_error("type", "pull: entity must be an integer");

    /* Optional attribute filter */
    ray_t* attr_filter = NULL;
    int64_t n_filter = 0;
    const int64_t* filter_ids = NULL;
    if (n == 3) {
        attr_filter = args[2];
        if (!ray_is_vec(attr_filter) || attr_filter->type != RAY_SYM)
            return ray_error("type", "pull: third arg must be a symbol vector [attr ...]");
        n_filter = attr_filter->len;
        filter_ids = (const int64_t*)ray_data(attr_filter);
    }

    int64_t entity_id = entity->i64;
    ray_t* e_col = ray_table_get_col_idx(db, 0);
    ray_t* a_col = ray_table_get_col_idx(db, 1);
    ray_t* v_col = ray_table_get_col_idx(db, 2);
    int64_t nrows = ray_table_nrows(db);

    const int64_t* e_data = (const int64_t*)ray_data(e_col);
    const int64_t* v_data = (const int64_t*)ray_data(v_col);

    /* Build dict: alternating key (sym atom) / value (i64 atom) */
    ray_t* dict = ray_list_new(0);
    if (RAY_IS_ERR(dict)) return dict;
    dict->attrs |= RAY_ATTR_DICT;

    for (int64_t r = 0; r < nrows; r++) {
        if (e_data[r] != entity_id) continue;
        int64_t a_val = ray_read_sym(ray_data(a_col), r, a_col->type, a_col->attrs);

        /* Check filter if present */
        if (attr_filter) {
            int found = 0;
            for (int64_t f = 0; f < n_filter; f++) {
                if (filter_ids[f] == a_val) { found = 1; break; }
            }
            if (!found) continue;
        }

        ray_t* key = ray_sym(a_val);
        if (RAY_IS_ERR(key)) { ray_release(dict); return key; }
        dict = ray_list_append(dict, key);
        ray_release(key);
        if (RAY_IS_ERR(dict)) return dict;

        ray_t* val = ray_i64(v_data[r]);
        if (RAY_IS_ERR(val)) { ray_release(dict); return val; }
        dict = ray_list_append(dict, val);
        ray_release(val);
        if (RAY_IS_ERR(dict)) return dict;
    }

    return dict;
}

/* ══════════════════════════════════════════
 * Datalog — rule definitions and query compilation
 * ══════════════════════════════════════════ */

/* Check if a symbol name starts with '?' (Datalog variable) */
static int is_dl_var(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    if (!s) return 0;
    const char* p = ray_str_ptr(s);
    return p && p[0] == '?';
}

/* ══════════════════════════════════════════
 * Datalog wrappers — thin layer over src/datalog/datalog.h
 *
 * Global rule storage lives in g_dl_rules[] / g_dl_n_rules.
 * ray_rule_fn parses Rayfall (rule ...) syntax and stores rules.
 * ray_query_fn builds a temporary dl_program_t, copies global rules,
 * registers the EAV table, evaluates to fixpoint, and returns results.
 * ══════════════════════════════════════════ */

/* Global rule storage: rules defined via (rule ...) persist across queries */
static dl_rule_t  g_dl_rules[DL_MAX_RULES];
static int        g_dl_n_rules = 0;

/* Variable name -> index map for parsing a single rule or query body */
typedef struct {
    int64_t syms[DL_MAX_ARITY * DL_MAX_BODY];
    int     n;
} dl_var_map_t;

static int dl_var_get_or_create(dl_var_map_t* map, int64_t sym_id) {
    for (int i = 0; i < map->n; i++)
        if (map->syms[i] == sym_id) return i;
    if (map->n >= DL_MAX_ARITY * DL_MAX_BODY) return -1;
    map->syms[map->n] = sym_id;
    return map->n++;
}

/* Map Rayfall comparison operator name to DL_CMP_* constant.
 * Returns -1 if not a recognized comparison. */
static int dl_cmp_op_from_name(const char* name) {
    if (strcmp(name, ">")  == 0) return DL_CMP_GT;
    if (strcmp(name, ">=") == 0) return DL_CMP_GE;
    if (strcmp(name, "<")  == 0) return DL_CMP_LT;
    if (strcmp(name, "<=") == 0) return DL_CMP_LE;
    if (strcmp(name, "==") == 0) return DL_CMP_EQ;
    if (strcmp(name, "!=") == 0) return DL_CMP_NE;
    return -1;
}

/* Map Rayfall arithmetic operator name to OP_* constant for dl_expr_t.
 * Returns -1 if not recognized. */
static int dl_arith_op_from_name(const char* name) {
    if (strcmp(name, "+") == 0) return OP_ADD;
    if (strcmp(name, "-") == 0) return OP_SUB;
    if (strcmp(name, "*") == 0) return OP_MUL;
    if (strcmp(name, "/") == 0) return OP_DIV;
    return -1;
}

/* Build a dl_expr_t from a Rayfall AST node.
 * Handles: integer constants, ?variables, (op expr expr). */
static dl_expr_t* dl_build_expr(ray_t* node, dl_var_map_t* vars) {
    if (!node) return NULL;
    if (node->type == -RAY_I64)
        return dl_expr_const(node->i64);
    if (node->type == -RAY_SYM && is_dl_var(node)) {
        int vi = dl_var_get_or_create(vars, node->i64);
        return (vi >= 0) ? dl_expr_var(vi) : NULL;
    }
    if (is_list(node) && ray_len(node) == 3) {
        ray_t** elems = (ray_t**)ray_data(node);
        if (elems[0]->type == -RAY_SYM) {
            ray_t* op_str = ray_sym_str(elems[0]->i64);
            if (op_str) {
                int op = dl_arith_op_from_name(ray_str_ptr(op_str));
                if (op >= 0) {
                    dl_expr_t* l = dl_build_expr(elems[1], vars);
                    dl_expr_t* r = dl_build_expr(elems[2], vars);
                    if (l && r) return dl_expr_binop(op, l, r);
                }
            }
        }
    }
    /* Fallback: treat symbols (non-variable) as constants (sym ID) */
    if (node->type == -RAY_SYM)
        return dl_expr_const(node->i64);
    return NULL;
}

/* Check if a Rayfall list clause is a triple pattern: (?e :attr ?v)
 * A triple pattern has exactly 3 elements and the first element is a
 * ?variable (distinguishing it from rule invocations where the first
 * element is a predicate name symbol). */
static bool dl_is_wildcard(ray_t* node) {
    if (node->type != -RAY_SYM) return false;
    ray_t* s = ray_sym_str(node->i64);
    return s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == '_';
}



static bool dl_is_triple_pattern(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) != 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    /* Position 0 must be a ?variable, wildcard _, integer constant,
     * or quoted symbol (not a bare name that could be a rule predicate).
     * Triple patterns: (?e :attr ?v), (_ :attr ?v), (1 :attr ?v) */
    if (is_dl_var(ce[0])) return true;
    if (ce[0]->type == -RAY_I64) return true;
    if (dl_is_wildcard(ce[0]) && ce[1]->type == -RAY_SYM && !is_dl_var(ce[1]))
        return true;  /* _ is always wildcard -- reserved, never a predicate */
    /* Quoted symbol (no RAY_ATTR_NAME) in position 0 + non-var symbol in position 1 */
    if (ce[0]->type == -RAY_SYM && !(ce[0]->attrs & RAY_ATTR_NAME)) {
        if (ce[1]->type == -RAY_SYM && !is_dl_var(ce[1]))
            return true;
    }
    return false;
}

/* Check if a clause is a negation: (not (...)) */
static bool dl_is_negation(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) != 2) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    return name && strcmp(ray_str_ptr(name), "not") == 0;
}

/* Check if a clause is a comparison: (> ?x ?y) or (> ?x 100) */
static bool dl_is_comparison(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) < 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    if (!name) return false;
    return dl_cmp_op_from_name(ray_str_ptr(name)) >= 0;
}

/* Check if a clause is an assignment: (= ?var expr) */
static bool dl_is_assignment(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) != 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    if (!name || strcmp(ray_str_ptr(name), "=") != 0) return false;
    /* LHS must be a variable */
    return is_dl_var(ce[1]);
}

/* Resolve an AST node to a variable or constant in a body atom.
 * Sets the body position to either a variable or constant.
 * For expressions like (quote x), evaluates them first. */
static ray_t* dl_set_body_pos(dl_rule_t* rule, int bidx, int pos,
                                ray_t* node, dl_var_map_t* vars) {
    if (is_dl_var(node)) {
        int vi = dl_var_get_or_create(vars, node->i64);
        dl_body_set_var(rule, bidx, pos, vi);
        return NULL;
    }
    if (node->type == -RAY_I64) {
        dl_body_set_const(rule, bidx, pos, node->i64);
        return NULL;
    }
    if (node->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(node->i64);
        if (s && strcmp(ray_str_ptr(s), "_") == 0) {
            /* Wildcard: create a fresh variable */
            int vi = vars->n++;
            vars->syms[vi] = -1 - vi;
            dl_body_set_var(rule, bidx, pos, vi);
        } else {
            dl_body_set_const(rule, bidx, pos, node->i64);
        }
        return NULL;
    }
    /* For other forms (e.g., (quote x)), evaluate to get constant */
    ray_t* val = ray_eval(node);
    if (!val || RAY_IS_ERR(val))
        return val ? val : ray_error("type", "rule: cannot evaluate constant in body");
    if (val->type == -RAY_I64) {
        dl_body_set_const(rule, bidx, pos, val->i64);
    } else if (val->type == -RAY_SYM) {
        dl_body_set_const(rule, bidx, pos, val->i64);
    } else {
        ray_release(val);
        return ray_error("type", "rule: unsupported constant type in body");
    }
    ray_release(val);
    return NULL;
}

/* Parse a single body clause and add it to the dl_rule_t.
 * Handles triple patterns, negations, comparisons, assignments,
 * and rule invocations (positive atoms). */
static ray_t* dl_parse_body_clause(dl_rule_t* rule, ray_t* clause,
                                     dl_var_map_t* vars) {
    if (!is_list(clause) || ray_len(clause) < 1)
        return ray_error("type", "rule/query: body clause must be a list");

    ray_t** ce = (ray_t**)ray_data(clause);
    int64_t clen = ray_len(clause);

    /* -- Triple pattern: (?e :attr ?v) -- */
    if (dl_is_triple_pattern(clause)) {
        /* Register as 3-arity atom on "eav" relation:
         * position 0 = entity, 1 = attr (constant), 2 = value */
        int bidx = dl_rule_add_atom(rule, "eav", 3);
        if (bidx < 0) return ray_error("domain", "rule: too many body literals");

        ray_t* err;
        err = dl_set_body_pos(rule, bidx, 0, ce[0], vars);
        if (err) return err;
        err = dl_set_body_pos(rule, bidx, 1, ce[1], vars);
        if (err) return err;
        err = dl_set_body_pos(rule, bidx, 2, ce[2], vars);
        if (err) return err;
        return NULL; /* success */
    }

    /* -- Negation: (not (?e :attr ?v))  or  (not (rule-name ?args...)) -- */
    if (dl_is_negation(clause)) {
        ray_t* inner = ce[1];
        if (!is_list(inner) || ray_len(inner) < 1)
            return ray_error("type", "not: inner clause must be a list");
        ray_t** ie = (ray_t**)ray_data(inner);
        int64_t ilen = ray_len(inner);

        if (dl_is_triple_pattern(inner)) {
            /* Negated triple: (not (?e :attr ?v)) */
            int bidx = dl_rule_add_neg(rule, "eav", 3);
            if (bidx < 0) return ray_error("domain", "rule: too many body literals");

            ray_t* err;
            err = dl_set_body_pos(rule, bidx, 0, ie[0], vars);
            if (err) return err;
            err = dl_set_body_pos(rule, bidx, 1, ie[1], vars);
            if (err) return err;
            err = dl_set_body_pos(rule, bidx, 2, ie[2], vars);
            if (err) return err;
        } else {
            /* Negated rule invocation: (not (rule-name ?a ?b)) */
            if (ie[0]->type != -RAY_SYM)
                return ray_error("type", "not: inner clause head must be a symbol");
            ray_t* pred_name = ray_sym_str(ie[0]->i64);
            if (!pred_name)
                return ray_error("type", "not: cannot resolve predicate name");

            int bidx = dl_rule_add_neg(rule, ray_str_ptr(pred_name), (int)(ilen - 1));
            if (bidx < 0) return ray_error("domain", "rule: too many body literals");

            for (int64_t j = 1; j < ilen; j++) {
                ray_t* err = dl_set_body_pos(rule, bidx, (int)(j - 1), ie[j], vars);
                if (err) return err;
            }
        }
        return NULL;
    }

    /* -- Assignment: (= ?var expr) -- */
    if (dl_is_assignment(clause)) {
        int target_vi = dl_var_get_or_create(vars, ce[1]->i64);
        dl_expr_t* expr = dl_build_expr(ce[2], vars);
        if (!expr)
            return ray_error("type", "rule: cannot parse assignment expression");
        dl_rule_add_assign(rule, target_vi, DL_OP_EQ, expr);
        return NULL;
    }

    /* -- Comparison: (> ?x ?y) or (> ?x 100) -- */
    if (dl_is_comparison(clause)) {
        ray_t* op_str = ray_sym_str(ce[0]->i64);
        int cmp_op = dl_cmp_op_from_name(ray_str_ptr(op_str));

        /* LHS */
        bool lhs_is_var = is_dl_var(ce[1]);
        int lhs_vi = lhs_is_var ? dl_var_get_or_create(vars, ce[1]->i64) : -1;
        bool lhs_is_const = (!lhs_is_var && (ce[1]->type == -RAY_I64 || ce[1]->type == -RAY_SYM));
        int64_t lhs_const = lhs_is_const ? ce[1]->i64 : 0;

        /* RHS */
        bool rhs_is_var = (clen > 2) && is_dl_var(ce[2]);
        int rhs_vi = rhs_is_var ? dl_var_get_or_create(vars, ce[2]->i64) : -1;
        bool rhs_is_const = (clen > 2) && !rhs_is_var &&
                            (ce[2]->type == -RAY_I64 || ce[2]->type == -RAY_SYM);
        int64_t rhs_const = rhs_is_const ? ce[2]->i64 : 0;

        if (lhs_is_var && rhs_is_var) {
            dl_rule_add_cmp(rule, cmp_op, lhs_vi, rhs_vi);
        } else if (lhs_is_var && rhs_is_const) {
            dl_rule_add_cmp_const(rule, cmp_op, lhs_vi, rhs_const);
        } else if (lhs_is_const && rhs_is_var) {
            /* Flip: const op var -> var flipped_op const */
            int flipped = cmp_op;
            switch (cmp_op) {
            case DL_CMP_GT: flipped = DL_CMP_LT; break;
            case DL_CMP_GE: flipped = DL_CMP_LE; break;
            case DL_CMP_LT: flipped = DL_CMP_GT; break;
            case DL_CMP_LE: flipped = DL_CMP_GE; break;
            default: break;
            }
            dl_rule_add_cmp_const(rule, flipped, rhs_vi, lhs_const);
        } else {
            /* Expression-based comparison */
            dl_expr_t* le = dl_build_expr(ce[1], vars);
            dl_expr_t* re = (clen > 2) ? dl_build_expr(ce[2], vars) : NULL;
            if (le && re)
                dl_rule_add_cmp_expr(rule, cmp_op, le, re);
            else
                return ray_error("type", "rule: cannot parse comparison operands");
        }
        return NULL;
    }

    /* -- Rule invocation / positive atom: (pred-name ?a ?b ...) -- */
    if (ce[0]->type == -RAY_SYM) {
        ray_t* pred_name = ray_sym_str(ce[0]->i64);
        if (!pred_name)
            return ray_error("type", "rule: cannot resolve predicate name");

        int bidx = dl_rule_add_atom(rule, ray_str_ptr(pred_name), (int)(clen - 1));
        if (bidx < 0) return ray_error("domain", "rule: too many body literals");

        for (int64_t j = 1; j < clen; j++) {
            ray_t* err = dl_set_body_pos(rule, bidx, (int)(j - 1), ce[j], vars);
            if (err) return err;
        }
        return NULL;
    }

    return ray_error("type", "rule/query: unrecognized body clause form");
}

/* (rule (head-name ?v1 ?v2 ...) clause1 clause2 ...)
 * Special form: args are NOT evaluated.
 * Parses the head and body into a dl_rule_t and stores it globally. */
ray_t* ray_rule_fn(ray_t** args, int64_t n) {
    if (n < 2)
        return ray_error("arity", "rule expects at least a head and one body clause");

    /* First arg: head -- must be a list (head-name ?v1 ?v2 ...) */
    ray_t* head = args[0];
    if (!is_list(head) || ray_len(head) < 1)
        return ray_error("type", "rule: head must be (name ?var ...)");

    ray_t** hd = (ray_t**)ray_data(head);
    int64_t hlen = ray_len(head);

    /* Head name */
    if (hd[0]->type != -RAY_SYM)
        return ray_error("type", "rule: head name must be a symbol");

    ray_t* head_name_str = ray_sym_str(hd[0]->i64);
    if (!head_name_str)
        return ray_error("type", "rule: cannot resolve head name");

    /* _ is reserved as wildcard -- cannot be a rule predicate name */
    if (ray_str_len(head_name_str) == 1 && ray_str_ptr(head_name_str)[0] == '_')
        return ray_error("domain", "rule: _ is reserved as wildcard");

    if (g_dl_n_rules >= DL_MAX_RULES)
        return ray_error("domain", "rule: too many rules (max 128)");

    /* Build variable map */
    dl_var_map_t vars;
    memset(&vars, 0, sizeof(vars));

    int head_arity = (int)(hlen - 1);
    dl_rule_t rule;
    dl_rule_init(&rule, ray_str_ptr(head_name_str), head_arity);

    /* Head variables */
    for (int i = 0; i < head_arity; i++) {
        ray_t* harg = hd[i + 1];
        if (is_dl_var(harg)) {
            int vi = dl_var_get_or_create(&vars, harg->i64);
            dl_rule_head_var(&rule, i, vi);
        } else if (harg->type == -RAY_I64) {
            dl_rule_head_const(&rule, i, harg->i64);
        } else if (harg->type == -RAY_SYM) {
            dl_rule_head_const(&rule, i, harg->i64);
        } else {
            return ray_error("type", "rule: head arguments must be ?variables or constants");
        }
    }

    /* Body clauses */
    for (int64_t i = 1; i < n; i++) {
        ray_t* err = dl_parse_body_clause(&rule, args[i], &vars);
        if (err) return err;
    }

    rule.n_vars = vars.n;

    /* Store globally */
    memcpy(&g_dl_rules[g_dl_n_rules++], &rule, sizeof(dl_rule_t));
    return ray_bool(true);
}

/* (query db (find ?a ?b ...) (where clause1 clause2 ...))
 * Special form: db is evaluated, find/where are NOT evaluated.
 * Creates a temporary dl_program_t, registers the EAV table,
 * copies global rules, builds a synthetic query rule, and evaluates. */
ray_t* ray_query_fn(ray_t** args, int64_t n) {
    if (n < 3)
        return ray_error("arity", "query expects: db (find ...) (where ...)");

    /* Evaluate db (first arg) */
    ray_t* db = ray_eval(args[0]);
    if (!db || RAY_IS_ERR(db)) return db ? db : ray_error("type", "query: db is null");
    if (db->type != RAY_TABLE) { ray_release(db); return ray_error("type", "query: first arg must be a datoms table"); }

    /* Parse find clause */
    ray_t* find_clause = args[1];
    if (!is_list(find_clause) || ray_len(find_clause) < 2) {
        ray_release(db);
        return ray_error("type", "query: second arg must be (find ?var ...)");
    }
    ray_t** find_elems = (ray_t**)ray_data(find_clause);
    int64_t find_len = ray_len(find_clause);

    /* Verify it starts with 'find' */
    if (find_elems[0]->type != -RAY_SYM) {
        ray_release(db);
        return ray_error("type", "query: expected (find ...)");
    }
    ray_t* find_name = ray_sym_str(find_elems[0]->i64);
    if (!find_name || strcmp(ray_str_ptr(find_name), "find") != 0) {
        ray_release(db);
        return ray_error("type", "query: expected (find ...) as second argument");
    }

    /* Collect find variable sym IDs */
    int64_t find_var_syms[DL_MAX_ARITY];
    int n_find_vars = 0;
    for (int64_t i = 1; i < find_len && n_find_vars < DL_MAX_ARITY; i++) {
        if (!is_dl_var(find_elems[i])) {
            ray_release(db);
            return ray_error("type", "query: find arguments must be ?variables");
        }
        find_var_syms[n_find_vars++] = find_elems[i]->i64;
    }

    /* Parse where clause */
    ray_t* where_clause = args[2];
    if (!is_list(where_clause) || ray_len(where_clause) < 2) {
        ray_release(db);
        return ray_error("type", "query: third arg must be (where clause ...)");
    }
    ray_t** where_elems = (ray_t**)ray_data(where_clause);
    int64_t where_len = ray_len(where_clause);

    /* Verify it starts with 'where' */
    if (where_elems[0]->type != -RAY_SYM) {
        ray_release(db);
        return ray_error("type", "query: expected (where ...)");
    }
    ray_t* where_name = ray_sym_str(where_elems[0]->i64);
    if (!where_name || strcmp(ray_str_ptr(where_name), "where") != 0) {
        ray_release(db);
        return ray_error("type", "query: expected (where ...) as third argument");
    }

    /* Build variable map for the query */
    dl_var_map_t vars;
    memset(&vars, 0, sizeof(vars));

    /* Pre-populate the variable map with find variables so they get
     * the lowest indices (0, 1, 2, ...) -- makes projection trivial */
    for (int i = 0; i < n_find_vars; i++)
        dl_var_get_or_create(&vars, find_var_syms[i]);

    /* Build synthetic query rule: __query(?find_vars...) :- body_clauses... */
    dl_rule_t qrule;
    dl_rule_init(&qrule, "__query", n_find_vars);
    for (int i = 0; i < n_find_vars; i++)
        dl_rule_head_var(&qrule, i, i);

    /* Parse body clauses into the query rule */
    for (int64_t i = 1; i < where_len; i++) {
        ray_t* err = dl_parse_body_clause(&qrule, where_elems[i], &vars);
        if (err) { ray_release(db); return err; }
    }
    qrule.n_vars = vars.n;

    /* Create temporary program */
    dl_program_t* prog = dl_program_new();
    if (!prog) { ray_release(db); return ray_error("oom", "query: cannot create program"); }

    /* Register the EAV table as a 3-arity "eav" relation.
     * The 'a' column is RAY_SYM with adaptive width -- the Datalog engine
     * operates on I64 data only, so convert SYM columns to I64 first. */
    {
        int64_t nrows_db = ray_table_nrows(db);
        ray_t* eav_tbl = ray_table_new(3);
        for (int c = 0; c < 3; c++) {
            ray_t* col = ray_table_get_col_idx(db, c);
            if (!col) continue;
            if (col->type == RAY_SYM) {
                /* Convert SYM -> I64: read sym IDs via ray_read_sym */
                ray_t* i64col = ray_vec_new(RAY_I64, nrows_db);
                if (i64col && !RAY_IS_ERR(i64col)) {
                    i64col->len = nrows_db;
                    int64_t* d = (int64_t*)ray_data(i64col);
                    for (int64_t r = 0; r < nrows_db; r++)
                        d[r] = ray_read_sym(ray_data(col), r, col->type, col->attrs);
                    eav_tbl = ray_table_add_col(eav_tbl, ray_table_col_name(db, c), i64col);
                    ray_release(i64col);
                }
            } else {
                eav_tbl = ray_table_add_col(eav_tbl, ray_table_col_name(db, c), col);
            }
        }
        dl_add_edb(prog, "eav", eav_tbl, 3);
        ray_release(eav_tbl);
    }

    /* Copy all global rules into the program */
    for (int i = 0; i < g_dl_n_rules; i++)
        dl_add_rule(prog, &g_dl_rules[i]);

    /* Add the synthetic query rule */
    dl_add_rule(prog, &qrule);

    /* Stratify and evaluate */
    if (dl_stratify(prog) != 0) {
        dl_program_free(prog);
        ray_release(db);
        return ray_error("domain", "query: unstratifiable negation cycle");
    }

    if (dl_eval(prog) != 0) {
        dl_program_free(prog);
        ray_release(db);
        return ray_error("domain", "query: evaluation failed");
    }

    /* Get the result */
    ray_t* raw = dl_query(prog, "__query");
    if (!raw || RAY_IS_ERR(raw)) {
        dl_program_free(prog);
        ray_release(db);
        return raw ? raw : ray_error("domain", "query: no result");
    }

    /* Build result table with user-friendly column names (the ?variable names) */
    int64_t nrows = ray_table_nrows(raw);
    int64_t ncols = ray_table_ncols(raw);
    ray_t* result = ray_table_new(n_find_vars);
    for (int i = 0; i < n_find_vars && i < (int)ncols; i++) {
        ray_t* col = ray_table_get_col_idx(raw, i);
        if (col)
            result = ray_table_add_col(result, find_var_syms[i], col);
    }

    /* Handle empty result: ensure schema is correct */
    if (nrows == 0 && n_find_vars > 0 && ray_table_ncols(result) == 0) {
        ray_release(result);
        result = ray_table_new(n_find_vars);
        for (int i = 0; i < n_find_vars; i++) {
            ray_t* ev = ray_vec_new(RAY_I64, 0);
            if (!RAY_IS_ERR(ev)) {
                result = ray_table_add_col(result, find_var_syms[i], ev);
                ray_release(ev);
            }
        }
    }

    dl_program_free(prog);
    ray_release(db);
    return result;
}

/* ══════════════════════════════════════════
 * Programmatic Datalog API builtins
 * ══════════════════════════════════════════ */

/* Opaque handle for dl_program_t stored in a ray_t atom.
 * We store the pointer in the i64 field. */
static ray_t* dl_wrap_program(dl_program_t* prog) {
    ray_t* obj = ray_alloc(0);
    if (!obj || RAY_IS_ERR(obj)) return ray_error("oom", NULL);
    obj->type = -RAY_I64;
    obj->i64 = (int64_t)(uintptr_t)prog;
    return obj;
}

static dl_program_t* dl_unwrap_program(ray_t* obj) {
    if (!obj || obj->type != -RAY_I64) return NULL;
    return (dl_program_t*)(uintptr_t)obj->i64;
}

/* (dl-program) — create a new empty dl_program_t */
ray_t* ray_dl_program_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", "dl-program takes no arguments");
    dl_program_t* prog = dl_program_new();
    if (!prog) return ray_error("oom", "dl-program: cannot allocate");
    return dl_wrap_program(prog);
}

/* (dl-add-edb prog "name" table arity) — register EDB */
ray_t* ray_dl_add_edb_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("arity", "dl-add-edb expects: prog name table arity");
    dl_program_t* prog = dl_unwrap_program(args[0]);
    if (!prog) return ray_error("type", "dl-add-edb: first arg must be a dl-program");

    /* Name can be a symbol or string */
    const char* name = NULL;
    ray_t* name_str = NULL;
    if (args[1]->type == -RAY_SYM) {
        name_str = ray_sym_str(args[1]->i64);
        name = name_str ? ray_str_ptr(name_str) : NULL;
    }
    if (!name) return ray_error("type", "dl-add-edb: name must be a symbol");

    if (args[2]->type != RAY_TABLE)
        return ray_error("type", "dl-add-edb: third arg must be a table");
    if (args[3]->type != -RAY_I64)
        return ray_error("type", "dl-add-edb: arity must be an integer");

    int rc = dl_add_edb(prog, name, args[2], (int)args[3]->i64);
    return (rc >= 0) ? ray_bool(true) : ray_error("domain", "dl-add-edb: failed");
}

/* (dl-stratify prog) — compute strata */
ray_t* ray_dl_stratify_fn(ray_t* x) {
    dl_program_t* prog = dl_unwrap_program(x);
    if (!prog) return ray_error("type", "dl-stratify: arg must be a dl-program");
    int rc = dl_stratify(prog);
    return (rc == 0) ? ray_bool(true) : ray_error("domain", "dl-stratify: unstratifiable");
}

/* (dl-eval prog) — evaluate to fixpoint */
ray_t* ray_dl_eval_fn(ray_t* x) {
    dl_program_t* prog = dl_unwrap_program(x);
    if (!prog) return ray_error("type", "dl-eval: arg must be a dl-program");
    int rc = dl_eval(prog);
    return (rc == 0) ? ray_bool(true) : ray_error("domain", "dl-eval: evaluation failed");
}

/* (dl-query prog "pred") — get result table */
ray_t* ray_dl_query_fn(ray_t* prog_obj, ray_t* pred_obj) {
    dl_program_t* prog = dl_unwrap_program(prog_obj);
    if (!prog) return ray_error("type", "dl-query: first arg must be a dl-program");

    const char* pred = NULL;
    if (pred_obj->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(pred_obj->i64);
        pred = s ? ray_str_ptr(s) : NULL;
    }
    if (!pred) return ray_error("type", "dl-query: pred must be a symbol");

    ray_t* result = dl_query(prog, pred);
    if (!result) return ray_error("domain", "dl-query: predicate not found");
    ray_retain(result);
    return result;
}

/* (dl-provenance prog "pred") — get provenance column */
ray_t* ray_dl_provenance_fn(ray_t* prog_obj, ray_t* pred_obj) {
    dl_program_t* prog = dl_unwrap_program(prog_obj);
    if (!prog) return ray_error("type", "dl-provenance: first arg must be a dl-program");

    const char* pred = NULL;
    if (pred_obj->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(pred_obj->i64);
        pred = s ? ray_str_ptr(s) : NULL;
    }
    if (!pred) return ray_error("type", "dl-provenance: pred must be a symbol");

    ray_t* prov = dl_get_provenance(prog, pred);
    if (!prov) return ray_error("domain", "dl-provenance: not available");
    ray_retain(prov);
    return prov;
}

/* Reset global Datalog rule storage (called from ray_lang_destroy) */
void ray_dl_reset_rules(void) {
    g_dl_n_rules = 0;
}
