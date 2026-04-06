/*
 * datalog.c — Datalog evaluation engine for Rayforce
 *
 * Compiles Datalog rules into ray_graph_t operation DAGs and evaluates
 * them to fixpoint using semi-naive evaluation with stratified negation.
 */
#include "ops/datalog.h"
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
