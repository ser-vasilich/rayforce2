#include "lang/eval.h"
#include "lang/env.h"
#include <string.h>

/* ── Compiler state ──
 * Internal buffers are td_t objects whose data area holds the raw
 * bytes / pointers. This avoids calling malloc/free. */
typedef struct {
    td_t    *code_obj;   /* TD_U8 vector used as growable byte buffer */
    uint8_t *code;       /* == td_data(code_obj) */
    int32_t  code_len;
    int32_t  code_cap;

    td_t    *consts_obj; /* TD_LIST used as growable pointer array */
    td_t   **consts;     /* == td_data(consts_obj) */
    int32_t  n_consts;
    int32_t  consts_cap;

    struct { int64_t sym_id; int32_t slot; } locals[256];
    int32_t  n_locals;
    int32_t  max_locals;
} compiler_t;

static void compile_expr(compiler_t *c, td_t *ast);

static void compiler_init(compiler_t *c) {
    memset(c, 0, sizeof(*c));
    c->code_cap = 256;
    c->code_obj = td_alloc(c->code_cap);
    c->code_obj->type = TD_U8;
    c->code_obj->len = 0;
    c->code = (uint8_t *)td_data(c->code_obj);

    c->consts_cap = 16;
    c->consts_obj = td_alloc(c->consts_cap * sizeof(td_t *));
    c->consts_obj->type = TD_LIST;
    c->consts_obj->len = 0;
    c->consts = (td_t **)td_data(c->consts_obj);
    memset(c->consts, 0, c->consts_cap * sizeof(td_t *));
}

static void compiler_destroy(compiler_t *c) {
    for (int32_t i = 0; i < c->n_consts; i++)
        if (c->consts[i]) td_release(c->consts[i]);
    td_release(c->consts_obj);
    td_release(c->code_obj);
}

/* ── Emit helpers ── */
static void emit(compiler_t *c, uint8_t byte) {
    if (c->code_len >= c->code_cap) {
        int32_t new_cap = c->code_cap * 2;
        td_t *new_obj = td_alloc(new_cap);
        new_obj->type = TD_U8;
        new_obj->len = 0;
        memcpy(td_data(new_obj), c->code, c->code_len);
        td_release(c->code_obj);
        c->code_obj = new_obj;
        c->code = (uint8_t *)td_data(new_obj);
        c->code_cap = new_cap;
    }
    c->code[c->code_len++] = byte;
}

static void emit_const(compiler_t *c, int32_t idx) {
    if (idx < 256) {
        emit(c, OP_LOADCONST);
        emit(c, (uint8_t)idx);
    } else {
        emit(c, OP_LOADCONST_W);
        emit(c, (uint8_t)(idx >> 8));
        emit(c, (uint8_t)(idx & 0xFF));
    }
}

/* ── Constant pool ── */
static int32_t add_constant(compiler_t *c, td_t *value) {
    for (int32_t i = 0; i < c->n_consts; i++) {
        td_t *v = c->consts[i];
        if (v == value) return i;
        if (v->type == value->type && td_is_atom(v)) {
            if (v->type == TD_ATOM_I64 && v->i64 == value->i64) return i;
            if (v->type == TD_ATOM_F64 && v->f64 == value->f64) return i;
            if (v->type == TD_ATOM_BOOL && v->b8 == value->b8) return i;
            if (v->type == TD_ATOM_SYM && v->i64 == value->i64 &&
                v->attrs == value->attrs) return i;
        }
    }
    if (c->n_consts >= c->consts_cap) {
        int32_t new_cap = c->consts_cap * 2;
        td_t *new_obj = td_alloc(new_cap * sizeof(td_t *));
        new_obj->type = TD_LIST;
        new_obj->len = 0;
        td_t **new_arr = (td_t **)td_data(new_obj);
        memcpy(new_arr, c->consts, c->n_consts * sizeof(td_t *));
        memset(new_arr + c->n_consts, 0, (new_cap - c->n_consts) * sizeof(td_t *));
        td_release(c->consts_obj);
        c->consts_obj = new_obj;
        c->consts = new_arr;
        c->consts_cap = new_cap;
    }
    td_retain(value);
    c->consts[c->n_consts] = value;
    return c->n_consts++;
}

