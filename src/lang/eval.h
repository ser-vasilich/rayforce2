#ifndef TD_EVAL_H
#define TD_EVAL_H

#include <teide/td.h>
#include <stdio.h>

/* ===== VM Bytecode Opcodes ===== */

enum {
    OP_RET = 0,       /* return top of stack */
    OP_JMP,           /* unconditional jump (2-byte signed offset) */
    OP_JMPF,          /* jump if false (2-byte signed offset) */
    OP_LOADCONST,     /* push constant pool[operand] (1-byte index) */
    OP_LOADENV,       /* push local variable (1-byte slot index) */
    OP_STOREENV,      /* pop and store into local (1-byte slot index) */
    OP_POP,           /* discard top of stack */
    OP_RESOLVE,       /* resolve global name: constant pool[operand] is sym_id */
    OP_CALL1,         /* call unary: pop fn + 1 arg, push result */
    OP_CALL2,         /* call binary: pop fn + 2 args, push result */
    OP_CALLN,         /* call variadic: operand = argc, pop fn + N args */
    OP_CALLF,         /* call compiled lambda: push frame, jump to callee */
    OP_CALLS,         /* tail call: reuse frame */
    OP_CALLD,         /* dynamic dispatch: fallback to td_eval() */
    OP_DUP,           /* duplicate top of stack */
    OP_LOADCONST_W,   /* push constant pool[operand] (2-byte index) */
    OP_RESOLVE_W,     /* resolve global name: 2-byte constant pool index */
    OP_TRAP,          /* push trap frame, 2-byte handler offset */
    OP_TRAP_END,      /* pop trap frame (success path) */
    OP__COUNT
};

/* ===== Compiled Lambda Layout =====
 *
 * A TD_ATOM_LAMBDA object with attrs & TD_FN_COMPILED stores compiled
 * bytecode in its data area:
 *
 *   data[0] = td_t* params_list   (same as interpreted)
 *   data[1] = td_t* body          (parsed body, same as interpreted)
 *   data[2] = td_t* bytecode      (TD_U8 vector of opcodes)
 *   data[3] = td_t* constants     (TD_LIST of constant pool entries)
 *   data[4] = int32_t n_locals    (number of local slots needed)
 */

#define TD_FN_COMPILED  0x40   /* lambda has been compiled to bytecode */

#define LAMBDA_PARAMS(lam)    (((td_t**)td_data(lam))[0])
#define LAMBDA_BODY(lam)      (((td_t**)td_data(lam))[1])
#define LAMBDA_BC(lam)        (((td_t**)td_data(lam))[2])
#define LAMBDA_CONSTS(lam)    (((td_t**)td_data(lam))[3])
#define LAMBDA_NLOCALS(lam)   (*((int32_t*)&((td_t**)td_data(lam))[4]))

#define LAMBDA_IS_COMPILED(lam) ((lam)->attrs & TD_FN_COMPILED)

/* ===== VM Types ===== */

#define VM_STACK_SIZE 1024

typedef struct {
    td_t   *fn;     /* lambda being executed */
    int32_t fp;     /* frame pointer */
    int32_t ip;     /* instruction pointer */
} vm_ctx_t;

typedef struct {
    int32_t  rp;        /* return stack depth at trap point */
    int32_t  sp;        /* stack depth at trap point */
    int32_t  handler_ip;/* IP of handler code */
    td_t    *fn;        /* function containing handler code */
    int32_t  fp;        /* frame pointer at trap point */
    int32_t  n_locals;  /* n_locals at trap point */
} vm_trap_t;

#define VM_TRAP_SIZE 16

typedef struct {
    int32_t  sp;                    /* stack pointer */
    int32_t  fp;                    /* frame pointer */
    int32_t  rp;                    /* return stack pointer */
    int32_t  id;                    /* VM identifier */
    td_t    *fn;                    /* current lambda */
    void    *heap;                  /* heap pointer (future use) */
    int32_t  tp;                    /* trap stack pointer */
    td_t    *ps[VM_STACK_SIZE];     /* program stack */
    vm_ctx_t rs[VM_STACK_SIZE];     /* return stack */
    vm_trap_t ts[VM_TRAP_SIZE];     /* trap frames */
} td_vm_t;

/* ===== Public API ===== */

/* Initialize the Rayfall runtime: symbols, environment, builtins. */
td_err_t td_lang_init(void);
void     td_lang_destroy(void);

/* Evaluate a parsed td_t object tree. */
td_t* td_eval(td_t* obj);

/* Parse + eval convenience. */
td_t* td_eval_str(const char* source);

/* Compile a lambda's body to bytecode. Called lazily on first invocation. */
void td_compile(td_t* lambda);

/* Reset compiler cached state (call from td_lang_destroy). */
void td_compile_reset(void);

/* Print a td_t value to a FILE stream. */
void td_lang_print(FILE* fp, td_t* val);

#endif /* TD_EVAL_H */
