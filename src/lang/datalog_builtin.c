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

/*  Datalog builtins — extracted from eval.c  */

#include "lang/eval_internal.h"
#include "lang/env.h"
#include "ops/datalog.h"
#include "table/sym.h"
#include "ops/ops.h"

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

    /* Value: accept i64 or sym. Store as i64 (sym → intern ID). */
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

/* Variable name → index map for parsing a single rule or query body */
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
        return true;  /* _ is always wildcard — reserved, never a predicate */
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

    /* ── Triple pattern: (?e :attr ?v) ── */
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

    /* ── Negation: (not (?e :attr ?v))  or  (not (rule-name ?args...)) ── */
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

    /* ── Assignment: (= ?var expr) ── */
    if (dl_is_assignment(clause)) {
        int target_vi = dl_var_get_or_create(vars, ce[1]->i64);
        dl_expr_t* expr = dl_build_expr(ce[2], vars);
        if (!expr)
            return ray_error("type", "rule: cannot parse assignment expression");
        dl_rule_add_assign(rule, target_vi, DL_OP_EQ, expr);
        return NULL;
    }

    /* ── Comparison: (> ?x ?y) or (> ?x 100) ── */
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
            /* Flip: const op var → var flipped_op const */
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

    /* ── Rule invocation / positive atom: (pred-name ?a ?b ...) ── */
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

    /* First arg: head — must be a list (head-name ?v1 ?v2 ...) */
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

    /* _ is reserved as wildcard — cannot be a rule predicate name */
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
     * the lowest indices (0, 1, 2, ...) — makes projection trivial */
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
     * The 'a' column is RAY_SYM with adaptive width — the Datalog engine
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