/* ── Local variable tracking ── */
static int32_t find_local(compiler_t *c, int64_t sym_id) {
    for (int32_t i = c->n_locals - 1; i >= 0; i--)
        if (c->locals[i].sym_id == sym_id) return c->locals[i].slot;
    return -1;
}

static int32_t add_local(compiler_t *c, int64_t sym_id) {
    if (c->n_locals >= 256) return -1;
    int32_t slot = c->n_locals;
    c->locals[c->n_locals].sym_id = sym_id;
    c->locals[c->n_locals].slot = slot;
    c->n_locals++;
    if (c->n_locals > c->max_locals) c->max_locals = c->n_locals;
    return slot;
}

/* ── Jump helpers ── */
static int32_t emit_jump(compiler_t *c, uint8_t opcode) {
    emit(c, opcode);
    int32_t patch_pos = c->code_len;
    emit(c, 0);
    emit(c, 0);
    return patch_pos;
}

static void patch_jump(compiler_t *c, int32_t pos) {
    int16_t offset = (int16_t)(c->code_len - pos - 2);
    c->code[pos]     = (uint8_t)(offset >> 8);
    c->code[pos + 1] = (uint8_t)(offset & 0xFF);
}

/* Cached sym IDs for special forms */
static int64_t sf_set = -1, sf_let = -1, sf_if = -1, sf_do = -1, sf_fn = -1;

static void init_sf_syms(void) {
    if (sf_set >= 0) return;
    sf_set = td_sym_intern("set", 3);
    sf_let = td_sym_intern("let", 3);
    sf_if  = td_sym_intern("if",  2);
    sf_do  = td_sym_intern("do",  2);
    sf_fn  = td_sym_intern("fn",  2);
}

/* ── Compile a list (special form or function call) ── */
static void compile_list(compiler_t *c, td_t *ast) {
    td_t **elems = (td_t **)td_data(ast);
    int64_t n = td_len(ast);
    td_t *head = elems[0];

    init_sf_syms();

    /* Check for special forms by name */
    if (head->type == TD_ATOM_SYM && (head->attrs & TD_ATTR_NAME)) {
        int64_t sym_id = head->i64;

        /* (set name value) — dynamic eval (set modifies global env) */
        if (sym_id == sf_set && n == 3) {
            int32_t idx = add_constant(c, ast);
            emit_const(c, idx);
            emit(c, OP_CALLD);
            emit(c, 0);
            return;
        }

        /* (let name value) — compile value, store in local slot */
        if (sym_id == sf_let && n == 3) {
            td_t *name_obj = elems[1];
            compile_expr(c, elems[2]);
            emit(c, OP_DUP);
            int32_t slot = find_local(c, name_obj->i64);
            if (slot < 0) slot = add_local(c, name_obj->i64);
            emit(c, OP_STOREENV);
            emit(c, (uint8_t)slot);
            return;
        }

        /* (if cond then else?) */
        if (sym_id == sf_if && n >= 3) {
            compile_expr(c, elems[1]);
            int32_t jmpf_pos = emit_jump(c, OP_JMPF);
            compile_expr(c, elems[2]);
            if (n >= 4) {
                int32_t jmp_pos = emit_jump(c, OP_JMP);
                patch_jump(c, jmpf_pos);
                compile_expr(c, elems[3]);
                patch_jump(c, jmp_pos);
            } else {
                int32_t jmp_pos = emit_jump(c, OP_JMP);
                patch_jump(c, jmpf_pos);
                td_t *zero = td_alloc(0);
                zero->type = TD_ATOM_I64;
                zero->i64 = 0;
                int32_t idx = add_constant(c, zero);
                td_release(zero);
                emit_const(c, idx);
                patch_jump(c, jmp_pos);
            }
            return;
        }

        /* (do expr1 expr2 ...) */
        if (sym_id == sf_do && n >= 2) {
            for (int64_t i = 1; i < n; i++) {
                if (i > 1) emit(c, OP_POP);
                compile_expr(c, elems[i]);
            }
            return;
        }

        /* (fn [params] body...) — nested lambda via dynamic eval */
        if (sym_id == sf_fn && n >= 3) {
            int32_t idx = add_constant(c, ast);
            emit_const(c, idx);
            emit(c, OP_CALLD);
            emit(c, 0);
            return;
        }
    }

    /* Look up head at compile time to determine call type */
    td_t *fn = NULL;
    if (head->type == TD_ATOM_SYM && (head->attrs & TD_ATTR_NAME))
        fn = td_env_get(head->i64);

    /* Unrecognized special form: dynamic eval on entire form */
    if (fn && (fn->attrs & TD_FN_SPECIAL_FORM)) {
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        emit(c, OP_CALLD);
        emit(c, 0);
        return;
    }

    /* General function call: compile head, args, then dispatch */
    compile_expr(c, head);
    int64_t argc = n - 1;
    for (int64_t i = 1; i < n; i++)
        compile_expr(c, elems[i]);

    if (fn) {
        switch (fn->type) {
        case TD_ATOM_UNARY:
            if (argc == 1) { emit(c, OP_CALL1); return; }
            break;
        case TD_ATOM_BINARY:
            if (argc == 2) { emit(c, OP_CALL2); return; }
            break;
        case TD_ATOM_VARY:
            emit(c, OP_CALLN);
            emit(c, (uint8_t)argc);
            return;
        case TD_ATOM_LAMBDA:
            emit(c, OP_CALLF);
            emit(c, (uint8_t)argc);
            return;
        default:
            break;
        }
    }

    emit(c, OP_CALLF);
    emit(c, (uint8_t)argc);
}

