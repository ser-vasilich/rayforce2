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
 * Error handling: try / raise
 * ══════════════════════════════════════════ */

static _Thread_local td_t *__raise_val = NULL;

/* (raise value) — raise an error with the given value */
static td_t* ray_raise(td_t* val) {
    if (__raise_val) td_release(__raise_val);
    td_retain(val);
    __raise_val = val;
    return TD_ERR_PTR(TD_ERR_DOMAIN);
}

/* Forward declaration for call_lambda (used by ray_try) */
static td_t* call_lambda(td_t* lambda, td_t** call_args, int64_t argc);

/* (try expr handler) — evaluate expr, if error call handler with error value.
 * Special form: receives unevaluated args. */
static td_t* ray_try(td_t* expr, td_t* handler_expr) {
    td_t* result = td_eval(expr);
    if (!TD_IS_ERR(result)) return result;

    /* Get error value (set by raise, or default for runtime errors) */
    td_t* err_val = __raise_val;
    __raise_val = NULL;
    if (!err_val) err_val = make_i64(0);

    /* Evaluate handler expression */
    td_t* handler = td_eval(handler_expr);
    if (TD_IS_ERR(handler)) {
        td_release(err_val);
        return handler;
    }

    /* Call handler with error value */
    td_t* handler_result;
    if (handler->type == TD_ATOM_LAMBDA) {
        td_t* args[1] = { err_val };
        handler_result = call_lambda(handler, args, 1);
    } else if (handler->type == TD_ATOM_UNARY) {
        td_unary_fn fn = (td_unary_fn)(uintptr_t)handler->i64;
        handler_result = fn(err_val);
    } else {
        handler_result = TD_ERR_PTR(TD_ERR_TYPE);
    }

    td_release(err_val);
    td_release(handler);
    return handler_result;
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
 * Stack-based VM executor (computed goto, frame-based)
 * ══════════════════════════════════════════ */

static _Thread_local td_vm_t *__VM = NULL;

static td_t* vm_exec(td_t* lambda, td_t** call_args, int64_t argc) {
    /* Computed goto dispatch table */
    static void *dispatch[OP__COUNT] = {
        [OP_RET]        = &&op_ret,
        [OP_JMP]        = &&op_jmp,
        [OP_JMPF]       = &&op_jmpf,
        [OP_LOADCONST]  = &&op_loadconst,
        [OP_LOADENV]    = &&op_loadenv,
        [OP_STOREENV]   = &&op_storeenv,
        [OP_POP]        = &&op_pop,
        [OP_RESOLVE]    = &&op_resolve,
        [OP_CALL1]      = &&op_call1,
        [OP_CALL2]      = &&op_call2,
        [OP_CALLN]      = &&op_calln,
        [OP_CALLF]      = &&op_callf,
        [OP_CALLS]      = &&op_calls,
        [OP_CALLD]      = &&op_calld,
        [OP_DUP]        = &&op_dup,
        [OP_LOADCONST_W] = &&op_loadconst_w,
        [OP_TRAP]       = &&op_trap,
        [OP_TRAP_END]   = &&op_trap_end,
    };

    td_vm_t vm;
    memset(&vm, 0, sizeof(vm));
    __VM = &vm;

    /* Set up initial frame */
    vm.fn = lambda;
    td_retain(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    vm.fp = 0;
    vm.sp = n_locals;

    /* Bind parameters into local slots */
    int64_t param_count = td_len(LAMBDA_PARAMS(lambda));
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        td_retain(call_args[i]);
        vm.ps[i] = call_args[i];
    }

    uint8_t *code = (uint8_t *)td_data(LAMBDA_BC(lambda));
    td_t **cpool = (td_t **)td_data(LAMBDA_CONSTS(lambda));
    int32_t ip = 0;

#define DISPATCH() goto *dispatch[code[ip++]]
#define PUSH(v)    (vm.ps[vm.sp++] = (v))
#define POP()      (vm.ps[--vm.sp])
#define PEEK()     (vm.ps[vm.sp - 1])
#define LOCAL(s)   (vm.ps[vm.fp + (s)])

    DISPATCH();

op_loadconst: {
    uint8_t idx = code[ip++];
    td_t *val = cpool[idx];
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadconst_w: {
    uint16_t idx = (uint16_t)(code[ip] << 8) | code[ip + 1];
    ip += 2;
    td_t *val = cpool[idx];
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadenv: {
    uint8_t slot = code[ip++];
    td_t *val = LOCAL(slot);
    if (val) td_retain(val);
    else val = make_i64(0);
    PUSH(val);
    DISPATCH();
}

op_storeenv: {
    uint8_t slot = code[ip++];
    td_t *val = POP();
    if (LOCAL(slot)) td_release(LOCAL(slot));
    LOCAL(slot) = val;
    DISPATCH();
}

op_pop: {
    if (vm.sp > vm.fp + n_locals) {
        td_t *val = POP();
        if (val) td_release(val);
    }
    DISPATCH();
}

op_dup: {
    td_t *val = PEEK();
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_resolve: {
    uint8_t idx = code[ip++];
    td_t *name_obj = cpool[idx];
    td_t *val = td_env_get(name_obj->i64);
    if (!val) goto vm_error;
    td_retain(val);
    PUSH(val);
    DISPATCH();
}

op_jmp: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ip += offset;
    DISPATCH();
}

op_jmpf: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    td_t *cond = POP();
    int truthy = 0;
    if (cond->type == TD_ATOM_BOOL) truthy = cond->b8;
    else if (cond->type == TD_ATOM_I64) truthy = cond->i64 != 0;
    else truthy = 1;
    td_release(cond);
    if (!truthy) ip += offset;
    DISPATCH();
}

op_call1: {
    td_t *arg = POP();
    td_t *fn_obj = POP();
    td_unary_fn fn = (td_unary_fn)(uintptr_t)fn_obj->i64;
    td_t *result = fn(arg);
    td_release(arg);
    td_release(fn_obj);
    PUSH(result);
    DISPATCH();
}

op_call2: {
    td_t *right = POP();
    td_t *left = POP();
    td_t *fn_obj = POP();
    td_binary_fn fn = (td_binary_fn)(uintptr_t)fn_obj->i64;
    td_t *result = fn(left, right);
    td_release(left);
    td_release(right);
    td_release(fn_obj);
    PUSH(result);
    DISPATCH();
}

op_calln: {
    uint8_t n = code[ip++];
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();
    td_vary_fn fn = (td_vary_fn)(uintptr_t)fn_obj->i64;
    td_t *result = fn(fn_args, n);
    for (int32_t i = 0; i < n; i++)
        td_release(fn_args[i]);
    td_release(fn_obj);
    PUSH(result);
    DISPATCH();
}

op_callf: {
    uint8_t n = code[ip++];
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();

    /* Compiled lambda: push frame, switch to callee bytecode */
    if (fn_obj->type == TD_ATOM_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            td_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* Push return frame */
            vm.rs[vm.rp++] = (vm_ctx_t){ .fn = vm.fn, .fp = vm.fp, .ip = ip };

            /* Set up new frame */
            vm.fn = fn_obj;  /* takes ownership of stack ref */
            vm.fp = vm.sp;
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            vm.sp += callee_locals;
            n_locals = callee_locals;

            /* Bind parameters */
            int64_t pcnt = td_len(LAMBDA_PARAMS(fn_obj));
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];  /* transfer ownership from args */
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                td_release(fn_args[i]);  /* excess args */

            /* Switch to callee bytecode */
            code = (uint8_t *)td_data(LAMBDA_BC(fn_obj));
            cpool = (td_t **)td_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Non-lambda or uncompiled: dispatch by type */
    {
        td_t *result;
        switch (fn_obj->type) {
        case TD_ATOM_UNARY:
            result = ((td_unary_fn)(uintptr_t)fn_obj->i64)(fn_args[0]);
            td_release(fn_args[0]);
            for (int32_t i = 1; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_BINARY:
            result = ((td_binary_fn)(uintptr_t)fn_obj->i64)(fn_args[0], fn_args[1]);
            td_release(fn_args[0]);
            td_release(fn_args[1]);
            for (int32_t i = 2; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_VARY:
            result = ((td_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_LAMBDA:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        default:
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            result = TD_ERR_PTR(TD_ERR_TYPE);
            break;
        }
        td_release(fn_obj);
        PUSH(result);
        DISPATCH();
    }
}

op_calls: {
    /* Tail call: reuse current frame (no return stack push) */
    uint8_t n = code[ip++];
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();

    if (fn_obj->type == TD_ATOM_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            td_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* Clean up current frame locals */
            for (int32_t i = 0; i < n_locals; i++)
                if (LOCAL(i)) { td_release(LOCAL(i)); LOCAL(i) = NULL; }

            /* Reuse frame: reset sp to fp, don't push return context */
            vm.sp = vm.fp;
            td_release(vm.fn);
            vm.fn = fn_obj;  /* takes ownership */
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            vm.sp += callee_locals;
            n_locals = callee_locals;

            int64_t pcnt = td_len(LAMBDA_PARAMS(fn_obj));
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                td_release(fn_args[i]);

            code = (uint8_t *)td_data(LAMBDA_BC(fn_obj));
            cpool = (td_t **)td_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Fallback: same as CALLF non-lambda path */
    {
        td_t *result;
        switch (fn_obj->type) {
        case TD_ATOM_UNARY:
            result = ((td_unary_fn)(uintptr_t)fn_obj->i64)(fn_args[0]);
            td_release(fn_args[0]);
            for (int32_t i = 1; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_BINARY:
            result = ((td_binary_fn)(uintptr_t)fn_obj->i64)(fn_args[0], fn_args[1]);
            td_release(fn_args[0]);
            td_release(fn_args[1]);
            for (int32_t i = 2; i < n; i++) td_release(fn_args[i]);
            break;
        case TD_ATOM_VARY:
            result = ((td_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        default:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) td_release(fn_args[i]);
            break;
        }
        td_release(fn_obj);
        PUSH(result);
        DISPATCH();
    }
}

op_calld: {
    /* Dynamic dispatch: evaluate AST directly via td_eval */
    uint8_t n = code[ip++];
    if (n == 0) {
        /* n=0: the AST itself is on the stack, eval it directly */
        td_t *ast = POP();
        td_t *result = td_eval(ast);
        td_release(ast);
        PUSH(result);
        DISPATCH();
    }
    /* n>0: build call list and eval */
    td_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    td_t *fn_obj = POP();

    td_t *call_list = td_alloc((n + 1) * sizeof(td_t *));
    call_list->type = TD_LIST;
    call_list->len = n + 1;
    td_t **elems = (td_t **)td_data(call_list);
    elems[0] = fn_obj;
    for (int32_t i = 0; i < n; i++)
        elems[i + 1] = fn_args[i];

    td_t *result = td_eval(call_list);
    td_release(call_list);
    PUSH(result);
    DISPATCH();
}

op_ret: {
    td_t *result = (vm.sp > vm.fp + n_locals) ? POP() : make_i64(0);
    td_retain(result);  /* protect from cleanup aliasing */

    /* Clean up current frame */
    while (vm.sp > vm.fp) {
        td_t *v = vm.ps[--vm.sp];
        if (v) td_release(v);
    }

    if (vm.rp == 0) {
        /* Top-level return */
        td_release(vm.fn);
        __VM = NULL;
        return result;  /* caller owns the retain */
    }

    /* Pop return frame */
    td_release(vm.fn);
    vm.rp--;
    vm.fn = vm.rs[vm.rp].fn;
    vm.fp = vm.rs[vm.rp].fp;
    ip = vm.rs[vm.rp].ip;
    code = (uint8_t *)td_data(LAMBDA_BC(vm.fn));
    cpool = (td_t **)td_data(LAMBDA_CONSTS(vm.fn));
    n_locals = LAMBDA_NLOCALS(vm.fn);
    PUSH(result);
    DISPATCH();
}

op_trap: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    vm.ts[vm.tp++] = (vm_trap_t){
        .rp = vm.rp, .sp = vm.sp, .handler_ip = ip + offset,
        .fn = vm.fn, .fp = vm.fp, .n_locals = n_locals
    };
    td_retain(vm.fn);
    DISPATCH();
}

op_trap_end: {
    if (vm.tp > 0) {
        vm.tp--;
        td_release(vm.ts[vm.tp].fn);
    }
    DISPATCH();
}

vm_error: {
    /* Check for trap frame */
    if (vm.tp > 0) {
        vm.tp--;
        vm_trap_t trap = vm.ts[vm.tp];

        /* Clean up return frames above trap point */
        while (vm.rp > trap.rp) {
            vm.rp--;
            if (vm.rs[vm.rp].fn) td_release(vm.rs[vm.rp].fn);
        }

        /* Clean up stack above trap point */
        while (vm.sp > trap.sp) {
            td_t *v = vm.ps[--vm.sp];
            if (v) td_release(v);
        }

        /* Get error value */
        td_t *err_val = __raise_val;
        __raise_val = NULL;
        if (!err_val) err_val = make_i64(0);

        /* Restore context and push error value */
        td_release(vm.fn);
        vm.fn = trap.fn;  /* takes ownership from trap frame */
        vm.fp = trap.fp;
        n_locals = trap.n_locals;
        code = (uint8_t *)td_data(LAMBDA_BC(vm.fn));
        cpool = (td_t **)td_data(LAMBDA_CONSTS(vm.fn));
        ip = trap.handler_ip;
        PUSH(err_val);
        DISPATCH();
    }

    /* No trap frame — regular error cleanup */
    for (int32_t i = 0; i < vm.sp; i++)
        if (vm.ps[i]) td_release(vm.ps[i]);
    td_release(vm.fn);
    for (int32_t i = 0; i < vm.rp; i++)
        td_release(vm.rs[i].fn);
    for (int32_t i = 0; i < vm.tp; i++)
        td_release(vm.ts[i].fn);
    __VM = NULL;
    return TD_ERR_PTR(TD_ERR_DOMAIN);
}

#undef DISPATCH
#undef PUSH
#undef POP
#undef PEEK
#undef LOCAL
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

    /* Error handling */
    register_unary("raise", TD_FN_NONE, ray_raise);
    register_binary("try",  TD_FN_SPECIAL_FORM, ray_try);
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
