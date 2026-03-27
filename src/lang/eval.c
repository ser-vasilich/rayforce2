#include "lang/eval.h"
#include "lang/env.h"
#include "lang/parse.h"
#include <string.h>

/* ══════════════════════════════════════════
 * Arithmetic builtins
 * ══════════════════════════════════════════ */

static td_t* make_i64(int64_t v) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_I64;
    obj->i64 = v;
    return obj;
}

static td_t* make_f64(double v) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_F64;
    obj->f64 = v;
    return obj;
}

static td_t* make_bool(uint8_t v) {
    td_t* obj = td_alloc(0);
    if (!obj) return TD_ERR_PTR(TD_ERR_OOM);
    obj->type = TD_ATOM_BOOL;
    obj->b8 = v;
    return obj;
}

/* Helpers to extract numeric value as double */
static int is_numeric(td_t* x) {
    return x->type == TD_ATOM_I64 || x->type == TD_ATOM_F64;
}

static double as_f64(td_t* x) {
    return (x->type == TD_ATOM_F64) ? x->f64 : (double)x->i64;
}

static int is_float_op(td_t* a, td_t* b) {
    return a->type == TD_ATOM_F64 || b->type == TD_ATOM_F64;
}

/* Binary arithmetic */
static td_t* ray_add(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) + as_f64(b));
    return make_i64(a->i64 + b->i64);
}

static td_t* ray_sub(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) - as_f64(b));
    return make_i64(a->i64 - b->i64);
}

static td_t* ray_mul(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) * as_f64(b));
    return make_i64(a->i64 * b->i64);
}

static td_t* ray_div(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    if (is_float_op(a, b)) return make_f64(as_f64(a) / as_f64(b));
    if (b->i64 == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    return make_i64(a->i64 / b->i64);
}

static td_t* ray_mod(td_t* a, td_t* b) {
    if (a->type != TD_ATOM_I64 || b->type != TD_ATOM_I64)
        return TD_ERR_PTR(TD_ERR_TYPE);
    if (b->i64 == 0) return TD_ERR_PTR(TD_ERR_DOMAIN);
    return make_i64(a->i64 % b->i64);
}

/* Comparison */
static td_t* ray_gt(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) > as_f64(b) ? 1 : 0);
}

static td_t* ray_lt(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) < as_f64(b) ? 1 : 0);
}

static td_t* ray_gte(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) >= as_f64(b) ? 1 : 0);
}

static td_t* ray_lte(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) <= as_f64(b) ? 1 : 0);
}

static td_t* ray_eq(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) == as_f64(b) ? 1 : 0);
}

static td_t* ray_neq(td_t* a, td_t* b) {
    if (!is_numeric(a) || !is_numeric(b)) return TD_ERR_PTR(TD_ERR_TYPE);
    return make_bool(as_f64(a) != as_f64(b) ? 1 : 0);
}

/* Logical */
static td_t* ray_and(td_t* a, td_t* b) {
    return make_bool((a->b8 && b->b8) ? 1 : 0);
}

static td_t* ray_or(td_t* a, td_t* b) {
    return make_bool((a->b8 || b->b8) ? 1 : 0);
}

/* Unary */
static td_t* ray_not(td_t* x) {
    return make_bool(x->b8 ? 0 : 1);
}

static td_t* ray_neg(td_t* x) {
    if (x->type == TD_ATOM_I64) return make_i64(-x->i64);
    if (x->type == TD_ATOM_F64) return make_f64(-x->f64);
    return TD_ERR_PTR(TD_ERR_TYPE);
}

/* ══════════════════════════════════════════
 * Special forms: set, let, if, do
 * ══════════════════════════════════════════ */

/* (set name value) — bind in global env. Receives unevaluated args. */
static td_t* ray_set(td_t* name_obj, td_t* val_expr) {
    if (name_obj->type != TD_ATOM_SYM)
        return TD_ERR_PTR(TD_ERR_TYPE);
    td_t* val = td_eval(val_expr);
    if (TD_IS_ERR(val)) return val;
    td_env_set(name_obj->i64, val);
    return val;  /* set returns the value */
}

/* (let name value) — bind in local scope. Receives unevaluated args. */
static td_t* ray_let(td_t* name_obj, td_t* val_expr) {
    if (name_obj->type != TD_ATOM_SYM)
        return TD_ERR_PTR(TD_ERR_TYPE);
    td_t* val = td_eval(val_expr);
    if (TD_IS_ERR(val)) return val;
    td_env_set_local(name_obj->i64, val);
    return val;
}