/* ── Compile expression ── */
static void compile_expr(compiler_t *c, td_t *ast) {
    if (!ast || TD_IS_ERR(ast)) return;

    if (td_is_atom(ast)) {
        if (ast->type == TD_ATOM_SYM && (ast->attrs & TD_ATTR_NAME)) {
            int32_t slot = find_local(c, ast->i64);
            if (slot >= 0) {
                emit(c, OP_LOADENV);
                emit(c, (uint8_t)slot);
            } else {
                int32_t idx = add_constant(c, ast);
                emit(c, OP_RESOLVE);
                emit(c, (uint8_t)idx);
            }
            return;
        }
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        return;
    }

    if (ast->type != TD_LIST) {
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        return;
    }

    if (td_len(ast) == 0) {
        int32_t idx = add_constant(c, ast);
        emit_const(c, idx);
        return;
    }

    compile_list(c, ast);
}

/* ── Public API ── */
void td_compile(td_t *lambda) {
    if (LAMBDA_IS_COMPILED(lambda)) return;

    compiler_t c;
    compiler_init(&c);

    /* Register params as locals */
    td_t *params_list = LAMBDA_PARAMS(lambda);
    int64_t param_count = td_len(params_list);
    td_t **param_syms = (td_t **)td_data(params_list);
    for (int64_t i = 0; i < param_count; i++)
        add_local(&c, param_syms[i]->i64);

    /* Compile body expressions */
    td_t *body = LAMBDA_BODY(lambda);
    int64_t body_count = td_len(body);
    td_t **body_exprs = (td_t **)td_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        if (i > 0) emit(&c, OP_POP);
        compile_expr(&c, body_exprs[i]);
    }
    emit(&c, OP_RET);

    /* Build bytecode vector */
    td_t *bc = td_alloc(c.code_len);
    bc->type = TD_U8;
    bc->len = c.code_len;
    memcpy(td_data(bc), c.code, c.code_len);

    /* Build constants list */
    td_t *consts = td_alloc(c.n_consts * sizeof(td_t *));
    consts->type = TD_LIST;
    consts->len = c.n_consts;
    td_t **cpool = (td_t **)td_data(consts);
    for (int32_t i = 0; i < c.n_consts; i++) {
        td_retain(c.consts[i]);
        cpool[i] = c.consts[i];
    }

    LAMBDA_BC(lambda) = bc;
    LAMBDA_CONSTS(lambda) = consts;
    LAMBDA_NLOCALS(lambda) = c.max_locals;
    lambda->attrs |= TD_FN_COMPILED;

    compiler_destroy(&c);
}