/* (if cond then else?) — conditional. Receives unevaluated args. */
static td_t* ray_cond(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);
    td_t* cond = td_eval(args[0]);
    if (TD_IS_ERR(cond)) return cond;
    int truthy = 0;
    if (cond->type == TD_ATOM_BOOL) truthy = cond->b8;
    else if (cond->type == TD_ATOM_I64) truthy = cond->i64 != 0;
    else truthy = 1;  /* non-null is truthy */
    td_release(cond);
    if (truthy) return td_eval(args[1]);
    if (n >= 3) return td_eval(args[2]);
    /* No else branch: return 0 */
    return make_i64(0);
}

/* (do expr1 expr2 ...) — evaluate in sequence, return last. Pushes local scope. */
static td_t* ray_do(td_t** args, int64_t n) {
    if (n == 0) return make_i64(0);
    td_env_push_scope();
    td_t* result = NULL;
    for (int64_t i = 0; i < n; i++) {
        if (result) td_release(result);
        result = td_eval(args[i]);
        if (TD_IS_ERR(result)) {
            td_env_pop_scope();
            return result;
        }
    }
    td_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Lambda functions
 * ══════════════════════════════════════════ */

/* (fn [params...] body...) — create a lambda object.
 * Stores params list and body expressions in data area. */
static td_t* ray_fn(td_t** args, int64_t n) {
    if (n < 2) return TD_ERR_PTR(TD_ERR_DOMAIN);
    /* args[0] = param vector (list of name symbols), args[1..n-1] = body exprs */
    td_t* params_list = args[0];

    /* Create lambda object with space for 5 slots:
     * [0] params, [1] body, [2] bytecode, [3] constants, [4] n_locals */
    td_t* lambda = td_alloc(5 * sizeof(td_t*));
    if (!lambda) return TD_ERR_PTR(TD_ERR_OOM);
    lambda->type = TD_ATOM_LAMBDA;
    lambda->attrs = 0;
    lambda->len = 0;

    /* Store params list */
    td_retain(params_list);
    LAMBDA_PARAMS(lambda) = params_list;

    /* Build body list: wrap body expressions in a TD_LIST */
    int64_t body_count = n - 1;
    td_t* body = td_alloc(body_count * sizeof(td_t*));
    if (!body) {
        td_release(params_list);
        td_release(lambda);
        return TD_ERR_PTR(TD_ERR_OOM);
    }
    body->type = TD_LIST;
    body->len = body_count;
    td_t** body_elems = (td_t**)td_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        td_retain(args[i + 1]);
        body_elems[i] = args[i + 1];
    }
    LAMBDA_BODY(lambda) = body;

    /* Clear compiled slots */
    LAMBDA_BC(lambda) = NULL;
    LAMBDA_CONSTS(lambda) = NULL;
    LAMBDA_NLOCALS(lambda) = 0;

    return lambda;
}

/* Execute compiled bytecode for a lambda. */
static td_t* vm_exec(td_t* lambda, td_t** call_args, int64_t argc);

/* Call a lambda: compile on first call, then execute bytecode. */
static td_t* call_lambda(td_t* lambda, td_t** call_args, int64_t argc) {
    /* Lazy compilation on first call */
    if (!LAMBDA_IS_COMPILED(lambda)) {
        td_compile(lambda);
    }

    /* If compilation succeeded, run bytecode; otherwise fall back to tree-walk */
    if (LAMBDA_IS_COMPILED(lambda)) {
        return vm_exec(lambda, call_args, argc);
    }

    /* Fallback: tree-walking interpreter */
    td_t* params_list = LAMBDA_PARAMS(lambda);
    td_t* body = LAMBDA_BODY(lambda);

    int64_t param_count = td_len(params_list);
    td_t** param_syms = (td_t**)td_data(params_list);

    td_env_push_scope();

    for (int64_t i = 0; i < param_count && i < argc; i++) {
        td_env_set_local(param_syms[i]->i64, call_args[i]);
    }

    int64_t body_count = td_len(body);
    td_t** body_exprs = (td_t**)td_data(body);
    td_t* result = NULL;
    for (int64_t i = 0; i < body_count; i++) {
        if (result) td_release(result);
        result = td_eval(body_exprs[i]);
        if (TD_IS_ERR(result)) {
            td_env_pop_scope();
            return result;
        }
    }

    td_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Stack-based VM executor (bytecode interpreter)
 * ══════════════════════════════════════════ */

#define VM_STACK_SIZE 256

static td_t* vm_exec(td_t* lambda, td_t** call_args, int64_t argc) {
    td_t* bc_vec = LAMBDA_BC(lambda);
    td_t* consts = LAMBDA_CONSTS(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    td_t* params_list = LAMBDA_PARAMS(lambda);

    uint8_t* code = (uint8_t*)td_data(bc_vec);
    td_t** cpool = (td_t**)td_data(consts);

    /* Stack and locals */
    td_t* stack[VM_STACK_SIZE];
    int32_t sp = 0;

    td_t* locals[256];
    memset(locals, 0, n_locals * sizeof(td_t*));

    /* Bind parameters into local slots */
    int64_t param_count = td_len(params_list);
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        td_retain(call_args[i]);
        locals[i] = call_args[i];
    }

    int32_t ip = 0;

    for (;;) {
        uint8_t op = code[ip++];
        switch (op) {

        case OP_RET: {
            td_t* result = (sp > 0) ? stack[--sp] : make_i64(0);
            /* Release remaining stack and locals */
            for (int32_t i = 0; i < sp; i++)
                if (stack[i]) td_release(stack[i]);
            for (int32_t i = 0; i < n_locals; i++)
                if (locals[i]) td_release(locals[i]);
            return result;
        }

        case OP_LOADCONST: {
            uint8_t idx = code[ip++];
            td_t* val = cpool[idx];
            td_retain(val);
            stack[sp++] = val;
            break;
        }

        case OP_LOADCONST_W: {
            uint16_t idx = (uint16_t)(code[ip] << 8) | code[ip + 1];
            ip += 2;
            td_t* val = cpool[idx];
            td_retain(val);
            stack[sp++] = val;
            break;
        }

        case OP_LOADENV: {
            uint8_t slot = code[ip++];
            td_t* val = locals[slot];
            if (val) td_retain(val);
            else val = make_i64(0);
            stack[sp++] = val;
            break;
        }

        case OP_STOREENV: {
            uint8_t slot = code[ip++];
            td_t* val = stack[--sp];
            if (locals[slot]) td_release(locals[slot]);
            locals[slot] = val;
            /* Store does NOT consume the value from perspective of the
             * expression — but our compiler emits DUP before STOREENV
             * when the value is needed. So here we just store. */
            break;
        }

        case OP_POP: {
            if (sp > 0) {
                td_t* val = stack[--sp];
                if (val) td_release(val);
            }
            break;
        }

        case OP_DUP: {
            td_t* val = stack[sp - 1];
            td_retain(val);
            stack[sp++] = val;
            break;
        }

        case OP_RESOLVE: {
            uint8_t idx = code[ip++];
            td_t* name_obj = cpool[idx];
            td_t* val = td_env_get(name_obj->i64);
            if (!val) {
                for (int32_t i = 0; i < sp; i++)
                    if (stack[i]) td_release(stack[i]);
                for (int32_t i = 0; i < n_locals; i++)
                    if (locals[i]) td_release(locals[i]);
                return TD_ERR_PTR(TD_ERR_DOMAIN);
            }
            td_retain(val);
            stack[sp++] = val;
            break;
        }

        case OP_JMP: {
            int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
            ip += 2;
            ip += offset;
            break;
        }

        case OP_JMPF: {
            int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
            ip += 2;
            td_t* cond = stack[--sp];
            int truthy = 0;
            if (cond->type == TD_ATOM_BOOL) truthy = cond->b8;
            else if (cond->type == TD_ATOM_I64) truthy = cond->i64 != 0;
            else truthy = 1;
            td_release(cond);
            if (!truthy) ip += offset;
            break;
        }

        case OP_CALL1: {
            td_t* arg = stack[--sp];
            td_t* fn_obj = stack[--sp];
            td_unary_fn fn = (td_unary_fn)(uintptr_t)fn_obj->i64;
            td_t* result = fn(arg);
            td_release(arg);
            td_release(fn_obj);
            stack[sp++] = result;
            break;
        }

        case OP_CALL2: {
            td_t* right = stack[--sp];
            td_t* left = stack[--sp];
            td_t* fn_obj = stack[--sp];

            if (fn_obj->attrs & TD_FN_SPECIAL_FORM) {
                /* Special forms receive unevaluated args — but in VM
                 * context args are already evaluated. For binary special
                 * forms like set/let this path shouldn't be hit (they're
                 * compiled to STOREENV). Use direct call. */
                td_binary_fn fn = (td_binary_fn)(uintptr_t)fn_obj->i64;
                td_t* result = fn(left, right);
                td_release(left);
                td_release(right);
                td_release(fn_obj);
                stack[sp++] = result;
            } else {
                td_binary_fn fn = (td_binary_fn)(uintptr_t)fn_obj->i64;
                td_t* result = fn(left, right);
                td_release(left);
                td_release(right);
                td_release(fn_obj);
                stack[sp++] = result;
            }
            break;
        }

        case OP_CALLN: {
            uint8_t n = code[ip++];
            td_t* args[64];
            for (int32_t i = n - 1; i >= 0; i--)
                args[i] = stack[--sp];
            td_t* fn_obj = stack[--sp];
            td_vary_fn fn = (td_vary_fn)(uintptr_t)fn_obj->i64;
            td_t* result = fn(args, n);
            for (int32_t i = 0; i < n; i++)
                td_release(args[i]);
            td_release(fn_obj);
            stack[sp++] = result;
            break;
        }

        case OP_CALLF: {
            /* Call a compiled lambda — recursive vm_exec for now.
             * Full frame-based dispatch comes in Task 4.2. */
            uint8_t n = code[ip++];
            td_t* args[64];
            for (int32_t i = n - 1; i >= 0; i--)
                args[i] = stack[--sp];
            td_t* fn_obj = stack[--sp];
            td_t* result = call_lambda(fn_obj, args, n);
            for (int32_t i = 0; i < n; i++)
                td_release(args[i]);
            td_release(fn_obj);
            stack[sp++] = result;
            break;
        }

        case OP_CALLD: {
            /* Dynamic dispatch — build a list and pass to td_eval */
            uint8_t n = code[ip++];
            td_t* args[64];
            for (int32_t i = n - 1; i >= 0; i--)
                args[i] = stack[--sp];
            td_t* fn_obj = stack[--sp];

            /* Build call list: (fn arg1 arg2 ...) */
            td_t* call_list = td_alloc((n + 1) * sizeof(td_t*));
            call_list->type = TD_LIST;
            call_list->len = n + 1;
            td_t** elems = (td_t**)td_data(call_list);
            elems[0] = fn_obj;  /* already retained from stack */
            for (int32_t i = 0; i < n; i++)
                elems[i + 1] = args[i];

            td_t* result = td_eval(call_list);
            td_release(call_list);
            stack[sp++] = result;
            break;
        }

        case OP_CALLS:
            /* Tail call — for now just do a regular CALLF.
             * True tail call optimization comes in Task 4.2. */
            /* fall through */
        default: {
            /* Unknown opcode — abort with error */
            for (int32_t i = 0; i < sp; i++)
                if (stack[i]) td_release(stack[i]);
            for (int32_t i = 0; i < n_locals; i++)
                if (locals[i]) td_release(locals[i]);
            return TD_ERR_PTR(TD_ERR_DOMAIN);
        }
        }
    }
}

/* ══════════════════════════════════════════
 * Builtin registration
 * ══════════════════════════════════════════ */

static void register_binary(const char* name, uint8_t attrs, td_binary_fn fn) {
    int64_t sym = td_sym_intern(name, strlen(name));
    td_t* obj = td_fn_binary(name, attrs, fn);
    td_env_set(sym, obj);
    td_release(obj);
}

static void register_unary(const char* name, uint8_t attrs, td_unary_fn fn) {
    int64_t sym = td_sym_intern(name, strlen(name));
    td_t* obj = td_fn_unary(name, attrs, fn);
    td_env_set(sym, obj);
    td_release(obj);
}

static void register_vary(const char* name, uint8_t attrs, td_vary_fn fn) {
    int64_t sym = td_sym_intern(name, strlen(name));
    td_t* obj = td_fn_vary(name, attrs, fn);
    td_env_set(sym, obj);
    td_release(obj);
}

static void td_register_builtins(void) {
    register_binary("+",   TD_FN_ATOMIC, ray_add);
    register_binary("-",   TD_FN_ATOMIC, ray_sub);
    register_binary("*",   TD_FN_ATOMIC, ray_mul);
    register_binary("/",   TD_FN_ATOMIC, ray_div);
    register_binary("%",   TD_FN_ATOMIC, ray_mod);
    register_binary(">",   TD_FN_NONE,   ray_gt);
    register_binary("<",   TD_FN_NONE,   ray_lt);
    register_binary(">=",  TD_FN_NONE,   ray_gte);
    register_binary("<=",  TD_FN_NONE,   ray_lte);
    register_binary("==",  TD_FN_NONE,   ray_eq);
    register_binary("!=",  TD_FN_NONE,   ray_neq);
    register_binary("and", TD_FN_NONE,   ray_and);
    register_binary("or",  TD_FN_NONE,   ray_or);
    register_unary("not",  TD_FN_NONE,   ray_not);
    register_unary("neg",  TD_FN_ATOMIC, ray_neg);

    /* Special forms */
    register_binary("set", TD_FN_SPECIAL_FORM, ray_set);
    register_binary("let", TD_FN_SPECIAL_FORM, ray_let);
    register_vary("if",    TD_FN_SPECIAL_FORM, ray_cond);
    register_vary("do",    TD_FN_SPECIAL_FORM, ray_do);
    register_vary("fn",    TD_FN_SPECIAL_FORM, ray_fn);
}

/* ══════════════════════════════════════════
 * Runtime lifecycle
 * ══════════════════════════════════════════ */

td_err_t td_lang_init(void) {
    td_err_t err = td_env_init();
    if (err != TD_OK) return err;
    td_register_builtins();
    return TD_OK;
}

void td_lang_destroy(void) {
    td_env_destroy();
}

/* ══════════════════════════════════════════
 * Tree-walking evaluator
 * ══════════════════════════════════════════ */

td_t* td_eval(td_t* obj) {
    if (!obj || TD_IS_ERR(obj)) return obj;

    /* Atoms: return themselves (retain) */
    if (td_is_atom(obj)) {
        /* Name reference: resolve from env */
        if (obj->type == TD_ATOM_SYM && (obj->attrs & TD_ATTR_NAME)) {
            td_t* val = td_env_get(obj->i64);
            if (!val) return TD_ERR_PTR(TD_ERR_DOMAIN);
            td_retain(val);
            return val;
        }
        td_retain(obj);
        return obj;
    }

    /* Non-list vectors: return themselves */
    if (obj->type != TD_LIST) { td_retain(obj); return obj; }

    /* Empty list */
    if (td_len(obj) == 0) { td_retain(obj); return obj; }

    /* List: evaluate first element, dispatch by type */
    td_t** elems = (td_t**)td_data(obj);
    td_t* head = td_eval(elems[0]);
    if (TD_IS_ERR(head)) return head;

    int64_t n = td_len(obj);

    switch (head->type) {
        case TD_ATOM_UNARY: {
            if (n < 2) { td_release(head); return TD_ERR_PTR(TD_ERR_DOMAIN); }
            td_unary_fn fn = (td_unary_fn)(uintptr_t)head->i64;
            td_t* arg = td_eval(elems[1]);
            td_release(head);
            if (TD_IS_ERR(arg)) return arg;
            td_t* result = fn(arg);
            td_release(arg);
            return result;
        }
        case TD_ATOM_BINARY: {
            if (n < 3) { td_release(head); return TD_ERR_PTR(TD_ERR_DOMAIN); }
            td_binary_fn fn = (td_binary_fn)(uintptr_t)head->i64;
            if (head->attrs & TD_FN_SPECIAL_FORM) {
                td_release(head);
                return fn(elems[1], elems[2]);
            }
            td_t* left = td_eval(elems[1]);
            if (TD_IS_ERR(left)) { td_release(head); return left; }
            td_t* right = td_eval(elems[2]);
            if (TD_IS_ERR(right)) { td_release(head); td_release(left); return right; }
            td_release(head);
            td_t* result = fn(left, right);
            td_release(left);
            td_release(right);
            return result;
        }
        case TD_ATOM_VARY: {
            td_vary_fn fn = (td_vary_fn)(uintptr_t)head->i64;
            if (head->attrs & TD_FN_SPECIAL_FORM) {
                td_release(head);
                return fn(elems + 1, n - 1);
            }
            int64_t argc = n - 1;
            td_t* args[64];
            for (int64_t i = 0; i < argc && i < 64; i++) {
                args[i] = td_eval(elems[i + 1]);
                if (TD_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) td_release(args[j]);
                    td_release(head);
                    return args[i];
                }
            }
            td_release(head);
            td_t* result = fn(args, argc);
            for (int64_t i = 0; i < argc; i++) td_release(args[i]);
            return result;
        }
        case TD_ATOM_LAMBDA: {
            int64_t argc = n - 1;
            td_t* args[64];
            for (int64_t i = 0; i < argc && i < 64; i++) {
                args[i] = td_eval(elems[i + 1]);
                if (TD_IS_ERR(args[i])) {
                    for (int64_t j = 0; j < i; j++) td_release(args[j]);
                    td_release(head);
                    return args[i];
                }
            }
            td_t* result = call_lambda(head, args, argc);
            for (int64_t i = 0; i < argc; i++) td_release(args[i]);
            td_release(head);
            return result;
        }
        default:
            td_release(head);
            return TD_ERR_PTR(TD_ERR_TYPE);
    }
}

td_t* td_eval_str(const char* source) {
    td_t* parsed = td_parse(source);
    if (TD_IS_ERR(parsed)) return parsed;
    td_t* result = td_eval(parsed);
    td_release(parsed);
    return result;
}
